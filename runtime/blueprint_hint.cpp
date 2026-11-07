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
// The picker's answer cannot be imported in its callback: the callback records the path and a counter, and the next
// presented frame starts the import on a thread of its own, so neither the present path nor the game waits for the walk
// of the game's memory.
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
#include "gfx.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "platform/platform.h"
#include "ui_text.h"
}
#include "ui/blueprint_picker.h"
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
    BP_HINT_STILL_GONE_MS = 120,
};

// ---- written on the game thread
static volatile int32_t g_screen;   // 1 while the Autobuild screen was the focus at the last pad read

// ---- written on the present path
static uint64_t g_screen_seen_ns;   // when the Autobuild screen was last read, whichever thread read it
static int g_held;                  // the button was down at the last frame, so one hold opens the picker once
static PlatMutex g_lock = PLAT_MUTEX_INIT;
static char g_pending[PLAT_PATH_MAX];
static unsigned g_pending_gen;      // bumped when the picker returned a file
static unsigned g_done_gen;         // the generation the present path has already acted on
static int g_dialog_open;
static int g_importing;
// The last import's outcome, shown beside the import row for a few seconds (see import_thread).
static char g_result_note[128];
static int g_result_ok;
static uint64_t g_result_until_ns;

// TOTK_BLUEPRINT_HINT=0 leaves the Autobuild screen entirely as the game draws it.
static int enabled(void) {
    const char *v = cfg_get("TOTK_BLUEPRINT_HINT");
    return !(v && !strcmp(v, "0"));
}

// 1 while the Autobuild screen is up. The screen is found among the open screens, not through the focus box: on that
// screen the focus box is only on a list row or a tab, and the import row must be there on all of it. The game thread
// reads it once per pad read, which is what lets bp_hint_poll take the button before the game sees it, and the present
// thread reads it too, because the game stops reading the pad for long stretches there. Either reading freshens the
// latch; it lapses only after STILL_GONE_MS.
// 1 while the Autobuild screen is up AND its selection is on one of the three tabs. The tab test is the one
// bp_hint_poll latches on the game thread; the present thread re-runs it because the game stops reading the pad for
// long stretches on this screen. The latch lapses only after STILL_GONE_MS, so a focus box that the game momentarily
// clears cannot blink the row.
static int screen_up(void) {
    if (!enabled()) return 0;
    const int up = __atomic_load_n(&g_screen, __ATOMIC_RELAXED) ||
                   (menu_cursor_screen_open(BP_HINT_SCREEN) && menu_cursor_focus_is_tab(BP_HINT_SCREEN));
    const uint64_t now = plat_now_ns();
    if (up) __atomic_store_n(&g_screen_seen_ns, now, __ATOMIC_RELAXED);
    const uint64_t seen = __atomic_load_n(&g_screen_seen_ns, __ATOMIC_RELAXED);
    return seen && now - seen < (uint64_t)BP_HINT_STILL_GONE_MS * 1000000ull;
}

int bp_hint_active(void) { return screen_up(); }

void bp_hint_poll(uint64_t *buttons) {
    static int logged;
    const int on_screen = enabled() && menu_cursor_screen_open(BP_HINT_SCREEN);
    // The row belongs to the screen's tab row, not to its whole surface: while the selection sits in the card list the
    // player is choosing a draft, and an import entry there would compete with that. So the screen being open is not
    // enough - the focus has to be on one of the three tabs (favourites, drafts, history).
    const int up = on_screen && menu_cursor_focus_is_tab(BP_HINT_SCREEN);
    __atomic_store_n(&g_screen, up, __ATOMIC_RELAXED);
    if (up) __atomic_store_n(&g_screen_seen_ns, plat_now_ns(), __ATOMIC_RELAXED);
    if (up != logged) {
        logged = up;
        rt_log("blueprint: hint %s", up ? "is on the Autobuild screen's tabs" : "left the Autobuild screen's tabs");
    }
    // The game's own use of this button on this screen ("Receive") needs Zelda Notes, which this port has not: taking it
    // keeps the press from doing something invisible and from reaching the game twice, once for the picker and once for
    // the share dialog that would never open.
    if (up) *buttons &= ~(1ull << B_PLUS);
}

// ---- the picker, and the import of what it returned

static void on_file(const char *path, void *ctx) {
    (void)ctx;
    // The chosen file's folder is remembered, so the next picker opens where this one was left (blueprint_dir.h).
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
    // The player pressed nothing else and is still looking at the screen: say what happened, next to the import row,
    // for a few seconds. Before this the only trace of a failed import was a log line nobody sees.
    snprintf(g_result_note, sizeof g_result_note, "%s", bp_import_text(r));
    g_result_ok = r == BP_IMPORT_OK;
    g_result_until_ns = plat_now_ns() + 4000ull * 1000000ull;
    plat_mutex_unlock(&g_lock);
    free(path);
}

// Present path: opens the port's own file picker, not the Windows dialog. The Windows dialog takes the focus away from
// the game and cannot be driven by the pad, and this entry exists so the import is reachable with the pad alone.
static void open_picker(void) {
    plat_mutex_lock(&g_lock);
    if (g_dialog_open || g_importing) {
        plat_mutex_unlock(&g_lock);
        return;
    }
    g_dialog_open = 1;
    plat_mutex_unlock(&g_lock);
    rt_log("blueprint: hint: opening the file picker");
    if (!bp_picker_open("", on_file, NULL)) {
        plat_mutex_lock(&g_lock);
        g_dialog_open = 0;
        plat_mutex_unlock(&g_lock);
        rt_log("blueprint: hint: the file picker is already open");
    }
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
    // A file the picker returned is the one thing to act on here, whatever the game shows by now: the player picked it
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

    // The last import's outcome, right of the row for a few seconds: gold when the blueprint went in, red when it did
    // not, with the reason. Before this, a refused file left no trace the player could see.
    char note[sizeof g_result_note];
    int ok = 0;
    plat_mutex_lock(&g_lock);
    const int show = g_result_note[0] && plat_now_ns() < g_result_until_ns;
    if (show) {
        snprintf(note, sizeof note, "%s", g_result_note);
        ok = g_result_ok;
    }
    plat_mutex_unlock(&g_lock);
    if (show) {
        const char *w = ui_text(ok ? UI_MENU_BLUEPRINT_IMPORTED : UI_MENU_BLUEPRINT_FAILED);
        char line[192];
        // On success the line is just the word: "Imported ok" reads like a stack trace, and the player can see the
        // result in the history behind the panel anyway. A failure keeps its reason, which is the whole point of it.
        if (ok) snprintf(line, sizeof line, "%s", w);
        else snprintf(line, sizeof line, "%s %s", w, note);
        const ImVec2 ls = ImGui::CalcTextSize(line);
        const float lx = tx + ts.x + BP_HINT_GAP * 2 * sx;
        const float ly = cy - ls.y * 0.5f;
        const ImU32 col = ok ? IM_COL32(236, 194, 84, 250) : IM_COL32(240, 120, 110, 250);
        dl->AddText(ImVec2(lx + 2 * sx, ly + 2 * sx), shadow, line);
        dl->AddText(ImVec2(lx, ly), col, line);
    }
}
