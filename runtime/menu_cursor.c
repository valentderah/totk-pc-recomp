// The mouse pointer in TotK 1.4.3's own menus (the title, the inventory, the System tab, Options, shops, dialogue
// choices): while a menu screen has the game's focus box (eui's BoxCursor, the frame the d-pad moves), the Windows
// pointer is released from the mouse camera and shown, the item under it takes the focus as if the d-pad had moved
// there (highlight, sound and description included), a left click on an item is A on it and a right click is B.
// Nothing is drawn and no game code is called: at each pad read (input_poll, the game thread) the open screens are read
// and the box is moved with the pad's own directions, so eui's BoxCursorControl does the move itself.
//
// The direct focus write the port started with (the three stores moveBoxCursor makes) does nothing in 1.4.3: the
// request bit is set by the game every frame from its own layout graph, so the cursor is recomputed back. The d-pad
// press is the one input the box actually obeys, so the hover is reached by pressing the direction that closes the gap
// between the focused item and the item under the pointer, once per box move.
//
// Adapted from Weeping of the Empire's engine/runtime/totk_menu_cursor.cpp @ df0586cdc5702ca19bbebfa59697e0b6980f6237
// (https://github.com/daniel-madrid-07/weeping-of-the-empire), Copyright (c) 2026 daniel-madrid-07, MIT License: the
// focus read, the hit test and the stale-hit-pane fallback, which it does for 1.0.0.
// The 1.4.3 placement is Wintego's totk-pc-recomp PR #605 (https://github.com/valentderah/totk-pc-recomp/pull/605):
//  - eui::ScreenMgr::sInstance: 1.0.0 main+0x4640010, 1.4.3 main+0x3acf960 (its createInstance main+0xc613d4 and
//    constructor main+0xc61450 match 1.0.0's main+0x119c5cc / +0x119c648 instruction for instruction, the object
//    0x450 bytes with the same field stores, so its layout is 1.0.0's);
//  - the screen's reserved node (+0x150), request bits (+0x175, bit 2) and draw target byte (+0x16c): the same stores
//    at 1.4.3 main+0x87903c -- kept here as the layout, unused for writes (see above).
// The other fields are 1.0.0's; menu_cursor logs what it reads the first time a menu opens (TOTK_MENU_CURSOR_LOG=1:
// every screen change), so a field that moved shows as a menu with no nodes or boxes off the layout.
//
// TOTK_MENU_CURSOR (live, default 1): 1 gives the menus the pointer; 0 leaves them to the keys and the pad.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "input_wsys.h"
#include "layout.h"
#include "platform/platform.h"

#define SCREEN_MGR_V143 0x3acf960ull   // eui::ScreenMgr::sInstance
#define MAIN_SIZE MOD_MAIN_SIZE        // main's image (code, data, bss) in 1.4.3

// eui::ScreenMgr, BoxCursorMgr, BoxCursorControl
enum { SM_BOX_MGR = 0x3f8, BM_CONTROLS = 0x28, CTL_ACTIVE = 0x58, CTL_RESERVED = 0x60 };
// eui::Screen
enum { SCR_LAYOUT = 0x28, SCR_DRAW_INFO = 0x60, SCR_NODES = 0x80, SCR_RESERVED = 0x150, SCR_STATE = 0x16e,
       SCR_REQUEST = 0x175, SCR_OPENED = 2 };
// eui::BoxCursorNode, AnimButton
enum { NODE_BUTTON = 0x18, NODE_SCREEN = 0x20, BTN_LAYOUT = 0x20, BTN_FLAGS = 0x40, BTN_HIT_PANE = 0x58 };
#define BTN_USABLE 0x20010u            // both set: active and enabled (ButtonGroup::Update's test)
// nn::ui2d::DrawInfo: projection (4 x 4) and view (3 x 4); nn::ui2d::Layout: root pane, size, name
enum { DI_PROJ = 0x10, DI_VIEW = 0x50, LYT_ROOT = 0x18, LYT_SIZE = 0x28, LYT_NAME = 0x30 };
// nn::ui2d::Pane
enum { PANE_PARENT = 0x18, PANE_SIZE = 0x50, PANE_FLAG = 0x58, PANE_ALPHA = 0x59, PANE_BASE = 0x5b, PANE_MTX = 0x70 };

static uint64_t main_lo, main_hi;

static int ok_at(uint64_t p, uint64_t n) {
    return p >= 0x10000 && p < (1ull << 47) && (p & 7) == 0 && plat_mem_readable(p, n);
}
static uint64_t rd64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static uint32_t rd32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static uint8_t rd8(uint64_t a) { return *(const volatile uint8_t *)(uintptr_t)a; }
static float rdf(uint64_t a) { float v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
// An object of the game: readable, its vtable in main's image
static int game_object(uint64_t p, uint64_t n) {
    if (!ok_at(p, n)) return 0;
    uint64_t vt = rd64(p);
    return vt >= main_lo && vt < main_hi;
}

// ---- panes (AnimButton::HitTest's rules: 1.0.0 main+0x222ea40, +0x223b090)
static int pane_shown(uint64_t pane) {
    if (!ok_at(pane, PANE_MTX + 0x20)) return 0;
    if (!(rd8(pane + PANE_FLAG) & 1) || !rd8(pane + PANE_ALPHA)) return 0;
    int depth = 0;
    for (uint64_t p = rd64(pane + PANE_PARENT); p; p = rd64(p + PANE_PARENT)) {
        if (!ok_at(p, PANE_MTX) || ++depth > 64) return 0;
        uint8_t f = rd8(p + PANE_FLAG);
        if (!(f & 1) || (!rd8(p + PANE_ALPHA) && (f & 2))) return 0;
    }
    return 1;
}
// The pane's box in layout coordinates (origin at the centre, y up): centre and half size
static void pane_box(uint64_t pane, float *cx, float *cy, float *hw, float *hh) {
    float w = rdf(pane + PANE_SIZE), h = rdf(pane + PANE_SIZE + 4);
    float m00 = rdf(pane + PANE_MTX), m11 = rdf(pane + PANE_MTX + 0x14);
    uint8_t base = rd8(pane + PANE_BASE), bh = base & 3, bv = base >> 2 & 3;
    *hw = fabsf(w * m00) * 0.5f, *hh = fabsf(h * m11) * 0.5f;
    *cx = rdf(pane + PANE_MTX + 0xc), *cy = rdf(pane + PANE_MTX + 0x1c);
    if (bh == 1) *cx += *hw; else if (bh == 2) *cx -= *hw;   // the translation is the left / right edge
    if (bv == 1) *cy -= *hh; else if (bv == 2) *cy += *hh;   // ... the top / bottom edge
}

// The screen's draw transform, for a screen drawn away from its centred layout space (scale and translation only)
typedef struct { int ok; float v[12], p[16]; } Xf;
static Xf screen_xf(uint64_t scr) {
    Xf x = {0};
    uint64_t di = rd64(scr + SCR_DRAW_INFO);
    if (!ok_at(di, DI_VIEW + sizeof x.v)) return x;
    memcpy(x.p, (const void *)(uintptr_t)(di + DI_PROJ), sizeof x.p);
    memcpy(x.v, (const void *)(uintptr_t)(di + DI_VIEW), sizeof x.v);
    for (int i = 0; i < 16; i++) if (!isfinite(x.p[i])) return x;
    for (int i = 0; i < 12; i++) if (!isfinite(x.v[i])) return x;
    x.ok = x.p[0] != 0 && x.p[5] != 0 && fabsf(x.p[1]) < 1e-5f && fabsf(x.p[4]) < 1e-5f && fabsf(x.v[1]) < 1e-4f &&
           fabsf(x.v[4]) < 1e-4f;
    return x;
}
static int to_ndc(const Xf *t, float x, float y, float *nx, float *ny) {
    float vx = t->v[0] * x + t->v[1] * y + t->v[3], vy = t->v[4] * x + t->v[5] * y + t->v[7],
          vz = t->v[8] * x + t->v[9] * y + t->v[11];
    float cx = t->p[0] * vx + t->p[1] * vy + t->p[2] * vz + t->p[3];
    float cy = t->p[4] * vx + t->p[5] * vy + t->p[6] * vz + t->p[7];
    float cw = t->p[12] * vx + t->p[13] * vy + t->p[14] * vz + t->p[15];
    if (!(fabsf(cw) > 1e-6f)) return 0;
    *nx = cx / cw, *ny = cy / cw;
    return 1;
}
// The transform maps the layout as the plain layout test does (centre to 0,0, corner to 1,1): it adds nothing
static int xf_plain(const Xf *t, float lw, float lh) {
    float ax, ay, bx, by;
    if (!to_ndc(t, 0, 0, &ax, &ay) || !to_ndc(t, lw / 2, lh / 2, &bx, &by)) return 1;
    return fabsf(ax) < 0.01f && fabsf(ay) < 0.01f && fabsf(bx - 1) < 0.01f && fabsf(by - 1) < 0.01f;
}

// The pointer: layout coordinates (x, y) and device coordinates (nx, ny), and the transform when it is not plain
typedef struct { float x, y, nx, ny; const Xf *t; } Pt;
static int pane_hit(uint64_t pane, const Pt *p) {
    if (!pane_shown(pane)) return 0;
    float cx, cy, hw, hh;
    pane_box(pane, &cx, &cy, &hw, &hh);
    if (!p->t) return p->x >= cx - hw && p->x <= cx + hw && p->y >= cy - hh && p->y <= cy + hh;
    float ax, ay, bx, by;
    if (!to_ndc(p->t, cx - hw, cy - hh, &ax, &ay) || !to_ndc(p->t, cx + hw, cy + hh, &bx, &by)) return 0;
    return p->nx >= fminf(ax, bx) && p->nx <= fmaxf(ax, bx) && p->ny >= fminf(ay, by) && p->ny <= fmaxf(ay, by);
}

// ---- screens and nodes
typedef struct { uint64_t node, screen; } Focus;
static uint64_t screen_mgr(void) {
    uint64_t slot = MOD_MAIN_BASE + SCREEN_MGR_V143, sm = rd64(slot);
    return game_object(sm, SM_BOX_MGR + 8) ? sm : 0;
}
// The focused node of an open screen, from the draw targets' box cursors (all 0: none, as in gameplay)
static Focus focus_now(void) {
    Focus none = {0};
    uint64_t sm = screen_mgr(), bm = sm ? rd64(sm + SM_BOX_MGR) : 0;
    if (!ok_at(bm, BM_CONTROLS + 16)) return none;
    for (int i = 0; i < 2; i++) {
        uint64_t ctl = rd64(bm + BM_CONTROLS + 8ull * i);
        if (!ok_at(ctl, CTL_RESERVED + 8)) continue;
        uint64_t node = rd64(ctl + CTL_ACTIVE);
        if (!ok_at(node, NODE_SCREEN + 8)) continue;
        uint64_t scr = rd64(node + NODE_SCREEN);
        if (!game_object(scr, SCR_REQUEST + 1) || rd8(scr + SCR_STATE) != SCR_OPENED) continue;
        return (Focus){node, scr};
    }
    return none;
}
static void layout_name(uint64_t scr, char *out, size_t n) {
    snprintf(out, n, "?");
    uint64_t lyt = rd64(scr + SCR_LAYOUT), name = ok_at(lyt, LYT_NAME + 8) ? rd64(lyt + LYT_NAME) : 0;
    if (name < 0x10000 || name >= (1ull << 47) || !plat_mem_readable(name, n)) return;
    memcpy(out, (const void *)(uintptr_t)name, n - 1);
    out[n - 1] = 0;
    for (char *q = out; *q; q++) if (*q < 0x20 || *q > 0x7e) { *q = 0; break; }
}
static void layout_size(uint64_t scr, float *lw, float *lh) {
    *lw = 1920, *lh = 1080;
    uint64_t lyt = rd64(scr + SCR_LAYOUT);
    if (!ok_at(lyt, LYT_SIZE + 8)) return;
    float w = rdf(lyt + LYT_SIZE), h = rdf(lyt + LYT_SIZE + 4);
    if (w >= 64 && w <= 8192 && h >= 64 && h <= 8192) *lw = w, *lh = h;
}
// The screen's focusable nodes: an sead OffsetList, head {prev, next} at +0x80, node-to-link offset at +0x94
#define MAX_NODES 512
static int nodes_of(uint64_t scr, uint64_t *node, uint64_t *btn) {
    uint64_t head = scr + SCR_NODES;
    int32_t off = (int32_t)rd32(head + 0x14), n = 0;
    if (off < 0 || off > 0x40) return 0;
    for (uint64_t link = rd64(head + 8); link != head && n < MAX_NODES; link = rd64(link + 8)) {
        if (!ok_at(link, 16)) break;
        uint64_t nd = link - (uint64_t)off;
        if (!ok_at(nd, NODE_SCREEN + 8)) continue;
        uint64_t b = rd64(nd + NODE_BUTTON);
        if (!ok_at(b, BTN_HIT_PANE + 8)) continue;
        node[n] = nd, btn[n] = b, n++;
    }
    return n;
}
static int usable(uint64_t btn) { return !(~rd32(btn + BTN_FLAGS) & BTN_USABLE); }
static uint64_t root_pane(uint64_t btn) {
    uint64_t blyt = rd64(btn + BTN_LAYOUT);
    return ok_at(blyt, LYT_ROOT + 8) ? rd64(blyt + LYT_ROOT) : 0;
}
// The node's box in the hit test's own space: its hit pane, or the buttons' root pane when the hit panes are stale
static int node_box(uint64_t b, int stale, float *cx, float *cy) {
    uint64_t pane = stale ? root_pane(b) : rd64(b + BTN_HIT_PANE);
    float hw, hh;
    if (!ok_at(pane, PANE_MTX + 0x20)) return 0;
    pane_box(pane, cx, cy, &hw, &hh);
    return isfinite(*cx) && isfinite(*cy);
}
// Every usable node's shown hit pane lies off the visible layout: the hit panes are stale (TotK never uses touch;
// the dialogue choices' sit far from where the choices are drawn), so the buttons' own parts panes stand for them
static int stale_hit_panes(int n, const uint64_t *btn, float lw, float lh, const Xf *t) {
    int seen = 0;
    for (int i = 0; i < n; i++) {
        if (!usable(btn[i])) continue;
        uint64_t pane = rd64(btn[i] + BTN_HIT_PANE);
        if (!ok_at(pane, PANE_MTX + 0x20) || !pane_shown(pane)) continue;
        float cx, cy, hw, hh, vx, vy;
        pane_box(pane, &cx, &cy, &hw, &hh);
        int in = t ? to_ndc(t, cx, cy, &vx, &vy) && fabsf(vx) <= 1 && fabsf(vy) <= 1
                   : fabsf(cx) <= lw / 2 && fabsf(cy) <= lh / 2;
        seen++;
        if (in) return 0;
    }
    return seen > 0;
}
// The usable node under the pointer, 0: none
static uint64_t node_at(int n, const uint64_t *node, const uint64_t *btn, const Pt *p, float lw, float lh) {
    if (!stale_hit_panes(n, btn, lw, lh, p->t)) {
        for (int i = 0; i < n; i++)
            if (usable(btn[i]) && pane_hit(rd64(btn[i] + BTN_HIT_PANE), p)) return node[i];
        return 0;
    }
    uint64_t found = 0;
    float best = 1e30f;
    for (int i = 0; i < n; i++) {   // parts panes can be several items wide: the nearest centre
        uint64_t pane = root_pane(btn[i]);
        if (!usable(btn[i]) || !pane_hit(pane, p)) continue;
        float cx, cy, hw, hh, d;
        pane_box(pane, &cx, &cy, &hw, &hh);
        d = (cx - p->x) * (cx - p->x) + (cy - p->y) * (cy - p->y);
        if (d < best) best = d, found = node[i];
    }
    return found;
}

// One box move per press, and the box needs its animation to land before the next one, so the press is a step: the
// direction that closes the larger gap first. The drive runs only after the pointer moved (wanted) and stops as soon as
// the box stops answering, so keys and pad keep a menu they were navigated with. 0: this screen's box is not ours to
// drive (the focus is not one of its nodes, or three presses left it where it was).
#define BOX_STEP_MS 90
#define BOX_STALL 3
static int driving;   // the box is on its way to the pointer's hover

static void drive_stop(void) { driving = 0; }

static int drive_focus(uint64_t *buttons, const Focus *f, uint64_t at, int n, const uint64_t *node,
                       const uint64_t *btn, int stale, uint64_t now, int wanted) {
    static uint64_t settle_until, last_node;
    static int stalls;
    if (wanted) driving = 1;
    if (!driving) return 0;
    if (now < settle_until) return 1;
    int fi = -1, ti = -1;
    for (int i = 0; i < n; i++) {
        if (node[i] == f->node) fi = i;
        if (node[i] == at) ti = i;
    }
    float fx, fy, tx, ty;
    if (fi < 0 || ti < 0 || !node_box(btn[fi], stale, &fx, &fy) || !node_box(btn[ti], stale, &tx, &ty)) {
        driving = 0;
        return 0;
    }
    float dx = tx - fx, dy = ty - fy;
    uint64_t bit;
    if (fabsf(dx) >= fabsf(dy)) bit = dx < 0 ? 1ull << B_LEFT : 1ull << B_RIGHT;
    else bit = dy > 0 ? 1ull << B_UP : 1ull << B_DOWN;
    if (f->node == last_node) { if (++stalls >= BOX_STALL) { driving = 0; return 0; } } else stalls = 0;
    last_node = f->node;
    settle_until = now + BOX_STEP_MS;
    *buttons |= bit;
    // The press is the drive's only visible act, so with the log on it is worth its own line.
    const char *e = cfg_get("TOTK_MENU_CURSOR_LOG");
    static uint64_t press_at;
    if (e && !strcmp(e, "1") && now - press_at >= 200) {
        press_at = now;
        rt_log("menu_cursor: the box is %.0f,%.0f from the hover: press %s", dx, dy,
               bit == 1ull << B_LEFT ? "left" : bit == 1ull << B_RIGHT ? "right" : bit == 1ull << B_UP ? "up" : "down");
    }
    return 1;
}

// A screen whose box does not answer the d-pad press: named once per layout, so the menus the pointer cannot drive show
// up in the log instead of doing nothing quietly.
static void give_up(const char *nm) {
    static char last[48];
    if (!strcmp(last, nm)) return;
    snprintf(last, sizeof last, "%s", nm);
    rt_log("menu_cursor: %s: the box did not move with the d-pad, so the pointer only shows and clicks there", nm);
}

// The screens the pointer is taken for: a whitelist, not "everything except these". The game's own HUD carries focus
// boxes too -- the throw and arrow strips, Autobuild's list, the map -- and taking the pointer there is a triple
// regression: input.c drops the wheel when the mouse is not captured (so the material strip stops stepping), the
// cursor appears to be pulled into the middle, and screen_watch's scope flips with it. Names are the layout names eui
// reports and menu_cursor logs each one the first time a screen is seen, so a menu missing here is one name away.
#define MENU_SCREENS_DEFAULT "Title_00,Pouch_00,SystemLoadList_00,SystemSaveLoad_00"
static int menu_screen(const char *nm) {
    const char *list = cfg_get("TOTK_MENU_CURSOR_SCREENS");
    if (!list || !*list) list = MENU_SCREENS_DEFAULT;
    size_t len = strlen(nm);
    for (const char *p = list; *p;) {
        const char *q = p;
        while (*q && *q != ',') q++;
        if ((size_t)(q - p) == len && !strncmp(p, nm, len)) return 1;
        p = *q == ',' ? q + 1 : q;
    }
    return 0;
}

static volatile int32_t g_pointer;   // a menu has the pointer: input_cursor_allowed releases the capture

int menu_cursor_pointer(void) { return __atomic_load_n(&g_pointer, __ATOMIC_RELAXED); }

static int enabled(void) {
    if (!RT_V143) return 0;
    const char *v = cfg_get("TOTK_MENU_CURSOR"), *in = cfg_get("TOTK_INPUT");
    return !(v && !strcmp(v, "0")) && !(in && !strcmp(in, "none"));
}

static void set_pointer(int on) {
    if (plat_xchg32(&g_pointer, on) == on) return;
    ikb_cursor_update();   // and the cursor stays where it is: parking it in the middle reads as the mouse being pulled
}

// The pointer on the game's picture, 0..1 from its top left (the present's fit, letterbox or pillarbox)
static int picture_pos(float *px, float *py) {
    float nx, ny, r[4];
    if (!ikb_pointer(&nx, &ny) || !gfx_picture_rect(r) || r[2] <= 0 || r[3] <= 0) return 0;
    *px = (nx - r[0]) / r[2], *py = (ny - r[1]) / r[3];
    return *px >= 0 && *px <= 1 && *py >= 0 && *py <= 1;
}

static void log_screen(const Focus *f, const char *nm, int n, float lw, float lh, const Xf *t, int stale) {
    static uint64_t last;
    static int lines;
    const char *e = cfg_get("TOTK_MENU_CURSOR_LOG");
    int verbose = e && !strcmp(e, "1");
    if (f->screen == last || (!verbose && lines >= 6)) return;
    last = f->screen, lines++;
    rt_log("menu_cursor: screen %s (layout %gx%g, %d nodes, %s hit panes, draw transform %s)", nm, lw, lh, n,
           stale ? "stale" : "own", t ? "scaled" : "plain");
}

// Once per pad read on the game thread: the pointer's hover and clicks, as pad buttons (A, B, the d-pad) for this read
void menu_cursor_poll(uint64_t *buttons) {
    static int inited, pending_a, held_l, held_r;
    static uint64_t pending_node, pending_until, hold_until, hold_bit;
    static float last_px = -1, last_py = -1;
    if (!inited) {
        inited = 1;
        main_lo = MOD_MAIN_BASE, main_hi = MOD_MAIN_BASE + MAIN_SIZE;
        if (RT_V143) rt_log("menu_cursor: on for 1.4.3 (TOTK_MENU_CURSOR=0 turns it off)");
    }
    if (!enabled()) { set_pointer(0); return; }
    Focus f = focus_now();
    char nm[48] = "";
    if (f.screen) layout_name(f.screen, nm, sizeof nm);
    int menu = f.screen && menu_screen(nm) && ikb_foreground();
    set_pointer(menu);
    int l = ikb_down(KEY_MOUSE_L), r = ikb_down(KEY_MOUSE_R);
    int click_l = l && !held_l, click_r = r && !held_r;
    held_l = l, held_r = r;
    uint64_t now = plat_now_ms();
    if (now < hold_until) {   // a click's button, held for 60 ms so the game sees one press whatever its read rate
        *buttons |= hold_bit;
        return;
    }
    if (!menu) { pending_a = 0, drive_stop(); last_px = last_py = -1; return; }
    static uint64_t node[MAX_NODES], btn[MAX_NODES];
    float lw, lh;
    layout_size(f.screen, &lw, &lh);
    Xf xf = screen_xf(f.screen);
    const Xf *tp = xf.ok && !xf_plain(&xf, lw, lh) ? &xf : NULL;
    int n = nodes_of(f.screen, node, btn);
    int stale = n && stale_hit_panes(n, btn, lw, lh, tp);
    log_screen(&f, nm, n, lw, lh, tp, stale);
    if (pending_a) {   // the focus a click asked for: A once the box has been walked onto it
        if (f.node == pending_node) {
            pending_a = 0, hold_until = now + 60, hold_bit = 1ull << B_A;
            *buttons |= hold_bit;
            return;
        }
        if (now > pending_until || !drive_focus(buttons, &f, pending_node, n, node, btn, stale, now, 1)) {
            pending_a = 0;
            if (f.node != pending_node) give_up(nm);
        }
        return;
    }
    if (click_r) { hold_until = now + 60, hold_bit = 1ull << B_B; *buttons |= hold_bit; return; }
    float px, py;
    if (!picture_pos(&px, &py)) return;
    int moved = fabsf(px - last_px) > 0.001f || fabsf(py - last_py) > 0.001f;
    last_px = px, last_py = py;
    Pt p = {(px - 0.5f) * lw, (0.5f - py) * lh, px * 2 - 1, 1 - py * 2, tp};
    uint64_t at = n ? node_at(n, node, btn, &p, lw, lh) : 0;
    // TOTK_MENU_CURSOR_LOG=1: the pointer's own numbers, so a hit test that misses shows what it was compared with.
    const char *e = cfg_get("TOTK_MENU_CURSOR_LOG");
    static uint64_t diag_at;
    if (e && !strcmp(e, "1") && now - diag_at >= 500) {
        diag_at = now;
        float cx = 0, cy = 0, hw = 0, hh = 0;
        for (int i = 0; i < n; i++)
            if (usable(btn[i]) && pane_shown(rd64(btn[i] + BTN_HIT_PANE))) {
                pane_box(rd64(btn[i] + BTN_HIT_PANE), &cx, &cy, &hw, &hh);
                break;
            }
        rt_log("menu_cursor: pointer %.3f,%.3f -> layout %.0f,%.0f dev %.3f,%.3f; %d nodes %s; first box %.0f,%.0f "
               "%.0fx%.0f; hit %s, focus %s",
               px, py, p.x, p.y, p.nx, p.ny, n, stale ? "stale" : "own", cx, cy, hw * 2, hh * 2,
               at ? "node" : "none", at && at == f.node ? "there" : "elsewhere");
    }
    if (!at) { drive_stop(); return; }
    if (at == f.node) {   // the box is already under the pointer: there is nothing to walk, but a click still means A
        drive_stop();
        if (click_l) { hold_until = now + 60, hold_bit = 1ull << B_A; *buttons |= hold_bit; }
        return;
    }
    if (click_l) pending_a = 1, pending_node = at, pending_until = now + 1200;
    if (!pending_a && !drive_focus(buttons, &f, at, n, node, btn, stale, now, moved)) give_up(nm);
}
