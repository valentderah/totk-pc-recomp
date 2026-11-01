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
#include "input_wsys.h"
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

int input_flight_key(void) {
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

void ikb_menu_closed(void) { cur_rel_update(rel_menu_closed); }
