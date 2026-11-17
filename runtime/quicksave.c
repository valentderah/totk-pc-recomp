// Quick save and quick load, 1.4.3 only (TOTK_QUICKSAVE, live, default 0): one key saves the game at once and another
// loads that save, F6 and F7 by default as in Half-Life (TOTK_QUICKSAVE_KEY, TOTK_QUICKLOAD_KEY: a key name of
// input_map.c, or none; live). Keyboard and mouse keys only: a gamepad has no quick save button. No menu shows: quick
// save asks the game directly, quick load runs the game's load menu under a held picture.
//
// Quick save asks the game for an autosave the way its own event graphs do (the execute of the sequence node
// OneShotAutoSaveMgrRequestAutoSave, 1.4.3 main+0x24fed3c): component 10 of the game's root is the AutoSaveMgr,
// main+0x513ab8 says whether it would autosave now (a cutscene, an event, a fight or a recent autosave say no) and
// main+0x1f8ed24 queues the request with kind 4, as that node does. The game then writes one of its autosave slots and
// commits the save; fs_host.c reports the slot folder that commit wrote (fs_save_commits), and quicksave copies it to
// <profile>/quicksave/slot, its name to <profile>/quicksave/slot.txt. A toast at the top of the picture says "Game
// saved" (ui/menu.cpp menu_toast), or why nothing was saved.
//
// Quick load: the game starts a load only from its own load list (System tab) or its Game Over screen, and loads the
// slot that screen names; nothing in play requests one. So quick load drives the load list as a player with a
// controller would, under a held picture: the toast "Loading the quick save" shows, the window keeps that picture
// (gfx_present_hold) while Plus, R to the System tab, Load, the list's first save and the confirmation's Load are
// pressed, and the picture comes back with the game's loading fade. From the confirmation on, the game's reads of the
// files of a slot open the quick save's files instead (fs_save_read_from), so it loads the quick save whichever slot
// the list offered first, and no slot of the save is written for it (quicksave_poll); the redirect ends with the load
// (reads_watch).
//
// Both start only in play: the world loaded, the HUD's PlayerStatus_00 open, no screen with the focus box, no dialogue
// box and no loading fade. Script lines QUICKSAVE and QUICKLOAD (input.c) start them as the keys do.
//
// Threads. 1.4.3 has no one thread for the AutoSaveMgr: a trace of the hooked functions (function-entry hooks on a test
// build) saw the game itself call AUTOSAVE_REQ from at least four guest threads, as events and sequence nodes run on the
// game's job workers, and the manager check its pending request (main+0x50ed8c, +0x50f3e8, with AUTOSAVE_CAN) on two
// others; the pad was read on six. So quick save calls AUTOSAVE_CAN and AUTOSAVE_REQ the way those events do: from a
// game thread between two of the game's own calls, the pad read (quicksave_guest, from ov_input.c's npad_states).
// AUTOSAVE_REQ only sets the manager's request flag, and the manager checks AUTOSAVE_CAN again before it saves. As
// several threads read the pad at once, one of them at a time steps quick save and quick load (qs_lock); the others give
// the game the same presses.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cfg.h"
#include "fs.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "layout.h"
#include "gfx.h"
#include "platform/platform.h"
#include "ui/menu.h"
#include "ui_text.h"

// 1.4.3 main: offsets from MOD_MAIN_BASE
#define GAME_ROOT 0x395dc70ull       // ** the game's root object (the sequence nodes' first read)
#define COMPONENT 0x89f9b0ull        // (root, index): the root's component of that index, 0 when none
#define AUTOSAVE_INDEX 10            // the AutoSaveMgr's component index
#define AUTOSAVE_TYPE 0xc1e348ull    // (): AutoSaveMgr's runtime type info, for its vtable's first method (the cast)
#define AUTOSAVE_CAN 0x513ab8ull     // (mgr, kind, 0): bit 0 when the game would autosave now
#define AUTOSAVE_REQ 0x1f8ed24ull    // (mgr, kind): queue an autosave
#define AUTOSAVE_KIND 4              // the kind OneShotAutoSaveMgrRequestAutoSave asks for
#define SAVE_MS 5000                 // the most the game takes from the request to the commit of the save (0.5 s seen)
#define REQ_FLAGS 0x128ull           // mgr+0x128: the request flags; AUTOSAVE_REQ sets bit 0 of their byte 5

enum { REQ_NONE, REQ_SAVE, REQ_LOAD };
static volatile int32_t g_request;   // REQ_*, set by the keys or a script, taken by quicksave_guest
static int saving;                   // 1 waiting for the commit of the requested autosave, 2 copying it
static uint32_t commits_before;
static uint64_t save_started;
static uint64_t req_flags;           // the request flags' byte AUTOSAVE_REQ set, 0 when the game had set it already

static int save_key = KEY_F6, load_key = KEY_F7;   // KEY_NONE: that key is off
static PlatMutex qs_lock = PLAT_MUTEX_INIT;          // the pad-reading thread that steps quick save / quick load

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
    int was = on, want = cfg_bool("TOTK_QUICKSAVE");
    static int told;
    seen = v, on = RT_V143 && want;
    // The game's addresses below are 1.4.3's: on 1.1.0 and 1.2.1 the setting is refused once, and no call is made
    if (want && !RT_V143 && !told) told = 1, rt_log("quicksave: game 1.4.3 only; quick save and quick load stay off");
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

// Why play is not at a point where quick save or load may start, NULL when it is
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

static void say(int text, const char *what, const char *why) {
    rt_log("%s: %s", what, why);
    menu_toast(text, 2500);
}

static int readable(uint64_t p, uint64_t n) { return p >= 0x10000 && p < (1ull << 47) && mem_guest_readable(p, n); }

// The game's AutoSaveMgr, found and cast as OneShotAutoSaveMgrRequestAutoSave's execute does; 0 when it has none
static uint64_t autosave_mgr(Cpu *c) {
    uint64_t g = MOD_MAIN_BASE + GAME_ROOT;
    if (!readable(g, 8) || !readable(LD64(g), 8)) return 0;
    uint64_t root = LD64(LD64(g));
    if (!readable(root, 8)) return 0;
    uint64_t mgr = rt_call(c, MOD_MAIN_BASE + COMPONENT, 2, root, (uint64_t)AUTOSAVE_INDEX);
    if (!readable(mgr, 0x1a8) || !readable(LD64(mgr), 8)) return 0;
    uint64_t type = rt_call(c, MOD_MAIN_BASE + AUTOSAVE_TYPE, 0);
    return rt_call(c, LD64(LD64(mgr)), 2, mgr, type) ? mgr : 0;
}

static void path_of(char *out, size_t cap, const char *leaf) {
    snprintf(out, cap, "%s/quicksave%s%s", rt_config()->profile, leaf ? "/" : "", leaf ? leaf : "");
}

static void save_start(Cpu *c) {
    const char *why = not_in_play();
    if (why) { say(UI_QS_NOT_NOW, "quicksave: not now", why); return; }
    uint64_t mgr = autosave_mgr(c);
    if (!mgr) { say(UI_QS_FAILED, "quicksave", "the game has no AutoSaveMgr"); return; }
    // The manager's timer (+0x150, seconds) holds autosaves back for some ten minutes after the game's last one; a
    // quick save is the player's own and skips it, as Save in the System tab does. The other checks of AUTOSAVE_CAN
    // stay: its flags, the game's flag that turns autosaves off (events, fights) and the player's state. AUTOSAVE_REQ
    // itself drops the request while flag +0x1a0 is set and the state +0x1a4 is 3 (but for kind 2).
    float timer;
    memcpy(&timer, (const void *)(uintptr_t)(mgr + 0x150), 4);
    ST32(mgr + 0x150, 0);
    int can = rt_call(c, MOD_MAIN_BASE + AUTOSAVE_CAN, 3, mgr, (uint64_t)AUTOSAVE_KIND, 0ull) & 1;
    if (can && LD8(mgr + 0x1a0) && LD32(mgr + 0x1a4) == 3) can = 0;
    if (!can) {
        memcpy((void *)(uintptr_t)(mgr + 0x150), &timer, 4);   // the game's own wait goes on as it was
        say(UI_QS_NOT_NOW, "quicksave: not now", "the game would not autosave here");
        return;
    }
    if (timer > 0) rt_log("quicksave: the game's autosave wait (%.0f s left) skipped", timer);
    char slot[64];
    commits_before = fs_save_commits(slot, sizeof slot);
    uint64_t flags = LD64(mgr + REQ_FLAGS) + 5;
    if (!readable(flags, 1)) { say(UI_QS_FAILED, "quicksave", "the AutoSaveMgr has no request flags"); return; }
    int pending = LD8(flags) & 1;   // the game's own request, which quick save then waits for
    rt_call(c, MOD_MAIN_BASE + AUTOSAVE_REQ, 2, mgr, (uint64_t)AUTOSAVE_KIND);
    if (!(LD8(flags) & 1)) { say(UI_QS_NOT_NOW, "quicksave: not now", "the game dropped the request"); return; }
    req_flags = pending ? 0 : flags;
    saving = 1, save_started = plat_now_ms();
    rt_log("quicksave: autosave requested");
}

// The slot the autosave wrote, copied to <profile>/quicksave on a thread of its own; saving goes back to 0 at its end
static char copy_slot[64];
static void copy_main(void *arg) {
    (void)arg;
    char dir[1200], data[1200], note[1200];
    path_of(dir, sizeof dir, NULL), path_of(data, sizeof data, "slot"), path_of(note, sizeof note, "slot.txt");
    plat_fs_mkdir(dir);
    if (fs_save_copy_slot(copy_slot, data) || !plat_file_write_atomic(note, copy_slot, strlen(copy_slot))) {
        say(UI_QS_FAILED, "quicksave", "the autosave could not be copied to the quick save");
    } else {
        rt_log("quicksave: saved: the game's autosave in %s (%.1f s), kept in %s", copy_slot,
               (plat_now_ms() - save_started) / 1000.0, data);
        menu_toast(UI_QS_SAVED, 2500);
    }
    __atomic_store_n(&saving, 0, __ATOMIC_RELEASE);
}

// After the request: the first commit that wrote a slot folder is the autosave
static void save_wait(void) {
    if (__atomic_load_n(&saving, __ATOMIC_ACQUIRE) != 1) return;   // 2: the copy runs
    char slot[64];
    uint32_t n = fs_save_commits(slot, sizeof slot);
    if (n == commits_before || !slot[0]) {
        // The game can take the request and still hold the autosave back (a fight in the Depths did): quick save then
        // takes its request back, so the game does not save some time later on its own, and says so
        if (plat_now_ms() - save_started > SAVE_MS) {
            if (req_flags) ST8(req_flags, LD8(req_flags) & ~1u);
            saving = 0;
            say(UI_QS_NOT_NOW, "quicksave: not now",
                req_flags ? "the game held the autosave back; the request is withdrawn" : "the game held its autosave back");
        }
        return;
    }
    saving = 2;   // the copy runs on its own thread: no file I/O on the game thread
    snprintf(copy_slot, sizeof copy_slot, "%s", slot);
    if (!plat_thread_spawn(copy_main, NULL, "quicksave", 0)) copy_main(NULL);
}

// ---- quick load: the game's load menu, driven under a held picture
enum { ST_IDLE, ST_TOAST, ST_OPEN, ST_TAB, ST_PICK, ST_LIST, ST_CONFIRM, ST_LOADING, ST_CLEANUP };
#define PRESS_MS 100        // a press, held long enough for any of the game's reads
#define GAP_MS 120          // released after each press before the next step looks again
#define SETTLE_MS 200       // a screen that just took the focus box plays its opening: act after this
#define TAB_MS 100          // a tab of the pause menu that just took the focus: act after this (R stops at the last)
#define TOAST_MS 150        // the toast is presented before the picture is held
#define STEP_MS 3000        // the most any step waits for its screen
#define LOAD_MS 10000       // the most the game takes from the confirmation to its loading fade
#define BACK_MS 500         // the load is over once the HUD has been back this long, the loading fade closed
#define READ_MAX_MS 120000  // the redirect ends this long after the confirmation whatever happened
#define MAX_TABS 12         // R presses to reach the System tab, the last one (R stops there)
#define MAX_BACK 10         // B presses to leave the menus after a failure

static struct {
    int state, tries;
    uint64_t state_at, press_until, gap_until, press_bit, focus_since, started;
    uint64_t reading, back_since;   // reading: when the redirect began, 0 none; back_since: the HUD back since then
    char focus[48];
} m;

int quicksave_busy(void) { return m.state != ST_IDLE; }

static void enter(int state, uint64_t now) { m.state = state, m.state_at = now, m.tries = 0; }

static void press(int bit, uint64_t now) {
    m.press_bit = 1ull << bit, m.press_until = now + PRESS_MS, m.gap_until = now + PRESS_MS + GAP_MS;
}

static void fail(const char *why, uint64_t now) {
    rt_log("quickload: %s; leaving the menus", why);
    fs_save_read_from(NULL);
    m.reading = m.back_since = 0;
    menu_toast(UI_QS_LOAD_FAILED, 2500);
    enter(ST_CLEANUP, now);
}

static void end(uint64_t now) {
    gfx_present_hold(0);
    enter(ST_IDLE, now);
}

// Once per pad read on the game thread, after menu_cursor_poll: 1 while quick load runs, with the game's input
// replaced by its presses. Quick load shows "Loading the quick save", then holds the window on that picture
// (gfx_present_hold) while it opens the pause menu, turns to the System tab, presses Load, picks the list's first save
// and confirms; from the confirmation on, the game's reads of a slot's files open the quick save's (fs_save_read_from),
// so whichever slot the list offered first, the game loads the quick save. The picture comes back with the game's
// loading fade, or as soon as a step fails (B then closes the menus).
// The redirect of the slot reads ends with the load: once the game read the quick save's files, its loading fade closed
// and the HUD (PlayerStatus_00) has been back BACK_MS; at once when a menu with the focus box opens (the load list
// must show and load the slots as they are); after READ_MAX_MS whatever happened.
static void reads_end(const char *why) {
    fs_save_read_from(NULL);
    rt_log("quickload: slot reads go to the save again (%s, %d files read from the quick save)", why,
           fs_save_read_from_hits());
    m.reading = m.back_since = 0;
}

static void reads_watch(uint64_t now) {
    char scr[48];
    uint64_t node;
    if (menu_ui_focus(scr, sizeof scr, &node)) { reads_end("a menu opened"); return; }
    if (now - m.reading > READ_MAX_MS) { reads_end("the load did not end in time"); return; }
    int back = fs_save_read_from_hits() > 0 && !menu_ui_open("LoadingFade_00") && menu_ui_open("PlayerStatus_00");
    if (!back) m.back_since = 0;
    else if (!m.back_since) m.back_since = now;
    else if (now - m.back_since >= BACK_MS) reads_end("the load is over");
}

static int poll_locked(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    if (m.reading && m.state == ST_IDLE) reads_watch(plat_now_ms());
    if (m.state == ST_IDLE && __atomic_load_n(&g_request, __ATOMIC_RELAXED) != REQ_LOAD) return 0;   // a plain read
    uint64_t now = plat_now_ms();
    if (m.state == ST_IDLE) {
        if (plat_cas32(&g_request, REQ_NONE, REQ_LOAD) != REQ_LOAD || !quicksave_on()) return 0;
        char data[1200];
        path_of(data, sizeof data, "slot");
        if (plat_fs_kind(data) != PLAT_FS_DIR) { say(UI_QS_NONE, "quickload", "there is no quick save"); return 0; }
        const char *why = not_in_play();
        if (why) { say(UI_QS_NOT_NOW, "quickload: not now", why); return 0; }
        rt_log("quickload: through the game's load menu, under the held picture");
        menu_toast(UI_QS_LOADING, 4000);
        m.started = now;
        enter(ST_TOAST, now);
    }
    plat_cas32(&g_request, REQ_NONE, REQ_LOAD);   // a key pressed while it runs is dropped, not queued
    *buttons = 0, *lx = *ly = *rx = *ry = 0;
    if (now < m.press_until) { *buttons = m.press_bit; return 1; }
    if (now < m.gap_until) return 1;
    char scr[48];
    uint64_t node;
    int focus = menu_ui_focus(scr, sizeof scr, &node);
    if (!focus || strcmp(scr, m.focus)) m.focus_since = now, snprintf(m.focus, sizeof m.focus, "%s", focus ? scr : "");
    int settled = focus && now - m.focus_since >= SETTLE_MS;
    uint64_t waited = now - m.state_at;
    if (m.state != ST_CLEANUP && m.state != ST_LOADING && now - m.started > 4 * STEP_MS) {
        fail("took too long", now);
        return 1;
    }
    switch (m.state) {
    case ST_TOAST:   // the toast reaches the screen; then the picture is held and Plus opens the pause menu
        if (waited < TOAST_MS) break;
        gfx_present_hold(1);
        press(B_PLUS, now);
        enter(ST_OPEN, now);
        break;
    case ST_OPEN:   // the pause menu (Pouch_00) takes the focus box
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
        if (menu_ui_button("PaCommonBtnThin_00", "Pa_Button_01", &ok)) { enter(ST_PICK, now); break; }
        if (m.tries++ >= MAX_TABS) { fail("the System tab was not found", now); break; }
        press(B_R, now);
        m.state_at = now;   // each tab gets its own wait
        break;
    }
    case ST_PICK: {   // Load (Pa_Button_01) takes the focus, then A
        int ok = 0;
        uint64_t b = menu_ui_button("PaCommonBtnThin_00", "Pa_Button_01", &ok);
        if (!b) { fail("the System tab closed", now); break; }
        if (!ok) { fail("the game does not allow loading now", now); break; }
        if (menu_ui_focus_to(b)) { press(B_A, now); enter(ST_LIST, now); }
        else if (waited > STEP_MS) fail("the focus did not move to Load", now);
        break;
    }
    case ST_LIST: {   // the load list (SystemLoadList_00): its first save takes the focus, then A
        if (!settled || strcmp(scr, "SystemLoadList_00")) {
            if (focus && settled && strcmp(scr, "Pouch_00")) fail("another menu opened", now);
            else if (waited > STEP_MS) fail("the list of saves did not open", now);
            break;
        }
        uint64_t b = menu_ui_first_save();
        if (!b) { fail("the list holds no save", now); break; }
        if (menu_ui_focus_to(b)) { press(B_A, now); enter(ST_CONFIRM, now); }
        else if (waited > STEP_MS) fail("the focus did not move to the save", now);
        break;
    }
    case ST_CONFIRM: {   // "Load this save?" (SystemSaveLoad_00): its first button and A, the slot reads redirected
        if (!settled || strcmp(scr, "SystemSaveLoad_00")) {
            if (focus && settled && strcmp(scr, "SystemLoadList_00")) fail("another menu opened", now);
            else if (waited > STEP_MS) fail("the game did not ask to confirm", now);
            break;
        }
        uint64_t b = menu_ui_button("PaCommonBtnThin_00", "Pa_CheckButton_00", NULL);
        if (!b) { fail("the confirmation has no first button", now); break; }
        if (menu_ui_focus_to(b)) {
            char data[1200];
            path_of(data, sizeof data, "slot");
            fs_save_read_from(data);
            m.reading = now, m.back_since = 0;
            press(B_A, now);
            enter(ST_LOADING, now);
        } else if (waited > STEP_MS) fail("the focus did not move to the confirmation", now);
        break;
    }
    case ST_LOADING:   // the game fades out and loads: the picture comes back with its fade
        if (menu_ui_open("LoadingFade_00")) {
            rt_log("quickload: loading the quick save (%.1f s)", (now - m.started) / 1000.0);
            end(now);
        } else if (focus && settled && strcmp(scr, "SystemSaveLoad_00")) {
            fail("another menu opened instead of loading", now);
        }
        else if (waited > LOAD_MS) fail("the load did not start", now);
        break;
    case ST_CLEANUP:   // B until no menu has the focus box; then the picture comes back
        if (!focus) { end(now); break; }
        if (m.tries++ >= MAX_BACK) {
            rt_log("quickload: a menu stays open after %d presses of B", MAX_BACK);
            end(now);
            break;
        }
        press(B_B, now);
        m.gap_until = now + PRESS_MS + 2 * GAP_MS;
        break;
    }
    if (now < m.press_until) *buttons = m.press_bit;
    return 1;
}

int quicksave_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    if (!m.reading && m.state == ST_IDLE && __atomic_load_n(&g_request, __ATOMIC_RELAXED) != REQ_LOAD) return 0;
    if (!plat_mutex_trylock(&qs_lock)) {   // another pad read steps it: this one gets the same presses
        if (m.state == ST_IDLE) return 0;
        *buttons = plat_now_ms() < m.press_until ? m.press_bit : 0, *lx = *ly = *rx = *ry = 0;
        return 1;
    }
    int r = poll_locked(buttons, lx, ly, rx, ry);
    plat_mutex_unlock(&qs_lock);
    return r;
}

static void guest_locked(Cpu *c);

// Once per game pad read (ov_input.c), with the reading thread's Cpu: quick save
void quicksave_guest(Cpu *c) {
    if (!saving && __atomic_load_n(&g_request, __ATOMIC_RELAXED) != REQ_SAVE) return;   // a plain read when idle
    if (!plat_mutex_trylock(&qs_lock)) return;
    guest_locked(c);
    plat_mutex_unlock(&qs_lock);
}

static void guest_locked(Cpu *c) {
    if (!saving && __atomic_load_n(&g_request, __ATOMIC_RELAXED) != REQ_SAVE) return;   // a plain read when idle
    if (saving) {
        save_wait();
        plat_cas32(&g_request, REQ_NONE, REQ_SAVE);   // a key pressed while it saves is dropped, not queued
        return;
    }
    if (plat_cas32(&g_request, REQ_NONE, REQ_SAVE) != REQ_SAVE || !quicksave_on()) return;
    if (m.state != ST_IDLE) return;
    save_start(c);
}
