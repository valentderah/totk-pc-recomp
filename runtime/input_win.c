// The Win32 back end of the keyboard and the mouse (input_wsys.h; input.c merges them with the gamepad): Win32 virtual
// keys to the key ids of input_map.h, the mouse camera's raw motion (raw input, WM_INPUT) and the cursor capture.
// Keys and their map: docs/controls.md.
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_win.h"
#include "input_map.h"
#include "mousecam.h"
#include "nvn_api.h"
#include "ui/menu.h"
#include "vk.h"

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
static volatile LONG cam_dx, cam_dy, cam_scroll, cam_reset;   // mouselook: counts, raw wheel delta (120 per notch), middle click
static volatile LONG pad_rs_x, pad_rs_y;                       // right stick without the mouse, for the camera hook
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
// Hide the cursor and confine it to the client area while the window is focused, not suspended, not behind a text entry
// or the menu, and the mouse camera is on; release it otherwise. Safe from any thread.
static void cursor_update(HWND h) {
    AcquireSRWLockExclusive(&cur_lock);
    int want = h && cur_active && !(cur_rel & CUR_REL) && !cur_suspend && input_cursor_allowed() &&
               GetForegroundWindow() == h && !IsIconic(h);
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

// Window procedure hook (gfx_window_win.c calls it first). Returns 1 with *res set when the message is fully handled.
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
            InterlockedAdd(&cam_dx, ri.data.mouse.lLastX);
            InterlockedAdd(&cam_dy, ri.data.mouse.lLastY);
        }
        break;   // DefWindowProc must see WM_INPUT for cleanup
    }
    case WM_MOUSEWHEEL:
        if (cur_captured) InterlockedAdd(&cam_scroll, GET_WHEEL_DELTA_WPARAM(w));
        break;
    case WM_SETCURSOR:
        if (cur_captured && LOWORD(l) == HTCLIENT) { SetCursor(NULL); *res = TRUE; return 1; }
        break;
    case WM_SYSCOMMAND:   // Alt pressed alone would enter the window menu loop, which stalls the game: Alt is a game key
        if ((w & 0xFFF0) == SC_KEYMENU && l == 0) { *res = 0; return 1; }
        break;
    case WM_KEYDOWN:   // Esc is unbound in the game layout: it releases the cursor and opens the menu (menu_window_msg)
        if (w == VK_ESCAPE && !(cur_rel & CUR_REL)) { cur_rel_update(rel_esc); cursor_update(h); }
        if (w == VK_F3 && !(l & (1 << 30))) mousecam_toggle();
        break;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:   // client area only; poll_merged drops the click
        if (m == WM_MBUTTONDOWN && cur_captured) InterlockedExchange(&cam_reset, 1);
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
    static MouseCamState cam_state;
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
        cam_state.carry[0] = cam_state.carry[1] = 0;
        frame = ~0ull;
    } else {
        uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        double age = frame == ~0ull ? 0 : (double)(now.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        if (fr != frame) {
            double dt = age < 0.004 ? 0.004 : age > 0.1 ? 0.1 : age;
            const int32_t mdx = InterlockedExchange(&mouse_dx, 0), mdy = InterlockedExchange(&mouse_dy, 0);
            const char *mode = cfg_value("TOTK_MOUSE_MODE");
            if (mode && !strcmp(mode, "linear")) {   // the old mapping: speed to deflection, through the game's curve
                cam_state.carry[0] = cam_state.carry[1] = 0;
                input_mouse_stick(mdx, mdy, dt, mouse_sens_get(), held);
            } else {
                MouseCamCfg cc = {mouse_sens_get(), (float)cfg_double("TOTK_MOUSE_Y_RATIO"), (float)cfg_double("TOTK_MOUSE_CURVE"),
                                  (float)cfg_double("TOTK_MOUSE_DEADZONE"), (float)cfg_double("TOTK_MOUSE_RATE")};
                input_mouse_camera(&cam_state, mdx, mdy, dt, &cc, held);
            }
            frame = fr;
            t0 = now;
        } else if (age > 0.1) {
            held[0] = held[1] = 0;   // frames stopped: do not hold a deflection
        }
    }
    int quiet = mousecam_suppress_stick();   // the camera hook rotates the camera: the stick must not as well
    out[0] = quiet ? 0 : held[0];
    out[1] = quiet ? 0 : held[1];
}

static int32_t axis(int16_t v) {
    int32_t a = v < -STICK_MAX ? -STICK_MAX : v;
    return a > -STICK_DEAD && a < STICK_DEAD ? 0 : a;
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
    sl[0] = axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX));
    sl[1] = -axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY));
    sr[0] = axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX));
    sr[1] = -axis(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY));
    return 1;
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
            InterlockedExchange(&pad_rs_x, sr[0]);
            InterlockedExchange(&pad_rs_y, sr[1]);
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
    return w && w == GetForegroundWindow() && key_down(VK_F8);
}

// ---- input_wsys.h
int ikb_foreground(void) {
    HWND w = gfx_window_handle();
    return w && w == GetForegroundWindow();
}

int ikb_down(int key) {
    int vk = key_vk(key);
    return vk && key_down(vk);
}

int ikb_escape(void) { return key_down(VK_ESCAPE); }

int ikb_captured(void) { return cur_captured != 0; }

void ikb_mouse_take(int32_t *dx, int32_t *dy) {
    *dx = InterlockedExchange(&mouse_dx, 0);
    *dy = InterlockedExchange(&mouse_dy, 0);
}

void ikb_cursor_update(void) { cursor_update(gfx_window_handle()); }

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

// Mouselook (mousecam.c): counts, wheel notches and the middle click since the last call.
int input_mouse_look_take(float *dx, float *dy, float *scroll, int *reset) {
    LONG x = InterlockedExchange(&cam_dx, 0), y = InterlockedExchange(&cam_dy, 0);
    LONG s = InterlockedExchange(&cam_scroll, 0), r = InterlockedExchange(&cam_reset, 0);
    if (!cur_captured) { *dx = *dy = *scroll = 0; *reset = 0; return 0; }
    *dx = (float)x; *dy = (float)y; *scroll = (float)s / WHEEL_DELTA; *reset = r != 0;
    return 1;
}
float input_mouse_sens(void) { return mouse_sens_get(); }
void input_stick_r_pad(int32_t out[2]) { out[0] = pad_rs_x; out[1] = pad_rs_y; }
