// Host input for nn::hid (runtime/ov_input.c): an SDL3 gamepad (no SDL window needed), the keyboard and the mouse,
// merged into Switch npad button bits and stick axes. Button order follows libnx HidNpadButton (ISC). The key and pad
// bindings are the tables in input_map.c (totk.ini overrides them). The keyboard and the mouse come from the window
// system's back end (input_wsys.h: input_win.c with Win32, input_sdl.c with SDL), which also owns the cursor capture.
// Keys and their map: docs/controls.md.
// The gamepad also rumbles (input_rumble) and, when it has a gyro and an accelerometer, feeds the six-axis sensor from a
// thread of its own (input_motion_start); with TOTK_MOUSE_GYRO=1 the mouse feeds it instead of turning the camera.
// Every SDL call on the gamepad is made under `lock`.
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
#include "nvn_api.h"
#include "ui/menu.h"
#include "vk.h"
#include "platform/platform.h"

enum { STICK_MAX = 32767, STICK_DIR = 16384, TRIGGER_ON = 16384, RESCAN_MS = 1000 };

static PlatMutex lock = PLAT_MUTEX_INIT;
static int state;                    // 0 not started, 1 running, -1 disabled or SDL failed
static volatile int32_t g_block, g_menu;   // text entry / host menu: 0 off, 1 open, 2 waiting for release
// The device the player used last (input_kbm_last): 1 keyboard and mouse, 0 the gamepad. A pad counts once a button is
// down or a stick is past half way (a drifting stick never switches), the mouse once it moved MOUSE_USED raw counts
// within one poll (a slow deliberate move does; a desk bump does not), the keyboard with any key or mouse button.
enum { PAD_USED = 16384, MOUSE_USED = 6 };
static volatile int32_t g_kbm_last = 1;
int input_kbm_last(void) { return __atomic_load_n(&g_kbm_last, __ATOMIC_RELAXED); }
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
static int32_t script_mouse[2];      // one-shot raw counts, consumed by the camera hook; under lock

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
        if (!strcmp(name, "MOUSE") || !strcmp(name, "LSTICK") || !strcmp(name, "RSTICK")) {
            int n = sscanf(line + off, "%ld %ld %lf", &a, &b, &c);
            if (n < 2) { rt_log("input: script line %d: %s needs x y [hold]", ln, name); continue; }
            e.stick = name[0] == 'M' ? 3 : name[0] == 'L' ? 1 : 2; e.x = (int32_t)a; e.y = (int32_t)b;
            if (e.stick == 3) e.hold = 1;   // a displacement, never a held speed
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
        if (e->stick == 3 && e->logged) continue;
        if (!e->logged) {
            e->logged = 1;
            if (e->timed) rt_log("input: script frame %llu at %.1f ms: %s", (unsigned long long)fr, w / 1000.0, e->text);
            else rt_log("input: script frame %llu tick %llu: %s", (unsigned long long)fr, (unsigned long long)tick, e->text);
        }
        if (e->stick == 3) {
            int64_t x = (int64_t)script_mouse[0] + e->x, y = (int64_t)script_mouse[1] + e->y;
            script_mouse[0] = (int32_t)(x < INT32_MIN ? INT32_MIN : x > INT32_MAX ? INT32_MAX : x);
            script_mouse[1] = (int32_t)(y < INT32_MIN ? INT32_MIN : y > INT32_MAX ? INT32_MAX : y);
        } else if (e->stick) { int32_t *s = e->stick == 1 ? sl : sr; s[0] = e->x; s[1] = e->y; }
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

void input_direct_mouse_sample(int enabled, int32_t *dx, int32_t *dy) {
    plat_mutex_lock(&lock);
    enabled = enabled && !g_block && !g_menu;
    *dx = script_mouse[0]; *dy = script_mouse[1];
    script_mouse[0] = script_mouse[1] = 0;
    if (!enabled) { *dx = *dy = 0; }
    else if (ikb_captured() && ikb_foreground() && input_cursor_allowed()) {
        int32_t x, y;
        ikb_mouse_take(&x, &y);
        int64_t sx = (int64_t)*dx + x, sy = (int64_t)*dy + y;
        *dx = (int32_t)(sx < INT32_MIN ? INT32_MIN : sx > INT32_MAX ? INT32_MAX : sx);
        *dy = (int32_t)(sy < INT32_MIN ? INT32_MIN : sy > INT32_MAX ? INT32_MAX : sy);
    }
    if (enabled && (*dx || *dy) && rec_state == 1) {
        char what[80];
        snprintf(what, sizeof what, "MOUSE %d %d", *dx, *dy);
        int64_t w = frame_step_world_us();
        uint64_t tick = frame_step_ticks();
        rec_line(tick, w, tick + 1, w, what);
        fflush(rec);
    }
    plat_mutex_unlock(&lock);
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

static int mouse_selector;
int input_direct_mouse_selector_active(void) { return __atomic_load_n(&mouse_selector, __ATOMIC_RELAXED) != 0; }

// Selector 1 is a radial wheel, 2..4 are distinct horizontal quick lists; only a wheel retains a virtual cursor.
// Under `lock`.
static void mouse_camera(int32_t out[2], int selector) {
    static uint64_t frame = ~0ull, t0;
    static int32_t held[2];
    static int was_selector;
    static float cursor[2];
    static InputMouseRow row;
    uint64_t now = plat_now_ns();
    int32_t dx, dy;
    if (!input_direct_mouse_enabled() || !ikb_captured()) selector = 0;
    __atomic_store_n(&mouse_selector, selector, __ATOMIC_RELAXED);
    if (selector) {
        if (was_selector != selector) cursor[0] = cursor[1] = 0, row = (InputMouseRow){0};
        was_selector = selector;
        ikb_mouse_take(&dx, &dy);
        if (selector == 1) input_direct_mouse_radial(dx, dy, mouse_sens_get(), cursor, out);
        else input_direct_mouse_row(dx, mouse_sens_get(), now, &row, out);
        held[0] = held[1] = 0; frame = ~0ull;
        return;
    }
    if (was_selector) {
        was_selector = 0; cursor[0] = cursor[1] = 0; row = (InputMouseRow){0};
        ikb_mouse_take(&dx, &dy);   // release the wheel without sending its last movement to the camera
        held[0] = held[1] = out[0] = out[1] = 0; frame = ~0ull;
        return;
    }
    if (input_direct_mouse_active() || input_mouse_gyro_enabled()) {
        held[0] = held[1] = out[0] = out[1] = 0;
        frame = ~0ull;
        return;   // the camera update takes the raw counts itself, once, outside the HID stick path; or the
                  // six-axis sensor does (TOTK_MOUSE_GYRO, motion_main)
    }
    if (!ikb_captured()) {
        ikb_mouse_take(&dx, &dy);
        held[0] = held[1] = 0;
        frame = ~0ull;
    } else {
        uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        double age = frame == ~0ull ? 0 : (double)(now - t0) / 1e9;
        if (fr != frame) {
            double dt = age < 0.004 ? 0.004 : age > 0.1 ? 0.1 : age;
            ikb_mouse_take(&dx, &dy);
            input_mouse_stick(dx, dy, dt, mouse_sens_get(), held);
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

// The shake key (B_SHAKE): the game detaches a part fused to what Ultrahand holds when the right stick swings left and
// right a few times. A press swings it for SHAKE_NS, holding keeps it swinging; each side lasts SHAKE_SIDE_NS, two
// frames at the game's 30 fps, so every side is read even at 20 fps. Returns the stick x, 0 while not shaking.
// Under lock.
#define SHAKE_NS 800000000ull
#define SHAKE_SIDE_NS 66666667ull
static int32_t shake_stick(int held) {
    static int was;
    static uint64_t from, until;
    uint64_t now = plat_now_ns();
    if (held && !was && now >= until) from = now;
    if (held && !was) until = now + SHAKE_NS;
    if (held && until < now) until = now;
    was = held;
    if (now >= until && !held) return 0;
    return (now - from) / SHAKE_SIDE_NS % 2 ? STICK_MAX : -STICK_MAX;
}

// The scroll wheel on the Purah Pad's map, which zooms with the right stick's y (up: in): each notch is a full-tilt
// pulse of MAP_ZOOM_PULSE_NS on that stick with MAP_ZOOM_GAP_NS of neutral after it, so a step of the zoom and a
// held zoom both see one notch as one push. A fast scroll's notches wait their turn (at most MAP_ZOOM_QUEUE), a turn the
// other way drops them. Returns the stick y for this poll, 0 between pulses and off the map. Under lock.
#define MAP_ZOOM_PULSE_NS 100000000ull
#define MAP_ZOOM_GAP_NS 50000000ull
#define MAP_ZOOM_QUEUE 4
static int32_t map_zoom_stick(int32_t notches, int on_map) {
    static int32_t queued, dir;
    static uint64_t until;   // the end of the pulse (dir != 0) or of the neutral after it
    uint64_t now = plat_now_ns();
    if (!on_map) { queued = dir = 0, until = 0; return 0; }
    if (notches) {
        if (queued && (notches > 0) != (queued > 0)) queued = 0;
        queued += notches;
        queued = queued > MAP_ZOOM_QUEUE ? MAP_ZOOM_QUEUE : queued < -MAP_ZOOM_QUEUE ? -MAP_ZOOM_QUEUE : queued;
    }
    if (dir) {
        if (now < until) return dir * STICK_MAX;
        dir = 0, until = now + MAP_ZOOM_GAP_NS;
        return 0;
    }
    if (now < until || !queued) return 0;
    dir = queued > 0 ? 1 : -1;
    queued -= dir;
    until = now + MAP_ZOOM_PULSE_NS;
    return dir * STICK_MAX;
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

static uint64_t g_buttons_last;   // the merged buttons of the last poll (input_buttons_last)

static void poll_merged(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    uint64_t b = 0;
    int32_t sl[2] = {0, 0}, sr[2] = {0, 0};
    plat_mutex_lock(&lock);
    if (!state) start();
    if (state > 0) {
        binds_refresh();
        if (state == 1) pad_read(&b, sl, sr);
        int pad_used = b || abs(sl[0]) > PAD_USED || abs(sl[1]) > PAD_USED || abs(sr[0]) > PAD_USED ||
                       abs(sr[1]) > PAD_USED, kbm_used = 0;
        if (ikb_foreground()) {
            uint8_t down[KEY_COUNT] = {0};
            // Mouse buttons count only while the cursor is captured, and a button already held when the capture began
            // (the click that refocused the window) is ignored until it is released.
            static int was_captured;
            static uint8_t mouse_ignore[KEY_COUNT];
            int cap = ikb_captured();
            for (int k = 1; k < KEY_COUNT; k++) {
                down[k] = ikb_down(k);
                kbm_used |= down[k];
                if (k < KEY_MOUSE_L || k > KEY_MOUSE_X2) continue;
                if (cap && !was_captured) mouse_ignore[k] = down[k];
                if (!down[k]) mouse_ignore[k] = 0;
                if (!cap || mouse_ignore[k]) down[k] = 0;
            }
            was_captured = cap;
            b |= input_map_buttons(down);
            int32_t k[2];
            walk_get();
            int walk = walk_key != KEY_NONE && down[walk_key];
            input_map_stick(down, 0, k);
            if (walk) k[0] = (int32_t)(k[0] * walk_scale), k[1] = (int32_t)(k[1] * walk_scale);
            pick(&sl[0], &sl[1], k[0], k[1]);
            input_map_stick(down, 1, k);
            if (walk) k[0] = (int32_t)(k[0] * walk_scale), k[1] = (int32_t)(k[1] * walk_scale);
            pick(&sr[0], &sr[1], k[0], k[1]);
            int selector = b & (1ull << B_L) ? 1 : b & (1ull << B_UP) ? 2 :
                           b & (1ull << B_LEFT) ? 3 : b & (1ull << B_RIGHT) ? 4 : 0;
            // Only while the wheel or a strip is on screen (menu_cursor.c reads the focused screen): L also soothes the
            // horse and turns menu tabs, the D-pad does other things too, and then the mouse stays on the camera.
            // Before the screens were read (the menu cursor off) the buttons alone decide, as before.
            if (selector && menu_cursor_shortcut() == 0) selector = 0;
            mouse_camera(k, selector);
            // On the map the left stick moves the cursor and the right one zooms: the mouse moves the cursor there,
            // the scroll wheel zooms (map_zoom_stick). Elsewhere the wheel is unbound; its notches are dropped.
            int map = menu_cursor_map();
            if (map) pick(&sl[0], &sl[1], k[0], k[1]);
            else pick(&sr[0], &sr[1], k[0], k[1]);
            int32_t zoom = map_zoom_stick(ikb_wheel_take(), map);
            if (zoom) pick(&sr[0], &sr[1], 0, zoom);
            int32_t shake = shake_stick(input_map_held(down, B_SHAKE));
            if (shake) sr[0] = shake, sr[1] = 0;
            // A mouse shake (mouse_direct.c) moves the stick with the mouse, side by side, and lets go when it rests
            else if (input_direct_mouse_shake()) sr[0] = input_direct_mouse_shake() * STICK_MAX, sr[1] = 0;
            static uint32_t travel_seen;
            uint32_t travel = ikb_mouse_travel();
            // Steam Input's gyro to mouse moves the mouse while the gamepad is in the hands (TOTK_MOUSE_GYRO)
            if (travel - travel_seen >= MOUSE_USED && !input_mouse_gyro_enabled()) kbm_used = 1;
            travel_seen = travel;
        }
        // A press or a full-way stick on the pad wins over keys held in the same poll: the player picked it up
        if (pad_used) __atomic_store_n(&g_kbm_last, 0, __ATOMIC_RELAXED);
        else if (kbm_used) __atomic_store_n(&g_kbm_last, 1, __ATOMIC_RELAXED);
    }
    script_apply(&b, sl, sr);
    rec_apply(b, sl, sr);
    // Minus and Plus held together (gamepad or keys) open or close the menu and never reach the game
    static int combo_held;
    int32_t st[4] = {sl[0], sl[1], sr[0], sr[1]};
    int toggle = input_menu_gate(&combo_held, menu_is_open(), &b, st);
    static int overlay_held;   // L3+R3 step the performance overlay (F10)
    int overlay = input_overlay_gate(&overlay_held, &b);
    sl[0] = st[0], sl[1] = st[1], sr[0] = st[2], sr[1] = st[3];
    plat_mutex_unlock(&lock);
    if (toggle) menu_toggle();
    if (overlay) menu_perf_cycle();
    stick_bits(&b, sl[0], sl[1], B_SL_LEFT);
    stick_bits(&b, sr[0], sr[1], B_SR_LEFT);
    __atomic_store_n(&g_buttons_last, b, __ATOMIC_RELAXED);
    *buttons = b; *lx = sl[0]; *ly = sl[1]; *rx = sr[0]; *ry = sr[1];
}

uint64_t input_buttons_last(void) { return __atomic_load_n(&g_buttons_last, __ATOMIC_RELAXED); }

// Text entry (swkbd_*.c): while it is open the game reads no input, as behind the keyboard applet; after it closes
// the game reads none until every button and stick direction is released, so the press that closed it stays there.
// g_block: 0 off, 1 text entry open, 2 closed, waiting for the release. The cursor is released while it is open.
// When g_block and g_menu last went to 2 (plat_now_ns). The wait for the release ends after RELEASE_WAIT_NS even with
// something held: an input that never reads as released (a button another program holds down, a key whose release
// went to another window, a stick that rests outside its dead zone) would otherwise keep the game deaf until restart
// (#462, #494). A press that closed the menu is let go well within the wait; one still held after it is meant.
static volatile uint64_t g_block_closed, g_menu_closed;
#define RELEASE_WAIT_NS 1000000000ull

// The capture also serves the mouse as the six-axis sensor (TOTK_MOUSE_GYRO), with the mouse camera at 0.
int input_cursor_allowed(void) {
    const char *in = cfg_get("TOTK_INPUT");
    return g_block != 1 && g_menu != 1 && (mouse_sens_get() > 0 || input_mouse_gyro_enabled()) &&
           !(in && !strcmp(in, "none")) && !menu_cursor_pointer();
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
    if (!g_block && !g_menu) menu_cursor_poll(buttons);
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
    plat_mutex_lock(&lock);
    if (!state) start();
    if (state == 1) pad_physical(p);
    plat_mutex_unlock(&lock);
    int got = INPUT_CAPTURE_NONE;
    if (!reset) {
        if (esc && !prev_esc) got = INPUT_CAPTURE_CANCEL;
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

int input_mouse_gyro_enabled(void) {
    static int on = -1;
    static unsigned seen;
    unsigned v = cfg_version();
    if (on < 0 || v != seen) {
        seen = v;
        on = cfg_bool("TOTK_MOUSE_GYRO");
    }
    return on;
}

// TOTK_MOUSE_GYRO_DOTS (mouse counts per full turn) and TOTK_MOUSE_GYRO_TEST (MOUSE_GYRO_TEST_*), live. Motion thread.
static double mouse_gyro_dots(void) {
    static double dots = -1;
    static unsigned seen;
    unsigned v = cfg_version();
    if (dots < 0 || v != seen) {
        seen = v;
        double d = cfg_double("TOTK_MOUSE_GYRO_DOTS");
        dots = d < 360 ? 360 : d > 20000 ? 20000 : d;
    }
    return dots;
}

static int mouse_gyro_test(void) {
    static int mode = -1;
    static unsigned seen;
    unsigned v = cfg_version();
    if (mode < 0 || v != seen) {
        seen = v;
        const char *t = cfg_value("TOTK_MOUSE_GYRO_TEST");
        mode = !strcmp(t, "rate") ? MOUSE_GYRO_TEST_RATE : !strcmp(t, "pose") ? MOUSE_GYRO_TEST_POSE
             : !strcmp(t, "calibrate") ? MOUSE_GYRO_TEST_CALIBRATE : !strcmp(t, "stream") ? MOUSE_GYRO_TEST_STREAM
             : !strcmp(t, "trace") ? MOUSE_GYRO_TEST_TRACE : !strcmp(t, "steep") ? MOUSE_GYRO_TEST_STEEP
             : !strcmp(t, "flick") ? MOUSE_GYRO_TEST_FLICK : MOUSE_GYRO_TEST_OFF;
        if (mode != MOUSE_GYRO_TEST_OFF) rt_log("input: mouse gyro diagnostic %s (mouse_gyro_test)", t);
    }
    return mode;
}

// The game's six-axis reads, for the mouse source's window: the interval between reads at least 2 ms apart, so more
// than one read within a frame counts once, smoothed; and every read, for the trace.
static volatile uint64_t read_last_ns, read_gap_ns, read_count;

void input_motion_read_note(uint64_t now) {
    __atomic_add_fetch(&read_count, 1, __ATOMIC_RELAXED);
    uint64_t last = __atomic_load_n(&read_last_ns, __ATOMIC_RELAXED);
    if (last && now >= last && now - last < 2000000) return;
    __atomic_store_n(&read_last_ns, now, __ATOMIC_RELAXED);
    if (!last || now < last || now - last > 200000000) return;   // the first read, or after a pause: no interval
    uint64_t gap = __atomic_load_n(&read_gap_ns, __ATOMIC_RELAXED), d = now - last;
    __atomic_store_n(&read_gap_ns, gap ? (gap * 3 + d) / 4 : d, __ATOMIC_RELAXED);
}

// Ticks of the mouse source per game frame: the game time a frame steps (frame_rate.c), or the interval between the
// game's six-axis reads when that is longer; 1 at 200 fps, 2 at 120, 7 at 30. The reads alone can come more than once
// a frame, more than 2 ms apart (1.2.1 at a 120 fps limit: under 7.5 ms apart on average), while the snapshot the
// aim reads is taken once a frame.
static int frame_window(void) {
    uint64_t gap = __atomic_load_n(&read_gap_ns, __ATOMIC_RELAXED), step = frame_step_ns();
    if (step > gap) gap = step;
    int w = (int)((gap + MOUSE_GYRO_TICK_NS / 2) / MOUSE_GYRO_TICK_NS);
    return w < 1 ? 1 : w > MOUSE_GYRO_WINDOW_MAX ? MOUSE_GYRO_WINDOW_MAX : w;
}

// mouse_gyro_test = stream: once a second of mouse motion, how its counts arrived. A program that sends the motion in
// bursts (Steam Input's gyro to mouse with a touch activator) leaves ticks without counts inside a movement, and the
// game, which reads the newest rate once a frame, then sees the aim stop; a source that tops out shows the same largest
// tick for every fast movement. Also the samples made up after a late wake (mouse_gyro_due).
typedef struct { uint64_t since; int ticks, moved, gap, longest, peak, window, late; } MouseStream;

static void stream_note(MouseStream *s, int32_t dx, int32_t dy, int window, uint64_t now, double dots, int late) {
    if (window > s->window) s->window = window;
    if (!s->since) s->since = now;
    s->ticks++;
    s->late += late;
    int c = (int)lround(sqrt((double)dx * dx + (double)dy * dy));
    if (c) {
        s->moved++;
        if (s->gap > s->longest) s->longest = s->gap;   // a pause counts once the motion goes on after it
        s->gap = 0;
        if (c > s->peak) s->peak = c;
    } else if (s->moved) s->gap++;
    if (now - s->since < 1000000000ull) return;
    if (s->moved)
        rt_log("input: mouse gyro stream: %d of %d ticks moved, longest pause %d ms, largest tick %d counts (%.0f degrees "
               "a second), spread over up to %d ticks, %d ticks made up after a late wake", s->moved, s->ticks,
               s->longest * 5, s->peak, s->peak * 360.0 / dots / (MOUSE_GYRO_TICK_NS / 1e9), s->window, s->late);
    memset(s, 0, sizeof *s);
}

// mouse_gyro_test = trace: every tick of the mouse source to mouse_gyro_trace.csv next to the log (the sample's time in
// milliseconds since the first, 1 for a sample made up after a late wake, the counts, the spread, the game's reads of
// the sensor since the last sample and its frame step, the tilt above flat and the rotation the game gets), to see what
// arrived around a moment the aim stopped. Motion thread.
static void trace_note(int32_t dx, int32_t dy, int window, const MouseGyro *g, const double gyro[3], uint64_t now,
                       int late) {
    static FILE *f;
    static int failed;
    static uint64_t t0, flushed, reads;
    if (!f && !failed) {
        char path[1024] = "mouse_gyro_trace.csv";
        const char *log = rt_log_path();
        const char *a = log ? strrchr(log, '/') : NULL, *b = log ? strrchr(log, '\\') : NULL, *end = a;
        if (b && (!end || b > end)) end = b;
        if (end) snprintf(path, sizeof path, "%.*s/mouse_gyro_trace.csv", (int)(end - log), log);
        f = fopen(path, "w");
        if (!f) { failed = 1; rt_log("input: cannot write the mouse gyro trace %s", path); return; }
        fprintf(f, "ms,late,dx,dy,window,reads,frame_ms,tilt_deg,rate_x_dps,rate_y_dps,rate_z_dps\n");
        rt_log("input: mouse gyro trace in %s", path);
        t0 = flushed = now;
        reads = __atomic_load_n(&read_count, __ATOMIC_RELAXED);
    }
    if (!f) return;
    const double deg = 180 / M_PI;
    uint64_t r = __atomic_load_n(&read_count, __ATOMIC_RELAXED);
    fprintf(f, "%.1f,%d,%d,%d,%d,%llu,%.2f,%.2f,%.1f,%.1f,%.1f\n", (double)(now - t0) / 1e6, late, dx, dy, window,
            (unsigned long long)(r - reads), frame_step_ns() / 1e6, g->pitch * deg, gyro[0] * deg, gyro[1] * deg,
            gyro[2] * deg);
    reads = r;
    if (now - flushed >= 1000000000ull) { fflush(f); flushed = now; }
}

// mouse_gyro_test = calibrate: the sideways turn of each movement, logged once the mouse has been still for a second
// when it came to 90 degrees or more. Turn the real controller one full turn with the gyro on and stop: the log should
// say about 360.
typedef struct { double counts; int still; } MouseTurn;

static void calibrate_note(MouseTurn *c, int32_t dx, int32_t dy, double dots) {
    if (dx || dy) {
        c->counts += dx;
        c->still = 0;
        return;
    }
    if (c->still >= 200 || ++c->still < 200) return;   // 200 ticks, a second, without counts end the movement
    double deg = c->counts * 360 / dots;
    if (fabs(deg) >= 90)
        rt_log("input: mouse gyro turned %.1f degrees %s in one movement", fabs(deg), deg > 0 ? "right" : "left");
    c->counts = 0;
}

// One wake of the mouse source (motion_mouse.c): the counts the window system collected while the cursor was captured
// (none while the menu is open or the window is in the background, so the pose holds still), into the sensor as the
// samples due on its schedule (mouse_gyro_due), shared evenly between them when a late wake makes some up. When the
// game started the sensor again the pose starts over at rest with the sensor's angle, and the flick diagnostic at the
// start of its cycle. 1.2.1 starts it once, at boot, so the tilt the mouse gives carries over from one aim to the next.
static void mouse_feed(MouseGyro *g, unsigned *restarts, uint64_t *next, uint64_t now) {
    uint64_t t;
    int n = mouse_gyro_due(next, now, &t);
    if (!n) return;
    MotionFusion *m = motion_global();
    int test = mouse_gyro_test();
    unsigned r = motion_restarts(m);
    static MouseFlick flick;   // the motion thread's, like the stream and the turn
    static MouseTurn turn;
    if (r != *restarts) {
        mouse_gyro_init(g);
        memset(&flick, 0, sizeof flick);
        *restarts = r;
    }
    const double dt = MOUSE_GYRO_TICK_NS / 1e9;
    int32_t dx, dy;
    ikb_mouse_take(&dx, &dy);
    int window = frame_window();
    static MouseStream stream;   // the motion thread's
    for (int i = 0; i < n; i++, t += MOUSE_GYRO_TICK_NS) {
        int32_t sx = (int32_t)((int64_t)dx * (i + 1) / n - (int64_t)dx * i / n),
                sy = (int32_t)((int64_t)dy * (i + 1) / n - (int64_t)dy * i / n);
        if (test == MOUSE_GYRO_TEST_FLICK) mouse_gyro_flick(&flick, dt, mouse_gyro_dots(), &sx, &sy);
        double acc[3], gyro[3], dir[9];
        if (test == MOUSE_GYRO_TEST_RATE || test == MOUSE_GYRO_TEST_POSE || test == MOUSE_GYRO_TEST_STEEP)
            mouse_gyro_test_step(g, test, dt, acc, gyro, dir);
        else mouse_gyro_step(g, sx, sy, dt, mouse_gyro_dots(), window, acc, gyro, dir);
        motion_feed_pose(m, acc, gyro, dir, dt, t, *restarts);   // dropped after a Start: the next wake recentres
        int traced = test == MOUSE_GYRO_TEST_TRACE || test == MOUSE_GYRO_TEST_FLICK;
        if (traced || test == MOUSE_GYRO_TEST_STREAM) stream_note(&stream, sx, sy, window, t, mouse_gyro_dots(), i > 0);
        if (traced) trace_note(sx, sy, window, g, gyro, t, i > 0);
        if (test == MOUSE_GYRO_TEST_CALIBRATE) calibrate_note(&turn, sx, sy, mouse_gyro_dots());
    }
}

// The gamepads update (reports read, rumble expiry) on every tick whatever feeds the sensor. Its source is the
// gamepad's sensors at 250 Hz: the newest gyro and accelerometer sample goes into the fusion; or with TOTK_MOUSE_GYRO=1
// the mouse at 200 Hz on a fixed schedule (the thread waits for the next sample's time), a pose of its own
// (motion_mouse.c), while the gamepad's sensors are left alone. Switching starts the sensor over, as the two sources'
// frames differ. Without a gamepad with sensors (and the mouse source off), or with motion off (TOTK_MOTION=0, live),
// nothing is fed and the six-axis sensor reads at rest.
static void motion_main(void *arg) {
    (void)arg;
    MouseGyro mouse;
    mouse_gyro_init(&mouse);
    unsigned restarts = motion_restarts(motion_global());
    uint64_t next = 0;   // the mouse source's next sample time, 0 when its schedule starts over
    int was_mouse = -1;
    for (;;) {
        int from_mouse = input_mouse_gyro_enabled();
        uint64_t wait = from_mouse ? MOUSE_GYRO_TICK_NS : 4000000, now = kern_now_ns();
        if (from_mouse && next) wait = next > now ? next - now : 0;
        plat_wait_ns(wait);
        float a[3], g[3];
        int have = 0;
        plat_mutex_lock(&lock);
        if (state == 1 && pad && SDL_GamepadConnected(pad)) {
            SDL_UpdateGamepads();
            have = !from_mouse && sensors_ready() && SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, a, 3) &&
                   SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, g, 3);
        }
        plat_mutex_unlock(&lock);
        if (from_mouse != was_mouse) {
            if (was_mouse >= 0) motion_restart(motion_global());
            rt_log("input: six-axis sensor fed by %s",
                   from_mouse ? "the mouse (mouse_gyro = 1)" : "the gamepad's gyro and accelerometer");
            was_mouse = from_mouse;
        }
        if (!input_motion_enabled()) {
            int32_t dx, dy;
            if (from_mouse) ikb_mouse_take(&dx, &dy);   // the mouse camera is off as well: nothing else takes them
            next = 0;
            continue;
        }
        if (from_mouse) { mouse_feed(&mouse, &restarts, &next, kern_now_ns()); continue; }
        next = 0;
        if (!have) continue;
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
        if (state < 0) motion_thread = -1;   // TOTK_INPUT=none
        else {   // also keyboard only (no SDL): the mouse can still be the sensor
            if (!plat_thread_spawn(motion_main, NULL, "motion", 0)) rt_fatal("input: cannot start the motion thread");
            motion_thread = 1;
        }
    }
    plat_mutex_unlock(&lock);
}
