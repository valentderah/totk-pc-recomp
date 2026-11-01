// The SDL3 back end of the keyboard and the mouse (input_wsys.h; input.c merges them with the gamepad): SDL scancodes to
// the key ids of input_map.h, the mouse camera's relative motion and the cursor capture (SDL's relative mouse mode).
// Key and button state comes from the window's events (window thread) and is read by any thread. Keys and their map:
// docs/controls.md.
//
// The capture follows the Win32 back end's rules (input_win.c): captured while the window has the focus, the mouse
// camera is on and no text entry or menu is open; Esc releases it until a click in the window (and closing the menu
// opened by that Esc gives it back); losing the focus releases it until the window has the focus again.
#include <string.h>

#include <SDL3/SDL.h>

#include "host.h"
#include "input.h"
#include "input_map.h"
#include "input_sdl.h"
#include "input_wsys.h"
#include "platform/platform.h"

static SDL_Window *win;
static volatile uint8_t key_held[KEY_COUNT];
static volatile int32_t esc_held, f8_held, focused;
static volatile int32_t mouse_dx, mouse_dy;   // relative motion since the last ikb_mouse_take, whole counts
static float rem_x, rem_y;                    // window thread: the fractions SDL reported beyond them
static volatile int32_t captured;             // relative mouse mode is on
enum { CUR_REL = 1, CUR_ESC = 2 };            // as input_win.c: released by Esc or a click elsewhere; by Esc
static volatile int32_t cur_rel, cur_suspend, cur_pending;
static uint32_t wake_event;                   // SDL_RegisterEvents: wakes the window thread for a cursor change

// SDL scancode of a key id, 0 for an id without a key (the mouse buttons).
static SDL_Scancode key_sc(int key) {
    if (key >= KEY_A && key <= KEY_Z) return (SDL_Scancode)(SDL_SCANCODE_A + (key - KEY_A));
    if (key >= KEY_1 && key <= KEY_9) return (SDL_Scancode)(SDL_SCANCODE_1 + (key - KEY_1));
    if (key >= KEY_NUM1 && key <= KEY_NUM9) return (SDL_Scancode)(SDL_SCANCODE_KP_1 + (key - KEY_NUM1));
    switch (key) {
    case KEY_0: return SDL_SCANCODE_0;
    case KEY_NUM0: return SDL_SCANCODE_KP_0;
    case KEY_SPACE: return SDL_SCANCODE_SPACE;
    case KEY_TAB: return SDL_SCANCODE_TAB;
    case KEY_LSHIFT: return SDL_SCANCODE_LSHIFT;
    case KEY_LCTRL: return SDL_SCANCODE_LCTRL;
    case KEY_UP: return SDL_SCANCODE_UP;
    case KEY_DOWN: return SDL_SCANCODE_DOWN;
    case KEY_LEFT: return SDL_SCANCODE_LEFT;
    case KEY_RIGHT: return SDL_SCANCODE_RIGHT;
    case KEY_ENTER: return SDL_SCANCODE_RETURN;
    case KEY_BACKSPACE: return SDL_SCANCODE_BACKSPACE;
    case KEY_RSHIFT: return SDL_SCANCODE_RSHIFT;
    case KEY_RCTRL: return SDL_SCANCODE_RCTRL;
    case KEY_LALT: return SDL_SCANCODE_LALT;
    case KEY_CAPSLOCK: return SDL_SCANCODE_CAPSLOCK;
    case KEY_INSERT: return SDL_SCANCODE_INSERT;
    case KEY_DELETE: return SDL_SCANCODE_DELETE;
    case KEY_HOME: return SDL_SCANCODE_HOME;
    case KEY_END: return SDL_SCANCODE_END;
    case KEY_PAGEUP: return SDL_SCANCODE_PAGEUP;
    case KEY_PAGEDOWN: return SDL_SCANCODE_PAGEDOWN;
    case KEY_F2: return SDL_SCANCODE_F2;
    case KEY_F3: return SDL_SCANCODE_F3;
    case KEY_F4: return SDL_SCANCODE_F4;
    case KEY_F5: return SDL_SCANCODE_F5;
    case KEY_F6: return SDL_SCANCODE_F6;
    case KEY_F7: return SDL_SCANCODE_F7;
    case KEY_F9: return SDL_SCANCODE_F9;
    case KEY_F12: return SDL_SCANCODE_F12;
    case KEY_NUM_ADD: return SDL_SCANCODE_KP_PLUS;
    case KEY_NUM_SUB: return SDL_SCANCODE_KP_MINUS;
    case KEY_NUM_MUL: return SDL_SCANCODE_KP_MULTIPLY;
    case KEY_NUM_DIV: return SDL_SCANCODE_KP_DIVIDE;
    case KEY_NUM_DECIMAL: return SDL_SCANCODE_KP_PERIOD;
    case KEY_MINUS: return SDL_SCANCODE_MINUS;
    case KEY_EQUALS: return SDL_SCANCODE_EQUALS;
    case KEY_LBRACKET: return SDL_SCANCODE_LEFTBRACKET;
    case KEY_RBRACKET: return SDL_SCANCODE_RIGHTBRACKET;
    case KEY_SEMICOLON: return SDL_SCANCODE_SEMICOLON;
    case KEY_APOSTROPHE: return SDL_SCANCODE_APOSTROPHE;
    case KEY_COMMA: return SDL_SCANCODE_COMMA;
    case KEY_PERIOD: return SDL_SCANCODE_PERIOD;
    case KEY_SLASH: return SDL_SCANCODE_SLASH;
    case KEY_BACKSLASH: return SDL_SCANCODE_BACKSLASH;
    case KEY_GRAVE: return SDL_SCANCODE_GRAVE;
    }
    return SDL_SCANCODE_UNKNOWN;
}

// The key id of a scancode (the inverse of key_sc, built on first use), 0 for none.
static int key_of_sc(SDL_Scancode sc) {
    static uint8_t map[SDL_SCANCODE_COUNT];
    static int built;
    if (!built) {
        for (int k = 1; k < KEY_COUNT; k++) {
            SDL_Scancode s = key_sc(k);
            if (s != SDL_SCANCODE_UNKNOWN) map[s] = (uint8_t)k;
        }
        built = 1;
    }
    return sc > 0 && sc < SDL_SCANCODE_COUNT ? map[sc] : 0;
}

static int mouse_key(uint8_t button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return KEY_MOUSE_L;
    case SDL_BUTTON_RIGHT: return KEY_MOUSE_R;
    case SDL_BUTTON_MIDDLE: return KEY_MOUSE_M;
    case SDL_BUTTON_X1: return KEY_MOUSE_X1;
    case SDL_BUTTON_X2: return KEY_MOUSE_X2;
    }
    return 0;
}

uint32_t input_key_char(int key) {
    if (key < KEY_MINUS || key > KEY_GRAVE) return 0;
    SDL_Keycode k = SDL_GetKeyFromScancode(key_sc(key), SDL_KMOD_NONE, false);
    return k & SDLK_SCANCODE_MASK ? 0 : (uint32_t)k;   // a keycode with the mask bit names no character
}

int input_flight_key(void) { return focused && f8_held; }

// ---- the cursor (window thread)
static void cursor_apply(void) {
    __atomic_store_n(&cur_pending, 0, __ATOMIC_RELAXED);
    if (!win) return;
    int want = focused && !(cur_rel & CUR_REL) && !cur_suspend && input_cursor_allowed() &&
               !(SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED);
    if (want == captured) return;
    if (!SDL_SetWindowRelativeMouseMode(win, want)) {
        rt_log("input: relative mouse mode %s failed (%s)", want ? "on" : "off", SDL_GetError());
        return;
    }
    plat_xchg32(&mouse_dx, 0);
    plat_xchg32(&mouse_dy, 0);
    rem_x = rem_y = 0;
    plat_xchg32(&captured, want);
}

// Any thread: the window thread applies the change at its next event.
void ikb_cursor_update(void) {
    plat_xchg32(&cur_pending, 1);
    if (wake_event) {
        SDL_Event e = {.user = {.type = wake_event}};
        SDL_PushEvent(&e);
    }
}

void input_cursor_suspend(int on) {
    plat_xchg32(&cur_suspend, on != 0);
    ikb_cursor_update();
}

int input_cursor_released(void) { return (cur_rel & CUR_REL) != 0; }

void ikb_menu_closed(void) {
    int32_t o = cur_rel;
    if (o & CUR_ESC) plat_cas32(&cur_rel, 0, o);
}

void input_sdl_window(SDL_Window *w) {
    win = w;
    wake_event = SDL_RegisterEvents(1);
    plat_xchg32(&focused, (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) != 0);
    cursor_apply();
}

void input_sdl_idle(void) {
    if (cur_pending) cursor_apply();
}

void input_sdl_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        int down = e->type == SDL_EVENT_KEY_DOWN;
        if (e->key.scancode == SDL_SCANCODE_ESCAPE) {
            plat_xchg32(&esc_held, down);
            // Esc is unbound in the game layout: it releases the cursor and opens the menu (ui/menu_sdl.cpp)
            if (down && !e->key.repeat && !(cur_rel & CUR_REL)) plat_xchg32(&cur_rel, CUR_REL | CUR_ESC);
        }
        if (e->key.scancode == SDL_SCANCODE_F8) plat_xchg32(&f8_held, down);
        int k = key_of_sc(e->key.scancode);
        if (k) key_held[k] = (uint8_t)down;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        int down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        int k = mouse_key(e->button.button);
        if (k) key_held[k] = (uint8_t)down;
        if (down && (cur_rel & CUR_REL)) plat_xchg32(&cur_rel, 0);   // a click in the window captures again
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (captured) {
            rem_x += e->motion.xrel;
            rem_y += e->motion.yrel;
            int32_t dx = (int32_t)rem_x, dy = (int32_t)rem_y;
            rem_x -= (float)dx, rem_y -= (float)dy;
            if (dx) plat_add32(&mouse_dx, dx);
            if (dy) plat_add32(&mouse_dy, dy);
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        plat_xchg32(&focused, 1);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        plat_xchg32(&focused, 0);
        plat_xchg32(&cur_rel, 0);   // coming back (Alt+Tab) captures again
        memset((void *)key_held, 0, sizeof key_held);   // the key-ups go elsewhere
        plat_xchg32(&esc_held, 0);
        plat_xchg32(&f8_held, 0);
        break;
    }
    cursor_apply();
}

// ---- input_wsys.h
int ikb_foreground(void) { return focused != 0; }
int ikb_down(int key) { return key > 0 && key < KEY_COUNT && key_held[key]; }
int ikb_escape(void) { return esc_held != 0; }
int ikb_captured(void) { return captured != 0; }

void ikb_mouse_take(int32_t *dx, int32_t *dy) {
    *dx = plat_xchg32(&mouse_dx, 0);
    *dy = plat_xchg32(&mouse_dy, 0);
}
