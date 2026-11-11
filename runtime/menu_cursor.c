// The mouse pointer in TotK 1.4.3's own menus (the title, the inventory, the System tab, Options, shops, dialogue
// choices): while a menu screen has the game's focus box (eui's BoxCursor, the frame the d-pad moves), the Windows
// pointer is released from the mouse camera and shown, the item under it takes the focus as if the d-pad had moved
// there (highlight, sound and description included), a left click on an item is A on it and a right click is B.
// Nothing is drawn and no game code is called: at each pad read (ov_input.c GetNpadStates, the game thread) the open
// screens are read and the focus is moved with the three writes eui::BoxCursorMgr::moveBoxCursor makes, so the game's
// own BoxCursorControl moves the box at its next update.
//
// Adapted from Weeping of the Empire's engine/runtime/totk_menu_cursor.cpp @ df0586cdc5702ca19bbebfa59697e0b6980f6237
// (https://github.com/daniel-madrid-07/weeping-of-the-empire), Copyright (c) 2026 daniel-madrid-07, MIT License: the
// focus read, the hit test, the stale-hit-pane fallback and the focus writes, which it does for 1.0.0.
// Placed in 1.4.3 by hand:
//  - eui::ScreenMgr::sInstance: 1.0.0 main+0x4640010, 1.4.3 main+0x3acf960 (its createInstance main+0xc613d4 and
//    constructor main+0xc61450 match 1.0.0's main+0x119c5cc / +0x119c648 instruction for instruction, the object
//    0x450 bytes with the same field stores, so its layout is 1.0.0's);
//  - the screen's reserved node (+0x150) and request bits (+0x175, bit 2) and its draw target byte (+0x16c): the same
//    stores at 1.4.3 main+0x87903c.
// The other fields are 1.0.0's; menu_cursor logs what it reads the first time a menu opens (TOTK_MENU_CURSOR_LOG=1:
// every screen change), so a field that moved shows as a menu with no nodes or boxes off the layout.
//
// Only while keyboard and mouse are the device used last (input_kbm_last): a gamepad player sees the menus unchanged.
// TOTK_MENU_CURSOR (live, default 1): 0 leaves the menus to the keys and the pad.
//
// With TOTK_MAP_CLICK=1 (live, default 0: one key per controller button) input.c makes the left mouse button A on the
// map (menu_cursor_map), so a click acts at the map's cursor. The map is found only while TOTK_MENU_CURSOR is on.
//
// The same read of the open screens tells input.c when a dialogue box is on screen (menu_cursor_dialogue), where the
// left mouse button presses B (it advances the text and skips its scroll) and the right one A, instead of their
// bindings (Y by default, which opens the dialogue log, and ZL). TOTK_DIALOGUE_CLICK (live, default 0: one key per
// controller button) switches that on its own: it does not need the pointer.
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
#define MAIN_SIZE 0x3adf000ull         // main's image (code, data, bss) in 1.4.3

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

// How far the pointer moves, in layout units (1280 x 720 across the picture: 1.5 window pixels at 1080p, 3 at 4K),
// before it takes back a focus that the wheel, the keys or the pad moved away from it (menu_cursor_poll)
#define DEAD_ZONE 6.0f

static uint64_t main_lo, main_hi;

static int ok_at(uint64_t p, uint64_t n) {
    return p >= 0x10000 && p < (1ull << 47) && (p & 7) == 0 && mem_guest_readable(p, n);
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
typedef struct { uint64_t control, node, screen; } Focus;
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
        return (Focus){ctl, node, scr};
    }
    return none;
}
static void layout_name(uint64_t scr, char *out, size_t n) {
    snprintf(out, n, "?");
    uint64_t lyt = rd64(scr + SCR_LAYOUT), name = ok_at(lyt, LYT_NAME + 8) ? rd64(lyt + LYT_NAME) : 0;
    if (name < 0x10000 || name >= (1ull << 47) || !mem_guest_readable(name, n)) return;
    memcpy(out, (const void *)(uintptr_t)name, n - 1);
    out[n - 1] = 0;
    for (char *q = out; *q; q++) if (*q < 0x20 || *q > 0x7e) { *q = 0; break; }
}
// The open screens: eui::ScreenMgr's screen array, count at +0x8 and its pointers at +0x10 (1.0.0's layout, which the
// 1.4.3 constructor keeps). The map has no focus box, so it is found here: the Purah Pad's frame (AppMenuOverlay_00)
// with AppMap_00 and the menu's background AppMenuBG_00 open. The Scope opens the frame over AppMap_00 without that
// background (Weeping of the Empire's engine/runtime/totk_menu_cursor.cpp census tells them apart the same way).
// The dialogue window (DIALOGUE_SCREEN) has no focus box either: the scan remembers its screen for dialogue_shown.
// A D-pad strip has no focus box either while it is held open: *strip is ShortCut_00 being open. *camera is the
// Purah Pad's camera (the ability wheel's camera): the frame and its pointer CameraPointer_00 open without AppMap_00,
// which the Scope has with them.
// TOTK_MENU_CURSOR_LOG=1 logs the open screens whenever the set changes.
enum { SM_COUNT = 0x8, SM_SCREENS = 0x10 };
#define DIALOGUE_SCREEN "MessageDialog_00"   // the box a character's lines and the game's messages are shown in
#define DIALOGUE_GAP_MS 500                  // how long after the box closed a click is still the dialogue's
static uint64_t dialogue_scr;                // game thread only
static int map_open(int *strip, int *camera) {
    uint64_t sm = screen_mgr();
    if (!sm) return 0;
    int32_t n = (int32_t)rd32(sm + SM_COUNT);
    uint64_t arr = rd64(sm + SM_SCREENS);
    *strip = *camera = 0;
    if (n <= 0 || n > 1024 || !ok_at(arr, 8ull * (uint64_t)n)) return 0;
    const char *e = cfg_get("TOTK_MENU_CURSOR_LOG");
    int verbose = e && !strcmp(e, "1"), pad = 0, map = 0, bg = 0, pointer = 0;
    char list[1024];
    size_t len = 0;
    uint64_t h = 1469598103934665603ull;
    for (int32_t i = 0; i < n; i++) {
        uint64_t scr = rd64(arr + 8ull * (uint64_t)i);
        if (!game_object(scr, SCR_REQUEST + 1) || rd8(scr + SCR_STATE) != SCR_OPENED) continue;
        char nm[48];
        layout_name(scr, nm, sizeof nm);
        pad |= !strcmp(nm, "AppMenuOverlay_00");
        map |= !strcmp(nm, "AppMap_00");
        bg |= !strcmp(nm, "AppMenuBG_00");
        if (!strcmp(nm, DIALOGUE_SCREEN)) dialogue_scr = scr;
        pointer |= !strcmp(nm, "CameraPointer_00");
        *strip |= !strcmp(nm, "ShortCut_00");
        if (!verbose) continue;
        for (const char *c = nm; *c; c++) h = (h ^ (uint8_t)*c) * 1099511628211ull;
        if (len < sizeof list - 64) len += (size_t)snprintf(list + len, sizeof list - len, "%s%s", len ? " " : "", nm);
    }
    static uint64_t last;
    static int lines;
    if (verbose && h != last && lines < 200) last = h, lines++, rt_log("menu_cursor: open screens: %s", list);
    *camera = pad && pointer && !map && !bg;
    return pad && map && bg;
}
// The dialogue window is open. Read at every pad read from the screen the last scan found open, so a click follows the
// box without the scan's 100 ms: the game keeps its screens, so the one found once is the one that opens the next time.
// A screen that is no longer that layout (a reload built the screens again) is forgotten until a scan finds it.
static int dialogue_shown(void) {
    uint64_t scr = dialogue_scr;
    if (!scr) return 0;
    if (!game_object(scr, SCR_REQUEST + 1)) { dialogue_scr = 0; return 0; }
    if (rd8(scr + SCR_STATE) != SCR_OPENED) return 0;
    char nm[48];
    layout_name(scr, nm, sizeof nm);
    if (strcmp(nm, DIALOGUE_SCREEN)) { dialogue_scr = 0; return 0; }
    return 1;
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
// The focus to node: the writes moveBoxCursor makes. The request bit is a byte eui also writes, so it is raised
// again until the focus is there.
static void move_focus(const Focus *f, uint64_t node) {
    memcpy((void *)(uintptr_t)(f->control + CTL_RESERVED), &node, 8);
    memcpy((void *)(uintptr_t)(f->screen + SCR_RESERVED), &node, 8);
    __atomic_fetch_or((uint8_t *)(uintptr_t)(f->screen + SCR_REQUEST), (uint8_t)4, __ATOMIC_SEQ_CST);
}

// Screens with a focus box that are gameplay, not menus: the L ability wheel, the D-pad strips, Autobuild's list, the
// Purah Pad's camera and the map (the stick moves its own cursor there)
static int gameplay_screen(const char *nm) {
    static const char *const names[] = {"ShortCutMainItem_00", "ShortCut_00", "AutoBuilder_00", "CameraPointer_00",
                                        "AppMap_00"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!strcmp(nm, names[i])) return 1;
    return 0;
}

static volatile int32_t g_pointer;   // a menu has the pointer: input_cursor_allowed releases the capture
static volatile int32_t g_map;       // the Purah Pad's map is open (not the Scope): the mouse moves its cursor
static volatile int32_t g_dialogue;  // a dialogue box is on screen and TOTK_DIALOGUE_CLICK is on: left B, right A
static volatile int32_t g_menu;      // a menu screen has the focus box, from either device (the scroll wheel's steps)
static volatile int32_t g_strip;     // a D-pad strip's screen is open (the scroll wheel's steps along it)
static volatile int32_t g_camera;    // the Purah Pad's camera is open (the scroll wheel zooms it)

int menu_cursor_pointer(void) { return __atomic_load_n(&g_pointer, __ATOMIC_RELAXED); }
int menu_cursor_map(void) { return __atomic_load_n(&g_map, __ATOMIC_RELAXED); }
int menu_cursor_dialogue(void) { return __atomic_load_n(&g_dialogue, __ATOMIC_RELAXED); }
int menu_cursor_menu(void) { return __atomic_load_n(&g_menu, __ATOMIC_RELAXED); }
int menu_cursor_strip(void) { return __atomic_load_n(&g_strip, __ATOMIC_RELAXED); }
int menu_cursor_camera(void) { return __atomic_load_n(&g_camera, __ATOMIC_RELAXED); }

static int enabled(void) {
    if (!RT_V143) return 0;
    const char *v = cfg_get("TOTK_MENU_CURSOR"), *in = cfg_get("TOTK_INPUT");
    return !(v && !strcmp(v, "0")) && !(in && !strcmp(in, "none"));
}
// The left click in dialogue: its own setting, on with the menu cursor off too
static int dialogue_enabled(void) {
    if (!RT_V143) return 0;
    const char *in = cfg_get("TOTK_INPUT");
    return cfg_bool("TOTK_DIALOGUE_CLICK") && !(in && !strcmp(in, "none"));
}

static void set_pointer(int on) {
    if (plat_xchg32(&g_pointer, on) == on) return;
    ikb_cursor_update();
    if (on) ikb_pointer_set(0.5f, 0.5f);   // the pointer appears in the middle, not where the capture left it
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

// Once per pad read on the game thread: the pointer's hover and clicks, as pad buttons (A, B) for this read
void menu_cursor_poll(uint64_t *buttons) {
    static int inited, pending_a, held_l, held_r, anchored;
    static uint64_t pending_node, pending_until, hold_until, hold_bit, menu_seen;
    static uint64_t ptr_node, last_focus;   // the item the pointer last pointed at, the focus at the last read
    static float last_px = -1, last_py = -1, anchor_px, anchor_py;
    if (!inited) {
        inited = 1;
        main_lo = MOD_MAIN_BASE, main_hi = MOD_MAIN_BASE + MAIN_SIZE;
        if (RT_V143) rt_log("menu_cursor: on for 1.4.3 (TOTK_MENU_CURSOR=0 turns it off)");
    }
    int cursor = enabled(), talk = dialogue_enabled();
    if (!cursor && !talk) {
        set_pointer(0);
        __atomic_store_n(&g_map, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_dialogue, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_menu, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_strip, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_camera, 0, __ATOMIC_RELAXED);
        return;
    }
    // The open screens are scanned every 100 ms, not at every pad read: each screen costs memory queries
    uint64_t now = plat_now_ms();
    static uint64_t map_at;
    static int map, map_logged, dialogue_logged, strip, camera;
    if (!map_at || now - map_at >= 100) map_at = now, map = map_open(&strip, &camera);
    // Between two lines of a conversation the game closes the box and opens it again, so a click made in that gap still
    // counts as dialogue for DIALOGUE_GAP_MS after the box was last open. The first clicks after a conversation fall
    // in it too and are B or A, as they are for a player who taps the button through the last line.
    // TOTK_MENU_CURSOR_LOG=1 logs how long each gap was.
    static uint64_t dialogue_seen;
    int box = talk && dialogue_shown();
    if (!talk) dialogue_seen = 0;
    if (box) {
        static int gap_lines;
        const char *e = cfg_get("TOTK_MENU_CURSOR_LOG");
        uint64_t gap = dialogue_seen ? now - dialogue_seen : 0;
        if (gap > 30 && gap < 5000 && gap_lines < 100 && e && !strcmp(e, "1"))
            gap_lines++, rt_log("menu_cursor: the dialogue box was closed for %llu ms", (unsigned long long)gap);
        dialogue_seen = now;
    }
    int dialogue = box || (dialogue_seen && now - dialogue_seen < DIALOGUE_GAP_MS);
    if (dialogue && !dialogue_logged)
        dialogue_logged = 1, rt_log("menu_cursor: a dialogue box is open (" DIALOGUE_SCREEN "): left click is B, "
                                    "right click A (TOTK_DIALOGUE_CLICK=1)");
    __atomic_store_n(&g_dialogue, dialogue, __ATOMIC_RELAXED);
    if (!cursor) {
        set_pointer(0);
        __atomic_store_n(&g_map, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_menu, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_strip, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_camera, 0, __ATOMIC_RELAXED);
        return;
    }
    __atomic_store_n(&g_strip, strip, __ATOMIC_RELAXED);
    __atomic_store_n(&g_camera, camera, __ATOMIC_RELAXED);
    Focus f = focus_now();
    char nm[48] = "";
    if (f.screen) layout_name(f.screen, nm, sizeof nm);
    if (map && !map_logged) map_logged = 1, rt_log("menu_cursor: the map is open: the mouse moves its cursor");
    __atomic_store_n(&g_map, map, __ATOMIC_RELAXED);
    // With the gamepad as the last device the menus stay as they are on the console: no pointer, the capture kept.
    // Moving the mouse (or a key) makes keyboard and mouse the last device and the pointer comes.
    __atomic_store_n(&g_menu, f.screen && !gameplay_screen(nm) && !map, __ATOMIC_RELAXED);
    int menu = f.screen && !gameplay_screen(nm) && ikb_foreground() && input_kbm_last();
    if (menu) menu_seen = now;
    // A menu's focus gap (a tab switch, a popup opening: no focus box for a frame or two) keeps the pointer where it
    // is, for up to 300 ms: releasing it there captured it again and centred it when the next tab took the focus
    int gap = !menu && !f.screen && menu_seen && now - menu_seen < 300 && ikb_foreground() && input_kbm_last();
    set_pointer(menu || gap);
    int l = ikb_down(KEY_MOUSE_L), r = ikb_down(KEY_MOUSE_R);
    int click_l = l && !held_l, click_r = r && !held_r;
    held_l = l, held_r = r;
    if (now < hold_until) {   // a click's button, held for 60 ms so the game sees one press whatever its read rate
        *buttons |= hold_bit;
        return;
    }
    if (gap) return;   // a click's focus request waits for the next screen
    if (!menu) { pending_a = 0, anchored = 0, ptr_node = last_focus = 0; last_px = last_py = -1; return; }
    if (pending_a) {   // the focus asked for by a click: A once it is there
        if (rd64(f.control + CTL_ACTIVE) == pending_node) {
            pending_a = 0, hold_until = now + 60, hold_bit = 1ull << B_A;
            *buttons |= hold_bit;
            return;
        }
        if (now > pending_until) pending_a = 0;
        else move_focus(&f, pending_node);
        return;
    }
    if (click_r) { hold_until = now + 60, hold_bit = 1ull << B_B; *buttons |= hold_bit; return; }
    float px, py;
    if (!picture_pos(&px, &py)) return;
    // The focus moved to another item than the pointer's (the scroll wheel, the keys, the pad): the pointer takes it
    // back only after it has left a dead zone of DEAD_ZONE layout units around where it was then, so a hand resting on
    // the mouse does not undo a scroll. A click still acts at once.
    if (f.node != last_focus) {
        if (f.node != ptr_node) anchored = 1, anchor_px = px, anchor_py = py;
        last_focus = f.node;
    }
    int moved = fabsf(px - last_px) > 0.001f || fabsf(py - last_py) > 0.001f;
    if (!moved && !click_l) return;
    last_px = px, last_py = py;
    float lw, lh;
    layout_size(f.screen, &lw, &lh);
    if (anchored && !click_l && fabsf(px - anchor_px) * lw < DEAD_ZONE && fabsf(py - anchor_py) * lh < DEAD_ZONE)
        return;
    anchored = 0;
    Xf xf = screen_xf(f.screen);
    Pt p = {(px - 0.5f) * lw, (0.5f - py) * lh, px * 2 - 1, 1 - py * 2, NULL};
    if (xf.ok && !xf_plain(&xf, lw, lh)) p.t = &xf;
    static uint64_t node[MAX_NODES], btn[MAX_NODES];
    int n = nodes_of(f.screen, node, btn);
    log_screen(&f, nm, n, lw, lh, p.t, n && stale_hit_panes(n, btn, lw, lh, p.t));
    uint64_t at = n ? node_at(n, node, btn, &p, lw, lh) : 0;
    if (!at) return;
    ptr_node = at;
    if (at != f.node) move_focus(&f, at);
    if (click_l) {
        if (at == f.node) { hold_until = now + 60, hold_bit = 1ull << B_A; *buttons |= hold_bit; }
        else pending_a = 1, pending_node = at, pending_until = now + 300;
    }
}

// The L ability wheel or a D-pad strip (materials, weapons, shields, bows) has the focus: 1 / 0, -1 with the menu
// cursor off (the screens are not read). input.c asks while L or the D-pad is held, before it gives the mouse to their
// selectors: L also soothes the horse and turns menu tabs, the D-pad does other things too.
int menu_cursor_shortcut(void) {
    if (!enabled()) return -1;
    Focus f = focus_now();
    char nm[48] = "";
    if (f.screen) layout_name(f.screen, nm, sizeof nm);
    return f.screen && (!strcmp(nm, "ShortCutMainItem_00") || !strcmp(nm, "ShortCut_00"));
}
