// SHA-256 (FIPS 180-4). The block function has two forms: one on the x86 SHA extensions (SHA-NI: Intel since Ice Lake
// and Goldmont, AMD since Zen), which hashes the RomFS image at about the speed of a disk read, and portable C for CPUs
// without them. sha256_selftest in the probes compares both against the FIPS test vectors.
#include "sha256.h"

#include <string.h>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t ror(uint32_t x, int n) { return x >> n | x << (32 - n); }

static void blocks_c(uint32_t h[8], const uint8_t *p, size_t n) {
    for (; n; n--, p += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ w[i - 15] >> 3;
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ w[i - 2] >> 10;
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = k + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
    }
}

#if defined(__x86_64__)
// The state lives in two registers as the SHA instructions want it: ABEF and CDGH. Each group of four rounds adds the
// round constants to four message words and runs sha256rnds2 twice; the message schedule for words 16..63 is
// msg2(msg1(W[t-16..], W[t-12..]) + W[t-7..], W[t-4..]).
__attribute__((target("sha,sse4.1"))) static void blocks_ni(uint32_t h[8], const uint8_t *p, size_t n) {
    const __m128i bswap = _mm_set_epi64x(0x0c0d0e0f08090a0bull, 0x0405060700010203ull);
    __m128i t = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)&h[0]), 0xb1);   // CDAB
    __m128i s1 = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)&h[4]), 0x1b);  // EFGH
    __m128i s0 = _mm_alignr_epi8(t, s1, 8);                                          // ABEF
    s1 = _mm_blend_epi16(s1, t, 0xf0);                                               // CDGH
    for (; n; n--, p += 64) {
        __m128i a0 = s0, c0 = s1, m[4];
        for (int g = 0; g < 16; g++) {
            __m128i *w = &m[g & 3];
            if (g < 4) *w = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 16 * g)), bswap);
            else
                *w = _mm_sha256msg2_epu32(_mm_add_epi32(_mm_sha256msg1_epu32(*w, m[(g + 1) & 3]),
                                                        _mm_alignr_epi8(m[(g + 3) & 3], m[(g + 2) & 3], 4)),
                                          m[(g + 3) & 3]);
            __m128i k = _mm_add_epi32(*w, _mm_loadu_si128((const __m128i *)&K[4 * g]));
            s1 = _mm_sha256rnds2_epu32(s1, s0, k);
            s0 = _mm_sha256rnds2_epu32(s0, s1, _mm_shuffle_epi32(k, 0x0e));
        }
        s0 = _mm_add_epi32(s0, a0);
        s1 = _mm_add_epi32(s1, c0);
    }
    t = _mm_shuffle_epi32(s0, 0x1b);                  // FEBA
    s1 = _mm_shuffle_epi32(s1, 0xb1);                 // DCHG
    _mm_storeu_si128((__m128i *)&h[0], _mm_blend_epi16(t, s1, 0xf0));   // DCBA
    _mm_storeu_si128((__m128i *)&h[4], _mm_alignr_epi8(s1, t, 8));      // HGFE
}
#endif

static void blocks(uint32_t h[8], const uint8_t *p, size_t n) {
#if defined(__x86_64__)
    static int ni = -1;
    if (ni < 0) ni = __builtin_cpu_supports("sha") && __builtin_cpu_supports("sse4.1");
    if (ni) return blocks_ni(h, p, n);
#endif
    blocks_c(h, p, n);
}

void sha256_init(Sha256 *s) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    s->bytes = 0;
    s->used = 0;
}

void sha256_update(Sha256 *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->bytes += n;
    if (s->used) {
        size_t k = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->buf + s->used, p, k);
        s->used += (uint32_t)k, p += k, n -= k;
        if (s->used < 64) return;
        blocks(s->h, s->buf, 1);
        s->used = 0;
    }
    if (n >= 64) {
        blocks(s->h, p, n / 64);
        p += n & ~(size_t)63, n &= 63;
    }
    memcpy(s->buf, p, n);
    s->used = (uint32_t)n;
}

void sha256_final(Sha256 *s, uint8_t out[32]) {
    uint64_t bits = s->bytes * 8;
    uint8_t pad[72] = {0x80};
    size_t k = (s->used < 56 ? 56 : 120) - s->used;
    for (int i = 0; i < 8; i++) pad[k + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(s, pad, k + 8);
    for (int i = 0; i < 8; i++)
        out[4 * i] = (uint8_t)(s->h[i] >> 24), out[4 * i + 1] = (uint8_t)(s->h[i] >> 16),
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8), out[4 * i + 3] = (uint8_t)s->h[i];
}

void sha256(const void *data, size_t n, uint8_t out[32]) {
    Sha256 s;
    sha256_init(&s);
    sha256_update(&s, data, n);
    sha256_final(&s, out);
}
