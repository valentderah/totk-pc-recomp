#include "audio_cat.h"
#include "host.h"
#include "platform/platform.h"

#include <stdatomic.h>
#include <string.h>
#include <strings.h>

#define AUDIO_RANGE_CAP 32768
#define COEF_TABLE_CAP 65536

typedef struct {
    uint64_t start;
    uint64_t end;
    uint8_t cat;
    uint8_t pad[7];
} AudioRange;

static AudioRange s_ranges[AUDIO_RANGE_CAP];
static _Atomic uint32_t s_range_head = 0;
static PlatMutex s_range_lock = PLAT_MUTEX_INIT;

typedef struct {
    _Atomic uint64_t hash;
    _Atomic uint8_t cat;
} CoefEntry;

static CoefEntry s_coef_table[COEF_TABLE_CAP];

typedef struct {
    uint64_t guest;
    uint8_t cat;
} DecoderEntry;

static DecoderEntry s_decoders[64];
static PlatMutex s_dec_lock = PLAT_MUTEX_INIT;

static int ci_str_has(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, nlen) == 0) return 1;
    }
    return 0;
}

static int is_all_zero32(const void *data) {
    const uint8_t *p = (const uint8_t *)data;
    for (int i = 0; i < 32; i++) {
        if (p[i]) return 0;
    }
    return 1;
}

static uint64_t fnv1a_coef32(const void *data) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 14695981039346656037ULL;
    for (int i = 0; i < 32; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h ? h : 1ULL;
}

static void register_coef32(const void *coef32, int cat) {
    if (!coef32 || cat <= AUDIO_CAT_UNKNOWN || cat > AUDIO_CAT_VOICE) return;
    if (is_all_zero32(coef32)) return;

    uint64_t h = fnv1a_coef32(coef32);
    for (uint32_t step = 0; step < 64; step++) {
        uint32_t idx = ((uint32_t)h + step) & (COEF_TABLE_CAP - 1);
        uint64_t cur = atomic_load_explicit(&s_coef_table[idx].hash, memory_order_acquire);
        if (cur == h) {
            uint8_t existing = atomic_load_explicit(&s_coef_table[idx].cat, memory_order_relaxed);
            if (cat == AUDIO_CAT_MUSIC || cat == AUDIO_CAT_VOICE || existing <= AUDIO_CAT_UNKNOWN || existing == AUDIO_CAT_SFX) {
                atomic_store_explicit(&s_coef_table[idx].cat, (uint8_t)cat, memory_order_release);
            }
            return;
        }
        if (cur == 0) {
            if (atomic_compare_exchange_strong_explicit(&s_coef_table[idx].hash, &cur, h, memory_order_acq_rel, memory_order_acquire) || cur == h) {
                atomic_store_explicit(&s_coef_table[idx].cat, (uint8_t)cat, memory_order_release);
                return;
            }
        }
    }
}

int audio_cat_lookup_coef(uint64_t extra_addr, uint64_t extra_size) {
    if (!extra_addr || extra_size < 32 || extra_addr >= RT_ADDRESS_LIMIT) return AUDIO_CAT_UNKNOWN;
    if (!mem_guest_readable(extra_addr, 32)) return AUDIO_CAT_UNKNOWN;
    const void *ptr = (const void *)(uintptr_t)extra_addr;
    if (is_all_zero32(ptr)) return AUDIO_CAT_UNKNOWN;
    uint64_t h = fnv1a_coef32(ptr);
    for (uint32_t step = 0; step < 64; step++) {
        uint32_t idx = ((uint32_t)h + step) & (COEF_TABLE_CAP - 1);
        uint64_t cur = atomic_load_explicit(&s_coef_table[idx].hash, memory_order_acquire);
        if (cur == h) {
            return (int)atomic_load_explicit(&s_coef_table[idx].cat, memory_order_acquire);
        }
        if (cur == 0) return AUDIO_CAT_UNKNOWN;
    }
    return AUDIO_CAT_UNKNOWN;
}

void audio_cat_scan_bwav(const void *data, uint64_t size, int cat) {
    if (!data || size < 0x40 || cat <= AUDIO_CAT_UNKNOWN) return;
    const uint8_t *b = (const uint8_t *)data;
    if (*(const uint32_t *)b != 0x56415742u) return;

    uint16_t num_ch = *(const uint16_t *)(b + 0x0E);
    if (num_ch == 0 || num_ch > 16) return;

    for (uint16_t ch = 0; ch < num_ch; ch++) {
        uint64_t ch_off = 0x10ull + (uint64_t)ch * 0x4Cull;
        if (ch_off + 0x30ull > size) break;
        uint16_t codec = *(const uint16_t *)(b + ch_off);
        if (codec == 1) {
            register_coef32(b + ch_off + 0x10, cat);
        }
    }
}

int audio_cat_from_path(const char *path) {
    if (!path) return AUDIO_CAT_UNKNOWN;

    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *fname = path;
    if (slash && slash + 1 > fname) fname = slash + 1;
    if (bslash && bslash + 1 > fname) fname = bslash + 1;
    if (strncasecmp(fname, "nv_", 3) == 0 || strncasecmp(fname, "vo_", 3) == 0) {
        return AUDIO_CAT_VOICE;
    }

    if (!ci_str_has(path, "sound/") &&
        !ci_str_has(path, "sound\\") &&
        !ci_str_has(path, "voice/") &&
        !ci_str_has(path, "voice\\") &&
        !ci_str_has(path, ".bars") &&
        !ci_str_has(path, ".bwav") &&
        !ci_str_has(path, ".bfsar")) {
        return AUDIO_CAT_UNKNOWN;
    }

    if (ci_str_has(path, "bgm_") ||
        ci_str_has(path, "/bgm/") ||
        ci_str_has(path, "\\bgm\\") ||
        ci_str_has(path, "sound/bgm") ||
        ci_str_has(path, "sound\\bgm")) {
        return AUDIO_CAT_MUSIC;
    }

    if (ci_str_has(path, "voice/") ||
        ci_str_has(path, "voice\\") ||
        ci_str_has(path, "_voice") ||
        ci_str_has(path, "voice_") ||
        ci_str_has(path, "player_voice") ||
        ci_str_has(path, "_vo_") ||
        ci_str_has(path, "/vo_") ||
        ci_str_has(path, "\\vo_") ||
        ci_str_has(path, "/nv_") ||
        ci_str_has(path, "\\nv_") ||
        ci_str_has(path, "shoutvoice") ||
        ci_str_has(path, "eventflowmsg") ||
        ci_str_has(path, "shoutmsg")) {
        return AUDIO_CAT_VOICE;
    }

    return AUDIO_CAT_SFX;
}

void audio_cat_tag_range(uint64_t addr, uint64_t size, int cat) {
    if (!addr || !size || cat <= AUDIO_CAT_UNKNOWN || cat > AUDIO_CAT_VOICE) return;

    plat_mutex_lock(&s_range_lock);
    uint32_t head = atomic_load_explicit(&s_range_head, memory_order_relaxed);
    uint32_t search_depth = head < 64 ? head : 64;
    for (uint32_t k = 0; k < search_depth; k++) {
        uint32_t idx = (head - 1 - k) & (AUDIO_RANGE_CAP - 1);
        if (s_ranges[idx].start == addr && s_ranges[idx].end == addr + size) {
            s_ranges[idx].cat = (uint8_t)cat;
            plat_mutex_unlock(&s_range_lock);
            return;
        }
    }

    uint32_t idx = head & (AUDIO_RANGE_CAP - 1);
    s_ranges[idx].start = addr;
    s_ranges[idx].end = addr + size;
    s_ranges[idx].cat = (uint8_t)cat;
    atomic_store_explicit(&s_range_head, head + 1, memory_order_release);
    plat_mutex_unlock(&s_range_lock);
}

int audio_cat_lookup(uint64_t addr) {
    if (!addr) return AUDIO_CAT_UNKNOWN;

    uint32_t head = atomic_load_explicit(&s_range_head, memory_order_acquire);
    uint32_t limit = head > AUDIO_RANGE_CAP ? head - AUDIO_RANGE_CAP : 0;
    for (uint32_t i = head; i > limit; i--) {
        uint32_t idx = (i - 1) & (AUDIO_RANGE_CAP - 1);
        uint64_t start = s_ranges[idx].start;
        uint64_t end = s_ranges[idx].end;
        if (addr >= start && addr < end) {
            return (int)s_ranges[idx].cat;
        }
    }

    if (addr >= 0x40 && addr < RT_ADDRESS_LIMIT) {
        uint64_t min_check = addr >= 256 ? addr - 256 : 0;
        min_check &= ~0xFull;
        for (uint64_t b = addr & ~0xFull; b >= min_check; b -= 16) {
            if (mem_guest_readable(b, 0x20)) {
                const uint32_t *p = (const uint32_t *)(uintptr_t)b;
                if (p[0] == 0x56415742u) {
                    const uint8_t *bp = (const uint8_t *)p;
                    uint16_t codec = *(const uint16_t *)(bp + 0x10);
                    if (codec == 2) return AUDIO_CAT_VOICE;
                    return AUDIO_CAT_SFX;
                }
            }
            if (b == 0) break;
        }
    }

    return AUDIO_CAT_UNKNOWN;
}

void audio_cat_scan_bars(uint64_t dst, uint64_t size, int fallback_cat) {
    if (!dst || !size) return;

    if (fallback_cat != AUDIO_CAT_UNKNOWN) {
        audio_cat_tag_range(dst, size, fallback_cat);
    }

    if (size < 16) return;
    const uint8_t *base = (const uint8_t *)(uintptr_t)dst;
    if (*(const uint32_t *)base != 0x53524142u) {
        return;
    }

    uint32_t count = *(const uint32_t *)(base + 0x0C);
    if (count > 16384 || 0x10ull + (uint64_t)count * 12ull > size) return;

    const uint32_t *pairs = (const uint32_t *)(base + 0x10 + count * 4);
    for (uint32_t k = 0; k < count; k++) {
        uint32_t amta_off = pairs[k * 2];
        uint32_t bwav_off = pairs[k * 2 + 1];
        if (bwav_off >= size) continue;

        int cat = (fallback_cat != AUDIO_CAT_UNKNOWN) ? fallback_cat : AUDIO_CAT_SFX;

        if (amta_off < size && amta_off + 12 <= size && *(const uint32_t *)(base + amta_off) == 0x41544D41u) {
            uint32_t amta_hdr_size = *(const uint32_t *)(base + amta_off + 8);
            uint32_t amta_scan_limit = amta_hdr_size ? amta_hdr_size : 256;
            if (amta_scan_limit > 256) amta_scan_limit = 256;
            if (amta_off + amta_scan_limit > size) {
                amta_scan_limit = (uint32_t)(size - amta_off);
            }
            for (uint32_t j = 0; j < count; j++) {
                uint32_t ao = pairs[j * 2];
                if (ao > amta_off && ao - amta_off < amta_scan_limit) {
                    amta_scan_limit = ao - amta_off;
                }
                uint32_t bo = pairs[j * 2 + 1];
                if (bo > amta_off && bo - amta_off < amta_scan_limit) {
                    amta_scan_limit = bo - amta_off;
                }
            }

            const char *amta_bytes = (const char *)(base + amta_off);
            for (uint32_t s = 0; s + 4 <= amta_scan_limit; s++) {
                if (memcmp(amta_bytes + s, "BGM_", 4) == 0) {
                    char prev = s > 0 ? amta_bytes[s - 1] : '\0';
                    int is_alnum_or_under = (prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') ||
                                            (prev >= '0' && prev <= '9') || prev == '_';
                    if (!is_alnum_or_under) {
                        cat = AUDIO_CAT_MUSIC;
                        break;
                    }
                }
                if (memcmp(amta_bytes + s, "_Vo_", 4) == 0 || memcmp(amta_bytes + s, "Vo_", 3) == 0 ||
                    memcmp(amta_bytes + s, "NV_", 3) == 0) {
                    cat = AUDIO_CAT_VOICE;
                    break;
                }
                if (s + 5 <= amta_scan_limit && memcmp(amta_bytes + s, "Voice", 5) == 0) {
                    cat = AUDIO_CAT_VOICE;
                    break;
                }
                if (amta_bytes[s] == 'P' && amta_bytes[s + 1] == 'V' &&
                    amta_bytes[s + 2] >= '0' && amta_bytes[s + 2] <= '9') {
                    cat = AUDIO_CAT_VOICE;
                    break;
                }
            }
        }

        uint64_t next_off = size;
        for (uint32_t j = 0; j < count; j++) {
            uint32_t o = pairs[j * 2 + 1];
            if (o > bwav_off && o < next_off) {
                next_off = o;
            }
        }
        uint64_t bwav_span = next_off - bwav_off;

        if (bwav_off + 0x40 <= size && *(const uint32_t *)(base + bwav_off) == 0x56415742u) {
            uint16_t codec = *(const uint16_t *)(base + bwav_off + 0x10);
            if (codec == 2 && cat == AUDIO_CAT_SFX) {
                cat = AUDIO_CAT_VOICE;
            }
            audio_cat_scan_bwav(base + bwav_off, size - bwav_off, cat);
            if (codec == 2 || cat != fallback_cat) {
                audio_cat_tag_range(dst + bwav_off, bwav_span, cat);
            }
        } else if (cat != fallback_cat) {
            audio_cat_tag_range(dst + bwav_off, bwav_span, cat);
        }
    }
}

void audio_cat_on_opus_decode(uint64_t decoder_guest, uint64_t in_addr, uint64_t out_addr, uint64_t out_size) {
    if (!out_addr || !out_size) return;

    int cat = audio_cat_lookup(in_addr);
    if (cat == AUDIO_CAT_UNKNOWN) {
        if (in_addr >= 0x40 && in_addr < RT_ADDRESS_LIMIT) {
            uint64_t min_check = in_addr >= 256 ? in_addr - 256 : 0;
            min_check &= ~0xFull;
            for (uint64_t b = in_addr & ~0xFull; b >= min_check; b -= 16) {
                if (mem_guest_readable(b, 0x20)) {
                    const uint32_t *p = (const uint32_t *)(uintptr_t)b;
                    if (p[0] == 0x56415742u) {
                        uint16_t codec = *(const uint16_t *)((const uint8_t *)p + 0x10);
                        if (codec == 2) { cat = AUDIO_CAT_VOICE; break; }
                    }
                }
                if (b == 0) break;
            }
        }
    }

    plat_mutex_lock(&s_dec_lock);
    if (cat == AUDIO_CAT_UNKNOWN && decoder_guest) {
        for (int i = 0; i < 64; i++) {
            if (s_decoders[i].guest == decoder_guest && s_decoders[i].cat != AUDIO_CAT_UNKNOWN) {
                cat = s_decoders[i].cat;
                break;
            }
        }
    }
    if (cat == AUDIO_CAT_UNKNOWN) {
        cat = AUDIO_CAT_VOICE;
    }
    if (decoder_guest) {
        int found = 0;
        for (int i = 0; i < 64; i++) {
            if (s_decoders[i].guest == decoder_guest) {
                if (cat != AUDIO_CAT_UNKNOWN) s_decoders[i].cat = (uint8_t)cat;
                found = 1;
                break;
            }
        }
        if (!found) {
            static int dec_head = 0;
            int slot = dec_head++ & 63;
            s_decoders[slot].guest = decoder_guest;
            s_decoders[slot].cat = (uint8_t)cat;
        }
    }
    plat_mutex_unlock(&s_dec_lock);

    plat_mutex_lock(&s_range_lock);
    uint32_t head = atomic_load_explicit(&s_range_head, memory_order_relaxed);
    uint32_t check_count = head < 32 ? head : 32;
    for (uint32_t k = 0; k < check_count; k++) {
        uint32_t idx = (head - 1 - k) & (AUDIO_RANGE_CAP - 1);
        if (s_ranges[idx].cat == (uint8_t)cat) {
            if (out_addr >= s_ranges[idx].start && out_addr + out_size <= s_ranges[idx].end) {
                plat_mutex_unlock(&s_range_lock);
                return;
            }
            if (out_addr == s_ranges[idx].end) {
                s_ranges[idx].end = out_addr + out_size;
                plat_mutex_unlock(&s_range_lock);
                return;
            }
            if (out_addr + out_size == s_ranges[idx].start) {
                s_ranges[idx].start = out_addr;
                plat_mutex_unlock(&s_range_lock);
                return;
            }
            if (out_addr >= (s_ranges[idx].start > 65536 ? s_ranges[idx].start - 65536 : 0) &&
                out_addr + out_size <= s_ranges[idx].end + 65536) {
                if (out_addr < s_ranges[idx].start) s_ranges[idx].start = out_addr;
                if (out_addr + out_size > s_ranges[idx].end) s_ranges[idx].end = out_addr + out_size;
                plat_mutex_unlock(&s_range_lock);
                return;
            }
        }
    }
    plat_mutex_unlock(&s_range_lock);

    audio_cat_tag_range(out_addr, out_size, cat);
}
