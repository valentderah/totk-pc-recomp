// The resource size table (restbl.h). Layout of a decompressed RESTBL, little-endian:
//
//   "RESTBL", u32 version (1), u32 name length (160), u32 hashed count, u32 named count
//   hashed entries {u32 crc32 of the key, u32 size}, sorted by the hash
//   named entries {char key[name length], u32 size}: the keys whose hash collides with another key's
//
// A key is a RomFS path without .zs or .mc. The estimates come from the 1.4.3 table against its own files: most types
// need the decompressed size plus at most 8 KiB (headers, alignment); a few need a multiple of it (bfres 1.5, bkres
// 2.05, bgyml 3.85, asb 1.4, bstar 1.2, bphsc 1.1). A type the table does not list takes 1.5 times its size plus 8 KiB.
#include <stdlib.h>
#include <string.h>

#include "restbl.h"

typedef struct { uint32_t hash, size; } Hashed;

struct Restbl {
    uint32_t version, name_len;
    Hashed *hashed;
    size_t nhashed, hashed_cap;
    uint8_t *named;            // named entries as in the file: key (name_len bytes, NUL padded) then u32 size
    size_t nnamed;
};

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i);
}

static uint32_t crc32_of(const char *s) {
    uint32_t c = ~0u;
    for (; *s; s++) {
        c ^= (uint8_t)*s;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

Restbl *restbl_parse(const uint8_t *data, size_t n) {
    if (n < 22 || memcmp(data, "RESTBL", 6)) return NULL;
    uint32_t version = rd32(data + 6), name_len = rd32(data + 10), nh = rd32(data + 14), nn = rd32(data + 18);
    if (!name_len || name_len > 4096 || (uint64_t)nh * 8 + (uint64_t)nn * (name_len + 4) != n - 22) return NULL;
    Restbl *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->version = version, t->name_len = name_len;
    t->hashed = malloc(((size_t)nh + 1) * sizeof *t->hashed);
    t->named = malloc((size_t)nn * (name_len + 4) + 1);
    if (!t->hashed || !t->named) {
        restbl_free(t);
        return NULL;
    }
    for (uint32_t i = 0; i < nh; i++) t->hashed[i] = (Hashed){rd32(data + 22 + i * 8), rd32(data + 26 + i * 8)};
    t->nhashed = t->hashed_cap = nh;
    if (!t->hashed_cap) t->hashed_cap = 1;
    memcpy(t->named, data + 22 + (size_t)nh * 8, (size_t)nn * (name_len + 4));
    t->nnamed = nn;
    return t;
}

void restbl_free(Restbl *t) {
    if (!t) return;
    free(t->hashed);
    free(t->named);
    free(t);
}

const char *restbl_key(const char *path, char *buf, size_t n) {
    size_t len = strlen(path);
    if (len >= 3 && (!strcmp(path + len - 3, ".zs") || !strcmp(path + len - 3, ".mc"))) len -= 3;
    if (len >= n) len = n - 1;
    memcpy(buf, path, len);
    buf[len] = 0;
    return buf;
}

// The named entry of key, or NULL.
static uint8_t *named_of(const Restbl *t, const char *key) {
    size_t len = strlen(key);
    if (len >= t->name_len) return NULL;
    for (size_t i = 0; i < t->nnamed; i++) {
        uint8_t *e = t->named + i * (t->name_len + 4);
        if (!memcmp(e, key, len + 1)) return e;
    }
    return NULL;
}

// The position of hash in the hashed entries, or where it would go.
static size_t hashed_at(const Restbl *t, uint32_t hash) {
    size_t lo = 0, hi = t->nhashed;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (t->hashed[m].hash < hash) lo = m + 1;
        else hi = m;
    }
    return lo;
}

int restbl_get(const Restbl *t, const char *key, uint32_t *size) {
    const uint8_t *e = named_of(t, key);
    if (e) {
        *size = rd32(e + t->name_len);
        return 1;
    }
    uint32_t h = crc32_of(key);
    size_t i = hashed_at(t, h);
    if (i < t->nhashed && t->hashed[i].hash == h) {
        *size = t->hashed[i].size;
        return 1;
    }
    return 0;
}

// The hashed entry of hash raised to size (added when missing); 1 when the table changed.
static int raise_hash(Restbl *t, uint32_t h, uint32_t size) {
    size_t i = hashed_at(t, h);
    if (i < t->nhashed && t->hashed[i].hash == h) {
        if (t->hashed[i].size >= size) return 0;
        t->hashed[i].size = size;
        return 1;
    }
    if (t->nhashed == t->hashed_cap) {
        size_t cap = t->hashed_cap * 2;
        Hashed *grown = realloc(t->hashed, cap * sizeof *grown);
        if (!grown) return 0;
        t->hashed = grown, t->hashed_cap = cap;
    }
    memmove(t->hashed + i + 1, t->hashed + i, (t->nhashed - i) * sizeof *t->hashed);
    t->hashed[i] = (Hashed){h, size};
    t->nhashed++;
    return 1;
}

int restbl_raise(Restbl *t, const char *key, uint32_t size) {
    uint8_t *e = named_of(t, key);
    if (!e) return raise_hash(t, crc32_of(key), size);
    if (rd32(e + t->name_len) >= size) return 0;
    wr32(e + t->name_len, size);
    return 1;
}

int restbl_merge(Restbl *t, const Restbl *base, const Restbl *shipped) {
    int raised = 0;
    for (size_t i = 0; i < shipped->nhashed; i++) {
        Hashed s = shipped->hashed[i];
        size_t k = hashed_at(base, s.hash);
        if (k < base->nhashed && base->hashed[k].hash == s.hash && base->hashed[k].size >= s.size) continue;
        raised += raise_hash(t, s.hash, s.size);
    }
    if (shipped->name_len != t->name_len) return raised;
    for (size_t i = 0; i < shipped->nnamed; i++) {
        const uint8_t *e = shipped->named + i * (shipped->name_len + 4);
        char key[4097];
        memcpy(key, e, shipped->name_len);
        key[shipped->name_len] = 0;
        uint32_t size = rd32(e + shipped->name_len), have;
        if (restbl_get(base, key, &have) && have >= size) continue;
        uint8_t *mine = named_of(t, key);
        if (mine) {
            if (rd32(mine + t->name_len) < size) wr32(mine + t->name_len, size), raised++;
            continue;
        }
        uint8_t *grown = realloc(t->named, (t->nnamed + 1) * (t->name_len + 4));
        if (!grown) return raised;
        t->named = grown;
        memcpy(t->named + t->nnamed * (t->name_len + 4), e, t->name_len + 4);
        t->nnamed++;
        raised++;
    }
    return raised;
}

int restbl_take(Restbl *t, const Restbl *from, const char *const *keys, size_t n) {
    int raised = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t size;
        if (restbl_get(from, keys[i], &size)) raised += restbl_raise(t, keys[i], size);
    }
    return raised;
}

uint8_t *restbl_write(const Restbl *t, size_t *n) {
    size_t size = 22 + t->nhashed * 8 + t->nnamed * (t->name_len + 4);
    uint8_t *out = malloc(size);
    if (!out) return NULL;
    memcpy(out, "RESTBL", 6);
    wr32(out + 6, t->version);
    wr32(out + 10, t->name_len);
    wr32(out + 14, (uint32_t)t->nhashed);
    wr32(out + 18, (uint32_t)t->nnamed);
    for (size_t i = 0; i < t->nhashed; i++) {
        wr32(out + 22 + i * 8, t->hashed[i].hash);
        wr32(out + 26 + i * 8, t->hashed[i].size);
    }
    memcpy(out + 22 + t->nhashed * 8, t->named, t->nnamed * (t->name_len + 4));
    *n = size;
    return out;
}

// ---- estimates

// The factor a type's entry needs over its decompressed size (1000 = 1.0); 0 for a type the table never lists.
static unsigned type_factor(const char *key) {
    static const struct { const char *ext; unsigned f; } types[] = {
        {"bfres", 1500}, {"bkres", 2050}, {"bgyml", 3850}, {"asb", 1400}, {"bstar", 1200}, {"bphsc", 1100},
        {"byml", 1000}, {"txtg", 1000}, {"pack", 1000}, {"bntx", 1000}, {"bphsh", 1000}, {"vsts", 1000},
        {"ainb", 1000}, {"bphnm", 1000}, {"bars", 1000}, {"chunk", 1000}, {"bfevfl", 1000}, {"quad", 1000},
        {"baev", 1000}, {"bcul", 1000}, {"crbin", 1000}, {"wbr", 1000}, {"bhtmp", 1000}, {"dpi", 1000},
        {"cai", 1000}, {"png", 1000}, {"cutinfo", 1000}, {"blwp", 1000}, {"sarc", 1000}, {"tscb", 1000},
        {"bfsha", 1000}, {"jpg", 1000}, {"beco", 1000}, {"bfarc", 1000}, {"blarc", 1000}, {"belnk", 1000},
        {"genvb", 1000}, {"bnsh", 1000}, {"bslnk", 1000}, {"baatarc", 1000}, {"bagst", 1000},
        {"bwav", 0}, {"webm", 0},
    };
    const char *leaf = strrchr(key, '/') ? strrchr(key, '/') + 1 : key, *dot = strrchr(leaf, '.');
    if (!dot) return 1500;
    for (size_t i = 0; i < sizeof types / sizeof *types; i++)
        if (!strcmp(dot + 1, types[i].ext)) return types[i].f;
    return 1500;
}

static uint32_t clamp32(uint64_t v) { return v > 0xFFFFFFF0u ? 0xFFFFFFF0u : (uint32_t)v; }

uint32_t restbl_estimate(const char *key, uint64_t size) {
    unsigned f = type_factor(key);
    if (!f) return 0;
    uint64_t e = (size * f + 999) / 1000 + 8192;
    return clamp32((e + 4095) & ~(uint64_t)4095);
}

uint32_t restbl_grow(const char *key, uint32_t entry, uint64_t old, uint64_t new_size, int scaled) {
    if (new_size <= old) return entry;
    if (scaled) return clamp32(old ? ((uint64_t)entry * new_size + old - 1) / old : restbl_estimate(key, new_size * 8));
    unsigned f = type_factor(key);
    if (!f) return entry;
    uint64_t e = entry + ((new_size - old) * f + 999) / 1000;
    return clamp32((e + 31) & ~(uint64_t)31);
}
