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
#include "nvn_api.h"
#include "renhdrum.h"
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
    if (input_direct_mouse_active()) {
        held[0] = held[1] = out[0] = out[1] = 0;
        frame = ~0ull;
        return;   // the camera update takes the raw counts itself, once, outside the HID stick path
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

// ---- RenHDrum dynamic loading, DualSense USB/Bluetooth DSP, Switch HD Rumble engine, and Xbox One Rumble
typedef uint32_t (*pfn_renhdrum_version)(void);
typedef int32_t (*pfn_renhdrum_settings_default)(RenHdcSettings *out_settings);
typedef RenHdcEngine *(*pfn_renhdrum_engine_create)(bool is_bluetooth, bool is_pro_controller);
typedef void (*pfn_renhdrum_engine_destroy)(RenHdcEngine *engine);
typedef int32_t (*pfn_renhdrum_engine_apply_settings)(RenHdcEngine *engine, const RenHdcSettings *settings);
typedef int32_t (*pfn_renhdrum_engine_push_sample)(
    RenHdcEngine *engine, int64_t now_us,
    float left_al, float left_fl, float left_ah, float left_fh,
    float right_al, float right_fl, float right_ah, float right_fh);
typedef int32_t (*pfn_renhdrum_engine_push_half_sample)(
    RenHdcEngine *engine, int64_t now_us, bool is_right,
    float al, float fl, float ah, float fh);
typedef int32_t (*pfn_renhdrum_engine_tick)(RenHdcEngine *engine, int64_t now_us, uint8_t *out_packet10);
typedef RenHdcDualSenseDsp *(*pfn_renhdrum_dualsense_dsp_create)(int32_t volume_percent, bool crisp_notes);
typedef void (*pfn_renhdrum_dualsense_dsp_destroy)(RenHdcDualSenseDsp *dsp);
typedef int32_t (*pfn_renhdrum_dualsense_dsp_set_target)(
    RenHdcDualSenseDsp *dsp,
    float left_al, float left_fl, float left_ah, float left_fh,
    float right_al, float right_fl, float right_ah, float right_fh);
typedef int32_t (*pfn_renhdrum_dualsense_dsp_set_half_target)(
    RenHdcDualSenseDsp *dsp, bool is_right,
    float al, float fl, float ah, float fh);
typedef int32_t (*pfn_renhdrum_dualsense_dsp_fill_f32)(
    RenHdcDualSenseDsp *dsp, float *out_interleaved_4ch, size_t frame_count);
typedef int32_t (*pfn_renhdrum_dualsense_dsp_build_bt_report)(
    RenHdcDualSenseDsp *dsp, uint8_t buffer_length, uint8_t *out_report142);
typedef RenHdcXboxRumble *(*pfn_renhdrum_xbox_rumble_create)(bool is_bluetooth);
typedef void (*pfn_renhdrum_xbox_rumble_destroy)(RenHdcXboxRumble *rumble);
typedef int32_t (*pfn_renhdrum_xbox_rumble_push_half_sample)(
    RenHdcXboxRumble *rumble, int64_t now_us, bool is_right,
    float low_amp, float low_freq, float high_amp, float high_freq);
typedef int32_t (*pfn_renhdrum_xbox_rumble_tick)(RenHdcXboxRumble *rumble, int64_t now_us, uint16_t *out_motors4);
typedef int32_t (*pfn_renhdrum_xbox_rumble_resend)(RenHdcXboxRumble *rumble);

typedef struct {
    pfn_renhdrum_version version;
    pfn_renhdrum_settings_default settings_default;
    pfn_renhdrum_engine_create engine_create;
    pfn_renhdrum_engine_destroy engine_destroy;
    pfn_renhdrum_engine_apply_settings engine_apply_settings;
    pfn_renhdrum_engine_push_sample engine_push_sample;
    pfn_renhdrum_engine_push_half_sample engine_push_half_sample;
    pfn_renhdrum_engine_tick engine_tick;
    pfn_renhdrum_dualsense_dsp_create dualsense_dsp_create;
    pfn_renhdrum_dualsense_dsp_destroy dualsense_dsp_destroy;
    pfn_renhdrum_dualsense_dsp_set_target dualsense_dsp_set_target;
    pfn_renhdrum_dualsense_dsp_set_half_target dualsense_dsp_set_half_target;
    pfn_renhdrum_dualsense_dsp_fill_f32 dualsense_dsp_fill_f32;
    pfn_renhdrum_dualsense_dsp_build_bt_report dualsense_dsp_build_bt_report;
    pfn_renhdrum_xbox_rumble_create xbox_rumble_create;
    pfn_renhdrum_xbox_rumble_destroy xbox_rumble_destroy;
    pfn_renhdrum_xbox_rumble_push_half_sample xbox_rumble_push_half_sample;
    pfn_renhdrum_xbox_rumble_tick xbox_rumble_tick;
    pfn_renhdrum_xbox_rumble_resend xbox_rumble_resend;
} RenHdrumApi;

static RenHdrumApi rh;
static int rh_state;                          // 0 not tried, 1 loaded, -1 unavailable
static PlatMutex dsp_lock = PLAT_MUTEX_INIT;  // guards dualsense_dsp for real-time audio callback
static RenHdcDualSenseDsp *dualsense_dsp;     // under dsp_lock (and lock on create/destroy)
static SDL_AudioStream *ds_audio_stream;      // under lock: 4-channel 48 kHz f32 USB audio stream
static int ds_init_audio;                     // 1 if we initialized SDL_INIT_AUDIO
static int ds_is_bt;                          // 1 if DualSense is connected over Bluetooth
static uint64_t ds_bt_last_active_ns;         // timestamp of last BT report 0x32 with non-zero signal
static uint64_t ds_bt_next_ns;                // next scheduled BT report 0x32 timestamp
static int ds_audio_tries;                    // USB audio open retry count (max 10)
static uint64_t ds_audio_last_try_ns;         // timestamp of last USB audio open attempt
static RenHdcEngine *renhdrum_engine;         // under lock: Switch Pro / Joy-Con HD rumble engine
static RenHdcXboxRumble *xbox_rumble;         // under lock: Xbox One / Series X|S 4-motor rumble converter
static int haptics_thread;                    // 0 not started, 1 running
static int hd_active_signal;                  // 1 while non-zero HD rumble is playing
static uint64_t hd_last_push_ns;              // timestamp of last non-zero HD rumble push
// Under lock: a second HID handle on the Bluetooth DualSense for output report 0x32. SDL_SendJoystickEffect cannot carry
// it: SDL's PS5 driver wraps whatever it is given into its own effects report (0x31 over Bluetooth, 0x02 over USB), so
// a 0x32 report sent that way would land in the rumble, volume and LED fields of a 0x31 report instead.
static SDL_hid_device *ds_hid;
static volatile int32_t hd_pad_kind;          // HD_PAD_* of the open gamepad (any thread)
static unsigned rh_cfg_seen = ~0u;
static int rh_hd_on = 1, rh_bt_level = 3, rh_hd_crisp = 1, rh_hd_bt_crisp = 1;
static int rh_ds_on = 1, rh_ds_vol = 100, rh_ds_crisp = 1, rh_ds_bt_crisp = 0;
static int rh_xbox_on = 1, rh_xbox_trig = 1;

static void renhdrum_read_cfg(void) {
    rh_cfg_seen = cfg_version();
    rh_hd_on = cfg_bool("TOTK_HD_RUMBLE") != 0;
    const char *spd = cfg_value("TOTK_HD_RUMBLE_BT_SPEED");
    rh_bt_level = !strcasecmp(spd, "stable") ? 1 : !strcasecmp(spd, "balanced") ? 2 : 3;
    rh_hd_crisp = cfg_bool("TOTK_HD_RUMBLE_CRISP") != 0;
    rh_hd_bt_crisp = cfg_bool("TOTK_HD_RUMBLE_BT_CRISP") != 0;
    rh_ds_on = cfg_bool("TOTK_DUALSENSE_HAPTICS") != 0;
    int vol = cfg_int("TOTK_DUALSENSE_VOLUME");
    rh_ds_vol = vol < 10 ? 10 : vol > 100 ? 100 : vol;
    rh_ds_crisp = cfg_bool("TOTK_DUALSENSE_CRISP") != 0;
    rh_ds_bt_crisp = cfg_bool("TOTK_DUALSENSE_BT_CRISP") != 0;
    rh_xbox_on = cfg_bool("TOTK_XBOX_RUMBLE") != 0;
    rh_xbox_trig = cfg_bool("TOTK_XBOX_TRIGGERS") != 0;
}

// Construct RenHdcSettings manually in memory (do NOT call renhdrum_settings_resolve, which queries Brontide IPC/files).
static RenHdcSettings renhdrum_make_settings(void) {
    RenHdcSettings s;
    memset(&s, 0, sizeof s);
    s.enabled = rh_hd_on != 0;
    s.usb_interval_ms = 15;
    s.bluetooth_level = (uint8_t)rh_bt_level;
    s.usb_auto_slow = true;
    s.sync_to_input = false;
    s.click_pulse = false;
    s.sync_bluetooth = false;
    s.usb_no_queue = false;
    s.usb_pause_ms = 0;
    s.envelope_5ms = true;
    s.frequency_in_packet = true;
    s.frequency_both_bands = true;
    s.bt_after_input = false;
    s.crisp_notes = rh_hd_crisp != 0;
    s.bt_crisp_notes = rh_hd_bt_crisp != 0;
    s.bt_fast = false;
    return s;
}

static RenHdcDualSenseDsp *renhdrum_create_dsp(int32_t vol, bool crisp) {
    // renhdrum_dualsense_dsp_create checks %APPDATA%/RenHDrum/renhdrum-settings.txt first; hide APPDATA briefly
    // when that file is present so totk.ini's volume and crisp_notes always take effect.
    char saved[PLAT_PATH_MAX], file[PLAT_PATH_MAX + 64];
    int hide = plat_getenv("APPDATA", saved, sizeof saved) &&
               snprintf(file, sizeof file, "%s/RenHDrum/renhdrum-settings.txt", saved) < (int)sizeof file &&
               plat_file_exists(file);
    if (hide) plat_setenv("APPDATA", NULL);
    RenHdcDualSenseDsp *dsp = rh.dualsense_dsp_create(vol, crisp);
    if (hide) plat_setenv("APPDATA", saved);
    return dsp;
}

static void renhdrum_close_pad(SDL_Gamepad *closing_pad);
static void renhdrum_open_pad(SDL_Gamepad *opened_pad);
static void renhdrum_sync_cfg(void);

// plat_exit runs no atexit handlers: the exit paths (gfx_window_closed, svc ExitProcess, the end of a --seconds run)
// call this first so the controller is left silent and the DSP / engine are destroyed.
void input_haptics_stop(void) {
    plat_mutex_lock(&lock);
    renhdrum_close_pad(pad);
    plat_mutex_unlock(&lock);
}

static int renhdrum_load(void) {
    if (rh_state) return rh_state == 1;
    const char *const libname = sizeof PLAT_EXE_SUFFIX > 1 ? "renhdrum.dll" : "librenhdrum.so";
    void *mod = NULL;
    char exe_path[PLAT_PATH_MAX];
    if (plat_exe_path(exe_path, sizeof exe_path)) {
        for (int depth = 0; depth < 4 && !mod; depth++) {
            char *slash = plat_last_sep(exe_path);
            if (!slash) break;
            *slash = '\0';
            char candidate[PLAT_PATH_MAX + 64];
            snprintf(candidate, sizeof candidate, "%s/%s", exe_path, libname);
            mod = plat_lib_open(candidate);
        }
    }
    if (!mod) {
        char rel[64];
        snprintf(rel, sizeof rel, "./%s", libname);
        mod = plat_lib_open(rel);
    }
    if (!mod && sizeof PLAT_EXE_SUFFIX == 1) mod = plat_lib_open(libname);
    if (!mod) {
        rh_state = -1;
        rt_log("input: %s not found, falling back to standard SDL rumble", libname);
        return 0;
    }
#define RH_SYM(name) (pfn_renhdrum_##name)plat_lib_sym(mod, "renhdrum_" #name)
    rh.version = RH_SYM(version);
    rh.settings_default = RH_SYM(settings_default);
    rh.engine_create = RH_SYM(engine_create);
    rh.engine_destroy = RH_SYM(engine_destroy);
    rh.engine_apply_settings = RH_SYM(engine_apply_settings);
    rh.engine_push_sample = RH_SYM(engine_push_sample);
    rh.engine_push_half_sample = RH_SYM(engine_push_half_sample);
    rh.engine_tick = RH_SYM(engine_tick);
    rh.dualsense_dsp_create = RH_SYM(dualsense_dsp_create);
    rh.dualsense_dsp_destroy = RH_SYM(dualsense_dsp_destroy);
    rh.dualsense_dsp_set_target = RH_SYM(dualsense_dsp_set_target);
    rh.dualsense_dsp_set_half_target = RH_SYM(dualsense_dsp_set_half_target);
    rh.dualsense_dsp_fill_f32 = RH_SYM(dualsense_dsp_fill_f32);
    rh.dualsense_dsp_build_bt_report = RH_SYM(dualsense_dsp_build_bt_report);
    rh.xbox_rumble_create = RH_SYM(xbox_rumble_create);
    rh.xbox_rumble_destroy = RH_SYM(xbox_rumble_destroy);
    rh.xbox_rumble_push_half_sample = RH_SYM(xbox_rumble_push_half_sample);
    rh.xbox_rumble_tick = RH_SYM(xbox_rumble_tick);
    rh.xbox_rumble_resend = RH_SYM(xbox_rumble_resend);
#undef RH_SYM

    if (!rh.version || !rh.engine_create || !rh.engine_destroy || !rh.engine_apply_settings ||
        !rh.engine_push_sample || !rh.engine_tick || !rh.dualsense_dsp_create || !rh.dualsense_dsp_destroy ||
        !rh.dualsense_dsp_set_target || !rh.dualsense_dsp_fill_f32 || !rh.dualsense_dsp_build_bt_report) {
        memset(&rh, 0, sizeof rh);
        rh_state = -1;
        rt_log("input: %s is missing required exports, falling back to standard SDL rumble", libname);
        return 0;
    }
    uint32_t ver = rh.version();
    rh_state = 1;
    rt_log("input: loaded %s (ABI v%u.%u.%u)", libname, ver >> 16 & 0xff, ver >> 8 & 0xff, ver & 0xff);
    return 1;
}

static void SDLCALL dualsense_audio_cb(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount) {
    (void)userdata;
    (void)total_amount;
    if (additional_amount <= 0) return;
    const int frame_bytes = 4 * (int)sizeof(float);
    int frames_needed = additional_amount / frame_bytes;
    if (frames_needed <= 0) return;
    float buf[512 * 4];
    while (frames_needed > 0) {
        int chunk = frames_needed > 512 ? 512 : frames_needed;
        plat_mutex_lock(&dsp_lock);
        int32_t rc = dualsense_dsp && rh.dualsense_dsp_fill_f32
                         ? rh.dualsense_dsp_fill_f32(dualsense_dsp, buf, (size_t)chunk)
                         : RENHDRUM_ERR_NULL_PTR;
        plat_mutex_unlock(&dsp_lock);
        if (rc != RENHDRUM_OK) memset(buf, 0, (size_t)chunk * 4 * sizeof(float));
        SDL_PutAudioStreamData(stream, buf, chunk * frame_bytes);
        frames_needed -= chunk;
    }
}

static int dualsense_try_open_audio(void) {
    if (ds_audio_stream) return 1;
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            rt_log("input: SDL audio init for DualSense haptics failed (%s)", SDL_GetError());
            return 0;
        }
        ds_init_audio = 1;
    }
    int count = 0;
    SDL_AudioDeviceID *devs = SDL_GetAudioPlaybackDevices(&count);
    if (!devs) return 0;
    SDL_AudioDeviceID target_dev = 0;
    for (int i = 0; i < count; i++) {
        const char *name = SDL_GetAudioDeviceName(devs[i]);
        if (!name) continue;
        if (strstr(name, "DualSense") != NULL || strstr(name, "Wireless Controller") != NULL) {
            SDL_AudioSpec spec;
            memset(&spec, 0, sizeof spec);
            if (SDL_GetAudioDeviceFormat(devs[i], &spec, NULL) && spec.channels >= 4) {
                target_dev = devs[i];
                break;
            }
        }
    }
    SDL_free(devs);
    if (!target_dev) return 0;
    SDL_AudioSpec desired;
    memset(&desired, 0, sizeof desired);
    desired.format = SDL_AUDIO_F32;
    desired.channels = 4;
    desired.freq = 48000;
    ds_audio_stream = SDL_OpenAudioDeviceStream(target_dev, &desired, dualsense_audio_cb, NULL);
    if (!ds_audio_stream) {
        rt_log("input: failed to open DualSense 4-channel audio stream (%s)", SDL_GetError());
        return 0;
    }
    SDL_ResumeAudioStreamDevice(ds_audio_stream);
    rt_log("input: DualSense 48 kHz 4-channel USB audio haptics stream active");
    return 1;
}

static void dualsense_maintain_audio(uint64_t now_ns) {
    if (!dualsense_dsp || ds_is_bt || ds_audio_stream || ds_audio_tries >= 10) return;
    if (ds_audio_last_try_ns && now_ns - ds_audio_last_try_ns < 2000000000ull) return;
    ds_audio_last_try_ns = now_ns;
    ds_audio_tries++;
    if (dualsense_try_open_audio() && pad) SDL_RumbleGamepad(pad, 0, 0, 0);
}

static void dualsense_bt_tick(uint64_t now_ns) {
    if (!dualsense_dsp || !ds_is_bt || !pad || !ds_hid) return;
    if (now_ns < ds_bt_next_ns) return;
    if (!ds_bt_next_ns || now_ns - ds_bt_next_ns > 100000000ull)
        ds_bt_next_ns = now_ns + RENHDRUM_DS5_BT_PERIOD_NS;
    else
        ds_bt_next_ns += RENHDRUM_DS5_BT_PERIOD_NS;
    uint8_t report142[RENHDRUM_DS5_BT_REPORT_SIZE];
    plat_mutex_lock(&dsp_lock);
    int32_t rc = rh.dualsense_dsp_build_bt_report(dualsense_dsp, RENHDRUM_DS5_BT_BUFFER_LENGTH_DEFAULT, report142);
    plat_mutex_unlock(&dsp_lock);
    if (rc == 1) ds_bt_last_active_ns = now_ns;
    // Keep sending for 2.0s after any active vibration (matching Brontide DualSenseBtLoop)
    // so the DualSense 3 kHz audio buffer does not starve and voice-coil haptics stay active.
    if (ds_bt_last_active_ns && now_ns - ds_bt_last_active_ns < 2000000000ull) {
        if (SDL_hid_write(ds_hid, report142, sizeof report142) < 0) {
            static volatile int32_t warned;
            if (!plat_xchg32(&warned, 1)) rt_log("input: DualSense report 0x32 write failed (%s)", SDL_GetError());
        }
    }
}

// The HID device of a Bluetooth DualSense, opened a second time for report 0x32: the joystick's own path (HIDAPI
// driver), else the first Bluetooth device with this vendor and product.
static SDL_hid_device *dualsense_open_hid(SDL_Joystick *joy, uint16_t vid, uint16_t pid) {
    const char *path = SDL_GetJoystickPath(joy);
    SDL_hid_device *d = path && *path ? SDL_hid_open_path(path) : NULL;
    if (!d) {
        SDL_hid_device_info *list = SDL_hid_enumerate(vid, pid);
        for (SDL_hid_device_info *it = list; it && !d; it = it->next)
            if (it->bus_type == SDL_HID_API_BUS_BLUETOOTH && it->path) d = SDL_hid_open_path(it->path);
        SDL_hid_free_enumeration(list);
    }
    return d;
}

static void switch_hd_tick(uint64_t now_ns) {
    if (!renhdrum_engine || !pad) return;
    uint8_t packet10[10];
    if (rh.engine_tick(renhdrum_engine, (int64_t)(now_ns / 1000u), packet10) == 1) {
        SDL_Joystick *joy = SDL_GetGamepadJoystick(pad);
        if (joy) SDL_SendJoystickEffect(joy, packet10, sizeof packet10);
    }
}

static void xbox_hd_tick(uint64_t now_ns) {
    if (!xbox_rumble || !pad || !rh.xbox_rumble_tick) return;
    uint16_t motors[4];
    if (rh.xbox_rumble_tick(xbox_rumble, (int64_t)(now_ns / 1000u), motors) == 1) {
        if (!SDL_RumbleGamepad(pad, motors[0], motors[1], 2000) && rh.xbox_rumble_resend)
            rh.xbox_rumble_resend(xbox_rumble);
        uint16_t lt = rh_xbox_trig ? motors[2] : 0, rt = rh_xbox_trig ? motors[3] : 0;
        SDL_RumbleGamepadTriggers(pad, lt, rt, 200);
    }
}

static void haptics_main(void *arg) {
    (void)arg;
    for (;;) {
        plat_wait_ns(1000000);
        plat_mutex_lock(&lock);
        if (pad && SDL_GamepadConnected(pad)) {
            renhdrum_sync_cfg();
            if (dualsense_dsp || renhdrum_engine || xbox_rumble) {
                uint64_t now = plat_now_ns();
                if (hd_active_signal && now - hd_last_push_ns > 250000000ull) {
                    hd_active_signal = 0;
                    if (dualsense_dsp) {
                        plat_mutex_lock(&dsp_lock);
                        rh.dualsense_dsp_set_target(dualsense_dsp, 0.0f, 160.0f, 0.0f, 320.0f, 0.0f, 160.0f, 0.0f, 320.0f);
                        plat_mutex_unlock(&dsp_lock);
                    }
                    if (renhdrum_engine)
                        rh.engine_push_sample(renhdrum_engine, (int64_t)(now / 1000u), 0.0f, 160.0f, 0.0f, 320.0f,
                                              0.0f, 160.0f, 0.0f, 320.0f);
                    if (xbox_rumble && rh.xbox_rumble_push_half_sample) {
                        int64_t now_us = (int64_t)(now / 1000u);
                        rh.xbox_rumble_push_half_sample(xbox_rumble, now_us, false, 0.0f, 160.0f, 0.0f, 320.0f);
                        rh.xbox_rumble_push_half_sample(xbox_rumble, now_us, true, 0.0f, 160.0f, 0.0f, 320.0f);
                    }
                }
                if (dualsense_dsp) {
                    if (ds_is_bt) dualsense_bt_tick(now);
                    else dualsense_maintain_audio(now);
                }
                if (renhdrum_engine) switch_hd_tick(now);
                if (xbox_rumble) xbox_hd_tick(now);
            }
        }
        plat_mutex_unlock(&lock);
    }
}

static void haptics_thread_start(void) {
    if (haptics_thread) return;
    if (plat_thread_spawn(haptics_main, NULL, "haptics", 0)) haptics_thread = 1;
}

static void renhdrum_close_pad(SDL_Gamepad *closing_pad) {
    if (ds_audio_stream) {
        SDL_AudioStream *s = ds_audio_stream;
        ds_audio_stream = NULL;
        SDL_PauseAudioStreamDevice(s);
        SDL_ClearAudioStream(s);
        SDL_DestroyAudioStream(s);
    }
    if (ds_init_audio) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        ds_init_audio = 0;
    }
    if (dualsense_dsp) {
        if (ds_hid && closing_pad && SDL_GamepadConnected(closing_pad)) {
            uint8_t report142[RENHDRUM_DS5_BT_REPORT_SIZE];
            plat_mutex_lock(&dsp_lock);
            rh.dualsense_dsp_set_target(dualsense_dsp, 0.0f, 160.0f, 0.0f, 320.0f, 0.0f, 160.0f, 0.0f, 320.0f);
            int32_t rc = rh.dualsense_dsp_build_bt_report(dualsense_dsp, RENHDRUM_DS5_BT_BUFFER_LENGTH_DEFAULT, report142);
            plat_mutex_unlock(&dsp_lock);
            if (rc >= 0) SDL_hid_write(ds_hid, report142, sizeof report142);
        }
        if (ds_hid) {
            SDL_hid_close(ds_hid);
            ds_hid = NULL;
        }
        plat_mutex_lock(&dsp_lock);
        RenHdcDualSenseDsp *d = dualsense_dsp;
        dualsense_dsp = NULL;
        plat_mutex_unlock(&dsp_lock);
        rh.dualsense_dsp_destroy(d);
    }
    if (renhdrum_engine) {
        if (closing_pad && SDL_GamepadConnected(closing_pad)) {
            static const uint8_t neutral10[10] = {0x10, 0x00, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
            SDL_Joystick *joy = SDL_GetGamepadJoystick(closing_pad);
            if (joy) SDL_SendJoystickEffect(joy, neutral10, sizeof neutral10);
        }
        rh.engine_destroy(renhdrum_engine);
        renhdrum_engine = NULL;
    }
    if (xbox_rumble) {
        if (closing_pad && SDL_GamepadConnected(closing_pad)) {
            SDL_RumbleGamepad(closing_pad, 0, 0, 0);
            SDL_RumbleGamepadTriggers(closing_pad, 0, 0, 0);
        }
        if (rh.xbox_rumble_destroy) rh.xbox_rumble_destroy(xbox_rumble);
        xbox_rumble = NULL;
    }
    ds_is_bt = 0;
    ds_bt_last_active_ns = 0;
    ds_bt_next_ns = 0;
    ds_audio_tries = 0;
    ds_audio_last_try_ns = 0;
    hd_active_signal = 0;
    hd_last_push_ns = 0;
}

static void renhdrum_open_pad(SDL_Gamepad *opened_pad) {
    renhdrum_close_pad(NULL);
    renhdrum_read_cfg();
    SDL_Joystick *joy = SDL_GetGamepadJoystick(opened_pad);
    if (!joy) {
        plat_xchg32(&hd_pad_kind, HD_PAD_NONE);
        return;
    }
    uint16_t vid = SDL_GetJoystickVendor(joy);
    uint16_t pid = SDL_GetJoystickProduct(joy);
    SDL_JoystickConnectionState conn = SDL_GetJoystickConnectionState(joy);
    bool wireless = conn == SDL_JOYSTICK_CONNECTION_WIRELESS;
    if (vid == 0x054C && (pid == 0x0CE6 || pid == 0x0DF2)) {
        if (!renhdrum_load()) return;
        plat_xchg32(&hd_pad_kind, wireless ? HD_PAD_DUALSENSE_BT : HD_PAD_DUALSENSE_USB);
        haptics_thread_start();
        if (!rh_ds_on) {
            rt_log("input: DualSense haptics off (TOTK_DUALSENSE_HAPTICS=0); using standard SDL rumble");
            return;
        }
        if (wireless) {
            ds_hid = dualsense_open_hid(joy, vid, pid);
            if (!ds_hid) {
                rt_log("input: Bluetooth DualSense HID device not opened (%s); standard SDL rumble", SDL_GetError());
                return;
            }
        }
        bool crisp = (wireless ? rh_ds_bt_crisp : rh_ds_crisp) != 0;
        RenHdcDualSenseDsp *dsp = renhdrum_create_dsp(rh_ds_vol, crisp);
        if (!dsp) {
            if (ds_hid) SDL_hid_close(ds_hid), ds_hid = NULL;
            return;
        }
        plat_mutex_lock(&dsp_lock);
        dualsense_dsp = dsp;
        plat_mutex_unlock(&dsp_lock);
        ds_is_bt = wireless ? 1 : 0;
        SDL_RumbleGamepad(opened_pad, 0, 0, 0);
        if (!ds_is_bt) dualsense_maintain_audio(plat_now_ns());
        rt_log("input: RenHDrum DualSense HD rumble active (%s, volume %d%%, crisp %s)",
               ds_is_bt ? "Bluetooth HID report 0x32" : "USB 48 kHz 4-channel audio VCM", rh_ds_vol, crisp ? "on" : "off");
    } else if (vid == 0x057E && (pid == 0x2009 || pid == 0x2006 || pid == 0x2007)) {
        if (!renhdrum_load()) return;
        plat_xchg32(&hd_pad_kind, wireless ? HD_PAD_SWITCH_BT : HD_PAD_SWITCH_USB);
        haptics_thread_start();
        if (!rh_hd_on) {
            rt_log("input: Switch HD rumble off (TOTK_HD_RUMBLE=0); using standard SDL rumble");
            return;
        }
        bool is_pro = pid == 0x2009;
        renhdrum_engine = rh.engine_create(wireless, is_pro);
        if (!renhdrum_engine) return;
        RenHdcSettings settings = renhdrum_make_settings();
        rh.engine_apply_settings(renhdrum_engine, &settings);
        rt_log("input: RenHDrum Switch HD rumble active (%s, %s)",
               is_pro ? "Pro Controller" : "Joy-Con", wireless ? "Bluetooth" : "USB");
    } else if (vid == 0x045E && SDL_GetGamepadType(opened_pad) == SDL_GAMEPAD_TYPE_XBOXONE) {
        if (!renhdrum_load() || !rh.xbox_rumble_create || !rh.xbox_rumble_push_half_sample || !rh.xbox_rumble_tick) {
            plat_xchg32(&hd_pad_kind, HD_PAD_NONE);
            return;
        }
        bool xbox_bt = conn == SDL_JOYSTICK_CONNECTION_WIRELESS ? true
                     : conn == SDL_JOYSTICK_CONNECTION_WIRED    ? false
                     : !(pid == 0x02D1 || pid == 0x02DD || pid == 0x02E3 || pid == 0x02EA || pid == 0x02FF ||
                         pid == 0x0B00 || pid == 0x0B0A || pid == 0x0B12);
        plat_xchg32(&hd_pad_kind, xbox_bt ? HD_PAD_XBOX_BT : HD_PAD_XBOX_USB);
        haptics_thread_start();
        if (!rh_xbox_on) {
            rt_log("input: Xbox One Rumble off (TOTK_XBOX_RUMBLE=0); using standard SDL rumble");
            return;
        }
        xbox_rumble = rh.xbox_rumble_create(xbox_bt);
        if (!xbox_rumble) return;
        rt_log("input: RenHDrum Xbox One Rumble active (%s, triggers %s)",
               xbox_bt ? "Bluetooth" : "USB", rh_xbox_trig ? "on" : "off");
    } else {
        plat_xchg32(&hd_pad_kind, HD_PAD_NONE);
    }
}

static void renhdrum_sync_cfg(void) {
    if (cfg_version() == rh_cfg_seen) return;
    int was_hd_on = rh_hd_on, was_bt_level = rh_bt_level, was_hd_crisp = rh_hd_crisp, was_hd_bt_crisp = rh_hd_bt_crisp;
    int was_ds_on = rh_ds_on, was_ds_vol = rh_ds_vol, was_ds_crisp = rh_ds_crisp, was_ds_bt_crisp = rh_ds_bt_crisp;
    int was_xbox_on = rh_xbox_on, was_xbox_trig = rh_xbox_trig;
    renhdrum_read_cfg();
    int kind = (int)plat_cas32(&hd_pad_kind, 0, 0);
    if (kind == HD_PAD_DUALSENSE_USB || kind == HD_PAD_DUALSENSE_BT) {
        if (rh_ds_on != was_ds_on) {
            if (!rh_ds_on) renhdrum_close_pad(pad);
            else renhdrum_open_pad(pad);
        } else if (dualsense_dsp) {
            int crisp = ds_is_bt ? rh_ds_bt_crisp : rh_ds_crisp;
            int was_crisp = ds_is_bt ? was_ds_bt_crisp : was_ds_crisp;
            if (rh_ds_vol != was_ds_vol || crisp != was_crisp) {
                RenHdcDualSenseDsp *new_dsp = renhdrum_create_dsp(rh_ds_vol, crisp != 0);
                if (new_dsp) {
                    plat_mutex_lock(&dsp_lock);
                    RenHdcDualSenseDsp *old = dualsense_dsp;
                    dualsense_dsp = new_dsp;
                    plat_mutex_unlock(&dsp_lock);
                    if (old) rh.dualsense_dsp_destroy(old);
                }
            }
        }
    } else if (kind == HD_PAD_SWITCH_USB || kind == HD_PAD_SWITCH_BT) {
        if (rh_hd_on != was_hd_on) {
            if (!rh_hd_on) renhdrum_close_pad(pad);
            else renhdrum_open_pad(pad);
        } else if (renhdrum_engine &&
                   (rh_bt_level != was_bt_level || rh_hd_crisp != was_hd_crisp || rh_hd_bt_crisp != was_hd_bt_crisp)) {
            RenHdcSettings s = renhdrum_make_settings();
            rh.engine_apply_settings(renhdrum_engine, &s);
        }
    } else if (kind == HD_PAD_XBOX_USB || kind == HD_PAD_XBOX_BT) {
        if (rh_xbox_on != was_xbox_on) {
            if (!rh_xbox_on) renhdrum_close_pad(pad);
            else renhdrum_open_pad(pad);
        } else if (was_xbox_trig && !rh_xbox_trig && pad && SDL_GamepadConnected(pad)) {
            SDL_RumbleGamepadTriggers(pad, 0, 0, 0);
        }
    }
}

// Open the first gamepad when none is open or the open one went away; logs once per change.
static void scan_pads(void) {
    if (pad && SDL_GamepadConnected(pad)) return;
    if (pad) {
        renhdrum_close_pad(pad);
        plat_xchg32(&hd_pad_kind, HD_PAD_NONE);
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
    renhdrum_open_pad(pad);
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
    // SDL3 on Windows defaults SDL_JOYSTICK_RAWINPUT to 0, opening Xbox controllers through XInput (which exposes only
    // the 2 grip motors and drops SDL_RumbleGamepadTriggers). Enable RAWINPUT when Xbox 4-motor trigger rumble is on.
    if ((cfg_bool("TOTK_XBOX_RUMBLE") && cfg_bool("TOTK_XBOX_TRIGGERS")) || cfg_bool("TOTK_RUMBLE_TRIGGERS"))
        SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "1");
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
            if (travel - travel_seen >= MOUSE_USED) kbm_used = 1;
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

int input_hd_pad_kind(void) {
    return (int)plat_cas32(&hd_pad_kind, 0, 0);
}

int input_hd_rumble(int side, float left_al, float left_fl, float left_ah, float left_fh,
                    float right_al, float right_fl, float right_ah, float right_fh) {
    int handled = 0;
    plat_mutex_lock(&lock);
    if (state == 1 && pad && SDL_GamepadConnected(pad)) {
        renhdrum_sync_cfg();
        uint64_t now = plat_now_ns();
        int non_zero = left_al > 0.0f || left_ah > 0.0f || right_al > 0.0f || right_ah > 0.0f;
        if (non_zero) {
            hd_active_signal = 1;
            hd_last_push_ns = now;
        }
        if (dualsense_dsp) {
            plat_mutex_lock(&dsp_lock);
            if (side == 0 && rh.dualsense_dsp_set_half_target) {
                rh.dualsense_dsp_set_half_target(dualsense_dsp, false, left_al, left_fl, left_ah, left_fh);
            } else if (side == 1 && rh.dualsense_dsp_set_half_target) {
                rh.dualsense_dsp_set_half_target(dualsense_dsp, true, right_al, right_fl, right_ah, right_fh);
            } else {
                rh.dualsense_dsp_set_target(dualsense_dsp, left_al, left_fl, left_ah, left_fh,
                                            right_al, right_fl, right_ah, right_fh);
            }
            plat_mutex_unlock(&dsp_lock);
            if (ds_is_bt) {
                if (non_zero && (!ds_bt_last_active_ns || now - ds_bt_last_active_ns >= 2000000000ull)) {
                    ds_bt_next_ns = now;
                }
                handled = ds_hid != NULL;
            } else {
                dualsense_maintain_audio(now);
                if (ds_audio_stream) {
                    SDL_RumbleGamepad(pad, 0, 0, 0);
                    handled = 1;
                }
            }
        } else if (renhdrum_engine) {
            int64_t now_us = (int64_t)(now / 1000u);
            if (side == 0 && rh.engine_push_half_sample)
                rh.engine_push_half_sample(renhdrum_engine, now_us, false, left_al, left_fl, left_ah, left_fh);
            else if (side == 1 && rh.engine_push_half_sample)
                rh.engine_push_half_sample(renhdrum_engine, now_us, true, right_al, right_fl, right_ah, right_fh);
            else
                rh.engine_push_sample(renhdrum_engine, now_us, left_al, left_fl, left_ah, left_fh,
                                      right_al, right_fl, right_ah, right_fh);
            switch_hd_tick(now);
            handled = 1;
        } else if (xbox_rumble && rh.xbox_rumble_push_half_sample) {
            int64_t now_us = (int64_t)(now / 1000u);
            if (side == 0)
                rh.xbox_rumble_push_half_sample(xbox_rumble, now_us, false, left_al, left_fl, left_ah, left_fh);
            else if (side == 1)
                rh.xbox_rumble_push_half_sample(xbox_rumble, now_us, true, right_al, right_fl, right_ah, right_fh);
            else {
                rh.xbox_rumble_push_half_sample(xbox_rumble, now_us, false, left_al, left_fl, left_ah, left_fh);
                rh.xbox_rumble_push_half_sample(xbox_rumble, now_us, true, right_al, right_fl, right_ah, right_fh);
            }
            xbox_hd_tick(now);
            handled = 1;
        }
    }
    plat_mutex_unlock(&lock);
    return handled;
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
