// Between input.c (gamepad, merging, scripts) and the keyboard and mouse of the window system's back end (input_win.c
// with Win32, input_sdl.c with SDL). Any thread.
#pragma once
#include <stdint.h>

// ---- what the back end provides
int ikb_foreground(void);                     // the game window is in front: keys count
int ikb_down(int key);                        // key id (input_map.h KEY_*) held, mouse buttons included
int ikb_escape(void);                         // Esc held (no key id: it belongs to the menu)
int ikb_captured(void);                       // the cursor is captured for the mouse camera
void ikb_mouse_take(int32_t *dx, int32_t *dy);   // raw mouse motion since the last call, in counts
// Look deltas since the last call for mousecam (input_mouse_look_take): whole counts, wheel notches (120 = one
// notch), middle-click reset. 1 when the cursor is captured. Exclusive with ikb_mouse_take while the caller holds
// the mouse (mousecam_suppress_stick): the backends accumulate one set; whoever takes drains it.
int ikb_look_take(float *dx, float *dy, float *scroll, int *reset);
void ikb_cursor_update(void);                 // what input_cursor_allowed says may have changed
void ikb_menu_closed(void);                   // the menu closed: a capture that Esc gave up comes back

// ---- what input.c provides the back end
int input_cursor_allowed(void);               // no text entry or menu open, the mouse camera on, input not disabled
