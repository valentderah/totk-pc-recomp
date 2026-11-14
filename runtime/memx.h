// Inline equality compares and copies for the replay's per-draw paths (TOTK_MEMX=0|1|check, default 1).
// ucrtbase's memcmp walks 8 bytes a step with a call around it, and the replay compares and copies a few hundred
// bytes at a time thousands of times a frame (pipeline states, shader answers, descriptor signatures): MX_EQ and
// MX_CPY do the same work with 32-byte loads in the caller, so a constant size unrolls and a variable one loops
// without a call. MX_EQ answers only equal or not, never an order. MX_CPY is memcpy: source and destination never
// overlap. 1: the inline forms. 0: memcmp/memcpy as before. check: memcmp/memcpy, and every MX_EQ also runs the
// inline compare and logs a different answer (vk_memx_bad). The mode is read once per process (vk_memx_init); calls
// before that take the off path.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern int vk_memx_mode;   // -1 before vk_memx_init, 0 off, 1 on, 2 check
void vk_memx_init(void);
void vk_memx_bad(const char *file, int line, size_t n);

typedef uint64_t mx_v4 __attribute__((vector_size(32), aligned(1)));
typedef uint64_t mx_v2 __attribute__((vector_size(16), aligned(1)));

static inline __attribute__((always_inline)) mx_v4 mx_ld4(const uint8_t *p) {
    mx_v4 v;
    __builtin_memcpy(&v, p, 32);
    return v;
}

static inline __attribute__((always_inline)) mx_v2 mx_ld2(const uint8_t *p) {
    mx_v2 v;
    __builtin_memcpy(&v, p, 16);
    return v;
}

static inline __attribute__((always_inline)) uint64_t mx_ld1(const uint8_t *p) {
    uint64_t v;
    __builtin_memcpy(&v, p, 8);
    return v;
}

static inline __attribute__((always_inline)) uint32_t mx_ldw(const uint8_t *p) {
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    return v;
}

// 1 when the n bytes at a and b are equal. Sizes of 32 and up compare in 128-byte steps with one branch each, the
// last partial step as an overlapping 32-byte load from the end; smaller sizes take two overlapping loads.
static inline __attribute__((always_inline)) int mx_eq(const void *a, const void *b, size_t n) {
    const uint8_t *p = a, *q = b;
    if (n >= 32) {
        size_t i = 0;
        for (; i + 128 <= n; i += 128) {
            mx_v4 d = (mx_ld4(p + i) ^ mx_ld4(q + i)) | (mx_ld4(p + i + 32) ^ mx_ld4(q + i + 32)) |
                      (mx_ld4(p + i + 64) ^ mx_ld4(q + i + 64)) | (mx_ld4(p + i + 96) ^ mx_ld4(q + i + 96));
            if (d[0] | d[1] | d[2] | d[3]) return 0;
        }
        mx_v4 d = {0, 0, 0, 0};
        for (; i + 32 <= n; i += 32) d |= mx_ld4(p + i) ^ mx_ld4(q + i);
        if (i < n) d |= mx_ld4(p + n - 32) ^ mx_ld4(q + n - 32);
        return !(d[0] | d[1] | d[2] | d[3]);
    }
    if (n >= 16) {
        mx_v2 d = (mx_ld2(p) ^ mx_ld2(q)) | (mx_ld2(p + n - 16) ^ mx_ld2(q + n - 16));
        return !(d[0] | d[1]);
    }
    if (n >= 8) return !((mx_ld1(p) ^ mx_ld1(q)) | (mx_ld1(p + n - 8) ^ mx_ld1(q + n - 8)));
    if (n >= 4) return !((mx_ldw(p) ^ mx_ldw(q)) | (mx_ldw(p + n - 4) ^ mx_ldw(q + n - 4)));
    for (size_t i = 0; i < n; i++)
        if (p[i] != q[i]) return 0;
    return 1;
}

// memcpy of n bytes between buffers that do not overlap: 32-byte moves, the last partial one overlapping the one
// before it (it rewrites bytes it already wrote with the same values).
static inline __attribute__((always_inline)) void mx_cpy(void *d, const void *s, size_t n) {
    uint8_t *p = d;
    const uint8_t *q = s;
    if (n >= 32) {
        mx_v4 last = mx_ld4(q + n - 32);
        for (size_t i = 0; i + 32 <= n; i += 32) {
            __asm__("" : "+r"(p));   // opaque to the loop-idiom pass, which would turn the loop back into a memcpy call
            mx_v4 v = mx_ld4(q + i);
            __builtin_memcpy(p + i, &v, 32);
        }
        __builtin_memcpy(p + n - 32, &last, 32);
        return;
    }
    if (n >= 16) {
        mx_v2 x = mx_ld2(q), y = mx_ld2(q + n - 16);
        __builtin_memcpy(p, &x, 16), __builtin_memcpy(p + n - 16, &y, 16);
        return;
    }
    if (n >= 8) {
        uint64_t x = mx_ld1(q), y = mx_ld1(q + n - 8);
        __builtin_memcpy(p, &x, 8), __builtin_memcpy(p + n - 8, &y, 8);
        return;
    }
    if (n >= 4) {
        uint32_t x = mx_ldw(q), y = mx_ldw(q + n - 4);
        __builtin_memcpy(p, &x, 4), __builtin_memcpy(p + n - 4, &y, 4);
        return;
    }
    for (size_t i = 0; i < n; i++) p[i] = q[i];
}

static inline __attribute__((always_inline)) int mx_eq_at(const void *a, const void *b, size_t n, const char *file,
                                                          int line) {
    if (vk_memx_mode == 1) return mx_eq(a, b, n);
    int r = !memcmp(a, b, n);
    if (vk_memx_mode == 2 && mx_eq(a, b, n) != r) vk_memx_bad(file, line, n);
    return r;
}

static inline __attribute__((always_inline)) void mx_cpy_sw(void *d, const void *s, size_t n) {
    if (vk_memx_mode == 1) mx_cpy(d, s, n);
    else memcpy(d, s, n);
}

#define MX_EQ(a, b, n) mx_eq_at((a), (b), (n), __FILE__, __LINE__)
#define MX_NE(a, b, n) (!MX_EQ(a, b, n))
#define MX_CPY(d, s, n) mx_cpy_sw((d), (s), (n))
