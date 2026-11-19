// The SDL3 back end of the keyboard and the mouse (input_wsys.h; input.c merges them with the gamepad): SDL scancodes to
// the key ids of input_map.h, the mouse camera's relative motion, the cursor capture (SDL's relative mouse mode), the
// pointer's place in the window (the game's menus, menu_cursor.c) and the mouse travel (the last-device switch,
// input_kbm_last). Key and button state comes from the window's events (window thread) and is read by any thread. Keys
// and their map: docs/controls.md.
//
// The capture follows the Win32 back end's rules (input_win.c): captured while the window has the focus, the mouse
// camera is on and no text entry or menu is open; Esc releases it until a click in the window (and closing the menu
// opened by that Esc gives it back); losing the focus releases it until the window has the focus again.
#include <math.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "input_sdl.h"
#include "input_wsys.h"
#include "gameplay_tweaks.h"
#include "platform/platform.h"

static SDL_Window *win;
static volatile uint8_t key_held[KEY_COUNT];
static volatile int32_t esc_held, f8_held, focused;
static volatile int32_t mouse_dx, mouse_dy;   // relative motion since the last ikb_mouse_take, whole counts
static volatile int32_t mouse_wheel;          // wheel turned while captured or over the window, in 1/120 notches
                                              // (ikb_wheel_take)
static float rem_x, rem_y;                    // window thread: the fractions SDL reported beyond them
static volatile int32_t mouse_travel;         // raw counts moved in total, captured or not (ikb_mouse_travel)
static float travel_rem;                      // window thread: the fraction of a count beyond mouse_travel
static volatile int32_t captured;             // relative mouse mode is on
// The pointer's place in the window, as fractions of its size packed 32.32 fixed point (x in the high word), written
// by the window thread from the mouse events and read by the game thread (menu_cursor.c through ikb_pointer).
// ptr_have is 0 whenever the pointer is not over the window, as the Win32 back end's client-rect test.
static volatile uint64_t ptr_pos;
static volatile int32_t ptr_have;
static volatile int32_t warp_pending;         // ikb_pointer_set asked the window thread to move the pointer
static float warp_nx, warp_ny;                // its target, read by the window thread once warp_pending is seen
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
    if (!want) gameplay_tweaks_reset();
}

// The pointer's place in the window (the window thread): x and y are SDL window units, the same space the mouse
// events report, so dividing by the window's size makes fractions of it whatever the pixel density is. At and past
// the edges the fraction is clamped to 0..1, where a letterboxed picture covers no item anyway.
static void pointer_store(float x, float y) {
    if (!win) return;
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    if (w < 1 || h < 1) return;
    double fx = x / w, fy = y / h;
    if (!(fx >= 0)) fx = 0;   // also NaN
    else if (fx > 1) fx = 1;
    if (!(fy >= 0)) fy = 0;
    else if (fy > 1) fy = 1;
    __atomic_store_n(&ptr_pos, (uint64_t)(uint32_t)(fx * 4294967295.0) << 32 | (uint32_t)(fy * 4294967295.0),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&ptr_have, 1, __ATOMIC_RELEASE);
}

// A warp asked for by ikb_pointer_set, done here as every window call: on the window thread, and after the cursor
// change of the same menu opening has been applied. SDL's warp makes a motion event where it works (X11); where it
// does not (some Wayland compositors) the stored place stands, so the item under the pointer is the place asked for
// even if the visible cursor stayed behind.
static void pointer_apply(void) {
    if (!plat_xchg32(&warp_pending, 0)) return;
    if (!win || !focused) return;
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    if (w < 1 || h < 1) return;
    float x = warp_nx * w, y = warp_ny * h;
    SDL_WarpMouseInWindow(win, x, y);
    pointer_store(x, y);
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

// pointer_image = 1: the pointer over the window while the cursor is not captured is cursor_art.c's, sized by the
// window's display scale; 0 (the default) keeps SDL's arrow. Window thread, at the window's creation and whenever the
// settings change (input_sdl_idle), so the menu switches it live. SDL's arrow stays when SDL refuses the image.
static void art_cursor(SDL_Window *w) {
    static SDL_Cursor *art;
    static int shown = -1, failed;
    static unsigned seen = ~0u;
    unsigned v = cfg_version();
    if (v == seen && shown >= 0) return;
    seen = v;
    int want = cfg_bool("TOTK_POINTER_IMAGE");
    if (want == shown) return;
    if (want && !art && !failed) {
        float scale = SDL_GetWindowDisplayScale(w);
        int size = (int)lrintf(32 * (scale > 0 ? scale : 1)), hot;
        size = size < 32 ? 32 : size > 128 ? 128 : size;
        SDL_Surface *s = SDL_CreateSurface(size, size, SDL_PIXELFORMAT_ARGB8888);
        if (s && s->pitch == size * 4) {
            cursor_art(s->pixels, size, &hot);
            art = SDL_CreateColorCursor(s, hot, hot);
        }
        if (s) SDL_DestroySurface(s);
        if (!art) failed = 1, rt_log("input: the pointer's image failed (%s); the arrow stays", SDL_GetError());
    }
    SDL_SetCursor(want && art ? art : SDL_GetDefaultCursor());
    shown = want;
}

void input_sdl_window(SDL_Window *w) {
    win = w;
    art_cursor(w);
    wake_event = SDL_RegisterEvents(1);
    plat_xchg32(&focused, (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) != 0);
    __atomic_store_n(&ptr_have, 0, __ATOMIC_RELEASE);   // the pointer's place is unknown until it enters or moves
    cursor_apply();
}

void input_sdl_idle(void) {
    if (cur_pending) cursor_apply();
    if (win) art_cursor(win);
    pointer_apply();
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
        if (!captured) pointer_store(e->button.x, e->button.y);   // a click can come without a motion first
        break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        // The travel counts any motion, captured or not, as the Win32 back end's raw input does: it is what makes a
        // moved mouse the device used last, so the menu pointer appears without a click or a key.
        travel_rem += fabsf(e->motion.xrel) + fabsf(e->motion.yrel);
        int32_t t = (int32_t)travel_rem;
        if (t) {
            travel_rem -= t;
            plat_add32(&mouse_travel, t);
        }
        if (!captured) pointer_store(e->motion.x, e->motion.y);   // in relative mode x and y are not the pointer's
        if (captured) {
            rem_x += e->motion.xrel;
            rem_y += e->motion.yrel;
            int32_t dx = (int32_t)rem_x, dy = (int32_t)rem_y;
            rem_x -= (float)dx, rem_y -= (float)dy;
            if (dx) plat_add32(&mouse_dx, dx);
            if (dy) plat_add32(&mouse_dy, dy);
        }
        break;
    }
    case SDL_EVENT_WINDOW_MOUSE_ENTER: {   // entering without moving yet: the place is where the enter found the pointer
        float x, y;
        SDL_GetMouseState(&x, &y);        // the window with mouse focus is this one during its own enter event
        pointer_store(x, y);
        break;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        __atomic_store_n(&ptr_have, 0, __ATOMIC_RELEASE);
        break;
    case SDL_EVENT_MOUSE_WHEEL:   // notches, + away from the player (a flipped wheel reports the other sign)
        // Captured, or released over the focused window: a game menu with the pointer takes it (input.c)
        if (captured || (focused && __atomic_load_n(&ptr_have, __ATOMIC_ACQUIRE))) {
            float y = e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y;
            plat_add32(&mouse_wheel, (int32_t)(y * 120));
        }
        if (input_gameplay_active()) {
            double delta = e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? e->wheel.y : -e->wheel.y;
            gameplay_tweaks_wheel(delta, ikb_down(KEY_C), ikb_down(KEY_MOUSE_R));
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
        __atomic_store_n(&ptr_have, 0, __ATOMIC_RELEASE);   // the pointer left with the focus, leave or not
        break;
    }
    cursor_apply();
    pointer_apply();   // after cursor_apply: a menu opening uncaptures first, then the pointer moves to its middle
}

// ---- input_wsys.h
int ikb_foreground(void) { return focused != 0; }
int ikb_down(int key) { return key > 0 && key < KEY_COUNT && key_held[key]; }
int ikb_escape(void) { return esc_held != 0; }
int ikb_captured(void) { return captured != 0; }

// The pointer's place in the window as fractions of its size (menu_cursor.c maps them onto the picture the present
// drew). 0 whenever the pointer is not over this window or the window has not the focus, as the Win32 back end's
// GetCursorPos test.
int ikb_pointer(float *nx, float *ny) {
    if (!focused || !__atomic_load_n(&ptr_have, __ATOMIC_ACQUIRE)) return 0;
    uint64_t p = __atomic_load_n(&ptr_pos, __ATOMIC_ACQUIRE);
    *nx = (float)(uint32_t)(p >> 32) / 4294967295.0f;
    *ny = (float)(uint32_t)p / 4294967295.0f;
    return 1;
}

// Moves the pointer there (menu_cursor.c: to the middle when a menu opens). The window thread warps at its next
// event, as SDL asks for its video thread; until then the warp target stands as the pointer's place.
void ikb_pointer_set(float nx, float ny) {
    if (!(nx >= 0 && nx <= 1) || !(ny >= 0 && ny <= 1)) return;
    warp_nx = nx, warp_ny = ny;
    __atomic_store_n(&warp_pending, 1, __ATOMIC_RELEASE);
    if (wake_event) {
        SDL_Event e = {.user = {.type = wake_event}};
        SDL_PushEvent(&e);
    }
}

uint32_t ikb_mouse_travel(void) { return (uint32_t)__atomic_load_n(&mouse_travel, __ATOMIC_RELAXED); }

void ikb_mouse_take(int32_t *dx, int32_t *dy) {
    *dx = plat_xchg32(&mouse_dx, 0);
    *dy = plat_xchg32(&mouse_dy, 0);
}

int32_t ikb_wheel_take(void) {
    int32_t o, n;
    do {
        o = mouse_wheel;
        n = o % 120;   // the part of a notch stays for the next call
    } while (!__atomic_compare_exchange_n(&mouse_wheel, &o, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    return o / 120;
}
