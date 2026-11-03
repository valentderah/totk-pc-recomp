// Correctly rounded fma/fmaf for x86-64 builds without FMA3: the -march=x86-64-v2 build (docs/building.md).
//
// runtime/simd.h lowers AArch64 FMADD/FMSUB and the RECPS/RSQRTS helpers to __builtin_elementwise_fma, which clang
// turns into a call to fma/fmaf when the target has no FMA instruction. The C runtime is not a substitute: on CPUs
// without FMA3 the UCRT's software fallback is documented as rounding incorrectly by up to half an ulp, while the FP
// lowering needs the single rounding the ARM instruction gives. This file supplies it.
//
// Adapted from musl src/math/fma.c and src/math/fmaf.c (MIT, Copyright (c) 2005-2020 Rich Felker and contributors).
// The result is rounded in the MXCSR rounding mode the caller set (simd.h simd_rm_enter), and MXCSR FTZ and DAZ are
// honoured as the hardware FMA would, so runtime/simd.h's fast paths keep their assumptions: denormal inputs read as
// zero and denormal results as zero under FPCR.FZ, without a trip through the slow path.
//
// The definitions exist only where the compiler would emit the calls. A -march=x86-64-v3 build defines __FMA__ and
// emits vfmadd instead, so this file contributes nothing there.
#include <stdint.h>

#if defined(__x86_64__) && !defined(__FMA__)

#include <float.h>
#include <immintrin.h>

#define MXCSR_DAZ 0x0040u
#define MXCSR_FTZ 0x8000u

static double fma_flush_d(double x) {
    union { double f; uint64_t i; } u = {x};
    if (!(u.i & 0x7ff0000000000000ull) && (u.i & 0x000fffffffffffffull)) u.i &= 0x8000000000000000ull;
    return u.f;
}

static float fma_flush_f(float x) {
    union { float f; uint32_t i; } u = {x};
    if (!(u.i & 0x7f800000u) && (u.i & 0x007fffffu)) u.i &= 0x80000000u;
    return u.f;
}

// ---- float: the exact product fits in a double, so one extra term and a round-to-odd step decide the float
// rounding. Halfway and tiny results are the only ones where rounding the double first can change the float result.
float fmaf(float x, float y, float z) {
    unsigned csr = _mm_getcsr();
    if (csr & MXCSR_DAZ) x = fma_flush_f(x), y = fma_flush_f(y), z = fma_flush_f(z);
    double xy = (double)x * y;   // exact
    union { double r; uint64_t i; } u = {xy + z};
    int e = u.i >> 52 & 0x7ff;
    int halfway = (u.i & 0x1fffffff) == 0x10000000;
    int tiny = e <= 0x3ff - 126 && e >= 0x3ff - 149;
    if (halfway || tiny) {
        if (e != 0x7ff) {
            // r+t is the exact sum in nearest rounding; otherwise t keeps its sign and whether it is zero, which
            // is all the round-to-odd step needs. An odd sticky bit makes the final conversion round once, in
            // whichever mode MXCSR asks for.
            int s = u.i >> 63;
            double t = s == (xy < z) ? xy - u.r + z : z - u.r + xy;
            if (t) {
                u.i -= s ^ (t < 0);
                u.i |= 1;
            }
        }
    }
    float r = (float)u.r;
    if (csr & MXCSR_FTZ) r = fma_flush_f(r);
    return r;
}

// ---- double: the significands are multiplied and aligned as integers, then converted back with one rounding.
#define ASUINT64(x) ((union { double f; uint64_t i; }){x}).i
#define ZEROINFNAN (0x7ff - 0x3ff - 52 - 1)

struct fma_num { uint64_t m; int e; int sign; };

static struct fma_num fma_normalize(double x) {
    uint64_t ix = ASUINT64(x);
    int e = ix >> 52;
    int sign = e & 0x800;
    e &= 0x7ff;
    if (!e) {
        ix = ASUINT64(x * 0x1p63);
        e = ix >> 52 & 0x7ff;
        e = e ? e - 63 : 0x800;
    }
    ix &= (1ull << 52) - 1;
    ix |= 1ull << 52;
    ix <<= 1;
    e -= 0x3ff + 52 + 1;
    return (struct fma_num){ix, e, sign};
}

static void fma_mul(uint64_t *hi, uint64_t *lo, uint64_t x, uint64_t y) {
    uint64_t xlo = (uint32_t)x, xhi = x >> 32;
    uint64_t ylo = (uint32_t)y, yhi = y >> 32;
    uint64_t t1 = xlo * ylo;
    uint64_t t2 = xlo * yhi + xhi * ylo;
    uint64_t t3 = xhi * yhi;
    *lo = t1 + (t2 << 32);
    *hi = t3 + (t2 >> 32) + (t1 > *lo);
}

// scalbn from musl src/math/scalbn.c (MIT): the fma's final scaling, kept here so the result does not depend on the
// C runtime's version. Its multiplications honour MXCSR like any other host FP operation.
static double fma_scalbn(double x, int n) {
    union { double f; uint64_t i; } u;
    double y = x;
    if (n > 1023) {
        y *= 0x1p1023;
        n -= 1023;
        if (n > 1023) {
            y *= 0x1p1023;
            n -= 1023;
            if (n > 1023) n = 1023;
        }
    } else if (n < -1022) {
        // Make sure the final n < -53, so the subnormal range rounds only once.
        y *= 0x1p-1022 * 0x1p53;
        n += 1022 - 53;
        if (n < -1022) {
            y *= 0x1p-1022 * 0x1p53;
            n += 1022 - 53;
            if (n < -1022) n = -1022;
        }
    }
    u.i = (uint64_t)(0x3ff + n) << 52;
    return y * u.f;
}

double fma(double x, double y, double z) {
#pragma STDC FENV_ACCESS ON
    unsigned csr = _mm_getcsr();
    if (csr & MXCSR_DAZ) x = fma_flush_d(x), y = fma_flush_d(y), z = fma_flush_d(z);

    struct fma_num nx = fma_normalize(x), ny = fma_normalize(y), nz = fma_normalize(z);
    if (nx.e >= ZEROINFNAN || ny.e >= ZEROINFNAN) return x * y + z;
    if (nz.e >= ZEROINFNAN) {
        if (nz.e > ZEROINFNAN) return x * y;   // z == 0
        return z;
    }

    uint64_t rhi, rlo, zhi, zlo;
    fma_mul(&rhi, &rlo, nx.m, ny.m);

    // Align the exponents: shift z left by kz and r right by kr with kz + kr == d.
    int e = nx.e + ny.e;
    int d = nz.e - e;
    if (d > 0) {
        if (d < 64) {
            zlo = nz.m << d;
            zhi = nz.m >> (64 - d);
        } else {
            zlo = 0;
            zhi = nz.m;
            e = nz.e - 64;
            d -= 64;
            if (d == 0) {
            } else if (d < 64) {
                rlo = rhi << (64 - d) | rlo >> d | !!(rlo << (64 - d));
                rhi = rhi >> d;
            } else {
                rlo = 1;
                rhi = 0;
            }
        }
    } else {
        zhi = 0;
        d = -d;
        if (d == 0) {
            zlo = nz.m;
        } else if (d < 64) {
            zlo = nz.m >> d | !!(nz.m << (64 - d));
        } else {
            zlo = 1;
        }
    }

    int sign = nx.sign ^ ny.sign;
    int samesign = !(sign ^ nz.sign);
    int nonzero = 1;
    if (samesign) {
        rlo += zlo;
        rhi += zhi + (rlo < zlo);
    } else {
        uint64_t t = rlo;
        rlo -= zlo;
        rhi = rhi - zhi - (t < rlo);
        if (rhi >> 63) {
            rlo = -rlo;
            rhi = -rhi - !!rlo;
            sign = !sign;
        }
        nonzero = !!rhi;
    }

    // Keep the top 63 bits in rhi; the last bit is sticky.
    if (nonzero) {
        e += 64;
        d = __builtin_clzll(rhi) - 1;
        rhi = rhi << d | rlo >> (64 - d) | !!(rlo << d);
    } else if (rlo) {
        d = __builtin_clzll(rlo) - 1;
        if (d < 0)
            rhi = rlo >> 1 | (rlo & 1);
        else
            rhi = rlo << d;
    } else {
        return x * y + z;   // exact +-0
    }
    e -= d;

    int64_t i = rhi;
    if (sign) i = -i;
    double r = i;   // |r| is in [0x1p62, 0x1p63]

    if (e < -1022 - 62) {
        // The result is subnormal before rounding.
        if (e == -1022 - 63) {
            double c = 0x1p63;
            if (sign) c = -c;
            if (r == c) {
                // The smallest normal after rounding: whether it underflows depends on the architecture. A double
                // to float conversion imitates the before-rounding rule the guest has.
                float fltmin = 0x0.ffffff8p-63 * FLT_MIN * r;
                r = DBL_MIN / FLT_MIN * fltmin;
                return (csr & MXCSR_FTZ) ? fma_flush_d(r) : r;
            }
            // One bit is lost when scaled: add another top bit so the conversion rounds only once if inexact.
            if (rhi << 53) {
                i = rhi >> 1 | (rhi & 1) | 1ull << 62;
                if (sign) i = -i;
                r = i;
                r = 2 * r - c;
                // Raise underflow portably, so the compiler cannot optimize the expression away.
                double tiny = DBL_MIN / FLT_MIN * r;
                r += (double)(tiny * tiny) * (r - r);
            }
        } else {
            // Only round once when scaled.
            d = 10;
            i = (rhi >> d | !!(rhi << (64 - d))) << d;
            if (sign) i = -i;
            r = i;
        }
    }
    r = fma_scalbn(r, e);
    if (csr & MXCSR_FTZ) r = fma_flush_d(r);
    return r;
}

#endif
