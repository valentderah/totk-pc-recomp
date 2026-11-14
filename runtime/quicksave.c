// Quick save and quick load, 1.4.3 only (TOTK_QUICKSAVE, live, default 0): one key saves the game and another loads the
// save it made, F6 and F7 by default as in Half-Life (TOTK_QUICKSAVE_KEY, TOTK_QUICKLOAD_KEY: a key name of
// input_map.c, or none; live). Both go through the game's own pause menu, as a player with a controller would: Plus
// opens it, R turns to the System tab, its first button (Save) or second (Load) takes the focus and A presses it, A on
// the confirmation's first button (Save, Load) answers yes. Load picks the list's entry without the autosave label, the
// game's one manual save, which is also where quick save writes. Nothing writes save files from here: the game saves
// and loads as it does from the menu, so it refuses wherever it refuses a player (the pause menu does not open, Save
// cannot be pressed) and quicksave stops.
//
// The menu is read with menu_cursor.c's screen reads (menu_ui_*): the screen that has the focus box and its buttons,
// found by their parts layout and root pane names; the focus moves with the same writes as the mouse pointer's. Each
// step waits for the screen it expects; another screen, or none within its time, ends the macro, and B is pressed
// until no menu has the focus box (a confirmation answers no, the pause menu closes). While the macro runs the game
// gets only its presses, not the player's input, and the mouse pointer leaves the menus alone.
//
// It starts only in play: the world loaded, the HUD's PlayerStatus_00 open, no screen with the focus box, no dialogue
// box and no loading fade. Script lines QUICKSAVE and QUICKLOAD (input.c) start it as the keys do.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "platform/platform.h"

enum { REQ_NONE, REQ_SAVE, REQ_LOAD };
enum { ST_IDLE, ST_OPEN, ST_TAB, ST_PICK, ST_LIST, ST_CONFIRM, ST_SAVED, ST_CLOSE, ST_LOADING, ST_CLEANUP };

#define PRESS_MS 100        // a press, held long enough for any of the game's reads
#define GAP_MS 150          // released after each press before the next step looks again
#define SETTLE_MS 250       // a screen that just took the focus box plays its opening: act after this
#define TAB_MS 100          // a tab of the pause menu that just took the focus: act after this (R stops at the last)
#define STEP_MS 3000        // the most any step waits for its screen
#define SAVE_MS 15000       // the most the game takes to write the save and show the System tab again
#define MAX_TABS 12         // R presses to reach the System tab, the last one (R stops there)
#define MAX_BACK 10         // B presses to leave the menus after a failure

static volatile int32_t g_request;   // REQ_*, set by the keys or a script, taken by quicksave_poll
static struct {
    int what, state, tries;
    uint64_t state_at, press_until, gap_until, press_bit, focus_since, started;
    char focus[48];
} m;

static int save_key = KEY_F6, load_key = KEY_F7;   // KEY_NONE: that key is off

static int key_id(const char *name) {
    for (int k = KEY_A; k < KEY_COUNT; k++)
        if (input_key_name(k) && !strcasecmp(name, input_key_name(k))) return k;
    return KEY_NONE;
}

// A key setting as a KEY_*: a key name of input_map.c (any case) or "none"; anything else is logged and the default
// from the settings table is used
static int key_setting(const char *var, const char *ini) {
    const char *v = cfg_value(var), *def = cfg_meta(var)->def;
    if (!strcasecmp(v, "none")) return KEY_NONE;
    int k = key_id(v);
    if (k == KEY_NONE) rt_log("quicksave: %s: unknown key '%s', %s is used", ini, v, def), k = key_id(def);
    return k;
}

// The keys are read with the switch, so with it off nothing is logged
int quicksave_on(void) {
    static unsigned seen = ~0u;
    static int on;
    unsigned v = cfg_version();
    if (v == seen) return on;
    int was = on;
    seen = v, on = RT_V143 && cfg_bool("TOTK_QUICKSAVE");
    if (!on) return 0;
    int s = key_setting("TOTK_QUICKSAVE_KEY", "quicksave_key"), l = key_setting("TOTK_QUICKLOAD_KEY", "quickload_key");
    if (l == s && l != KEY_NONE) {
        rt_log("quicksave: quickload_key is the quick save key %s: it saves, no key loads", input_key_name(l));
        l = KEY_NONE;
    }
    if (!was || s != save_key || l != load_key)
        rt_log("quicksave: %s saves, %s loads", s ? input_key_name(s) : "no key", l ? input_key_name(l) : "no key");
    save_key = s, load_key = l;
    return on;
}

void quicksave_request(int load) {
    if (!quicksave_on()) return;
    plat_cas32(&g_request, load ? REQ_LOAD : REQ_SAVE, REQ_NONE);
}

void quicksave_keys(uint8_t *down, int act) {
    static int held_s, held_l;
    int s = save_key && down[save_key], l = load_key && down[load_key];
    if (act && s && !held_s) quicksave_request(0);
    if (act && l && !held_l) quicksave_request(1);
    held_s = s, held_l = l;
    down[save_key] = down[load_key] = 0;   // KEY_NONE is slot 0, never a key
}

int quicksave_busy(void) { return m.state != ST_IDLE; }

static const char *name(void) { return m.what == REQ_LOAD ? "quickload" : "quicksave"; }

static void enter(int state, uint64_t now) { m.state = state, m.state_at = now, m.tries = 0; }

static void press(int bit, uint64_t now) {
    m.press_bit = 1ull << bit, m.press_until = now + PRESS_MS, m.gap_until = now + PRESS_MS + GAP_MS;
}

static void fail(const char *why, uint64_t now) {
    rt_log("%s: %s; leaving the menus", name(), why);
    enter(ST_CLEANUP, now);
}

// Why play is not at a point where the macro may open the pause menu, NULL when it is
static const char *not_in_play(void) {
    char scr[48];
    uint64_t node;
    if (frame_step_world_us() < 0) return "no save is loaded";
    if (menu_ui_focus(scr, sizeof scr, &node)) return "a menu is open";
    if (menu_ui_open("LoadingFade_00") || menu_ui_open("EventFade_00")) return "the game is loading";
    if (menu_ui_open("MessageDialog_00")) return "a dialogue box is open";
    if (!menu_ui_open("PlayerStatus_00")) return "the HUD is hidden (a cutscene or an event)";
    return NULL;
}

// Once per pad read on the game thread, after menu_cursor_poll: 1 while the macro runs, with the game's input
// replaced by its presses
int quicksave_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    if (m.state == ST_IDLE && !__atomic_load_n(&g_request, __ATOMIC_RELAXED)) return 0;   // a plain read when idle
    uint64_t now = plat_now_ms();
    if (m.state == ST_IDLE) {
        int req = plat_xchg32(&g_request, REQ_NONE);
        if (req == REQ_NONE || !quicksave_on()) return 0;
        m.what = req;
        const char *why = not_in_play();
        if (why) { rt_log("%s: not now: %s", name(), why); return 0; }
        rt_log("%s: through the game's System tab", name());
        m.started = now;
        enter(ST_OPEN, now);
        press(B_PLUS, now);
    }
    plat_xchg32(&g_request, REQ_NONE);   // a key pressed while it runs is dropped, not queued
    *buttons = 0, *lx = *ly = *rx = *ry = 0;
    if (now < m.press_until) { *buttons = m.press_bit; return 1; }
    if (now < m.gap_until) return 1;
    char scr[48];
    uint64_t node;
    int focus = menu_ui_focus(scr, sizeof scr, &node);
    if (!focus || strcmp(scr, m.focus)) m.focus_since = now, snprintf(m.focus, sizeof m.focus, "%s", focus ? scr : "");
    int settled = focus && now - m.focus_since >= SETTLE_MS;
    uint64_t waited = now - m.state_at;
    if (m.state != ST_CLEANUP && now - m.started > 2 * SAVE_MS) { fail("took too long", now); return 1; }
    switch (m.state) {
    case ST_OPEN:   // Plus: the pause menu (Pouch_00) takes the focus box
        if (focus && strcmp(scr, "Pouch_00")) { fail("another menu opened", now); break; }
        if (settled) enter(ST_TAB, now);
        else if (waited > STEP_MS) fail("the pause menu did not open", now);
        break;
    case ST_TAB: {   // R until the System tab's buttons show
        int ok;
        if (focus && strcmp(scr, "Pouch_00")) { fail("another menu opened", now); break; }
        if (!focus || now - m.focus_since < TAB_MS) {
            if (waited > STEP_MS) fail("the pause menu lost the focus", now);
            break;
        }
        if (menu_ui_button("PaCommonBtnThin_00", "Pa_Button_00", &ok)) { enter(ST_PICK, now); break; }
        if (m.tries++ >= MAX_TABS) { fail("the System tab was not found", now); break; }
        press(B_R, now);
        m.state_at = now;   // each tab gets its own wait
        break;
    }
    case ST_PICK: {   // Save (Pa_Button_00) or Load (Pa_Button_01) takes the focus, then A
        int ok = 0;
        uint64_t b = menu_ui_button("PaCommonBtnThin_00", m.what == REQ_LOAD ? "Pa_Button_01" : "Pa_Button_00", &ok);
        if (!b) { fail("the System tab closed", now); break; }
        if (!ok) { fail(m.what == REQ_LOAD ? "the game does not allow loading now" : "the game does not allow saving here", now); break; }
        if (menu_ui_focus_to(b)) { press(B_A, now); enter(m.what == REQ_LOAD ? ST_LIST : ST_CONFIRM, now); }
        else if (waited > STEP_MS) fail("the focus did not move to the button", now);
        break;
    }
    case ST_LIST: {   // Load's list (SystemLoadList_00): the manual save takes the focus, then A
        if (!settled || strcmp(scr, "SystemLoadList_00")) {
            if (focus && settled && strcmp(scr, "Pouch_00")) fail("another menu opened", now);
            else if (waited > STEP_MS) fail("the list of saves did not open", now);
            break;
        }
        uint64_t b = menu_ui_manual_save();
        if (!b) { fail("there is no manual save to load (quick save makes one)", now); break; }
        if (menu_ui_focus_to(b)) { press(B_A, now); enter(ST_CONFIRM, now); }
        else if (waited > STEP_MS) fail("the focus did not move to the manual save", now);
        break;
    }
    case ST_CONFIRM: {   // "Save the game?" / "Load this save?" (SystemSaveLoad_00): its first button, then A
        if (!settled || strcmp(scr, "SystemSaveLoad_00")) {
            int expected = !strcmp(scr, m.what == REQ_LOAD ? "SystemLoadList_00" : "Pouch_00");
            if (focus && settled && !expected) fail("another menu opened", now);
            else if (waited > STEP_MS) fail("the game did not ask to confirm", now);
            break;
        }
        uint64_t b = menu_ui_button("PaCommonBtnThin_00", "Pa_CheckButton_00", NULL);
        if (!b) { fail("the confirmation has no first button", now); break; }
        if (menu_ui_focus_to(b)) { press(B_A, now); enter(m.what == REQ_LOAD ? ST_LOADING : ST_SAVED, now); }
        else if (waited > STEP_MS) fail("the focus did not move to the confirmation", now);
        break;
    }
    case ST_SAVED:   // the game writes the save and shows the System tab again: B closes the pause menu
        if (settled && !strcmp(scr, "Pouch_00")) { press(B_B, now); enter(ST_CLOSE, now); }
        else if (focus && settled && strcmp(scr, "SystemSaveLoad_00")) fail("another menu opened after saving", now);
        else if (waited > SAVE_MS) fail("the save did not finish", now);
        break;
    case ST_CLOSE:
        if (!focus) { rt_log("quicksave: saved (%.1f s)", (now - m.started) / 1000.0); enter(ST_IDLE, now); }
        else if (waited > STEP_MS) fail("the pause menu did not close", now);
        break;
    case ST_LOADING:   // the game fades out and loads: the macro ends there
        if (menu_ui_open("LoadingFade_00")) {
            rt_log("quickload: loading the manual save (%.1f s)", (now - m.started) / 1000.0);
            enter(ST_IDLE, now);
        } else if (focus && settled && strcmp(scr, "SystemSaveLoad_00")) fail("another menu opened instead of loading", now);
        else if (waited > SAVE_MS) fail("the load did not start", now);
        break;
    case ST_CLEANUP:   // B until no menu has the focus box
        if (!focus) { enter(ST_IDLE, now); break; }
        if (m.tries++ >= MAX_BACK) { rt_log("%s: a menu stays open after %d presses of B", name(), MAX_BACK); enter(ST_IDLE, now); break; }
        press(B_B, now);
        m.gap_until = now + PRESS_MS + 2 * GAP_MS;
        break;
    }
    if (now < m.press_until) *buttons = m.press_bit;
    return 1;
}
