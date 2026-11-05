// The blueprint import entry on the game's own Autobuild screen: the ⊕ button and the word "Import" in the screen's
// bottom guide bar, that button opening the host's file picker, and the chosen .cai or .txt file going into the
// Autobuild history through blueprint_import.h.
//
// The game draws that bar itself (blyt/AutoBuilder_00.bflyt: the A_Guide_00 align pane and its Pa_Guide_0N parts, filled
// from LayoutMsg/AutoBuilder_00.msbt). This row is drawn over the game, right of the bar's "Back" hint, by the present
// path's ImGui overlay, which already runs while the settings menu is closed (ui/menu.cpp's performance overlay path).
// Nothing of the game's archives is edited, so no mod, resolution or language of the game can be disturbed, and the row
// works on every game version and every pad layout.
//
// The screen is recognised by the layout name runtime/menu_cursor.c already reads for its cursor work: the Autobuild
// screen is AutoBuilder_00. That name is read once per pad read on the game thread. Reading it there rather than in the
// overlay means the game's input for the frame is decided before the game sees it, so the "Receive" press that this
// port cannot use (it needs Zelda Notes) can be taken over by this entry in the same poll.
//
// The name comes from the game's focus box, which the Autobuild screen does not always keep, so it reads "" on some of
// the polls it is open. Taking that as "left the screen" makes the row and the overlay blink; the screen is treated as
// gone only after it has stayed gone for STILL_GONE_MS, which is far longer than the gaps a focus box leaves.
//
// The file dialog runs on folder_pick's own thread, so its answer cannot be imported there: the callback records the path
// and a counter, and the next presented frame starts the import on a thread of its own, as the Blueprints tab does, so
// neither the present path nor the game waits for the walk of the game's memory.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"

#include "blueprint_hint.h"

extern "C" {
#include "blueprint_dir.h"
#include "blueprint_import.h"
#include "cfg.h"
#include "folder_pick.h"
#include "gfx.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "platform/platform.h"
#include "ui_text.h"
}
#include "ui/menu.h"

// The row's place on the game's 1920x1080 UI grid. A_Guide_00 sits at x -350, y -477 and its four Pa_Guide_0N parts sit
// 290.334 apart from x -319.25; the fourth ("Back") is at 551.752, so a fifth part would start at 842.086. In pixels from
// the picture's top left, its left edge is 960 + (-350 + 842.086) = 1452, and the bar's centre line is 540 - (-477) = 1017.
enum {
    BP_HINT_GRID_W = 1920, BP_HINT_GRID_H = 1080,
    BP_HINT_LEFT = 1440,
    BP_HINT_CENTRE_Y = 1017,
    BP_HINT_ICON_R = 26,   // the ⊕ circle's radius, about the size of the game's own footer key icons
    BP_HINT_GAP = 12,
    BP_HINT_STILL_GONE_MS = 400,
};

// ---- written on the game thread
static volatile int32_t g_screen;   // 1 while the Autobuild screen was the focus at the last pad read

// ---- written on the present path
static uint64_t g_screen_seen_ns;   // when the Autobuild screen was last read, whichever thread read it
static int g_held;                  // the button was down at the last frame, so one hold opens one dialog
static PlatMutex g_lock = PLAT_MUTEX_INIT;
static char g_pending[PLAT_PATH_MAX];
static unsigned g_pending_gen;      // bumped when the dialog returned a file
static unsigned g_done_gen;         // the generation the present path has already acted on
static int g_dialog_open;
static int g_importing;

// TOTK_BLUEPRINT_HINT=0 leaves the Autobuild screen entirely as the game draws it.
static int enabled(void) {
    const char *v = cfg_get("TOTK_BLUEPRINT_HINT");
    return !(v && !strcmp(v, "0"));
}

// 1 while the Autobuild screen is up. The name is read on both threads: the game thread reads it once per pad read, which
// is what lets bp_hint_poll take the button before the game sees it, and the present thread reads it too, because the game
// stops reading the pad for long stretches on that screen and the game thread's last reading would then keep the row up
// or down long after the screen changed. Either reading freshens the latch; it lapses only after STILL_GONE_MS.
static int screen_up(void) {
    if (!enabled()) return 0;
    char name[48];
    const int up = __atomic_load_n(&g_screen, __ATOMIC_RELAXED) ||
                   (menu_cursor_screen_name(name, (int)sizeof name) && !strcmp(name, BP_HINT_SCREEN));
    const uint64_t now = plat_now_ns();
    if (up) __atomic_store_n(&g_screen_seen_ns, now, __ATOMIC_RELAXED);
    const uint64_t seen = __atomic_load_n(&g_screen_seen_ns, __ATOMIC_RELAXED);
    return seen && now - seen < (uint64_t)BP_HINT_STILL_GONE_MS * 1000000ull;
}

int bp_hint_active(void) { return screen_up(); }

void bp_hint_poll(uint64_t *buttons) {
    static char name[48];
    static int logged;
    name[0] = 0;
    const int up = enabled() && menu_cursor_screen_name(name, (int)sizeof name) && !strcmp(name, BP_HINT_SCREEN);
    __atomic_store_n(&g_screen, up, __ATOMIC_RELAXED);
    if (up) __atomic_store_n(&g_screen_seen_ns, plat_now_ns(), __ATOMIC_RELAXED);
    if (up != logged) {
        logged = up;
        rt_log("blueprint: hint %s the Autobuild screen", up ? "is on" : "left");
    }
    // The game's own use of this button on this screen ("Receive") needs Zelda Notes, which this port has not: taking it
    // keeps the press from doing something invisible and from reaching the game twice, once for the dialog and once for
    // the share dialog that would never open.
    if (up) *buttons &= ~(1ull << B_PLUS);
}

// ---- the dialog, and the import of what it returned

static void on_file(const char *path, void *ctx) {
    (void)ctx;
    // The dialog's answer carries the folder it was browsed in, so the screen's own entry keeps the same memory as the
    // Blueprints tab's button does (blueprint_dir.h).
    if (path && *path) bp_dir_remember(path);
    plat_mutex_lock(&g_lock);
    g_dialog_open = 0;
    if (path && *path) {
        snprintf(g_pending, sizeof g_pending, "%s", path);
        g_pending_gen++;
    }
    plat_mutex_unlock(&g_lock);
    rt_log("blueprint: hint chose %s", path && *path ? path : "no file");
}

static void import_thread(void *arg) {
    char *path = (char *)arg;
    const int r = bp_import_newest(path);
    rt_log("blueprint: hint: %s: %s", path, bp_import_text(r));
    plat_mutex_lock(&g_lock);
    g_importing = 0;
    plat_mutex_unlock(&g_lock);
    free(path);
}

// Present path: the dialog is up or an import runs, so the press is already in hand.
static void open_picker(void) {
    plat_mutex_lock(&g_lock);
    if (g_dialog_open || g_importing) {
        plat_mutex_unlock(&g_lock);
        return;
    }
    g_dialog_open = 1;
    plat_mutex_unlock(&g_lock);
    static const char *const patterns[] = {"*.cai", "*.txt"};
    rt_log("blueprint: hint: opening the file picker");
    file_pick("", patterns, 2, on_file, NULL);
}

// Present path: hands the file to a thread of its own, because bp_import_newest walks the game's memory.
static void start_import(const char *path) {
    plat_mutex_lock(&g_lock);
    if (g_importing) {
        plat_mutex_unlock(&g_lock);
        return;
    }
    g_importing = 1;
    plat_mutex_unlock(&g_lock);
    char *copy = (char *)malloc(strlen(path) + 1);
    if (!copy || !plat_thread_spawn(import_thread, copy, "blueprint import", 0)) {
        rt_log("blueprint: hint: cannot start the import thread");
        free(copy);
        plat_mutex_lock(&g_lock);
        g_importing = 0;
        plat_mutex_unlock(&g_lock);
        return;
    }
    strcpy(copy, path);
}

void bp_hint_frame(void) {
    // A file the dialog returned is the one thing to act on here, whatever the game shows by now: the player picked it
    // while the Autobuild screen was up. Acting on it before the button read keeps a fast second press from being lost.
    char path[PLAT_PATH_MAX];
    unsigned gen;
    plat_mutex_lock(&g_lock);
    gen = g_pending_gen;
    snprintf(path, sizeof path, "%s", g_pending);
    plat_mutex_unlock(&g_lock);
    if (gen != g_done_gen) {
        g_done_gen = gen;
        if (path[0]) start_import(path);
    }

    // The settings menu owns the input while it is open, and this row is not drawn then either.
    if (menu_is_open() || !bp_hint_active()) {
        g_held = 0;
        return;
    }
    uint64_t keys = 0;
    int32_t lx, ly, rx, ry;
    input_poll_ui(&keys, &lx, &ly, &rx, &ry);   // never blocked: this press belongs to the host, not to the game
    const int down = (int)((keys >> B_PLUS) & 1);
    if (down && !g_held) open_picker();
    g_held = down;
}

// ---- the row

// The ⊕ of the game's own footer: a filled disc with a plus cut out of it.
static void plus_icon(ImDrawList *dl, ImVec2 c, float r, ImU32 col) {
    dl->AddCircleFilled(c, r, col, 32);
    const float t = r * 0.19f, a = r * 0.60f;   // half thickness and half length of the bars
    const ImU32 hole = IM_COL32(0, 0, 0, 235);
    dl->AddRectFilled(ImVec2(c.x - a, c.y - t), ImVec2(c.x + a, c.y + t), hole);
    dl->AddRectFilled(ImVec2(c.x - t, c.y - a), ImVec2(c.x + t, c.y + a), hole);
}

void bp_hint_draw(int width, int height, float scale) {
    float r[4];
    if (!bp_hint_active() || menu_is_open() || width <= 0 || height <= 0) return;
    if (!gfx_picture_rect(r) || r[2] <= 0 || r[3] <= 0) return;   // the picture inside letterbox bars

    const float px = r[2] * width, py = r[3] * height;
    const float sx = px / BP_HINT_GRID_W;               // one grid pixel in screen pixels
    const float x0 = r[0] * width + BP_HINT_LEFT * sx;
    const float cy = r[1] * height + BP_HINT_CENTRE_Y * py / BP_HINT_GRID_H;

    const char *word = ui_text(UI_MENU_BLUEPRINT_HINT);
    const ImVec2 ts = ImGui::CalcTextSize(word);
    const float icon_r = BP_HINT_ICON_R * sx;
    const float y0 = cy - ts.y * 0.5f;
    ImDrawList *dl = ImGui::GetForegroundDrawList();

    // The game's own hints put the button first and the word after it, so this row does the same: the ⊕ at its left
    // edge, the word to its right, both on the guide bar's centre line.
    const ImU32 shadow = IM_COL32(0, 0, 0, 150);
    const ImVec2 c(x0 + icon_r, cy);
    dl->AddCircle(ImVec2(c.x + 2 * sx, c.y + 2 * sx), icon_r, shadow, 32, 3 * scale);
    plus_icon(dl, c, icon_r, IM_COL32(255, 255, 255, 245));
    const float tx = x0 + icon_r * 2 + BP_HINT_GAP * sx;
    // A drop shadow keeps the word readable over a bright scene without looking like a menu: the same text, offset
    // down and right, in black at low alpha.
    dl->AddText(ImVec2(tx + 2 * sx, y0 + 2 * sx), shadow, word);
    dl->AddText(ImVec2(tx, y0), IM_COL32(255, 255, 255, 245), word);
}
