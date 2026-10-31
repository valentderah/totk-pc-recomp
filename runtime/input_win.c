// Host input for nn::hid (runtime/ov_input.c): an SDL3 gamepad (no SDL window, no SDL video), the keyboard and the mouse,
// merged into Switch npad button bits and stick axes. Button order follows libnx HidNpadButton (ISC). The key and pad bindings
// are the tables in input_map.c (totk.ini overrides them); this file translates Win32 virtual keys to its key ids, reads the mouse (raw input, WM_INPUT)
// and owns the cursor capture. Keys and their map: docs/controls.md.
// The gamepad also rumbles (input_rumble) and, when it has a gyro and an accelerometer, feeds the six-axis sensor from a
// thread of its own (input_motion_start). Every SDL call on the gamepad is made under `lock`.
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "nvn_api.h"
#include "ui/menu.h"
#include "vk.h"

enum { STICK_MAX = 32767, STICK_DIR = 16384, TRIGGER_ON = 16384, RESCAN_MS = 1000 };

static SRWLOCK lock = SRWLOCK_INIT;
static int state;                    // 0 not started, 1 running, -1 disabled or SDL failed
static SDL_Gamepad *pad;
static uint64_t next_scan;
static int pad_rumble;               // the open gamepad has rumble motors
static volatile LONG pad_labels[4];  // ... and the labels of its south, east, west, north buttons (PAD_LABEL_*), any thread
static int pad_east_a;               // ... and labels its east face button A (Nintendo layout under TOTK_PAD_LAYOUT=auto)
static int pad_layout = -1;          // PAD_LAYOUT_* in effect for the open gamepad, -1 to pick again; live (cfg_version)
static unsigned pad_layout_seen;
static volatile LONG pad_layout_shown = -1;   // pad_layout for the menu (any thread), -1 without a gamepad
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

// Win32 virtual key of a key id (the one place that knows them), 0 for an id without a key.
static int key_vk(int key) {
    if (key >= KEY_A && key <= KEY_Z) return 'A' + (key - KEY_A);
    if (key >= KEY_0 && key <= KEY_9) return '0' + (key - KEY_0);
    if (key >= KEY_NUM0 && key <= KEY_NUM9) return VK_NUMPAD0 + (key - KEY_NUM0);
    switch (key) {
    case KEY_SPACE: return VK_SPACE;
    case KEY_TAB: return VK_TAB;
    case KEY_LSHIFT: return VK_LSHIFT;
    case KEY_LCTRL: return VK_LCONTROL;
    case KEY_UP: return VK_UP;
    case KEY_DOWN: return VK_DOWN;
    case KEY_LEFT: return VK_LEFT;
    case KEY_RIGHT: return VK_RIGHT;
    case KEY_MOUSE_L: return VK_LBUTTON;
    case KEY_MOUSE_R: return VK_RBUTTON;
    case KEY_MOUSE_M: return VK_MBUTTON;
    case KEY_MOUSE_X1: return VK_XBUTTON1;
    case KEY_MOUSE_X2: return VK_XBUTTON2;
    case KEY_ENTER: return VK_RETURN;
    case KEY_BACKSPACE: return VK_BACK;
    case KEY_RSHIFT: return VK_RSHIFT;
    case KEY_RCTRL: return VK_RCONTROL;
    case KEY_LALT: return VK_LMENU;
    case KEY_CAPSLOCK: return VK_CAPITAL;
    case KEY_INSERT: return VK_INSERT;
    case KEY_DELETE: return VK_DELETE;
    case KEY_HOME: return VK_HOME;
    case KEY_END: return VK_END;
    case KEY_PAGEUP: return VK_PRIOR;
    case KEY_PAGEDOWN: return VK_NEXT;
    case KEY_F2: return VK_F2;
    case KEY_F3: return VK_F3;
    case KEY_F4: return VK_F4;
    case KEY_F5: return VK_F5;
    case KEY_F6: return VK_F6;
    case KEY_F7: return VK_F7;
    case KEY_F9: return VK_F9;
    case KEY_F12: return VK_F12;
    case KEY_NUM_ADD: return VK_ADD;
    case KEY_NUM_SUB: return VK_SUBTRACT;
    case KEY_NUM_MUL: return VK_MULTIPLY;
    case KEY_NUM_DIV: return VK_DIVIDE;
    case KEY_NUM_DECIMAL: return VK_DECIMAL;
    case KEY_MINUS: return VK_OEM_MINUS;
    case KEY_EQUALS: return VK_OEM_PLUS;
    case KEY_LBRACKET: return VK_OEM_4;
    case KEY_RBRACKET: return VK_OEM_6;
    case KEY_SEMICOLON: return VK_OEM_1;
    case KEY_APOSTROPHE: return VK_OEM_7;
    case KEY_COMMA: return VK_OEM_COMMA;
    case KEY_PERIOD: return VK_OEM_PERIOD;
    case KEY_SLASH: return VK_OEM_2;
    case KEY_BACKSLASH: return VK_OEM_5;
    case KEY_GRAVE: return VK_OEM_3;
    }
    return 0;
}

uint32_t input_key_char(int key) {
    if (key < KEY_MINUS || key > KEY_GRAVE) return 0;
    return MapVirtualKeyW((UINT)key_vk(key), MAPVK_VK_TO_CHAR) & 0x7fffffffu;   // the top bit marks a dead key
}

// ---- mouse camera: raw motion accumulated by the window thread, one stick sample per presented frame
static volatile LONG mouse_dx, mouse_dy;   // raw counts since the last frame sample
static volatile LONG cur_captured;         // cursor hidden and confined to the client area
static volatile LONG cur_active;           // WM_ACTIVATEAPP: the window's application is active
// Bits: CUR_REL, Esc or activation by a click on the title bar or border: until a client click; CUR_ESC, set with CUR_REL
// by Esc (not when it was already set): closing the menu gives the capture back. One word, changed by one CAS.
enum { CUR_REL = 1, CUR_ESC = 2 };
static volatile LONG cur_rel;
static void cur_rel_update(LONG (*f)(LONG)) {
    LONG o, n;
    do { o = cur_rel; n = f(o); } while (n != o && InterlockedCompareExchange(&cur_rel, n, o) != o);
}
static LONG rel_esc(LONG o) { return o & CUR_REL ? o : CUR_REL | CUR_ESC; }
static LONG rel_title(LONG o) { (void)o; return CUR_REL; }
static LONG rel_clear(LONG o) { (void)o; return 0; }
static LONG rel_menu_closed(LONG o) { return o & CUR_ESC ? 0 : o; }
static volatile LONG cur_suspend;          // F11 or a modal size or move loop under way
static SRWLOCK cur_lock = SRWLOCK_INIT;    // serialises cursor_update (window thread, input_block callers)
static volatile LONG g_block;              // text entry state, see input_block
static volatile LONG g_menu;               // in-game menu state, see input_menu
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

// Hide the cursor and confine it to the client area while the window is focused, not suspended, not behind a text entry
// or the menu, and the mouse camera is on; release it otherwise. Safe from any thread.
static void cursor_update(HWND h) {
    const char *in = cfg_get("TOTK_INPUT");
    AcquireSRWLockExclusive(&cur_lock);
    int want = h && cur_active && !(cur_rel & CUR_REL) && !cur_suspend && g_block != 1 && g_menu != 1 && mouse_sens_get() > 0 &&
               !(in && !strcmp(in, "none")) && GetForegroundWindow() == h && !IsIconic(h);
    RECT r;
    POINT tl = {0, 0};
    if (want && GetClientRect(h, &r) && ClientToScreen(h, &tl)) {
        RECT clip = {tl.x, tl.y, tl.x + r.right, tl.y + r.bottom};
        ClipCursor(&clip);
        if (!cur_captured) { InterlockedExchange(&mouse_dx, 0); InterlockedExchange(&mouse_dy, 0); }
        InterlockedExchange(&cur_captured, 1);
    } else {
        if (cur_captured) ClipCursor(NULL);
        InterlockedExchange(&cur_captured, 0);
    }
    ReleaseSRWLockExclusive(&cur_lock);
}

int input_cursor_released(void) { return (cur_rel & CUR_REL) != 0; }

void input_cursor_suspend(int on) {
    InterlockedExchange(&cur_suspend, on != 0);
    cursor_update(gfx_window_handle());
}

// Window procedure hook (gfx_window.c calls it first). Returns 1 with *res set when the message is fully handled.
int input_window_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res) {
    switch (m) {
    case WM_CREATE: {
        RAWINPUTDEVICE rid = {.usUsagePage = 1, .usUsage = 2, .dwFlags = 0, .hwndTarget = h};   // generic mouse
        if (!RegisterRawInputDevices(&rid, 1, sizeof rid)) rt_log("input: raw mouse registration failed (%lu)", GetLastError());
        break;
    }
    case WM_INPUT: {
        RAWINPUT ri;
        UINT sz = sizeof ri;
        if (cur_captured && GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            InterlockedAdd(&mouse_dx, ri.data.mouse.lLastX);
            InterlockedAdd(&mouse_dy, ri.data.mouse.lLastY);
        }
        break;   // DefWindowProc must see WM_INPUT for cleanup
    }
    case WM_SETCURSOR:
        if (cur_captured && LOWORD(l) == HTCLIENT) { SetCursor(NULL); *res = TRUE; return 1; }
        break;
    case WM_SYSCOMMAND:   // Alt pressed alone would enter the window menu loop, which stalls the game: Alt is a game key
        if ((w & 0xFFF0) == SC_KEYMENU && l == 0) { *res = 0; return 1; }
        break;
    case WM_KEYDOWN:   // Esc is unbound in the game layout: it releases the cursor and opens the menu (menu_window_msg)
        if (w == VK_ESCAPE && !(cur_rel & CUR_REL)) { cur_rel_update(rel_esc); cursor_update(h); }
        break;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:   // client area only; poll_merged drops the click
        if (cur_rel & CUR_REL) { cur_rel_update(rel_clear); cursor_update(h); }
        break;
    case WM_MOUSEACTIVATE:   // sent before WM_ACTIVATEAPP: a title bar or border click must not capture
        if (LOWORD(l) != HTCLIENT) cur_rel_update(rel_title);
        break;
    case WM_ACTIVATEAPP:
        if (!w) cur_rel_update(rel_clear);   // Alt+Tab back in captures again
        InterlockedExchange(&cur_active, w != 0);
        cursor_update(h);
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_SIZE: case WM_MOVE:
        cursor_update(h);
        break;
    case WM_ENTERSIZEMOVE:
        input_cursor_suspend(1);
        break;
    case WM_EXITSIZEMOVE:
        input_cursor_suspend(0);
        break;
    case WM_CLOSE:
        InterlockedExchange(&cur_active, 0);
        cursor_update(h);
        break;
    }
    return 0;
}

static int key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// The mouse camera as a right-stick sample, held for the whole presented frame. Under `lock`.
static void mouse_camera(int32_t out[2]) {
    static uint64_t frame = ~0ull;
    static LARGE_INTEGER t0;
    static int32_t held[2];
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    if (!cur_captured) {
        InterlockedExchange(&mouse_dx, 0);
        InterlockedExchange(&mouse_dy, 0);
        held[0] = held[1] = 0;
        frame = ~0ull;
    } else {
        uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        double age = frame == ~0ull ? 0 : (double)(now.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        if (fr != frame) {
            double dt = age < 0.004 ? 0.004 : age > 0.1 ? 0.1 : age;
            input_mouse_stick(InterlockedExchange(&mouse_dx, 0), InterlockedExchange(&mouse_dy, 0), dt, mouse_sens_get(), held);
            frame = fr;
            t0 = now;
        } else if (age > 0.1) {
            held[0] = held[1] = 0;   // frames stopped: do not hold a deflection
        }
    }
    out[0] = held[0];
    out[1] = held[1];
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
        InterlockedExchange(&pad_layout_shown, -1);
        for (int i = 0; i < 4; i++) InterlockedExchange(&pad_labels[i], PAD_LABEL_NONE);
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
        InterlockedExchange(&pad_labels[i], lb == SDL_GAMEPAD_BUTTON_LABEL_A          ? PAD_LABEL_A
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
    InterlockedExchange(&pad_layout_shown, pad_layout);
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

static void start(void) {
    binds_refresh();
    const char *e = cfg_get("TOTK_INPUT");
    if (e && !strcmp(e, "none")) { state = -1; rt_log("input: disabled (TOTK_INPUT=none)"); return; }
    if (!SDL_Init(SDL_INIT_GAMEPAD)) { state = -1; rt_log("input: SDL gamepad init failed (%s), keyboard only", SDL_GetError()); }
    else state = 1;
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
    if ((!pad || !SDL_GamepadConnected(pad)) && GetTickCount64() >= next_scan) { next_scan = GetTickCount64() + RESCAN_MS; scan_pads(); }
    SDL_UpdateGamepads();
    if (!pad || !SDL_GamepadConnected(pad)) return 0;
    for (int p = PAD_SOUTH; p < PAD_ZL; p++) pd[p] = SDL_GetGamepadButton(pad, pad_sdl[p]);
    pd[PAD_ZL] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TRIGGER_ON;
    pd[PAD_ZR] = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER_ON;
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
    AcquireSRWLockExclusive(&lock);
    if (state == 1 && pad && SDL_GamepadConnected(pad)) {   // the game's poll keeps the state fresh (SDL_UpdateGamepads)
        pct[0] = input_stick_percent(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX),
                                     SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
        pct[1] = input_stick_percent(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX),
                                     SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
        ok = 1;
    }
    ReleaseSRWLockExclusive(&lock);
    return ok;
}

static void poll_merged(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    uint64_t b = 0;
    int32_t sl[2] = {0, 0}, sr[2] = {0, 0};
    AcquireSRWLockExclusive(&lock);
    if (!state) start();
    if (state > 0) {
        binds_refresh();
        if (state == 1) pad_read(&b, sl, sr);
        HWND w = gfx_window_handle();
        if (w && w == GetForegroundWindow()) {
            uint8_t down[KEY_COUNT] = {0};
            // Mouse buttons count only while the cursor is captured, and a button already held when the capture began
            // (the click that refocused the window) is ignored until it is released.
            static int was_captured;
            static uint8_t mouse_ignore[KEY_COUNT];
            int cap = cur_captured != 0;
            for (int k = 1; k < KEY_COUNT; k++) {
                int vk = key_vk(k);
                down[k] = vk && key_down(vk);
                if (k < KEY_MOUSE_L || k > KEY_MOUSE_X2) continue;
                if (cap && !was_captured) mouse_ignore[k] = down[k];
                if (!down[k]) mouse_ignore[k] = 0;
                if (!cap || mouse_ignore[k]) down[k] = 0;
            }
            was_captured = cap;
            b |= input_map_buttons(down);
            int32_t k[2];
            input_map_stick(down, 0, k);
            pick(&sl[0], &sl[1], k[0], k[1]);
            input_map_stick(down, 1, k);
            pick(&sr[0], &sr[1], k[0], k[1]);
            mouse_camera(k);
            pick(&sr[0], &sr[1], k[0], k[1]);
        }
    }
    script_apply(&b, sl, sr);
    rec_apply(b, sl, sr);
    // Minus and Plus held together (gamepad or keys) open or close the menu and never reach the game
    static int combo_held;
    int32_t st[4] = {sl[0], sl[1], sr[0], sr[1]};
    int toggle = input_menu_gate(&combo_held, menu_is_open(), &b, st);
    sl[0] = st[0], sl[1] = st[1], sr[0] = st[2], sr[1] = st[3];
    ReleaseSRWLockExclusive(&lock);
    if (toggle) menu_toggle();
    stick_bits(&b, sl[0], sl[1], B_SL_LEFT);
    stick_bits(&b, sr[0], sr[1], B_SR_LEFT);
    *buttons = b; *lx = sl[0]; *ly = sl[1]; *rx = sr[0]; *ry = sr[1];
}

// Text entry (swkbd_win.c): while it is open the game reads no input, as behind the keyboard applet; after it closes
// the game reads none until every button and stick direction is released, so the press that closed it stays there.
// g_block (above): 0 off, 1 text entry open, 2 closed, waiting for the release. The cursor is released while it is open.
void input_block(int on) {
    InterlockedExchange(&g_block, on ? 1 : 2);
    cursor_update(gfx_window_handle());
}

// The in-game menu (ui/menu.cpp) blocks the game's input the same way: g_menu 0 closed, 1 open, 2 closed, waiting for
// the release. The cursor is released while it is open, for the menu's mouse.
void input_menu(int open) {
    InterlockedExchange(&g_menu, open ? 1 : 2);
    if (!open) cur_rel_update(rel_menu_closed);   // the capture before Esc comes back
    cursor_update(gfx_window_handle());
}

// 1 while flag (g_block, g_menu) blocks the game's input; state 2 ends at the first poll with nothing held.
static int blocked(volatile LONG *flag, uint64_t buttons) {
    LONG k = *flag;
    return k && !(k == 2 && !buttons && InterlockedCompareExchange(flag, 0, 2) == 2);
}

void input_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    poll_merged(buttons, lx, ly, rx, ry);
    int text = blocked(&g_block, *buttons), menu = blocked(&g_menu, *buttons);
    if (!text && !menu) return;
    *buttons = 0; *lx = *ly = *rx = *ry = 0;
}

void input_poll_ui(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    poll_merged(buttons, lx, ly, rx, ry);
}

int input_pad_read(uint64_t *buttons, int32_t *lx, int32_t *ly) {
    uint64_t b = 0;
    int32_t sl[2] = {0, 0}, sr[2] = {0, 0};
    AcquireSRWLockExclusive(&lock);
    if (!state) start();
    if (state > 0) binds_refresh();
    int ok = state == 1 && pad_read(&b, sl, sr);
    ReleaseSRWLockExclusive(&lock);
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
    return (int)InterlockedCompareExchange(&pad_layout_shown, 0, 0);
}

int input_pad_label(int pad_id) {
    if (pad_id < PAD_SOUTH || pad_id > PAD_NORTH) return PAD_LABEL_NONE;
    return (int)InterlockedCompareExchange(&pad_labels[pad_id - PAD_SOUTH], 0, 0);
}

void input_map_snapshot(InputMap *m) {
    AcquireSRWLockExclusive(&lock);
    if (!state) start();
    if (state > 0) binds_refresh();
    *m = input_map;
    ReleaseSRWLockExclusive(&lock);
}

int input_capture(int reset, int *id) {
    static uint8_t prev_k[KEY_COUNT], prev_p[PAD_COUNT];
    static int prev_esc;
    uint8_t k[KEY_COUNT] = {0}, p[PAD_COUNT] = {0};
    HWND w = gfx_window_handle();
    int fg = w && w == GetForegroundWindow();
    for (int i = 1; i < KEY_COUNT; i++) k[i] = fg && key_down(key_vk(i));
    int esc = fg && key_down(VK_ESCAPE);
    AcquireSRWLockExclusive(&lock);
    if (!state) start();
    if (state == 1) pad_physical(p);
    ReleaseSRWLockExclusive(&lock);
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
    AcquireSRWLockExclusive(&lock);
    if (state == 1 && pad && pad_rumble) {
        ok = SDL_RumbleGamepad(pad, low, high, ms);
        if (triggers) SDL_RumbleGamepadTriggers(pad, high, high, ms);   // unsupported pads return false: ignored
    }
    ReleaseSRWLockExclusive(&lock);
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
static DWORD WINAPI motion_main(void *arg) {
    (void)arg;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (;;) {
        if (timer) {
            LARGE_INTEGER due = {.QuadPart = -40000};   // 4 ms
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else Sleep(4);
        float a[3], g[3];
        int have = 0;
        AcquireSRWLockExclusive(&lock);
        if (pad && SDL_GamepadConnected(pad)) {
            SDL_UpdateGamepads();
            have = sensors_ready() && SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, a, 3) &&
                   SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, g, 3);
        }
        ReleaseSRWLockExclusive(&lock);
        if (!have || !input_motion_enabled()) continue;
        double acc[3], gyro[3];
        input_motion_axes(a, g, acc, gyro);
        motion_feed(motion_global(), acc, gyro, kern_now_ns());
    }
    return 0;
}

void input_motion_start(void) {
    AcquireSRWLockExclusive(&lock);
    if (!state) start();
    if (!motion_thread) {
        if (!input_motion_enabled()) rt_log("input: motion off (TOTK_MOTION=0); it can be turned on while the game runs");
        if (state != 1) motion_thread = -1;
        else {
            HANDLE h = CreateThread(NULL, 0, motion_main, NULL, 0, NULL);
            if (!h) rt_fatal("input: motion thread CreateThread failed (error %lu)", GetLastError());
            CloseHandle(h);
            motion_thread = 1;
        }
    }
    ReleaseSRWLockExclusive(&lock);
}
