// Mouselook for the camera (runtime/mousecam.c), port of mcrib884/totk-mousecam.
#pragma once
#include <stdint.h>

// input_win.c asks: should the mouse stay out of the right stick? 1 while the camera hook is rotating the camera
// (the hook ran within the last 250 ms with the cursor captured and the game not paused). In menus, with the hook off or
// with F3 off it is 0, and the old mouse-to-stick path (docs/controls.md "Mouse camera") serves the cursor.
int mousecam_suppress_stick(void);

// F3 (window thread): mouselook on or off. Off gives the old mouse-to-stick path back.
void mousecam_toggle(void);
