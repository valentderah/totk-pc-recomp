// The game's current eui screen for the PC scheme's context. The ScreenMgr/BoxCursor offsets are the 1.4.3 placement of
// Weeping of the Empire's menu-cursor read (MIT, Copyright (c) 2026 daniel-madrid-07), as placed by Wintego's totk-pc-
// recomp PR #605 (runtime/menu_cursor.c): eui::ScreenMgr::sInstance main+0x3acf960, its BoxCursorMgr at +0x3f8, the
// focused node's screen at node+0x20, the screen's nn::ui2d::Layout at screen+0x28 and the layout's name at +0x30. Only
// the read is used here; the cursor, hit test and focus writes are not.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input_map.h"
#include "kbm_scheme.h"
#include "layout.h"
#include "platform/platform.h"
#include "screen_watch.h"

#define SCREEN_MGR_V143 0x3acf960ull
#define MAIN_SIZE MOD_MAIN_SIZE

enum { SM_BOX_MGR = 0x3f8, BM_CONTROLS = 0x28, CTL_ACTIVE = 0x58 };
enum { SCR_LAYOUT = 0x28, SCR_STATE = 0x16e, SCR_OPENED = 2 };
enum { NODE_SCREEN = 0x20 };
enum { LYT_NAME = 0x30 };

static uint64_t main_lo, main_hi;

static int ok_at(uint64_t p, uint64_t n) {
    return p >= 0x10000 && p < (1ull << 47) && (p & 7) == 0 && plat_mem_readable(p, n);
}
static uint64_t rd64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static uint8_t rd8(uint64_t a) { return *(const volatile uint8_t *)(uintptr_t)a; }

static int game_object(uint64_t p, uint64_t n) {
    if (!ok_at(p, n)) return 0;
    uint64_t vt = rd64(p);
    return vt >= main_lo && vt < main_hi;
}

static uint64_t screen_mgr(void) {
    uint64_t slot = MOD_MAIN_BASE + SCREEN_MGR_V143;
    if (!ok_at(slot, 8)) return 0;
    uint64_t sm = rd64(slot);
    return game_object(sm, SM_BOX_MGR + 8) ? sm : 0;
}

// The focused node's open screen, or 0. The BoxCursorMgr holds the two draw targets' controls.
static uint64_t focus_screen(void) {
    uint64_t sm = screen_mgr(), bm = sm ? rd64(sm + SM_BOX_MGR) : 0;
    if (!ok_at(bm, BM_CONTROLS + 16)) return 0;
    for (int i = 0; i < 2; i++) {
        uint64_t ctl = rd64(bm + BM_CONTROLS + 8ull * i);
        if (!ok_at(ctl, CTL_ACTIVE + 8)) continue;
        uint64_t node = rd64(ctl + CTL_ACTIVE);
        if (!ok_at(node, NODE_SCREEN + 8)) continue;
        uint64_t scr = rd64(node + NODE_SCREEN);
        if (!game_object(scr, SCR_STATE + 1) || rd8(scr + SCR_STATE) != SCR_OPENED) continue;
        return scr;
    }
    return 0;
}

static void layout_name(uint64_t scr, char *out, size_t n) {
    out[0] = 0;
    uint64_t lyt = rd64(scr + SCR_LAYOUT), name = ok_at(lyt, LYT_NAME + 8) ? rd64(lyt + LYT_NAME) : 0;
    if (name < 0x10000 || name >= (1ull << 47) || !plat_mem_readable(name, n)) return;
    memcpy(out, (const void *)(uintptr_t)name, n - 1);
    out[n - 1] = 0;
    for (char *q = out; *q; q++)
        if (*q < 0x20 || *q > 0x7e) {
            *q = 0;
            break;
        }
}

static int starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
static int has(const char *s, const char *p) { return strstr(s, p) != NULL; }

// The layout name to the scheme's context. The names are the game's blyt layouts (the menu cursor logs Title_00,
// SystemLoadList_00, SystemSaveLoad_00, Pouch_00; its gameplay list adds ShortCut*, AutoBuilder_00, CameraPointer_00,
// AppMap_00). Everything with a focus box that is not listed as gameplay is a menu.
static int scope_of(const char *nm) {
    if (!nm || !*nm) return KBM_SC_MENUS;
    if (starts(nm, "Title")) return KBM_SC_TITLE;
    if (starts(nm, "SystemLoad") || starts(nm, "SystemSave")) return KBM_SC_SAVES;
    if (starts(nm, "Pouch") || starts(nm, "Pause") || starts(nm, "AppPause") || starts(nm, "PaPause")) return KBM_SC_PAUSE;
    if (starts(nm, "AppMap") || starts(nm, "PaMap") || starts(nm, "PaAppMap")) return KBM_SC_MAP;
    if (starts(nm, "ShortCut")) return KBM_SC_QUICKMENU;
    if (starts(nm, "AutoBuilder")) return KBM_SC_CONSTRUCT;
    if (starts(nm, "Camera")) return KBM_SC_CAMERA;
    if (starts(nm, "Scope")) return KBM_SC_SCOPE;
    if (starts(nm, "Shop") || starts(nm, "PlacementShop")) return KBM_SC_SHOP;
    if (starts(nm, "Recipe")) return KBM_SC_RECIPE;
    if (has(nm, "MessageLogChoice") || starts(nm, "Choice")) return KBM_SC_CHOICE;
    if (has(nm, "MessageDialog") || starts(nm, "Dialog")) return KBM_SC_DIALOGUE;
    if (has(nm, "Option")) return KBM_SC_OPTIONS;
    if (starts(nm, "Loading")) return KBM_SC_LOADING;
    if (starts(nm, "Horse") || starts(nm, "PaHorse") || starts(nm, "OwnedHorse")) return KBM_SC_MENUS;
    if (starts(nm, "AppMenu") || starts(nm, "Menu")) return KBM_SC_MENUS;
    return KBM_SC_MENUS;
}

static volatile int32_t g_scope = KBM_SC_WORLD;

static int enabled(void) {
    if (!RT_V143) return 0;
    const char *v = cfg_get("TOTK_SCREEN_WATCH"), *in = cfg_get("TOTK_INPUT");
    return !(v && !strcmp(v, "0")) && !(in && !strcmp(in, "none"));
}

static int log_on(void) {
    const char *v = cfg_get("TOTK_SCREEN_WATCH_LOG");
    return !(v && !strcmp(v, "0"));
}

void screen_watch_poll(uint64_t buttons) {
    static int inited, map_on;
    static uint64_t prev;
    if (!inited) {
        inited = 1;
        main_lo = MOD_MAIN_BASE, main_hi = MOD_MAIN_BASE + MAIN_SIZE;
    }
    if (!enabled()) {
        g_scope = KBM_SC_WORLD;
        return;
    }
    uint64_t edge = buttons & ~prev;
    prev = buttons;
    // The map has no focus box (the stick moves its own cursor): the game's Minus / the scheme's M toggles it, and B
    // closes it. That is an edge, so it cannot chatter.
    //
    // The quick-menu strips are NOT readable here: they never own the box cursor (a play session logs ShortCut zero
    // times), and guessing "a strip is open" from a held D-pad direction is wrong twice over -- the scheme's own
    // movement and focus rows carry those bits, and its 70 ms pulse makes a held key appear on one poll and not the
    // next, which flips the context every frame (433 quickmenu lines against 441 gameplay ones in one session). The
    // strips therefore stay in the world context; only a real screen change moves it.
    //
    // A held bit may still switch the context, but only after it has been held steadily for SW_STEADY_MS: the pulse
    // alone can then never reach it.
#define SW_STEADY_MS 150
    if (edge & (1ull << B_MINUS)) map_on = !map_on;
    if (edge & (1ull << B_B)) map_on = 0;
    static uint64_t aim_since;
    uint64_t scr = focus_screen();
    char nm[48] = "";
    int sc;
    if (scr) {
        layout_name(scr, nm, sizeof nm);
        sc = scope_of(nm);
    } else if (map_on) {
        sc = KBM_SC_MAP;
    } else if (buttons & (1ull << B_ZR)) {
        if (!aim_since) aim_since = plat_now_ms();
        sc = plat_now_ms() - aim_since >= SW_STEADY_MS ? KBM_SC_AIM : g_scope;
    } else {
        aim_since = 0;
        sc = KBM_SC_WORLD;
    }
    int old = g_scope;
    g_scope = sc;
    if (log_on() && old != sc) {
        const char *what = nm[0] ? nm
                           : sc == KBM_SC_AIM ? "(aiming)"
                           : sc == KBM_SC_MAP ? "(map)"
                           : sc == KBM_SC_QUICKMENU ? "(quick menu)"
                           : "(gameplay)";
        rt_log("screen: %s -> %s", what, kbm_scheme_scope_name(sc));
    }
}

int screen_watch_ctx(int host_menu_open) {
    if (host_menu_open) return KBM_SC_PAUSE;
    if (!enabled()) return KBM_SC_WORLD;
    return g_scope;
}
