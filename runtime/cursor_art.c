// The mouse pointer's image in the game window while the cursor is not captured (the game's menus, the settings menu):
// a gold arrowhead with a notched back, a dark outline, a turquoise gem and a soft shadow, drawn here at the size the
// window's DPI asks for instead of the Windows arrow. input_win.c and input_sdl.c make the system cursor from it.
// Nothing comes from the game's files.
#include <math.h>
#include <stdint.h>

#include "input_wsys.h"

typedef struct { float x, y; } V;

// In units of a 32-pixel cursor: the tip at the top left (the hotspot), the wings to the right and below, the notch
// between them
static const V SHAPE[] = {{1.5f, 1.5f}, {25.0f, 10.5f}, {14.5f, 14.5f}, {10.5f, 25.0f}};
static const V GEM[] = {{10.4f, 7.6f}, {13.0f, 10.2f}, {10.4f, 12.8f}, {7.8f, 10.2f}};
#define OUTLINE 1.7f   // the dark edge's width
#define SHADOW 1.6f    // the shadow's offset to the bottom right

static float seg_dist(V p, V a, V b) {
    float dx = b.x - a.x, dy = b.y - a.y, t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / (dx * dx + dy * dy);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float ex = a.x + t * dx - p.x, ey = a.y + t * dy - p.y;
    return sqrtf(ex * ex + ey * ey);
}

// Distance to the polygon's edge, negative inside (even-odd: the shapes are simple)
static float poly_sdf(V p, const V *v, int n) {
    float d = 1e9f;
    int in = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float e = seg_dist(p, v[j], v[i]);
        if (e < d) d = e;
        if ((v[i].y > p.y) != (v[j].y > p.y) && p.x < (v[j].x - v[i].x) * (p.y - v[i].y) / (v[j].y - v[i].y) + v[i].x)
            in = !in;
    }
    return in ? -d : d;
}

// One sample's colour (straight r, g, b in 0..1) and coverage
static float sample(V p, float *r, float *g, float *b) {
    float d = poly_sdf(p, SHAPE, 4);
    if (d < 0) {
        if (-d < OUTLINE) { *r = 0.17f, *g = 0.11f, *b = 0.04f; return 1; }
        if (poly_sdf(p, GEM, 4) < 0) {   // the gem, lighter towards its top left
            float t = (p.x + p.y - 20.4f) / 5.2f + 0.5f;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            *r = 0.62f - 0.42f * t, *g = 0.98f - 0.22f * t, *b = 0.92f - 0.12f * t;
            return 1;
        }
        // gold, pale at the tip and deeper at the wings; a thin light line inside the outline
        float t = (p.x + p.y) / 34.0f;
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        float rim = -d < OUTLINE + 1.0f ? 0.12f : 0;
        *r = fminf(1, 1.00f - 0.12f * t + rim), *g = fminf(1, 0.93f - 0.32f * t + rim),
        *b = fminf(1, 0.70f - 0.52f * t + rim);
        return 1;
    }
    V s = {p.x - SHADOW, p.y - SHADOW};
    float ds = poly_sdf(s, SHAPE, 4);
    if (ds < 0.8f) {   // the shadow, soft at its edge
        *r = *g = *b = 0;
        float a = ds < -0.8f ? 1 : (0.8f - ds) / 1.6f;
        return 0.38f * a;
    }
    return 0;
}

void cursor_art(uint32_t *argb, int size, int *hot) {
    float k = 32.0f / (float)size;
    enum { SS = 4 };   // 4 x 4 samples a pixel
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < SS; sy++)
                for (int sx = 0; sx < SS; sx++) {
                    V p = {(x + (sx + 0.5f) / SS) * k, (y + (sy + 0.5f) / SS) * k};
                    float sr, sg, sb, c = sample(p, &sr, &sg, &sb);
                    r += sr * c, g += sg * c, b += sb * c, a += c;
                }
            uint32_t px = 0;
            if (a > 0) {   // straight alpha, as Windows and SDL take it
                r /= a, g /= a, b /= a, a /= SS * SS;
                px = (uint32_t)lrintf(a * 255) << 24 | (uint32_t)lrintf(r * 255) << 16 |
                     (uint32_t)lrintf(g * 255) << 8 | (uint32_t)lrintf(b * 255);
            }
            argb[y * size + x] = px;
        }
    *hot = (int)(1.5f / k);
}
