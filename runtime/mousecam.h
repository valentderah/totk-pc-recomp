// Mouselook for the camera (runtime/mousecam.c): mousecam_toggle / mousecam_suppress_stick /
// input_mouse_look_take follow the mcrib884/totk-mousecam camera-maths idea (GPL-2.0) as
// a recomp host function; this file does not contain that mod's code.
#pragma once
#include <stdint.h>

// input.c / input_win.c: should the mouse stay out of the right stick? 1 while the camera hook is rotating the camera
// (the hook ran within the last 250 ms with the cursor captured and the game not paused). In menus, with the hook off or
// with F3 off it is 0, and the old mouse-to-stick path (docs/controls.md "Mouse camera") serves the cursor.
int mousecam_suppress_stick(void);

// F3 (input poll): mouselook on or off. Off gives the old mouse-to-stick path back.
void mousecam_toggle(void);

// Raw look deltas since the last take (input_win.c / input_sdl.c): whole mouse counts, wheel notches, middle-click
// reset. Returns 1 while the cursor is captured for the mouse camera (the hook may rewrite the camera), 0 otherwise
// (deltas are dropped).
int input_mouse_look_take(float *dx, float *dy, float *scroll, int *reset);

// TOTK_MOUSE_SENS (0 = mouse camera off; live). Same scale the stick path uses.
float input_mouse_sens(void);

// Right-stick sample for absolute mode (gamepad only, -32767..32767).
void input_stick_r_pad(int32_t rs[2]);
