// Host input for nn::hid (runtime/ov_input.c): an SDL3 gamepad (no SDL window needed), the keyboard and the mouse,
// merged into Switch npad button bits and stick axes. Button order follows libnx HidNpadButton (ISC). The key and pad
// bindings are the tables in input_map.c (totk.ini overrides them). The keyboard and the mouse come from the window
// system's back end (input_wsys.h: input_win.c with Win32, input_sdl.c with SDL), which also owns the cursor capture.
// Keys and their map: docs/controls.md.
// The gamepad also rumbles (input_rumble) and, when it has a gyro and an accelerometer, feeds the six-axis sensor from a
// thread of its own (input_motion_start). Every SDL call on the gamepad is made under `lock`.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "input_wsys.h"
#include "kbm_scheme.h"
#include "screen_watch.h"
#include "nvn_api.h"
#include "ui/menu.h"
#include "vk.h"
#include "platform/platform.h"

enum { STICK_MAX = 32767, STICK_DIR = 16384, TRIGGER_ON = 16384, RESCAN_MS = 1000 };

static PlatMutex lock = PLAT_MUTEX_INIT;
static int state;                    // 0 not started, 1 running, -1 disabled or SDL failed
static SDL_Gamepad *pad;
static uint64_t next_scan;
static int pad_rumble;               // the open gamepad has rumble motors
static volatile int32_t pad_labels[4];  // ... and the labels of its south, east, west, north buttons (PAD_LABEL_*), any thread
static int pad_east_a;               // ... and labels its east face button A (Nintendo layout under TOTK_PAD_LAYOUT=auto)
static int pad_layout = -1;          // PAD_LAYOUT_* in effect for the open gamepad, -1 to pick again; live (cfg_version)
static unsigned pad_layout_seen;
static volatile int32_t pad_layout_shown = -1;   // pad_layout for the menu (any thread), -1 without a gamepad
static SDL_JoystickID sensor_pad;    // the gamepad whose sensors the motion thread set up, 0 none
static int sensor_ok;                // ... and it has both sensors, enabled
static int motion_thread;            // 0 not started, 1 running, -1 off (TOTK_MOTION=0, no SDL)

// SDL buttons of the gamepad inputs (input_map.h PAD_*), by position; PAD_ZL and PAD_ZR are the triggers (axes).
static const SDL_GamepadButton pad_sdl[PAD_COUNT] = {
    [PAD_SOUTH] = SDL_GAMEPAD_BUTTON_SOUTH, [PAD_EAST] = SDL_GAMEPAD_BUTTON_EAST, [PAD_WEST] = SDL_GAMEPAD_BUTTON_WEST,
    [PAD_NORTH] = SDL_GAMEPAD_BUTTON_NORTH, [PAD_LSTICK] = SDL_GAMEPAD_BUTTON_LEFT_STICK,
    [PAD_RSTICK] = SDL_GAMEPAD_BUTTON_RIGHT_STICK, [PAD_L] = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    [PAD_R] = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, [PAD_START] = SDL_GAMEPAD_BUTTON_START, [PAD_BACK] = SDL_GAMEPAD_BUTTON_BACK,
    [PAD_DPAD_LEFT] = SDL_GAMEPAD_BUTTON_DPAD_LEFT, [PAD_DPAD_UP] = SDL_GAMEPAD_BUTTON_DPAD_UP,
    [PAD_DPAD_RIGHT] = SDL_GAMEPAD_BUTTON_DPAD_RIGHT, [PAD_DPAD_DOWN] = SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    [PAD_LPADDLE1] = SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, [PAD_RPADDLE1] = SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,
    [PAD_LPADDLE2] = SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, [PAD_RPADDLE2] = SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,
};

// TOTK_INPUT_SCRIPT=<file>: lines "<frame> <button> [hold]" or "<frame> LSTICK|RSTICK <x> <y> [hold]", ORed into the real input.
// <frame> and its hold are 60 Hz ticks of game time since the first present (frame_step_ticks: presents at 60 fps), so
// the title menus get the presses at the same game time at any frame limit. A line "@<ms> ..." is timed instead: <ms>
// and its hold are milliseconds of the world clock (frame_step_world_us, game time since the loaded save's first
// frame), so it lands at the same game time at any frame rate and load length. A timed event shorter than a frame is
// applied once. In a script with timed lines (a recording) the untimed ones belong before the world clock: they stop
// when it starts, as the load ends at another tick in each run.
// at, hold: ticks, or microseconds of the world clock when timed.
typedef struct { uint64_t at, hold; int bit, stick, timed; int32_t x, y; int logged; char text[48]; } ScriptEv;
static const struct { const char *name; int bit; } script_buttons[] = {
    {"A", B_A}, {"B", B_B}, {"X", B_X}, {"Y", B_Y}, {"LCLICK", B_STICK_L}, {"RCLICK", B_STICK_R}, {"L", B_L}, {"R", B_R},
    {"ZL", B_ZL}, {"ZR", B_ZR}, {"PLUS", B_PLUS}, {"MINUS", B_MINUS}, {"LEFT", B_LEFT}, {"UP", B_UP}, {"RIGHT", B_RIGHT}, {"DOWN", B_DOWN},
};
static ScriptEv *script_ev;
static int script_n, script_state;   // 0 not loaded, 1 loaded
static int script_mixed;             // the script has timed lines: its untimed ones end where the world clock starts

static void script_load(void) {
    script_state = 1;
    const char *path = cfg_get("TOTK_INPUT_SCRIPT");
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) { rt_log("input: cannot open TOTK_INPUT_SCRIPT %s", path); return; }
    char line[256];
    int cap = 0, ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char name[32];
        double at;
        int off = 0, timed = line[0] == '@';
        if (sscanf(line + timed, "%lf %31s%n", &at, name, &off) < 2 || at < 0) continue;
        off += timed;
        double unit = timed ? 1000.0 : 1.0;   // ms to us, or presents
        ScriptEv e = {.at = (uint64_t)(at * unit + 0.5), .hold = timed ? 66667 : 4, .bit = -1, .timed = timed};
        long a, b;
        double c;
        if (!strcmp(name, "LSTICK") || !strcmp(name, "RSTICK")) {
            int n = sscanf(line + off, "%ld %ld %lf", &a, &b, &c);
            if (n < 2) { rt_log("input: script line %d: %s needs x y [hold]", ln, name); continue; }
            e.stick = name[0] == 'L' ? 1 : 2; e.x = (int32_t)a; e.y = (int32_t)b;
            if (n >= 3 && c > 0) e.hold = (uint64_t)(c * unit + 0.5);
            snprintf(e.text, sizeof e.text, "%s %d %d", name, e.x, e.y);
        } else {
            for (size_t i = 0; i < sizeof script_buttons / sizeof *script_buttons; i++)
                if (!strcmp(name, script_buttons[i].name)) e.bit = script_buttons[i].bit;
            if (e.bit < 0) { rt_log("input: script line %d: unknown button %s", ln, name); continue; }
            if (sscanf(line + off, "%lf", &c) == 1 && c > 0) e.hold = (uint64_t)(c * unit + 0.5);
            snprintf(e.text, sizeof e.text, "%s", name);
        }
        if (script_n == cap) {
            cap = cap ? cap * 2 : 32;
            script_ev = realloc(script_ev, (size_t)cap * sizeof *script_ev);
            if (!script_ev) { script_n = 0; fclose(f); return; }
        }
        script_ev[script_n++] = e;
        script_mixed |= timed;
    }
    fclose(f);
    rt_log("input: script %s, %d events", path, script_n);
}

static void script_apply(uint64_t *b, int32_t sl[2], int32_t sr[2]) {
    if (!script_state) script_load();
    if (!script_n) return;
    uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), tick = frame_step_ticks();
    int64_t w = frame_step_world_us();
    for (int i = 0; i < script_n; i++) {
        ScriptEv *e = &script_ev[i];
        if (e->timed ? w < 0 : script_mixed && w >= 0) continue;
        uint64_t now = e->timed ? (uint64_t)w : tick;
        if (now < e->at || (now >= e->at + e->hold && (e->logged || !e->timed))) continue;
        if (!e->logged) {
            e->logged = 1;
            if (e->timed) rt_log("input: script frame %llu at %.1f ms: %s", (unsigned long long)fr, w / 1000.0, e->text);
            else rt_log("input: script frame %llu tick %llu: %s", (unsigned long long)fr, (unsigned long long)tick, e->text);
        }
        if (e->stick) { int32_t *s = e->stick == 1 ? sl : sr; s[0] = e->x; s[1] = e->y; }
        else *b |= 1ull << e->bit;
    }
}

// TOTK_INPUT_RECORD=<file>: writes the merged input as TOTK_INPUT_SCRIPT lines, one per button press and per stick
// segment, so a played session replays as a script. A stick segment ends when an axis moves by more than REC_STICK_STEP.
// An event that begins on the world clock is written timed ("@<ms>", hold in ms), one before it in ticks
// (frame_step_ticks). When the world clock starts, held buttons and sticks are cut there and go on as timed events.
enum { REC_STICK_STEP = 2048 };
static FILE *rec;
static int rec_state;                // 0 not opened, 1 recording, -1 off
static uint64_t rec_down[16];        // tick + 1 at which each button went down, 0 while up
static int64_t rec_down_w[16];       // the world clock then, -1 before it started
static uint64_t rec_stick_from[2];   // tick + 1 at which the current non-zero stick segment began
static int64_t rec_stick_w[2];
static int32_t rec_stick[2][2];
static int64_t rec_last_w = -1;      // the world clock at the previous apply

// "<tick> <what> <hold>" or "@<ms> <what> <hold ms>": from tick from and world clock from_w to now.
static void rec_line(uint64_t from, int64_t from_w, uint64_t fr, int64_t w, const char *what) {
    if (from_w >= 0 && w >= from_w)
        fprintf(rec, "@%.1f %s %.1f\n", from_w / 1000.0, what, (w > from_w ? w - from_w : 1000) / 1000.0);
    else
        fprintf(rec, "%llu %s %llu\n", (unsigned long long)from, what, (unsigned long long)(fr > from ? fr - from : 1));
}

static void rec_close_stick(int s, uint64_t fr, int64_t w) {
    if (!rec_stick_from[s]) return;
    char what[48];
    snprintf(what, sizeof what, "%s %d %d", s ? "RSTICK" : "LSTICK", rec_stick[s][0], rec_stick[s][1]);
    rec_line(rec_stick_from[s] - 1, rec_stick_w[s], fr, w, what);
    rec_stick_from[s] = 0;
}

static void rec_flush_all(void) {
    if (rec_state != 1) return;
    uint64_t fr = frame_step_ticks() + 1;
    int64_t w = frame_step_world_us();
    for (int i = 0; i < 16; i++)
        if (rec_down[i]) { rec_line(rec_down[i] - 1, rec_down_w[i], fr, w, script_buttons[i].name); rec_down[i] = 0; }
    rec_close_stick(0, fr, w);
    rec_close_stick(1, fr, w);
    fflush(rec);
}

static void rec_apply(uint64_t b, const int32_t sl[2], const int32_t sr[2]) {
    if (!rec_state) {
        const char *path = cfg_get("TOTK_INPUT_RECORD");   // no default: not recording when unset
        rec = path && *path ? fopen(path, "w") : NULL;
        rec_state = rec ? 1 : -1;
        if (path && *path && !rec) rt_log("input: cannot open TOTK_INPUT_RECORD %s", path);
        if (rec) { fprintf(rec, "# Recorded input (TOTK_INPUT_RECORD); frame = 60 Hz ticks of game time, @ms = world clock.\n"); atexit(rec_flush_all);
                   rt_log("input: recording to %s", path); }
    }
    if (rec_state != 1) return;
    uint64_t fr = frame_step_ticks();
    int64_t w = frame_step_world_us();
    int wrote = 0;
    if (w >= 0 && rec_last_w < 0) {   // the world clock starts: what is held goes on as timed events from @0
        for (int i = 0; i < 16; i++)
            if (rec_down[i]) {
                rec_line(rec_down[i] - 1, -1, fr, -1, script_buttons[i].name);
                rec_down[i] = fr + 1, rec_down_w[i] = w;
                wrote = 1;
            }
        for (int s = 0; s < 2; s++)
            if (rec_stick_from[s]) {
                rec_close_stick(s, fr, -1);
                rec_stick_from[s] = fr + 1, rec_stick_w[s] = w;
                wrote = 1;
            }
    }
    rec_last_w = w;
    for (int i = 0; i < 16; i++) {
        int bit = script_buttons[i].bit, down = (b >> bit & 1) != 0;
        if (down && !rec_down[i]) rec_down[i] = fr + 1, rec_down_w[i] = w;
        else if (!down && rec_down[i]) {
            rec_line(rec_down[i] - 1, rec_down_w[i], fr, w, script_buttons[i].name);
            rec_down[i] = 0;
            wrote = 1;
        }
    }
    const int32_t *st[2] = {sl, sr};
    for (int s = 0; s < 2; s++) {
        int moved = abs(st[s][0] - rec_stick[s][0]) > REC_STICK_STEP || abs(st[s][1] - rec_stick[s][1]) > REC_STICK_STEP ||
                    ((st[s][0] || st[s][1]) != (rec_stick[s][0] || rec_stick[s][1]));
        if (!moved) continue;
        if (rec_stick_from[s]) { rec_close_stick(s, fr, w); wrote = 1; }
        rec_stick[s][0] = st[s][0]; rec_stick[s][1] = st[s][1];
        if (st[s][0] || st[s][1]) rec_stick_from[s] = fr + 1, rec_stick_w[s] = w;
    }
    if (wrote) fflush(rec);
}

static float mouse_sens = -1;              // TOTK_MOUSE_SENS, 0 = mouse camera off; live (cfg_version)
static unsigned mouse_sens_seen;

static float mouse_sens_get(void) {
    unsigned ver = cfg_version();
    if (mouse_sens >= 0 && ver == mouse_sens_seen) return mouse_sens;
    mouse_sens_seen = ver;
    double v = cfg_double("TOTK_MOUSE_SENS");
    mouse_sens = (float)(v < 0 ? 0 : v > 10 ? 10 : v);
    return mouse_sens;
}

// TOTK_MOUSE_AIM_SCALE: the mouse camera's sensitivity while ZR (the bow) is held, 1 = unchanged.
static float mouse_aim_scale = -1;
static unsigned mouse_aim_seen;
static float mouse_aim_get(void) {
    unsigned ver = cfg_version();
    if (mouse_aim_scale >= 0 && ver == mouse_aim_seen) return mouse_aim_scale;
    mouse_aim_seen = ver;
    double v = cfg_double("TOTK_MOUSE_AIM_SCALE");
    if (!(v > 0)) v = 1;
    mouse_aim_scale = (float)(v < 0.1 ? 0.1 : v > 3 ? 3 : v);
    return mouse_aim_scale;
}

// ---- walk key: the keyboard's stick samples scaled down while a key is held (TOTK_WALK_KEY, TOTK_WALK_SCALE)
// A keyboard key is all or nothing, so every keyboard-driven stick sample is +/-32767 and the game only ever sees a
// full tilt. Holding this key scales those two samples, which gives the half tilt a gamepad gets from its analog range,
// so the keyboard can walk as well as run. yuzu and eden do the same with their modifier and modifier_scale settings.
// Only the keyboard is scaled: the gamepad's axes and the mouse camera sample their own values.
static int walk_key = KEY_NONE;        // TOTK_WALK_KEY as a KEY_*, KEY_NONE = off; live (cfg_version)
static float walk_scale = 0.51f;       // TOTK_WALK_SCALE, 0.2 to 0.9; live (cfg_version)
static unsigned walk_seen = ~0u;

// The ini takes the key names the Controls tab writes; a hand-written line may spell them in any case.
static int name_matches(const char *name, const char *key) {
    for (; *name && *key; name++, key++) {
        unsigned char a = (unsigned char)*name, b = (unsigned char)*key;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return *name == *key;
}

static void walk_get(void) {
    unsigned ver = cfg_version();
    if (ver == walk_seen) return;
    walk_seen = ver;
    const char *name = cfg_value("TOTK_WALK_KEY");   // "none" (the default) turns the key off
    int key = KEY_NONE;
    if (*name && !name_matches(name, "none")) {
        for (int k = KEY_A; k < KEY_COUNT && key == KEY_NONE; k++)
            if (input_key_name(k) && name_matches(name, input_key_name(k))) key = k;
        if (key == KEY_NONE) rt_log("input: walk_key: unknown key '%s', the keyboard sticks keep their full tilt", name);
    }
    // 0.51 rather than 0.5: a keyboard sample is 32767, and 32767 * 0.5 = 16383 falls one short of stick_bits'
    // STICK_DIR (16384), which would leave the game's stick-direction bits clear while the walk key is held. The value
    // in the settings table carries that default; this code only rejects what the table's range does not allow.
    const char *sv = cfg_value("TOTK_WALK_SCALE");
    char *end;
    double d = strtod(sv, &end);
    double s = end == sv || *end || !(d >= 0.2 && d <= 0.9) ? strtod(cfg_meta("TOTK_WALK_SCALE")->def, NULL) : d;
    if (key != walk_key || (float)s != walk_scale) {
        walk_key = key;
        walk_scale = (float)s;
        rt_log("input: walk key %s, keyboard sticks at %.0f%% of their tilt (mouse and gamepad unchanged)",
               key != KEY_NONE ? input_key_name(key) : "off", s * 100);
    }
}

// ---- the PC scheme (TOTK_KBM_SCHEME): the keyboard and the mouse routed by kbm_scheme.c's context table
// (ported from Weeping of the Empire) instead of the flat table in input_map.c. Off by default: the Controls tab
// and the TOTK_BIND_* lines edit input_map.c's table, which the scheme does not read.
static int kbm_scheme_on = -1;             // -1 unknown, 0 off, 1 on; live (cfg_version)
static unsigned kbm_scheme_seen;

static int kbm_on(void) {
    unsigned ver = cfg_version();
    if (kbm_scheme_on >= 0 && ver == kbm_scheme_seen) return kbm_scheme_on;
    kbm_scheme_seen = ver;
    const char *v = cfg_value("TOTK_KBM_SCHEME");
    int on = *v && !name_matches(v, "0") && !name_matches(v, "off") && !name_matches(v, "none");
    // kbm_binds is live: every cfg change reparses the rows, whether the switch flipped or not
    if (on) kbm_scheme_set_binds(cfg_get("TOTK_KBM_BINDS"));
    if (on != kbm_scheme_on) {
        if (on) {
            kbm_scheme_init();
            rt_log("input: PC scheme on: %d rows, %d unknown keys, Esc is the game's menu key, "
                   "the Controls tab and TOTK_BIND_* do not apply",
                   kbm_scheme_row_count(), kbm_scheme_unknown_key_count());
        } else if (kbm_scheme_on == 1) {
            rt_log("input: PC scheme off, the bindings of docs/controls.md are back");
        }
    }
    kbm_scheme_on = on;
    return on;
}

// The same switch for the window thread (ui/menu_win.cpp, ui/menu_sdl.cpp, input_win.c, input_sdl.c): while the
// scheme runs, Esc is the game's key, so only F1 opens the host menu and Esc leaves the cursor alone.
int input_kbm_scheme(void) {
    const char *v = cfg_value("TOTK_KBM_SCHEME");
    return *v && !name_matches(v, "0") && !name_matches(v, "off") && !name_matches(v, "none");
}

// TOTK_CAMLOG=<ms>: the mouse camera's raw counts and the stick it produced, for chasing input feel.
static void camlog(int32_t dx, int32_t dy, const int32_t held[2], int cap, int path, int aim) {
    static uint64_t last;
    int ms = cfg_int("TOTK_CAMLOG");
    if (ms <= 0) return;
    uint64_t now = plat_now_ns();
    if (last && now - last < (uint64_t)ms * 1000000ull) return;
    last = now;
    rt_log("input: camlog dx=%d dy=%d stick=%d,%d captured=%d path=%d aim=%d", dx, dy, held[0], held[1], cap, path, aim);
}

// The ability wheel (L held) picks a segment by where the right stick points and keeps the pick while the stick is
// held. The mouse sample is a velocity (input_mouse_stick): it is back to the middle the moment the mouse stops, so
// the wheel's pick slid back to the centre -- "it sticks to the middle". While L is held the counts push a virtual
// stick instead that stays where it was pushed until the wheel closes, so the pick stays under the pointer.
enum { MOUSE_WHEEL_COUNTS = 220 };   // counts for a full deflection (a nudge back to the middle centres it)
static int32_t wheel_stick[2];

// Direct mouse look (TOTK_MOUSE_DIRECT, the `mousedirect` set in recomp/patches-v143.txt): the game's stick step adds
// (total - applied) * k to the processed right stick after its dead zone and curve, so the mouse skips both. The patch
// page holds 32-bit running totals and the totals the game applied last: +0x100/+0x104 total x/y, +0x108/+0x10c
// applied x/y, +0x110 the stick units per count. The runtime drops motion the game did not apply for a while (menus,
// loading, cutscenes) instead of saving it up for the first step after them.
enum { MD_TOTAL_X = 0x100, MD_TOTAL_Y = 0x104, MD_APPLIED_X = 0x108, MD_APPLIED_Y = 0x10c, MD_K = 0x110,
       MD_PITCH = 0x114 };
enum { MD_STALE_PRESENTS = 8 };       // steps skipped before the saved motion is dropped
static uint32_t md_seen[2];
static int md_stale;
static int md_was_on;
static int md_logged;

static int mouse_direct_get(void) {
    static int on = -1;
    static unsigned seen;
    unsigned ver = cfg_version();
    if (on < 0 || ver != seen) {
        seen = ver;
        on = cfg_bool("TOTK_MOUSE_DIRECT");
        int set = rt_patch_set("mousedirect");
        if (set >= 0) rt_patch_switch(set, on);
    }
    return on;
}

static float mouse_direct_sens_get(void) {
    static float sens = -1;
    static unsigned seen;
    unsigned ver = cfg_version();
    if (sens < 0 || ver != seen) {
        seen = ver;
        double v = cfg_double("TOTK_MOUSE_DIRECT_SENS");
        if (!(v > 0)) v = 1;
        sens = (float)(v < 0.05 ? 0.05 : v > 10 ? 10 : v);
    }
    return sens;
}

// The game turns pitch by 51 y |y| and yaw by 70 x, so the direct path shapes the mouse's pitch by
// sign(v) sqrt(g |v|), g = 70/51: the pitch per count then matches yaw. The patch reads g from the page.
static float mouse_direct_pitch_get(void) {
    static float gain = -1;
    static unsigned seen;
    unsigned ver = cfg_version();
    if (gain < 0 || ver != seen) {
        seen = ver;
        double v = cfg_double("TOTK_MOUSE_DIRECT_PITCH");
        if (!(v > 0)) v = 70.0 / 51.0;
        gain = (float)(v < 0.2 ? 0.2 : v > 4 ? 4 : v);
    }
    return gain;
}

static uint32_t md_load(uint8_t *page, unsigned off) {
    return __atomic_load_n((uint32_t *)(void *)(page + off), __ATOMIC_RELAXED);
}

static void md_store(uint8_t *page, unsigned off, uint32_t v) {
    __atomic_store_n((uint32_t *)(void *)(page + off), v, __ATOMIC_RELAXED);
}

// The game did not run the stick step (menus, loading, wheel, lost capture): take its applied totals up to now, so the
// first step after it applies no motion from before.
static void mouse_direct_sync(void) {
    uint8_t *page = rt_patch_page();
    if (!page) return;
    uint32_t x = md_load(page, MD_TOTAL_X), y = md_load(page, MD_TOTAL_Y);
    md_store(page, MD_APPLIED_X, x);
    md_store(page, MD_APPLIED_Y, y);
    md_seen[0] = x;
    md_seen[1] = y;
    md_stale = 0;
    float zero = 0;
    memcpy(page + MD_K, &zero, sizeof zero);
}

static void mouse_direct_frame(int32_t dx, int32_t dy, int aim) {
    uint8_t *page = rt_patch_page();
    if (!page) return;
    uint32_t tx = md_load(page, MD_TOTAL_X) + (uint32_t)dx;
    uint32_t ty = md_load(page, MD_TOTAL_Y) + (uint32_t)dy;
    md_store(page, MD_TOTAL_X, tx);
    md_store(page, MD_TOTAL_Y, ty);
    uint32_t ax = md_load(page, MD_APPLIED_X), ay = md_load(page, MD_APPLIED_Y);
    if (ax != md_seen[0] || ay != md_seen[1]) {
        if (!md_logged) {
            rt_log("input: direct mouse look: the game applied the mouse");
            md_logged = 1;
        }
        md_seen[0] = ax;
        md_seen[1] = ay;
        md_stale = 0;
    } else if (++md_stale > MD_STALE_PRESENTS) {
        md_store(page, MD_APPLIED_X, tx);
        md_store(page, MD_APPLIED_Y, ty);
        md_seen[0] = tx;
        md_seen[1] = ty;
        md_stale = 0;
    }
    float ratio;
    memcpy(&ratio, page, sizeof ratio);   // page +0: the 30 Hz ratio (frame_rate.c)
    if (!(ratio > 0.1f) || ratio > 10) ratio = 1;
    float k = 0.01f * mouse_sens_get() * (aim ? mouse_aim_get() : 1.0f) * mouse_direct_sens_get() / ratio;
    memcpy(page + MD_K, &k, sizeof k);
    float gain = mouse_direct_pitch_get();
    memcpy(page + MD_PITCH, &gain, sizeof gain);
}

// Hold-to-tilt: the mouse feeds the virtual controller's six-axis for the game's motion mechanics (the gyro shrines)
// while TOTK_MOUSE_MOTION_KEY is held (default k; none off). The direct mouse look is paused then, so the mouse does
// not turn the view as well. TOTK_MOUSE_MOTION_SPEED scales the tilt.
static double mm_R[3][3];
static int mm_live;
static uint64_t mm_last_ns;

static int mouse_motion_key_get(void) {
    static int key = -2;
    static unsigned seen;
    unsigned ver = cfg_version();
    if (key == -2 || ver != seen) {
        seen = ver;
        key = KEY_NONE;
        const char *name = cfg_value("TOTK_MOUSE_MOTION_KEY");
        if (name && *name && strcasecmp(name, "none") && strcasecmp(name, "off"))
            for (int k = KEY_A; k < KEY_COUNT; k++)
                if (input_key_name(k) && !strcasecmp(input_key_name(k), name)) { key = k; break; }
    }
    return key;
}

static double mouse_motion_speed_get(void) {
    static double speed = -1;
    static unsigned seen;
    unsigned ver = cfg_version();
    if (speed < 0 || ver != seen) {
        seen = ver;
        double v = cfg_double("TOTK_MOUSE_MOTION_SPEED");
        if (!(v > 0)) v = 1;
        speed = v < 0.05 ? 0.05 : v > 4 ? 4 : v;
    }
    return speed;
}

static int real_imu_live(void) {
    return pad && SDL_GamepadConnected(pad) && SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) &&
           SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL);
}

static int mouse_motion_held(void) {
    int key = mouse_motion_key_get();
    return key != KEY_NONE && ikb_down(key) && !real_imu_live();
}

static void mouse_motion_stop(void) {
    if (!mm_live) return;
    mm_live = 0;
    rt_log("input: mouse motion feed off");
}

static void mouse_motion_feed(int32_t dx, int32_t dy) {
    uint64_t now = plat_now_ns();
    double dt = mm_live && now > mm_last_ns ? (double)(now - mm_last_ns) / 1e9 : 0.004;
    if (!(dt > 0) || dt > 0.05) dt = 0.004;
    mm_last_ns = now;
    if (!mm_live) {
        memset(mm_R, 0, sizeof mm_R);
        mm_R[0][0] = mm_R[1][1] = mm_R[2][2] = 1;
        mm_live = 1;
        rt_log("input: mouse motion feed on (%.2f speed)", mouse_motion_speed_get());
    }
    double k = 0.002 * mouse_motion_speed_get();
    double w[3] = {-dy * k / dt, 0, -dx * k / dt};   // right = yaw right (-z), down = pitch down (-x)
    for (int r = 0; r < 3; r++) {
        double *v = mm_R[r];
        double cr[3] = {w[1] * v[2] - w[2] * v[1], w[2] * v[0] - w[0] * v[2], w[0] * v[1] - w[1] * v[0]};
        for (int i = 0; i < 3; i++) v[i] -= cr[i] * dt;
        double n = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (n > 0)
            for (int i = 0; i < 3; i++) v[i] /= n;
    }
    double acc[3] = {-mm_R[2][0], -mm_R[2][1], -mm_R[2][2]};   // gravity, read as -up (face up: z = -1 g)
    motion_feed(motion_global(), acc, w, now);
}

// The mouse camera as a right-stick sample, held for the whole presented frame. Under `lock`. The sample is divided by
// the camera speed factor the game runs with (camera_speed.c, cam_speed_applied), which multiplies every right-stick
// turn rate, so TOTK_MOUSE_SENS alone sets the mouse's speed; below 1 the sample stays within the stick's range, so the
// mouse's top speed drops with the factor.
static void mouse_camera(int32_t out[2], int aim, int wheel) {
    static uint64_t frame = ~0ull, t0;
    static int32_t held[2];
    uint64_t now = plat_now_ns();
    int32_t dx = 0, dy = 0;
    int flood = cfg_int("TOTK_MOUSE_FLOOD");
    int cap = ikb_captured();
    int direct = mouse_direct_get();
    if (md_was_on && !direct) mouse_direct_sync();
    md_was_on = direct;
    int motion = mouse_motion_held();
    if (!motion && mm_live) mouse_motion_stop();
    if (motion && cap) {
        uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        if (fr != frame) {
            ikb_mouse_take(&dx, &dy);
            mouse_motion_feed(dx, dy);
            frame = fr;
            t0 = now;
        }
        held[0] = held[1] = 0;
        out[0] = out[1] = 0;
        if (direct) mouse_direct_sync();
        camlog(dx, dy, held, cap, 3, aim);
        return;
    }
    if (wheel && cap) {
        if (direct) mouse_direct_sync();   // the wheel consumes the counts; keep its pick on the stick
        uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        if (fr != frame) {
            ikb_mouse_take(&dx, &dy);
            double x = wheel_stick[0] / (double)STICK_MAX + dx / (double)MOUSE_WHEEL_COUNTS;
            double y = wheel_stick[1] / (double)STICK_MAX - dy / (double)MOUSE_WHEEL_COUNTS;
            double len = sqrt(x * x + y * y);
            if (len > 1) x /= len, y /= len;
            if (x * x + y * y < 0.02) x = y = 0;   // back over the middle: the wheel shows its centre again
            wheel_stick[0] = (int32_t)lround(x * STICK_MAX);
            wheel_stick[1] = (int32_t)lround(y * STICK_MAX);
            frame = fr;
            t0 = now;
        }
        out[0] = wheel_stick[0], out[1] = wheel_stick[1];
        camlog(dx, dy, wheel_stick, cap, 0, aim);
        return;
    }
    wheel_stick[0] = wheel_stick[1] = 0;
    if (!cap) {
        ikb_mouse_take(&dx, &dy);
        if (direct) mouse_direct_sync();
        mouse_motion_stop();
        held[0] = held[1] = 0;
        frame = ~0ull;
        camlog(dx, dy, held, cap, 0, aim);
    } else {
        uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        double age = frame == ~0ull ? 0 : (double)(now - t0) / 1e9;
        if (fr != frame) {
            double dt = age < 0.004 ? 0.004 : age > 0.1 ? 0.1 : age;
            ikb_mouse_take(&dx, &dy);
            if (flood > 0) dx += flood;
            if (direct) {
                mouse_direct_frame(dx, dy, aim);
                held[0] = held[1] = 0;
                frame = fr;
                t0 = now;
                out[0] = out[1] = 0;
                camlog(dx, dy, held, cap, 2, aim);
                return;
            }
            float sens = mouse_sens_get() * (aim ? mouse_aim_get() : 1.0f);
            input_mouse_stick(dx, dy, dt, sens, held);
            frame = fr;
            t0 = now;
        } else if (age > 0.1) {
            held[0] = held[1] = 0;   // frames stopped: do not hold a deflection
        }
    }
    double k = cam_speed_applied(), x = held[0] / k, y = held[1] / k, len = sqrt(x * x + y * y);
    if (len > STICK_MAX) x *= STICK_MAX / len, y *= STICK_MAX / len;
    out[0] = (int32_t)lround(x);
    out[1] = (int32_t)lround(y);
    camlog(dx, dy, held, cap, 0, aim);
}

static float stick_dead[2] = {-1, -1};     // input_pad_deadzone of the left and right stick; live (cfg_version)
static unsigned stick_dead_seen;

static float stick_dead_get(int s) {
    unsigned ver = cfg_version();
    if (stick_dead[0] < 0 || ver != stick_dead_seen) {
        stick_dead_seen = ver;
        stick_dead[0] = input_pad_deadzone(0);
        stick_dead[1] = input_pad_deadzone(1);
    }
    return stick_dead[s];
}

// One gamepad stick through the dead zone; SDL's Y grows downwards, the Switch's upwards.
static void stick(SDL_GamepadAxis ax, SDL_GamepadAxis ay, float dead, int32_t out[2]) {
    input_pad_stick(SDL_GetGamepadAxis(pad, ax), -(int32_t)SDL_GetGamepadAxis(pad, ay), dead, out);
}

// Open the first gamepad when none is open or the open one went away; logs once per change.
static void scan_pads(void) {
    if (pad && SDL_GamepadConnected(pad)) return;
    if (pad) {
        SDL_CloseGamepad(pad); pad = NULL; sensor_pad = 0; sensor_ok = 0;
        plat_xchg32(&pad_layout_shown, -1);
        for (int i = 0; i < 4; i++) plat_xchg32(&pad_labels[i], PAD_LABEL_NONE);
        rt_log("input: gamepad disconnected");
    }
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    for (int i = 0; ids && i < n && !pad; i++) pad = SDL_OpenGamepad(ids[i]);
    SDL_free(ids);
    if (!pad) return;
    pad_rumble = SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
    pad_east_a = SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_EAST) == SDL_GAMEPAD_BUTTON_LABEL_A;
    for (int i = 0; i < 4; i++) {
        SDL_GamepadButtonLabel lb = SDL_GetGamepadButtonLabel(pad, pad_sdl[PAD_SOUTH + i]);
        plat_xchg32(&pad_labels[i], lb == SDL_GAMEPAD_BUTTON_LABEL_A          ? PAD_LABEL_A
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_B        ? PAD_LABEL_B
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_X        ? PAD_LABEL_X
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_Y        ? PAD_LABEL_Y
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_CROSS    ? PAD_LABEL_CROSS
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_CIRCLE   ? PAD_LABEL_CIRCLE
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_SQUARE   ? PAD_LABEL_SQUARE
                                            : lb == SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE ? PAD_LABEL_TRIANGLE
                                                                                      : PAD_LABEL_NONE);
    }
    pad_layout = -1;
    int motion = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL);
    rt_log("input: gamepad \"%s\" (%s, %s, east button %s)", SDL_GetGamepadName(pad), pad_rumble ? "rumble" : "no rumble",
           motion ? "gyro and accelerometer" : "no motion sensors", pad_east_a ? "labelled A" : "not labelled A");
}

// Under lock: the layout of the open gamepad, picked again when a setting or the gamepad changed; logs each change.
static int pad_layout_get(void) {
    unsigned ver = cfg_version();
    if (pad_layout >= 0 && ver == pad_layout_seen) return pad_layout;
    pad_layout_seen = ver;
    int was = pad_layout;
    const char *set = cfg_value("TOTK_PAD_LAYOUT");
    pad_layout = input_pad_layout_pick(set, pad_east_a);
    if (pad_layout != was)
        rt_log("input: gamepad layout %s (pad_layout = %s)", pad_layout == PAD_LAYOUT_XBOX ? "xbox" : "nintendo", set);
    plat_xchg32(&pad_layout_shown, pad_layout);
    return pad_layout;
}

// The TOTK_BIND_* values last applied, one per line; the tables are built again when one changes (the menu's remapping rows write
// them while the game runs). Under lock.
static unsigned binds_seen = ~0u;
static char binds_applied[4096];

static void binds_refresh(void) {
    unsigned v = cfg_version();
    if (v == binds_seen) return;
    binds_seen = v;
    char now[sizeof binds_applied];
    size_t len = 0;
    for (int a = 0; a < ACTION_COUNT; a++) {
        char var[32];
        snprintf(var, sizeof var, "TOTK_BIND_%s", input_action_name(a));
        const char *s = cfg_get(var);
        len += (size_t)snprintf(now + len, len < sizeof now ? sizeof now - len : 0, "%s\n", s ? s : "");
    }
    if (!strcmp(now, binds_applied)) return;
    memcpy(binds_applied, now, sizeof now);
    input_map_configure();
}

// TOTK_TEST_PAD=<lx>,<ly>,<rx>,<ry> (test runs): an SDL virtual gamepad whose sticks rest at these raw SDL axis values
// (-32768..32767, down is +y), attached before the first scan so it is the gamepad the game reads: drift and full
// deflection go through the same mapping and dead zone as a real pad.
static void test_pad_attach(void) {
    static SDL_Joystick *joy;   // kept open: a virtual joystick's axes are set through an open handle
    const char *e = cfg_get("TOTK_TEST_PAD");
    int v[4];
    if (!e || !*e) return;
    if (sscanf(e, "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]) != 4) {
        rt_log("input: TOTK_TEST_PAD=%s is not <lx>,<ly>,<rx>,<ry>; no test gamepad", e);
        return;
    }
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.name = "TotK test gamepad";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    joy = id ? SDL_OpenJoystick(id) : NULL;
    if (!joy) {
        rt_log("input: test gamepad not attached (%s)", SDL_GetError());
        return;
    }
    static const SDL_GamepadAxis axes[4] = {SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, SDL_GAMEPAD_AXIS_RIGHTX,
                                            SDL_GAMEPAD_AXIS_RIGHTY};
    for (int i = 0; i < 4; i++)
        SDL_SetJoystickVirtualAxis(joy, axes[i], (Sint16)(v[i] < -32768 ? -32768 : v[i] > 32767 ? 32767 : v[i]));
    rt_log("input: test gamepad attached, sticks at %d,%d %d,%d", v[0], v[1], v[2], v[3]);
}

static void start(void) {
    binds_refresh();
    const char *e = cfg_get("TOTK_INPUT");
    if (e && !strcmp(e, "none")) { state = -1; rt_log("input: disabled (TOTK_INPUT=none)"); return; }
    if (!SDL_Init(SDL_INIT_GAMEPAD)) { state = -1; rt_log("input: SDL gamepad init failed (%s), keyboard only", SDL_GetError()); }
    else state = 1;
    if (state == 1) test_pad_attach();
    if (state == 1) {
        scan_pads();
        if (!pad) rt_log("input: no gamepad (keyboard only until one is connected)");
    }
    if (state == -1) state = 2;   // keyboard only
}

static void stick_bits(uint64_t *b, int32_t x, int32_t y, int first) {
    if (x < -STICK_DIR) *b |= 1ull << (first + 0);
    if (y > STICK_DIR) *b |= 1ull << (first + 1);
    if (x > STICK_DIR) *b |= 1ull << (first + 2);
    if (y < -STICK_DIR) *b |= 1ull << (first + 3);
}

static void pick(int32_t *x, int32_t *y, int32_t kx, int32_t ky) {
    int64_t a = (int64_t)*x * *x + (int64_t)*y * *y, b = (int64_t)kx * kx + (int64_t)ky * ky;
    if (b > a) { *x = kx; *y = ky; }
}

// Under lock, state 1: the open gamepad's inputs by position (pd[PAD_*] != 0 held); 0 without a connected gamepad.
static int pad_physical(uint8_t pd[PAD_COUNT]) {
    if ((!pad || !SDL_GamepadConnected(pad)) && plat_now_ns() / 1000000u >= next_scan) { next_scan = plat_now_ns() / 1000000u + RESCAN_MS; scan_pads(); }
    SDL_UpdateGamepads();
    if (!pad || !SDL_GamepadConnected(pad)) return 0;
    for (int p = PAD_SOUTH; p < PAD_ZL; p++) pd[p] = SDL_GetGamepadButton(pad, pad_sdl[p]);
    pd[PAD_ZL] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TRIGGER_ON;
    pd[PAD_ZR] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER_ON;
    for (int p = PAD_ZR + 1; p < PAD_COUNT; p++) pd[p] = SDL_GetGamepadButton(pad, pad_sdl[p]);   // false without paddles
    return 1;
}

// Under lock, state 1: the open gamepad's buttons (Switch bits after TOTK_PAD_LAYOUT and the TOTK_BIND_* lines) and
// sticks; 0 without a connected gamepad.
static int pad_read(uint64_t *b, int32_t sl[2], int32_t sr[2]) {
    uint8_t pd[PAD_COUNT] = {0};
    if (!pad_physical(pd)) return 0;
    *b |= input_map_pad_buttons(pd, pad_layout_get());
    stick(SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, stick_dead_get(0), sl);
    stick(SDL_GAMEPAD_AXIS_RIGHTX, SDL_GAMEPAD_AXIS_RIGHTY, stick_dead_get(1), sr);
    return 1;
}

int input_pad_stick_percent(double pct[2]) {
    int ok = 0;
    pct[0] = pct[1] = 0;
    plat_mutex_lock(&lock);
    if (state == 1 && pad && SDL_GamepadConnected(pad)) {   // the game's poll keeps the state fresh (SDL_UpdateGamepads)
        pct[0] = input_stick_percent(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX),
                                     SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
        pct[1] = input_stick_percent(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX),
                                     SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
        ok = 1;
    }
    plat_mutex_unlock(&lock);
    return ok;
}

// TOTK_KEY_MIN_HOLD_MS: a movement key released still reads held for this long (Weeping's key_min_hold_ms). Without
// it a fast keyboard tap at 144 fps can stop and restart the walk between the game's own samples.
static uint8_t move_key[KEY_COUNT];
static unsigned move_seen = ~0u;
static void movement_key_flags(void) {
    unsigned v = cfg_version();
    if (v == move_seen) return;
    move_seen = v;
    memset(move_key, 0, sizeof move_key);
    move_key[KEY_W] = move_key[KEY_A] = move_key[KEY_S] = move_key[KEY_D] = 1;   // the PC scheme's movement rows
    static const int acts[4] = {B_SL_LEFT, B_SL_UP, B_SL_RIGHT, B_SL_DOWN};
    uint8_t list[INPUT_BINDING_MAX];
    for (int i = 0; i < 4; i++) {
        int n = input_map_keys_of(&input_map, acts[i], list, INPUT_BINDING_MAX);
        for (int j = 0; j < n; j++)
            if (list[j] > KEY_NONE && list[j] < KEY_COUNT) move_key[list[j]] = 1;
    }
}

static void key_min_hold(uint8_t down[KEY_COUNT]) {
    static uint8_t prev[KEY_COUNT];
    static uint64_t until[KEY_COUNT];
    int ms = cfg_int("TOTK_KEY_MIN_HOLD_MS");
    if (ms < 0) ms = 0;
    if (ms > 1000) ms = 1000;
    if (!ms) {
        memcpy(prev, down, sizeof prev);
        memset(until, 0, sizeof until);
        return;
    }
    movement_key_flags();
    uint64_t now = plat_now_ns() / 1000000u;
    for (int k = 1; k < KEY_COUNT; k++) {
        if (!move_key[k]) { prev[k] = 0; continue; }
        if (down[k]) {
            prev[k] = 1;
            until[k] = 0;
        } else {
            if (prev[k]) { prev[k] = 0; until[k] = now + (uint64_t)ms; }
            if (until[k] && now < until[k]) down[k] = 1;
            else until[k] = 0;
        }
    }
}

// TOTK_SPRINT_TOGGLE / TOTK_LOCK_TOGGLE: the key's own bits become a latch (never the pad's). Sprint drops 300 ms after
// movement stops; the menu clears both so a toggled key cannot leak into it.
static void input_toggles(uint64_t *keys, const int32_t sl[2], int menu_open) {
    static uint64_t prev;
    static int sprint_latch, lock_latch;
    static uint64_t still_since;
    uint64_t raw = *keys, edges = raw & ~prev;
    if (menu_open) { sprint_latch = lock_latch = 0; still_since = 0; }
    if (cfg_bool("TOTK_SPRINT_TOGGLE")) {
        if (edges & (1ull << B_B)) sprint_latch ^= 1;
        if (sprint_latch) {
            *keys |= 1ull << B_B;
            uint64_t now = plat_now_ns() / 1000000u;
            if (sl[0] || sl[1]) still_since = 0;
            else if (!still_since) still_since = now;
            else if (now - still_since > 300) { sprint_latch = 0; *keys &= ~(1ull << B_B); still_since = 0; }
        } else {
            *keys &= ~(1ull << B_B);
            still_since = 0;
        }
    } else {
        sprint_latch = 0;
        still_since = 0;
    }
    if (cfg_bool("TOTK_LOCK_TOGGLE")) {
        if (edges & (1ull << B_ZL)) lock_latch ^= 1;
        if (lock_latch) *keys |= 1ull << B_ZL;
        else *keys &= ~(1ull << B_ZL);
    } else {
        lock_latch = 0;
    }
    prev = raw;
}

// A wheel notch is an event, not a held key. Upstream holds it for 70 ms and then rests as long, one notch at a time,
// so the game's own sampling sees separate steps and a quick scroll queues instead of merging into one press
// (hle_hid.cpp, wheel_read). Two polls were too short for that: a notch at 144 fps lasted 14 ms, under one sample of a
// 30 Hz menu, and two notches in one poll collapsed into one step. TOTK_WHEEL_STEP_MS; 0 keeps a notch for one poll.
enum { WHEEL_QUEUE_MAX = 8 };

static uint32_t wheel_step_ms(void) {
    int ms = cfg_int("TOTK_WHEEL_STEP_MS");
    return ms < 0 ? 0 : ms > 200 ? 200 : (uint32_t)ms;
}
static struct {
    int up, down;      // notches waiting, by direction
    int dir;           // +1 up, -1 down: the notch being played
    int phase;         // 0 idle, 1 the notch held, 2 the rest before the next
    uint64_t at;
} g_wheel;

static void wheel_flush(void) { g_wheel.up = g_wheel.down = 0, g_wheel.phase = 0; }

// The key this poll holds for the wheel: one notch of the queue at a time, wheel_step_ms() down and as long up.
// The menu's ImGui and the bind capture take the notches themselves, so the queue is dropped while either reads it.
static void wheel_keys(int notches, int menu_open, int captured, uint8_t down[KEY_COUNT]) {
    if (menu_open || !captured) {
        wheel_flush();
        return;
    }
    if (notches > 0) g_wheel.up = g_wheel.up + notches > WHEEL_QUEUE_MAX ? WHEEL_QUEUE_MAX : g_wheel.up + notches;
    else if (notches < 0) g_wheel.down = g_wheel.down - notches > WHEEL_QUEUE_MAX ? WHEEL_QUEUE_MAX : g_wheel.down - notches;
    uint64_t now = plat_now_ns() / 1000000u;
    if (!g_wheel.phase && (g_wheel.up || g_wheel.down)) {
        g_wheel.dir = g_wheel.up ? 1 : -1;
        if (g_wheel.dir > 0) g_wheel.up--;
        else g_wheel.down--;
        g_wheel.phase = 1;
        g_wheel.at = now;
    }
    uint32_t step = wheel_step_ms();
    if (g_wheel.phase == 1) {
        down[g_wheel.dir > 0 ? KEY_WHEEL_UP : KEY_WHEEL_DOWN] = 1;
        if (now - g_wheel.at >= step) g_wheel.phase = 2, g_wheel.at = now;
    } else if (g_wheel.phase == 2 && now - g_wheel.at >= step) {
        g_wheel.phase = 0;
    }
}

// The presented frame's length, for the PC scheme's frame-counted steps (a wheel notch's strip step). gfx_counters.frames
// moves once a presented frame and the poll may run more than once a frame, so a change of the counter is what is timed;
// a hitch is dropped rather than smoothed into the step.
static uint32_t g_frame_ms = 16;   // the presented frame, ms: the scheme's steps and the wheel's flick count frames

static void frame_clock(void) {
    static uint64_t frame = ~0ull, at;
    uint32_t ms = g_frame_ms;
    uint64_t now = plat_now_ns(), fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (fr == frame) return;
    if (frame != ~0ull && now > at) {
        uint64_t dt = (now - at) / 1000000u;
        if (dt >= 4 && dt <= 100) ms = ms ? (uint32_t)((ms * 7 + dt) / 8) : (uint32_t)dt;
    }
    frame = fr;
    at = now;
    g_frame_ms = ms;
    kbm_scheme_set_frame_ms(ms);
}

// A wheel notch with a quick menu already in front: the strip or the wheel holds itself open on the player's own key,
// so the notch only walks it -- one 2-frame flick of the right stick, no d-pad press and no release (Weeping's
// wheel_read, its quick-menu branch).
static int32_t g_flick;
static uint64_t g_flick_until;

static void poll_merged(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    uint64_t b = 0;
    uint64_t pad_bits = 0; // the gamepad's own bits: the host chords are read from these under the PC scheme
    int32_t sl[2] = {0, 0}, sr[2] = {0, 0};
    int scheme = 0;
    plat_mutex_lock(&lock);
    if (!state) start();
    if (state > 0) {
        binds_refresh();
        if (state == 1) {
            pad_read(&b, sl, sr);
            pad_bits = b;
        }
        if (ikb_foreground()) {
            frame_clock();
            uint8_t down[KEY_COUNT] = {0};
            // Mouse buttons count only while the cursor is captured, and a button already held when the capture began
            // (the click that refocused the window) is ignored until it is released.
            static int was_captured;
            static uint8_t mouse_ignore[KEY_COUNT];
            int cap = ikb_captured();
            for (int k = 1; k < KEY_COUNT; k++) {
                down[k] = ikb_down(k);
                if (k < KEY_MOUSE_L || k > KEY_MOUSE_X2) continue;
                if (cap && !was_captured) mouse_ignore[k] = down[k];
                if (!down[k]) mouse_ignore[k] = 0;
                if (!cap || mouse_ignore[k]) down[k] = 0;
            }
            was_captured = cap;
            int menu_open = menu_is_open();
            scheme = kbm_on();
            // Not while the menu is open: the bind capture and ImGui read the wheel there (wheel_keys, above).
            int wheel = menu_open ? 0 : ikb_wheel_take();
            if (!cap) wheel = 0;
            wheel_keys(wheel, menu_open, cap, down);
            key_min_hold(down);
            uint64_t key_bits = 0;
            int32_t k[2];
            if (scheme) {
                // The context table decides. The mouse camera stays this file's: no row of the scheme carries it,
                // and the direct mouse look needs the game-side veneer (docs/kbm-port.md).
                int32_t kl[2] = {0, 0}, kr[2] = {0, 0};
                kbm_scheme_set_ctx(screen_watch_ctx(menu_open));
                kbm_scheme_frame(down, plat_now_ns() / 1000000u, &key_bits, kl, kr);
                pick(&sl[0], &sl[1], kl[0], kl[1]);
                pick(&sr[0], &sr[1], kr[0], kr[1]);
            } else {
                uint64_t kb = input_map_buttons(down);
                // The game samples Npad slower than the window presents: a key or mouse button released between two
                // samples keeps its bit for two more polls, so a quick tap is never lost at high frame rates.
                static uint64_t kb_prev;
                static uint8_t kb_pulse[16];
                uint64_t released = kb_prev & ~kb;
                for (int i = 0; i < 16; i++)
                    if (released & (1ull << i)) kb_pulse[i] = 2;
                kb_prev = kb;
                for (int i = 0; i < 16; i++)
                    if (kb_pulse[i]) {
                        kb |= 1ull << i;
                        kb_pulse[i]--;
                    }
                key_bits = kb;
                walk_get();
                int walk = walk_key != KEY_NONE && down[walk_key];
                input_map_stick(down, 0, k);
                if (walk) k[0] = (int32_t)(k[0] * walk_scale), k[1] = (int32_t)(k[1] * walk_scale);
                pick(&sl[0], &sl[1], k[0], k[1]);
                input_map_stick(down, 1, k);
                if (walk) k[0] = (int32_t)(k[0] * walk_scale), k[1] = (int32_t)(k[1] * walk_scale);
                pick(&sr[0], &sr[1], k[0], k[1]);
            }
            input_toggles(&key_bits, sl, menu_open);
            b = pad_bits | key_bits;
            // L is the ability wheel: the mouse then pushes a stick that stays (mouse_camera), not a velocity sample.
            mouse_camera(k, (int)((b >> B_ZR) & 1), (int)((b >> B_L) & 1));
            pick(&sr[0], &sr[1], k[0], k[1]);
            if (g_flick && plat_now_ns() / 1000000u < g_flick_until) pick(&sr[0], &sr[1], g_flick, 0);
            else g_flick = 0;
        } else {
            ikb_wheel_take();   // not queued while the window is behind others
            wheel_flush();
        }
    }
    script_apply(&b, sl, sr);
    rec_apply(b, sl, sr);
    // Minus and Plus held together (gamepad or keys) open or close the menu and never reach the game
    static int combo_held;
    int32_t st[4] = {sl[0], sl[1], sr[0], sr[1]};
    // 0.3.1's chords are its own keys' (Tab is Plus, M is Minus, Ctrl and V the stick clicks). The scheme's keyboard is
    // not that table -- M is the map and Esc the pause menu there -- so its keys never form them: the pad still does.
    uint64_t chords = scheme ? pad_bits : ~0ull;
    int toggle = input_menu_gate(&combo_held, menu_is_open(), &b, st, chords);
    static int overlay_held;   // L3+R3 step the performance overlay (F10)
    int overlay = input_overlay_gate(&overlay_held, &b, chords);
    sl[0] = st[0], sl[1] = st[1], sr[0] = st[2], sr[1] = st[3];
    plat_mutex_unlock(&lock);
    if (toggle) menu_toggle();
    if (overlay) menu_perf_cycle();
    stick_bits(&b, sl[0], sl[1], B_SL_LEFT);
    stick_bits(&b, sr[0], sr[1], B_SR_LEFT);
    *buttons = b; *lx = sl[0]; *ly = sl[1]; *rx = sr[0]; *ry = sr[1];
}

// Text entry (swkbd_*.c): while it is open the game reads no input, as behind the keyboard applet; after it closes
// the game reads none until every button and stick direction is released, so the press that closed it stays there.
// g_block: 0 off, 1 text entry open, 2 closed, waiting for the release. The cursor is released while it is open.
static volatile int32_t g_block;
static volatile int32_t g_menu;   // the in-game menu, the same way (input_menu)
// When g_block and g_menu last went to 2 (plat_now_ns). The wait for the release ends after RELEASE_WAIT_NS even with
// something held: an input that never reads as released (a button another program holds down, a key whose release
// went to another window, a stick that rests outside its dead zone) would otherwise keep the game deaf until restart
// (#462, #494). A press that closed the menu is let go well within the wait; one still held after it is meant.
static volatile uint64_t g_block_closed, g_menu_closed;
#define RELEASE_WAIT_NS 1000000000ull

int input_cursor_allowed(void) {
    const char *in = cfg_get("TOTK_INPUT");
    return g_block != 1 && g_menu != 1 && mouse_sens_get() > 0 && !(in && !strcmp(in, "none")) && !menu_cursor_pointer();
}

void input_block(int on) {
    if (!on) __atomic_store_n(&g_block_closed, plat_now_ns(), __ATOMIC_RELEASE);
    plat_xchg32(&g_block, on ? 1 : 2);
    ikb_cursor_update();
}

// The in-game menu (ui/menu.cpp) blocks the game's input the same way: g_menu 0 closed, 1 open, 2 closed, waiting for
// the release. The cursor is released while it is open, for the menu's mouse.
void input_menu(int open) {
    if (!open) __atomic_store_n(&g_menu_closed, plat_now_ns(), __ATOMIC_RELEASE);
    plat_xchg32(&g_menu, open ? 1 : 2);
    if (!open) ikb_menu_closed();   // the capture before Esc comes back
    ikb_wheel_take();               // notches while the menu was open belong to it, not the game
    ikb_cursor_update();
}

// 1 while flag (g_block, g_menu) blocks the game's input; state 2 ends at the first poll with nothing held, or with
// something still held RELEASE_WAIT_NS after closed. A stick axis counts as held as well: with the walk key the
// keyboard's sticks move part way without setting a direction bit, so a gate that watched the buttons alone would let
// that movement through the moment the menu or the text entry closes.
static int blocked(volatile int32_t *flag, volatile uint64_t *closed, const char *what, uint64_t buttons,
                   const int32_t *axes) {
    int32_t k = *flag;
    if (k != 2) return k != 0;
    int held = buttons != 0;
    for (int i = 0; i < 4; i++) held |= axes[i] != 0;
    if (held && plat_now_ns() - __atomic_load_n(closed, __ATOMIC_ACQUIRE) < RELEASE_WAIT_NS) return 1;
    if (plat_cas32(flag, 0, 2) != 2) return *flag != 0;   // reopened meanwhile
    if (held)
        rt_log("input: the %s closed 1 s ago and input is still held (buttons %#llx, sticks %d %d %d %d); the game "
               "gets it from now on", what, (unsigned long long)buttons, axes[0], axes[1], axes[2], axes[3]);
    return 0;
}

void input_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    poll_merged(buttons, lx, ly, rx, ry);
    screen_watch_poll(*buttons);   // the game thread's pad read: the eui screen for the scheme's next context
    if (!g_block && !g_menu) menu_cursor_poll(buttons);   // the game's menus: hover and click as A/B, walked with the d-pad
    const int32_t axes[4] = {*lx, *ly, *rx, *ry};
    int text = blocked(&g_block, &g_block_closed, "text entry", *buttons, axes),
        menu = blocked(&g_menu, &g_menu_closed, "menu", *buttons, axes);
    if (!text && !menu) return;
    *buttons = 0; *lx = *ly = *rx = *ry = 0;
}

void input_poll_ui(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    poll_merged(buttons, lx, ly, rx, ry);
}

int input_pad_read(uint64_t *buttons, int32_t *lx, int32_t *ly) {
    uint64_t b = 0;
    int32_t sl[2] = {0, 0}, sr[2] = {0, 0};
    plat_mutex_lock(&lock);
    if (!state) start();
    if (state > 0) binds_refresh();
    int ok = state == 1 && pad_read(&b, sl, sr);
    plat_mutex_unlock(&lock);
    *buttons = b;
    *lx = sl[0];
    *ly = sl[1];
    return ok;
}

uint16_t rumble_motor(float a, float curve, float strength) {
    if (!(a > 0) || !(strength > 0)) return 0;
    float v = powf(a > 1 ? 1 : a, curve) * strength * 65535.0f + 0.5f;
    return v >= 65535.0f ? 65535 : (uint16_t)v;
}

int input_pad_layout(void) {
    return (int)plat_cas32(&pad_layout_shown, 0, 0);
}

int input_pad_label(int pad_id) {
    if (pad_id < PAD_SOUTH || pad_id > PAD_NORTH) return PAD_LABEL_NONE;
    return (int)plat_cas32(&pad_labels[pad_id - PAD_SOUTH], 0, 0);
}

void input_map_snapshot(InputMap *m) {
    plat_mutex_lock(&lock);
    if (!state) start();
    if (state > 0) binds_refresh();
    *m = input_map;
    plat_mutex_unlock(&lock);
}

int input_capture(int reset, int *id) {
    static uint8_t prev_k[KEY_COUNT], prev_p[PAD_COUNT];
    static int prev_esc;
    uint8_t k[KEY_COUNT] = {0}, p[PAD_COUNT] = {0};
    int fg = ikb_foreground();
    for (int i = 1; i < KEY_COUNT; i++) k[i] = fg && ikb_down(i);
    int esc = fg && ikb_escape();
    int wheel = ikb_wheel_take();   // a notch is an event, not a held key
    if (!fg) wheel = 0;
    plat_mutex_lock(&lock);
    if (!state) start();
    if (state == 1) pad_physical(p);
    plat_mutex_unlock(&lock);
    int got = INPUT_CAPTURE_NONE;
    if (!reset) {
        if (esc && !prev_esc) got = INPUT_CAPTURE_CANCEL;
        if (!got && wheel) got = INPUT_CAPTURE_KEY, *id = wheel > 0 ? KEY_WHEEL_UP : KEY_WHEEL_DOWN;
        for (int i = 1; i < KEY_COUNT && !got; i++)
            if (k[i] && !prev_k[i]) got = INPUT_CAPTURE_KEY, *id = i;
        for (int i = 1; i < PAD_COUNT && !got; i++)
            if (p[i] && !prev_p[i]) got = INPUT_CAPTURE_PAD, *id = i;
    }
    memcpy(prev_k, k, sizeof k);
    memcpy(prev_p, p, sizeof p);
    prev_esc = esc;
    return got;
}

int input_rumble_triggers_enabled(void) {
    return cfg_bool("TOTK_RUMBLE_TRIGGERS");
}

int input_rumble(uint16_t low, uint16_t high, uint32_t ms) {
    int ok = 0;
    static int triggers = -1;
    static unsigned seen;
    if (triggers < 0 || seen != cfg_version()) seen = cfg_version(), triggers = input_rumble_triggers_enabled();
    plat_mutex_lock(&lock);
    if (state == 1 && pad && pad_rumble) {
        ok = SDL_RumbleGamepad(pad, low, high, ms);
        if (triggers) SDL_RumbleGamepadTriggers(pad, high, high, ms);   // unsupported pads return false: ignored
    }
    plat_mutex_unlock(&lock);
    return ok;
}

// SDL gamepad sensor axes (SDL_sensor.h, a gamepad held in front of you): x to the right, y up out of the face (lying
// flat face up, +y points at the ceiling), z toward the player (out of the bottom edge). The accelerometer reads the
// push against gravity in m/s^2 (lying flat: y = +9.81); the gyro reads rad/s, counter-clockwise positive (right hand).
// Switch six-axis axes (nn::hid, as MacNativeGames' GCMotion feed and its Pro Controller HID path use them): x to the
// right, y toward the top edge (away from the player), z out of the face; acceleration as gravity reads (lying flat:
// z = -1 G). So Switch x = SDL x, Switch y = -SDL z, Switch z = SDL y: a rotation (right-handed to right-handed), so the
// gyro keeps its sense, while the acceleration is also negated (push -> gravity) and scaled to G.
void input_motion_axes(const float a[3], const float g[3], double acc[3], double gyro[3]) {
    acc[0] = -a[0] / SDL_STANDARD_GRAVITY;
    acc[1] = a[2] / SDL_STANDARD_GRAVITY;
    acc[2] = -a[1] / SDL_STANDARD_GRAVITY;
    gyro[0] = g[0];
    gyro[1] = -g[2];
    gyro[2] = g[1];
}

// Under lock: (re)enable the sensors when the open gamepad changed; 1 if it has both and they are on.
static int sensors_ready(void) {
    SDL_JoystickID id = SDL_GetGamepadID(pad);
    if (id == sensor_pad) return sensor_ok;
    sensor_pad = id;
    // Nothing drains the SDL event queue: no event per sample (the data still updates, SDL_SendJoystickSensor)
    SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_SENSOR_UPDATE, false);
    sensor_ok =SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) &&
                SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true) &&
                SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
    if (sensor_ok) rt_log("input: motion sensors on (gyro %.0f Hz, accelerometer %.0f Hz)",
                          (double)SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_GYRO),
                          (double)SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_ACCEL));
    else rt_log("input: \"%s\" has no gyro and accelerometer: the six-axis sensor reads a controller at rest",
                SDL_GetGamepadName(pad));
    return sensor_ok;
}

int input_motion_enabled(void) {
    static int on = -1;
    static unsigned seen;
    unsigned v = cfg_version();
    if (on < 0 || v != seen) {
        seen = v;
        on = cfg_bool("TOTK_MOTION");
    }
    return on;
}

// 250 Hz: the gamepads update (reports read, rumble expiry) and the newest gyro and accelerometer sample goes into the
// fusion. Without a gamepad with sensors, or with motion off (TOTK_MOTION=0, live), nothing is fed and the six-axis
// sensor reads at rest.
static void motion_main(void *arg) {
    (void)arg;
    for (;;) {
        plat_wait_ns(4000000);
        float a[3], g[3];
        int have = 0;
        plat_mutex_lock(&lock);
        if (pad && SDL_GamepadConnected(pad)) {
            SDL_UpdateGamepads();
            have = sensors_ready() && SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, a, 3) &&
                   SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, g, 3);
        }
        plat_mutex_unlock(&lock);
        if (!have || !input_motion_enabled()) continue;
        double acc[3], gyro[3];
        input_motion_axes(a, g, acc, gyro);
        motion_feed(motion_global(), acc, gyro, kern_now_ns());
    }
}

void input_motion_start(void) {
    plat_mutex_lock(&lock);
    if (!state) start();
    if (!motion_thread) {
        if (!input_motion_enabled()) rt_log("input: motion off (TOTK_MOTION=0); it can be turned on while the game runs");
        if (state != 1) motion_thread = -1;
        else {
            if (!plat_thread_spawn(motion_main, NULL, "motion", 0)) rt_fatal("input: cannot start the motion thread");
            motion_thread = 1;
        }
    }
    plat_mutex_unlock(&lock);
}
