// Between input.c (gamepad, merging, scripts) and the keyboard and mouse of the window system's back end (input_win.c
// with Win32, input_sdl.c with SDL). Any thread.
#pragma once
#include <stdint.h>

// ---- what the back end provides
int ikb_foreground(void);                     // the game window is in front: keys count
int ikb_down(int key);                        // key id (input_map.h KEY_*) held, mouse buttons included
int ikb_escape(void);                         // Esc held (no key id: it belongs to the menu)
// The pointer in the client area, 0..1 from its top left; 0 when the window is not in front or the pointer is outside
int ikb_pointer(float *nx, float *ny);
void ikb_pointer_set(float nx, float ny);   // moves the pointer there (the window in front only)
int ikb_captured(void);                       // the cursor is captured for the mouse camera
void ikb_mouse_take(int32_t *dx, int32_t *dy);   // raw mouse motion since the last call, in counts
uint32_t ikb_mouse_travel(void);              // raw counts the mouse moved in total (|dx| + |dy|), captured or not
int32_t ikb_wheel_take(void);                 // whole wheel notches turned since the last call while the cursor was
                                              // captured or over the focused window (+ away from the player); a
                                              // high-resolution wheel's part of a notch is kept
void ikb_cursor_update(void);                 // what input_cursor_allowed says may have changed
void ikb_menu_closed(void);                   // the menu closed: a capture that Esc gave up comes back

// ---- shared by the back ends
// cursor_art.c: the pointer's image at size x size pixels (0xAARRGGBB, straight alpha, rows from the top) and its
// hotspot (*hot, the same in x and y). Pure.
void cursor_art(uint32_t *argb, int size, int *hot);

// ---- what input.c provides the back end
int input_cursor_allowed(void);               // no text entry or menu open, the mouse camera on, input not disabled
