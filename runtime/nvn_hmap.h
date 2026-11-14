// Ported from MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn/nvn_hmap.h @ b75be8cf (MIT, Copyright (c) 2026
// jhaizhou; origin d2r-mobile); Chinese duplicate comments dropped, code unchanged but for
// nvn_hm_get's key compare (MX_EQ, memx.h).
//
// Open-addressing hash table from fixed-length byte keys -> pointers (linear probing + backward shift on deletion, no
// tombstones). The replay hot path uses it instead of a boxed dictionary: keys are not boxed, no allocation, a lookup
// does only one hash + memcmp.
// No locking: callers protect it with their own locks. Ownership of values belongs to the caller (put / del return the
// replaced / deleted old value).
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "memx.h"

typedef struct { uint8_t *keys; void **vals; uint32_t klen, cap, n; } nvn_hmap;   // vals[i] == NULL means an empty slot

static inline uint64_t nvn_hm_hash(const void *k, uint32_t len) {
    const uint8_t *p = k; uint64_t h = 0x9e3779b97f4a7c15ull ^ len;
    for (; len >= 8; p += 8, len -= 8) { uint64_t w; memcpy(&w, p, 8); h = (h ^ w) * 0xff51afd7ed558ccdull; h ^= h >> 32; }
    if (len) { uint64_t w = 0; memcpy(&w, p, len); h = (h ^ w) * 0xff51afd7ed558ccdull; }
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull; h ^= h >> 33;
    return h;
}
static inline void nvn_hm_init(nvn_hmap *m, uint32_t klen) { memset(m, 0, sizeof *m); m->klen = klen; }
static inline void *nvn_hm_get(const nvn_hmap *m, const void *k) {
    if (!m->n) return NULL;
    for (uint32_t i = (uint32_t)nvn_hm_hash(k, m->klen) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (!m->vals[i]) return NULL;
        if (MX_EQ(m->keys + (size_t)i * m->klen, k, m->klen)) return m->vals[i];
    }
}
static inline void *nvn_hm_put(nvn_hmap *m, const void *k, void *v);
static inline void nvn_hm_grow(nvn_hmap *m) {
    nvn_hmap o = *m;
    m->cap = o.cap ? o.cap * 2 : 64; m->n = 0;
    m->keys = malloc((size_t)m->cap * m->klen); m->vals = calloc(m->cap, sizeof *m->vals);
    if (!m->keys || !m->vals) abort();
    for (uint32_t i = 0; i < o.cap; i++) if (o.vals[i]) nvn_hm_put(m, o.keys + (size_t)i * o.klen, o.vals[i]);
    free(o.keys); free(o.vals);
}
// v must not be NULL; returns the old value for the same key (NULL if none)
static inline void *nvn_hm_put(nvn_hmap *m, const void *k, void *v) {
    if ((m->n + 1) * 4 > m->cap * 3) nvn_hm_grow(m);   // Load factor <= 3/4
    for (uint32_t i = (uint32_t)nvn_hm_hash(k, m->klen) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        uint8_t *ki = m->keys + (size_t)i * m->klen;
        if (!m->vals[i]) { memcpy(ki, k, m->klen); m->vals[i] = v; m->n++; return NULL; }
        if (!memcmp(ki, k, m->klen)) { void *old = m->vals[i]; m->vals[i] = v; return old; }
    }
}
// After deletion, shift back later entries on the same probe chain to fill the hole, so that a lookup can stop at the
// first empty slot
static inline void *nvn_hm_del(nvn_hmap *m, const void *k) {
    if (!m->n) return NULL;
    uint32_t mask = m->cap - 1, i = (uint32_t)nvn_hm_hash(k, m->klen) & mask;
    for (;; i = (i + 1) & mask) {
        if (!m->vals[i]) return NULL;
        if (!memcmp(m->keys + (size_t)i * m->klen, k, m->klen)) break;
    }
    void *old = m->vals[i];
    for (uint32_t j = i;;) {
        m->vals[i] = NULL;
        for (;;) {
            j = (j + 1) & mask;
            if (!m->vals[j]) { m->n--; return old; }
            uint32_t h = (uint32_t)nvn_hm_hash(m->keys + (size_t)j * m->klen, m->klen) & mask;
            if (i <= j ? (i < h && h <= j) : (i < h || h <= j)) continue;   // Ideal position is within (i, j]: cannot move forward
            break;
        }
        memcpy(m->keys + (size_t)i * m->klen, m->keys + (size_t)j * m->klen, m->klen); m->vals[i] = m->vals[j];
        i = j;
    }
}
// Clear; when rel is non-null, call it once for each value (releasing ownership)
static inline void nvn_hm_clear(nvn_hmap *m, void (*rel)(void *)) {
    for (uint32_t i = 0; i < m->cap; i++) if (m->vals[i]) { if (rel) rel(m->vals[i]); m->vals[i] = NULL; }
    m->n = 0;
}
