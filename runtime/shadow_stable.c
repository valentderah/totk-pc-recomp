// Stable sun shadow cascades on game 1.4.3 (set stable, recomp/patches-v143.txt; TOTK_STABLE_SHADOWS=1).
//
// The game fits its four sun cascades anew every frame, so the shadow-map texels land somewhere else on a static wall
// each frame and shadow edges crawl and flicker while the camera moves:
// - the shadow module (main+0x5690a0, one call per cascade) builds each cascade from a light view whose x and y axes
//   follow the camera's heading, so the texel grid turns with the camera;
// - the view update (main+0xb2fb0 -> main+0xb372c) then crops each cascade to its casters in main+0xc31a0, which also
//   warps cascades 1-3 with a light-space perspective that follows the camera (record mode 0; cascade 0 has mode 1 and
//   stays orthographic);
// - the crop changes the cascade's size by a few hundredths of a percent every frame.
//
// The stable set takes cascades 1-3 down cascade 0's unwarped path (two branches in main+0xc31a0) and sends both calls
// of main+0xc31a0 through cascade_fit below. After the game's fit, cascade_fit replaces the x and y rows of the
// cascade's matrix with a square on fixed world axes (perpendicular to the sun) around the bounding sphere of the
// cascade's slice of the view. The sphere's radius depends only on the slice distances and the field of view, so it
// stays the same while the camera turns or moves, and its centre moves in whole texels. A static point then keeps its
// texel from one frame to the next. The depth row stays the game's (fitted to the casters), as does the light view.
//
// The record of a cascade (0x590 bytes), as main+0x5690a0 and main+0xc31a0 leave it:
//   +0x68  mode (0: warped, 1: orthographic)      +0x13c near, +0x14c far: the slice of the view, metres
//   +0x150 light view, 3 rows of 4 (main+0xc31a0 copies it to +0x230): a rotation whose row 2 is the sun axis
//   +0x260 matrix F, 4 rows of 4: light view space -> clip space; row 3 is (0, 0, 0, 1) when not warped
//   +0x2a0 lookup T = B * F * light view, B taking clip x, y, z to x/2+1/2, 1/2-y/2, z/2+1/2 (main+0xc31a0's tail)
//   +0x2e0 camera view, 3 rows of 4; +0x310 camera projection, 4 rows of 4 (OpenGL style, looking down -z)
// main+0xb2fb0 then copies F into the cascade camera's projection (the shadow pass draws with F * light view), and the
// lighting reads T.
//
// TOTK_STABLE_SNAP=0 (test): the unwarped cascades only, as the game fits them.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "layout.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

#ifdef RT_PATCH_PAGE
#define HOST_CASCADE_FIT (RT_PATCH_PAGE + 0xf80)   // the bl of the stable set's two sites in main+0xb372c
#define CASCADE_FIT_V143 0xc31a0                   // the game's fit: x0 record, w1 width, w2 height, s0, s1

static void rd(uint64_t a, float *f, int n) { memcpy(f, (const void *)(uintptr_t)a, 4 * (size_t)n); }
static void wr(uint64_t a, const float *f, int n) { memcpy((void *)(uintptr_t)a, f, 4 * (size_t)n); }
static double dot3(const double *a, const double *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static int unit3(double *a) {
    double l = sqrt(dot3(a, a));
    if (!(l > 1e-6)) return 0;
    a[0] /= l, a[1] /= l, a[2] /= l;
    return 1;
}

// The radius each record last used: kept while the slice's own radius stays within 4 % below it, so the jitter of the
// split distances never changes the texel size; a wider view grows it at once, a narrower one shrinks it.
static struct { uint64_t rec; double r; } held[8];
static double held_radius(uint64_t rec, double r) {
    int i = 0;
    while (i < 8 && held[i].rec && held[i].rec != rec) i++;
    if (i == 8) i = 0;   // more than eight records: start over (the game has four)
    if (held[i].rec != rec || !(r <= held[i].r && r >= held[i].r * 0.96)) held[i].rec = rec, held[i].r = r * 1.02;
    return held[i].r;
}

static void stabilize(uint64_t rec, uint32_t w, uint32_t h) {
    float F[4][4], V[3][4], C[3][4], P[4][4], n, f;
    rd(rec + 0x260, &F[0][0], 16);
    rd(rec + 0x150, &V[0][0], 12);
    rd(rec + 0x2e0, &C[0][0], 12);
    rd(rec + 0x310, &P[0][0], 16);
    rd(rec + 0x13c, &n, 1);
    rd(rec + 0x14c, &f, 1);
    if (w < 16 || h < 16 || F[3][0] != 0 || F[3][1] != 0 || F[3][2] != 0 || F[3][3] != 1) return;   // still warped
    if (!(n >= 0 && f > n && P[0][0] > 0 && P[1][1] > 0)) return;

    // The slice's bounding sphere: on the camera axis at the distance where the near and far corners are equally far
    // (at most the far plane), from the camera's half-angles (an off-centre projection counts its wider side).
    double tx = (1 + fabs(P[0][2])) / P[0][0], ty = (1 + fabs(P[1][2])) / P[1][1], k = tx * tx + ty * ty;
    double dc = 0.5 * ((double)f + n) * (1 + k);
    if (dc > f) dc = f;
    double r = fmax(sqrt((dc - n) * (dc - n) + (double)n * n * k), sqrt((f - dc) * (f - dc) + (double)f * f * k));
    r = held_radius(rec, r);
    double pos[3], fwd[3], centre[3];
    for (int i = 0; i < 3; i++) {
        pos[i] = -(C[0][i] * (double)C[0][3] + C[1][i] * (double)C[1][3] + C[2][i] * (double)C[2][3]);
        fwd[i] = -C[2][i];
    }
    if (!unit3(fwd)) return;
    for (int i = 0; i < 3; i++) centre[i] = pos[i] + fwd[i] * dc;

    // Fixed axes across the sun: world z crossed with the sun axis, or world x while the sun is near the z axis
    // (with hysteresis, so the choice does not flip back and forth).
    static int use_x;
    double d[3] = {V[2][0], V[2][1], V[2][2]};
    if (!unit3(d)) return;
    if (fabs(d[2]) > 0.95) use_x = 1;
    else if (fabs(d[2]) < 0.85) use_x = 0;
    double u[3] = {use_x ? 0 : -d[1], use_x ? -d[2] : d[0], use_x ? d[1] : 0};   // (x or z) cross d
    if (!unit3(u)) return;
    double v[3] = {d[1] * u[2] - d[2] * u[1], d[2] * u[0] - d[0] * u[2], d[0] * u[1] - d[1] * u[0]};   // d cross u

    // Positions along u and v are measured from an anchor near the camera, not from the world origin: the sun axis
    // turns slowly with the time of day, and measured from the origin (a few kilometres away) that turn alone would
    // slide every texel by millimetres each frame. Near the anchor the slide is a few hundred times smaller. The anchor
    // moves when the camera has gone 512 m from it (one step of the grid then).
    static double anchor[3];
    static int anchored;
    double off[3] = {centre[0] - anchor[0], centre[1] - anchor[1], centre[2] - anchor[2]};
    if (!anchored || dot3(off, off) > 512.0 * 512.0) {
        for (int i = 0; i < 3; i++) anchor[i] = floor(centre[i] + 0.5), off[i] = centre[i] - anchor[i];
        anchored = 1;
    }

    // The centre in whole texels: then every texel edge sits on a multiple of the texel size along u and v.
    double sx = 2 * r / w, sy = 2 * r / h;
    double cu = floor(dot3(u, off) / sx + 0.5) * sx, cv = floor(dot3(v, off) / sy + 0.5) * sy;

    // F's x and y rows act on light view coordinates L = R p + t (R, t: the light view):
    // u.(p - anchor) = (R u).L - (R u).t - u.anchor.
    double uL[3], vL[3], t[3] = {V[0][3], V[1][3], V[2][3]};
    for (int i = 0; i < 3; i++) {
        uL[i] = V[i][0] * u[0] + V[i][1] * u[1] + V[i][2] * u[2];
        vL[i] = V[i][0] * v[0] + V[i][1] * v[1] + V[i][2] * v[2];
    }
    for (int i = 0; i < 3; i++) F[0][i] = (float)(uL[i] / r), F[1][i] = (float)(vL[i] / r);
    F[0][3] = (float)(-(dot3(uL, t) + dot3(u, anchor) + cu) / r);
    F[1][3] = (float)(-(dot3(vL, t) + dot3(v, anchor) + cv) / r);
    wr(rec + 0x260, &F[0][0], 8);

    // T = B * F * light view, as main+0xc31a0 computes it.
    double M[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            M[i][j] = F[i][0] * (double)V[0][j] + F[i][1] * (double)V[1][j] + F[i][2] * (double)V[2][j] + (j == 3 ? F[i][3] : 0);
    float T[4][4];
    for (int j = 0; j < 4; j++) {
        T[0][j] = (float)(0.5 * (M[0][j] + M[3][j]));
        T[1][j] = (float)(0.5 * (M[3][j] - M[1][j]));
        T[2][j] = (float)(0.5 * (M[2][j] + M[3][j]));
        T[3][j] = (float)M[3][j];
    }
    wr(rec + 0x2a0, &T[0][0], 16);
}

// Both calls of main+0xc31a0 in main+0xb372c come here (the stable set's sites): the game's fit, then the stable one.
// s0 and s1 pass through untouched in v0 and v1.
static void cascade_fit(Cpu *c) {
    static int snap = -1;
    if (snap < 0) {
        const char *e = getenv("TOTK_STABLE_SNAP");
        snap = !(e && !strcmp(e, "0"));
    }
    uint64_t rec = c->x[0];
    uint32_t w = (uint32_t)c->x[1], h = (uint32_t)c->x[2];
    rt_call(c, MOD_MAIN_BASE + CASCADE_FIT_V143, 3, rec, (uint64_t)w, (uint64_t)h);
    if (snap) stabilize(rec, w, h);
}

__attribute__((constructor)) static void shadow_stable_register(void) { rt_register_host(HOST_CASCADE_FIT, cascade_fit); }
#endif
