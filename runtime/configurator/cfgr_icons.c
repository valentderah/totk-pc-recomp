// totk-config: the window's icons, drawn as lines and shapes into a square (Nuklear command buffer), so they scale
// with the display and need no image files or symbol font. Each icon is laid out on a 0..1 grid inside the square.
#include <math.h>

#include "configurator/cfgr_icons.h"

typedef struct {
    struct nk_command_buffer *c;
    float x, y, s, t;   // the square, line thickness
    struct nk_color col;
} Pen;

static float X(const Pen *p, float u) { return p->x + u * p->s; }
static float Y(const Pen *p, float v) { return p->y + v * p->s; }

static void line(const Pen *p, float u0, float v0, float u1, float v1) {
    nk_stroke_line(p->c, X(p, u0), Y(p, v0), X(p, u1), Y(p, v1), p->t, p->col);
}

// an open polyline through n points (u, v pairs)
static void poly(const Pen *p, const float *uv, int n) {
    float pts[32];
    for (int i = 0; i < n && i < 16; i++) pts[2 * i] = X(p, uv[2 * i]), pts[2 * i + 1] = Y(p, uv[2 * i + 1]);
    nk_stroke_polyline(p->c, pts, n, p->t, p->col);
}

static void fill_poly(const Pen *p, const float *uv, int n) {   // convex only
    float pts[32];
    for (int i = 0; i < n && i < 16; i++) pts[2 * i] = X(p, uv[2 * i]), pts[2 * i + 1] = Y(p, uv[2 * i + 1]);
    nk_fill_polygon(p->c, pts, n, p->col);
}

static void circle(const Pen *p, float cu, float cv, float r) {
    nk_stroke_circle(p->c, nk_rect(X(p, cu - r), Y(p, cv - r), 2 * r * p->s, 2 * r * p->s), p->t, p->col);
}

static void dot(const Pen *p, float cu, float cv, float r) {
    nk_fill_circle(p->c, nk_rect(X(p, cu - r), Y(p, cv - r), 2 * r * p->s, 2 * r * p->s), p->col);
}

static void rect(const Pen *p, float u0, float v0, float u1, float v1, float round) {
    nk_stroke_rect(p->c, nk_rect(X(p, u0), Y(p, v0), (u1 - u0) * p->s, (v1 - v0) * p->s), round * p->s, p->t, p->col);
}

static void tri(const Pen *p, float u0, float v0, float u1, float v1, float u2, float v2) {
    nk_fill_triangle(p->c, X(p, u0), Y(p, v0), X(p, u1), Y(p, v1), X(p, u2), Y(p, v2), p->col);
}

// An arc from angle a0 to a1 (radians, screen axes: y down, so a growing angle turns clockwise) with an arrow head at
// a1 pointing on along the arc, or at a0 pointing back when back is set.
static void arc_arrow(const Pen *p, float cu, float cv, float r, float a0, float a1, int back) {
    nk_stroke_arc(p->c, X(p, cu), Y(p, cv), r * p->s, a0, a1, p->t, p->col);
    float a = back ? a0 : a1, dir = back ? -1.f : 1.f;
    float px = cu + r * cosf(a), py = cv + r * sinf(a);
    float tx = -sinf(a) * dir, ty = cosf(a) * dir;   // tangent along the arc's way
    float nx = cosf(a), ny = sinf(a);                // outward
    float h = 0.17f, w = 0.13f;
    tri(p, px + tx * h, py + ty * h, px + nx * w - tx * 0.02f, py + ny * w - ty * 0.02f, px - nx * w - tx * 0.02f,
        py - ny * w - ty * 0.02f);
}

void icon_draw(struct nk_command_buffer *c, Icon icon, struct nk_rect r, struct nk_color col) {
    float s = fminf(r.w, r.h);
    Pen p = {c, r.x + (r.w - s) / 2, r.y + (r.h - s) / 2, s, fmaxf(1.4f, s / 11), col};
    switch (icon) {
    case ICON_SCREEN:
        rect(&p, 0.08f, 0.16f, 0.92f, 0.70f, 0.08f);
        line(&p, 0.5f, 0.70f, 0.5f, 0.84f);
        line(&p, 0.30f, 0.86f, 0.70f, 0.86f);
        break;
    case ICON_GAME: {   // a four-point star
        float a[] = {0.5f, 0.06f, 0.62f, 0.38f, 0.5f, 0.5f, 0.38f, 0.38f};
        float b[] = {0.94f, 0.5f, 0.62f, 0.62f, 0.5f, 0.5f, 0.62f, 0.38f};
        float d[] = {0.5f, 0.94f, 0.38f, 0.62f, 0.5f, 0.5f, 0.62f, 0.62f};
        float e[] = {0.06f, 0.5f, 0.38f, 0.38f, 0.5f, 0.5f, 0.38f, 0.62f};
        fill_poly(&p, a, 4), fill_poly(&p, b, 4), fill_poly(&p, d, 4), fill_poly(&p, e, 4);
        dot(&p, 0.82f, 0.18f, 0.06f);
        break;
    }
    case ICON_SOUND: {
        float body[] = {0.10f, 0.38f, 0.28f, 0.38f, 0.50f, 0.18f, 0.50f, 0.82f, 0.28f, 0.62f, 0.10f, 0.62f};
        fill_poly(&p, body, 6);
        nk_stroke_arc(c, X(&p, 0.52f), Y(&p, 0.5f), 0.18f * s, -0.9f, 0.9f, p.t, col);
        nk_stroke_arc(c, X(&p, 0.52f), Y(&p, 0.5f), 0.34f * s, -0.9f, 0.9f, p.t, col);
        break;
    }
    case ICON_CONTROLS:
        rect(&p, 0.06f, 0.28f, 0.94f, 0.76f, 0.22f);
        line(&p, 0.20f, 0.52f, 0.40f, 0.52f);
        line(&p, 0.30f, 0.42f, 0.30f, 0.62f);
        dot(&p, 0.66f, 0.46f, 0.06f);
        dot(&p, 0.78f, 0.58f, 0.06f);
        break;
    case ICON_AMIIBO: {
        circle(&p, 0.5f, 0.26f, 0.14f);
        float body[] = {0.30f, 0.72f, 0.36f, 0.50f, 0.64f, 0.50f, 0.70f, 0.72f};
        poly(&p, body, 4);
        rect(&p, 0.16f, 0.74f, 0.84f, 0.90f, 0.08f);
        break;
    }
    case ICON_MENU:
        for (int i = 0; i < 3; i++) {
            float v = 0.26f + 0.24f * (float)i;
            dot(&p, 0.16f, v, 0.06f);
            line(&p, 0.32f, v, 0.88f, v);
        }
        break;
    case ICON_ADVANCED: {
        static const float knob[3] = {0.34f, 0.68f, 0.48f};
        for (int i = 0; i < 3; i++) {
            float v = 0.24f + 0.26f * (float)i;
            line(&p, 0.10f, v, 0.90f, v);
            dot(&p, knob[i], v, 0.10f);
        }
        break;
    }
    case ICON_MODS: {
        float hex[] = {0.5f, 0.08f, 0.88f, 0.29f, 0.88f, 0.71f, 0.5f, 0.92f, 0.12f, 0.71f, 0.12f, 0.29f, 0.5f, 0.08f};
        poly(&p, hex, 7);
        float in[] = {0.12f, 0.29f, 0.5f, 0.5f, 0.88f, 0.29f};
        poly(&p, in, 3);
        line(&p, 0.5f, 0.5f, 0.5f, 0.92f);
        break;
    }
    case ICON_EXPERT: {
        float flask[] = {0.40f, 0.12f, 0.40f, 0.40f, 0.14f, 0.88f, 0.86f, 0.88f, 0.60f, 0.40f, 0.60f, 0.12f};
        poly(&p, flask, 6);
        line(&p, 0.32f, 0.12f, 0.68f, 0.12f);
        float liquid[] = {0.29f, 0.62f, 0.71f, 0.62f, 0.83f, 0.84f, 0.17f, 0.84f};
        fill_poly(&p, liquid, 4);
        break;
    }
    case ICON_OTHER:
        circle(&p, 0.28f, 0.5f, 0.17f);
        line(&p, 0.45f, 0.5f, 0.92f, 0.5f);
        line(&p, 0.76f, 0.5f, 0.76f, 0.68f);
        line(&p, 0.88f, 0.5f, 0.88f, 0.64f);
        break;
    case ICON_UPDATES: {
        line(&p, 0.5f, 0.10f, 0.5f, 0.62f);
        float head[] = {0.30f, 0.44f, 0.5f, 0.64f, 0.70f, 0.44f};
        poly(&p, head, 3);
        float tray[] = {0.12f, 0.64f, 0.12f, 0.88f, 0.88f, 0.88f, 0.88f, 0.64f};
        poly(&p, tray, 4);
        break;
    }
    case ICON_SEARCH:
        circle(&p, 0.42f, 0.42f, 0.26f);
        line(&p, 0.62f, 0.62f, 0.88f, 0.88f);
        break;
    case ICON_FOLDER: {
        float f[] = {0.08f, 0.22f, 0.38f, 0.22f, 0.46f, 0.32f, 0.92f, 0.32f, 0.92f, 0.82f, 0.08f, 0.82f, 0.08f, 0.22f};
        poly(&p, f, 7);
        break;
    }
    case ICON_GLOBE:
        circle(&p, 0.5f, 0.5f, 0.40f);
        nk_stroke_circle(c, nk_rect(X(&p, 0.33f), Y(&p, 0.10f), 0.34f * s, 0.80f * s), p.t, col);
        line(&p, 0.10f, 0.5f, 0.90f, 0.5f);
        line(&p, 0.5f, 0.10f, 0.5f, 0.90f);
        break;
    case ICON_RESET:   // counter-clockwise arrow
        arc_arrow(&p, 0.5f, 0.54f, 0.30f, -2.2f, 2.4f, 1);
        break;
    case ICON_RESTART:   // clockwise arrow
        arc_arrow(&p, 0.5f, 0.5f, 0.32f, -1.2f, 3.6f, 0);
        break;
    case ICON_BOLT: {
        float a[] = {0.60f, 0.04f, 0.22f, 0.56f, 0.50f, 0.56f};
        float b[] = {0.50f, 0.44f, 0.78f, 0.44f, 0.40f, 0.96f};
        fill_poly(&p, a, 3), fill_poly(&p, b, 3);
        float m[] = {0.50f, 0.44f, 0.60f, 0.44f, 0.50f, 0.56f, 0.40f, 0.56f};
        fill_poly(&p, m, 4);
        break;
    }
    case ICON_PLAY:
        tri(&p, 0.22f, 0.12f, 0.86f, 0.5f, 0.22f, 0.88f);
        break;
    case ICON_PLUS:
        line(&p, 0.5f, 0.16f, 0.5f, 0.84f);
        line(&p, 0.16f, 0.5f, 0.84f, 0.5f);
        break;
    case ICON_PENCIL: {
        float body[] = {0.62f, 0.14f, 0.86f, 0.38f, 0.38f, 0.86f, 0.14f, 0.62f};
        poly(&p, body, 4);
        float tip[] = {0.14f, 0.62f, 0.38f, 0.86f, 0.10f, 0.90f};
        fill_poly(&p, tip, 3);
        line(&p, 0.52f, 0.24f, 0.76f, 0.48f);
        break;
    }
    case ICON_TRASH: {
        line(&p, 0.12f, 0.24f, 0.88f, 0.24f);
        float handle[] = {0.38f, 0.24f, 0.40f, 0.10f, 0.60f, 0.10f, 0.62f, 0.24f};
        poly(&p, handle, 4);
        float body[] = {0.20f, 0.24f, 0.26f, 0.90f, 0.74f, 0.90f, 0.80f, 0.24f};
        poly(&p, body, 4);
        line(&p, 0.42f, 0.40f, 0.42f, 0.76f);
        line(&p, 0.58f, 0.40f, 0.58f, 0.76f);
        break;
    }
    case ICON_WARN: {
        float t[] = {0.5f, 0.08f, 0.94f, 0.88f, 0.06f, 0.88f, 0.5f, 0.08f};
        poly(&p, t, 4);
        line(&p, 0.5f, 0.36f, 0.5f, 0.62f);
        dot(&p, 0.5f, 0.75f, 0.05f);
        break;
    }
    case ICON_CLOSE:
        line(&p, 0.22f, 0.22f, 0.78f, 0.78f);
        line(&p, 0.78f, 0.22f, 0.22f, 0.78f);
        break;
    case ICON_UP: {
        float ch[] = {0.20f, 0.66f, 0.5f, 0.34f, 0.80f, 0.66f};
        poly(&p, ch, 3);
        break;
    }
    case ICON_DOWN: {
        float ch[] = {0.20f, 0.34f, 0.5f, 0.66f, 0.80f, 0.34f};
        poly(&p, ch, 3);
        break;
    }
    case ICON_CHECK: {
        float ck[] = {0.14f, 0.52f, 0.40f, 0.78f, 0.88f, 0.24f};
        poly(&p, ck, 3);
        break;
    }
    case ICON_DOT:
        dot(&p, 0.5f, 0.5f, 0.22f);
        break;
    case ICON_RING:
        circle(&p, 0.5f, 0.5f, 0.22f);
        break;
    case ICON_INFO:
        circle(&p, 0.5f, 0.5f, 0.40f);
        line(&p, 0.5f, 0.44f, 0.5f, 0.72f);
        dot(&p, 0.5f, 0.30f, 0.05f);
        break;
    case ICON_NONE:
    case ICON_COUNT:
        break;
    }
}
