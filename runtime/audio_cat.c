// Sound categories (music, effects, voices) for the volume_music / volume_sfx / volume_voice settings.
//
// The audio renderer only sees sample memory, so the category is learned where the game loads its sounds:
//   - RomFS opens and reads of Sound/Resource/ files (romfs.c romfs_open / romfs_read): the file name gives the
//     category (BGM_* music, NV_* and *_Vo_* / *Voice* voices, the rest effects);
//   - zstd decodes of BARS banks (ov_zstd.c decode): each bank entry's AMTA name refines the bank's category;
//   - Opus decodes (ov_media.c ov_decode): the decoded PCM inherits the category of the packet it came from.
// Two tables answer the mixer (audio_mixer.c voice_category):
//   - DSP-ADPCM coefficient tables, by a hash of their 32 bytes, read from each BWAV channel header. The game copies
//     these coefficients next to the voice, so a voice's own table names its sound wherever its samples live;
//   - guest address ranges of loaded or decoded audio, newest first, for PCM voices (Opus output).
// Everything here runs only while one of the three settings is below 100 (audio_cat_active): at the defaults no table
// is written or read. A setting moved during play classifies what loads from then on; sounds loaded before (an area's
// effect banks, a stream opened earlier) are found when their next stream read lands, otherwise they count as effects.
#include "audio_cat.h"
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

#include <stdatomic.h>
#include <string.h>
#include <strings.h>

#define RANGE_CAP 4096    // address ranges, a ring: the oldest go first. Lookups scan it, so it stays small
#define COEF_CAP 65536    // coefficient tables, open addressing; a full probe run drops the new table (it counts as SFX)
#define COEF_PROBES 64

typedef struct {
    uint64_t start, end;
    uint8_t cat;
} Range;

static Range ranges[RANGE_CAP];
static uint32_t range_head;              // ranges written so far; under range_lock
static PlatMutex range_lock = PLAT_MUTEX_INIT;

typedef struct {
    _Atomic uint64_t hash;   // 0 = free; a slot is claimed once and never freed
    _Atomic uint8_t cat;
} Coef;

static Coef coefs[COEF_CAP];

typedef struct {
    uint64_t guest;
    uint8_t cat;
} Decoder;

static Decoder decoders[64];             // the category each Opus decoder produced last; under decoder_lock
static unsigned decoder_next;
static PlatMutex decoder_lock = PLAT_MUTEX_INIT;

static _Atomic uint64_t coef_count[4], range_count, bars_count;

int audio_cat_active(void) {
    static _Atomic unsigned seen = ~0u;
    static _Atomic int on;
    unsigned ver = cfg_version();
    if (atomic_load_explicit(&seen, memory_order_acquire) != ver) {
        int a = cfg_double("TOTK_VOLUME_MUSIC") < 100 || cfg_double("TOTK_VOLUME_SFX") < 100 ||
                cfg_double("TOTK_VOLUME_VOICE") < 100;
        atomic_store_explicit(&on, a, memory_order_relaxed);
        atomic_store_explicit(&seen, ver, memory_order_release);
    }
    return atomic_load_explicit(&on, memory_order_relaxed);
}

static int ci_str_has(const char *haystack, const char *needle) {
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++)
        if (strncasecmp(haystack, needle, nlen) == 0) return 1;
    return 0;
}

static int all_zero32(const uint8_t *p) {
    for (int i = 0; i < 32; i++)
        if (p[i]) return 0;
    return 1;
}

static uint64_t fnv1a32(const uint8_t *p) {
    uint64_t h = 14695981039346656037ULL;
    for (int i = 0; i < 32; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}

// A table seen under several categories keeps music or voice over effects: one sample shared by a voice line and an
// effect bank follows the voice setting.
static void register_coef32(const uint8_t *coef, int cat) {
    if (cat <= AUDIO_CAT_UNKNOWN || cat > AUDIO_CAT_VOICE || all_zero32(coef)) return;
    uint64_t h = fnv1a32(coef);
    for (uint32_t step = 0; step < COEF_PROBES; step++) {
        Coef *e = &coefs[((uint32_t)h + step) & (COEF_CAP - 1)];
        uint64_t cur = atomic_load_explicit(&e->hash, memory_order_acquire);
        if (cur == 0 && atomic_compare_exchange_strong_explicit(&e->hash, &cur, h, memory_order_acq_rel,
                                                                memory_order_acquire)) {
            atomic_store_explicit(&e->cat, (uint8_t)cat, memory_order_release);
            atomic_fetch_add_explicit(&coef_count[cat], 1, memory_order_relaxed);
            return;
        }
        if (cur == h) {
            if (cat != AUDIO_CAT_SFX) atomic_store_explicit(&e->cat, (uint8_t)cat, memory_order_release);
            return;
        }
    }
}

int audio_cat_lookup_coef(uint64_t extra_addr, uint64_t extra_size) {
    if (!extra_addr || extra_size < 32 || extra_addr >= RT_ADDRESS_LIMIT || !mem_guest_readable(extra_addr, 32))
        return AUDIO_CAT_UNKNOWN;
    const uint8_t *p = (const uint8_t *)(uintptr_t)extra_addr;
    if (all_zero32(p)) return AUDIO_CAT_UNKNOWN;
    uint64_t h = fnv1a32(p);
    for (uint32_t step = 0; step < COEF_PROBES; step++) {
        Coef *e = &coefs[((uint32_t)h + step) & (COEF_CAP - 1)];
        uint64_t cur = atomic_load_explicit(&e->hash, memory_order_acquire);
        if (cur == h) return atomic_load_explicit(&e->cat, memory_order_acquire);   // 0 while the writer is midway
        if (cur == 0) break;
    }
    return AUDIO_CAT_UNKNOWN;
}

// BWAV: "BWAV", channel count at 0x0E, then 0x4C-byte channel infos from 0x10: codec u16 (1 = DSP-ADPCM) and the 16
// ADPCM coefficients at +0x10.
void audio_cat_scan_bwav(const void *data, uint64_t size, int cat) {
    if (!data || size < 0x40 || cat <= AUDIO_CAT_UNKNOWN) return;
    const uint8_t *b = data;
    uint32_t magic;
    uint16_t nch;
    memcpy(&magic, b, 4);
    memcpy(&nch, b + 0x0E, 2);
    if (magic != 0x56415742u || nch == 0 || nch > 16) return;
    for (uint16_t ch = 0; ch < nch; ch++) {
        uint64_t at = 0x10ull + ch * 0x4Cull;
        if (at + 0x30 > size) break;
        uint16_t codec;
        memcpy(&codec, b + at, 2);
        if (codec == 1) register_coef32(b + at + 0x10, cat);
    }
}

// Every sound bank and stream of the game is under Sound/Resource/ (Sound/Resource/*.bars.zs, Sound/Resource/Stream/
// *.bwav); everything else is not audio. Creature and player vocal banks (Bokoblin_Voice0, Player_Voice, *_Vo_*)
// count as voices, as do the NV_* character voice lines.
int audio_cat_from_path(const char *path) {
    if (!path || strncmp(path, "Sound/Resource/", 15)) return AUDIO_CAT_UNKNOWN;
    const char *name = strrchr(path, '/') + 1;
    if (!strncasecmp(name, "nv_", 3) || !strncasecmp(name, "vo_", 3)) return AUDIO_CAT_VOICE;
    if (ci_str_has(name, "bgm_")) return AUDIO_CAT_MUSIC;
    if (ci_str_has(name, "_voice") || ci_str_has(name, "voice_") || ci_str_has(name, "_vo_") ||
        ci_str_has(name, "shoutvoice") || ci_str_has(name, "shoutmsg"))
        return AUDIO_CAT_VOICE;
    return AUDIO_CAT_SFX;
}

void audio_cat_tag_range(uint64_t addr, uint64_t size, int cat) {
    if (!addr || !size || cat <= AUDIO_CAT_UNKNOWN || cat > AUDIO_CAT_VOICE) return;
    plat_mutex_lock(&range_lock);
    // A stream refills the same buffers: the same range read again only changes its category.
    uint32_t depth = range_head < 64 ? range_head : 64;
    for (uint32_t k = 0; k < depth; k++) {
        Range *r = &ranges[(range_head - 1 - k) & (RANGE_CAP - 1)];
        if (r->start == addr && r->end == addr + size) {
            r->cat = (uint8_t)cat;
            plat_mutex_unlock(&range_lock);
            return;
        }
    }
    Range *r = &ranges[range_head & (RANGE_CAP - 1)];
    r->start = addr;
    r->end = addr + size;
    r->cat = (uint8_t)cat;
    range_head++;
    plat_mutex_unlock(&range_lock);
    atomic_fetch_add_explicit(&range_count, 1, memory_order_relaxed);
}

// The newest range holding addr; newer loads into reused memory win over older ones.
int audio_cat_lookup(uint64_t addr) {
    if (!addr) return AUDIO_CAT_UNKNOWN;
    int cat = AUDIO_CAT_UNKNOWN;
    plat_mutex_lock_shared(&range_lock);
    uint32_t n = range_head < RANGE_CAP ? range_head : RANGE_CAP;
    for (uint32_t k = 0; k < n; k++) {
        const Range *r = &ranges[(range_head - 1 - k) & (RANGE_CAP - 1)];
        if (addr >= r->start && addr < r->end) {
            cat = r->cat;
            break;
        }
    }
    plat_mutex_unlock_shared(&range_lock);
    return cat;
}

// BARS: "BARS", entry count at 0x0C, then count CRC32 name hashes and count (AMTA offset, BWAV offset) pairs. The AMTA
// block holds the entry's name; music and voice names inside an effects bank (a boss bank with its BGM) get their own
// category. ADPCM entries register their coefficients; other entries tag their bytes.
void audio_cat_scan_bars(uint64_t dst, uint64_t size, int fallback_cat) {
    if (!dst || size < 16) return;
    atomic_fetch_add_explicit(&bars_count, 1, memory_order_relaxed);
    if (fallback_cat != AUDIO_CAT_UNKNOWN) audio_cat_tag_range(dst, size, fallback_cat);
    const uint8_t *base = (const uint8_t *)(uintptr_t)dst;
    uint32_t count;
    memcpy(&count, base + 0x0C, 4);
    if (count > 16384 || 0x10ull + (uint64_t)count * 12ull > size) return;
    const uint8_t *pairs = base + 0x10 + (uint64_t)count * 4;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t amta_off, bwav_off;
        memcpy(&amta_off, pairs + k * 8ull, 4);
        memcpy(&bwav_off, pairs + k * 8ull + 4, 4);
        if (bwav_off >= size) continue;
        int cat = fallback_cat != AUDIO_CAT_UNKNOWN ? fallback_cat : AUDIO_CAT_SFX;
        if (amta_off < size) {
            uint64_t limit = size - amta_off < 512 ? size - amta_off : 512;
            const char *a = (const char *)(base + amta_off);
            for (uint64_t s = 0; s + 4 <= limit; s++) {
                if (!memcmp(a + s, "BGM_", 4)) { cat = AUDIO_CAT_MUSIC; break; }
                if (!memcmp(a + s, "Vo_", 3) || !memcmp(a + s, "NV_", 3)) { cat = AUDIO_CAT_VOICE; break; }
                if (s + 5 <= limit && !memcmp(a + s, "Voice", 5)) { cat = AUDIO_CAT_VOICE; break; }
            }
        }
        const uint8_t *w = base + bwav_off;
        uint32_t magic = 0;
        uint16_t codec = 0;
        if (bwav_off + 0x40 <= size) {
            memcpy(&magic, w, 4);
            memcpy(&codec, w + 0x10, 2);
        }
        if (magic == 0x56415742u) audio_cat_scan_bwav(w, size - bwav_off, cat);
        if (cat == fallback_cat || (magic == 0x56415742u && codec == 1)) continue;
        // The entry ends where the next BWAV starts: one pass over the offsets, only for the few entries tagged here.
        uint64_t next = size;
        for (uint32_t j = 0; j < count; j++) {
            uint32_t o;
            memcpy(&o, pairs + j * 8ull + 4, 4);
            if (o > bwav_off && o < next) next = o;
        }
        audio_cat_tag_range(dst + bwav_off, next - bwav_off, cat);
    }
}

// The PCM out of one Opus packet takes the packet's category, else the decoder's last one, else effects. Consecutive
// packets of one decoder fill one buffer, so the output grows the newest matching range instead of adding one each.
void audio_cat_on_opus_decode(uint64_t decoder_guest, uint64_t in_addr, uint64_t out_addr, uint64_t out_size) {
    if (!out_addr || !out_size) return;
    int cat = audio_cat_lookup(in_addr);
    plat_mutex_lock(&decoder_lock);
    int slot = -1;
    for (int i = 0; i < 64 && decoder_guest; i++)
        if (decoders[i].guest == decoder_guest) slot = i;
    if (cat == AUDIO_CAT_UNKNOWN && slot >= 0) cat = decoders[slot].cat;
    if (cat == AUDIO_CAT_UNKNOWN) cat = AUDIO_CAT_SFX;
    if (decoder_guest) {
        if (slot < 0) {
            slot = (int)(decoder_next++ & 63);
            decoders[slot].guest = decoder_guest;
        }
        decoders[slot].cat = (uint8_t)cat;
    }
    plat_mutex_unlock(&decoder_lock);

    plat_mutex_lock(&range_lock);
    uint32_t depth = range_head < 32 ? range_head : 32;
    for (uint32_t k = 0; k < depth; k++) {
        Range *r = &ranges[(range_head - 1 - k) & (RANGE_CAP - 1)];
        if (r->cat != (uint8_t)cat) continue;
        uint64_t lo = r->start > 65536 ? r->start - 65536 : 0, hi = r->end + 65536;
        if (out_addr >= lo && out_addr + out_size <= hi) {
            if (out_addr < r->start) r->start = out_addr;
            if (out_addr + out_size > r->end) r->end = out_addr + out_size;
            plat_mutex_unlock(&range_lock);
            return;
        }
    }
    plat_mutex_unlock(&range_lock);
    audio_cat_tag_range(out_addr, out_size, cat);
}

void audio_cat_log_tables(void) {
    rt_log("audio: categories known: coefficient tables music %llu, sfx %llu, voice %llu; address ranges %llu "
           "(ring %d); BARS banks scanned %llu",
           (unsigned long long)atomic_load(&coef_count[AUDIO_CAT_MUSIC]),
           (unsigned long long)atomic_load(&coef_count[AUDIO_CAT_SFX]),
           (unsigned long long)atomic_load(&coef_count[AUDIO_CAT_VOICE]), (unsigned long long)atomic_load(&range_count),
           RANGE_CAP, (unsigned long long)atomic_load(&bars_count));
}
