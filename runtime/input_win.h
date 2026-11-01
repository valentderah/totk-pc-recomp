// The Win32 window's input seams: gfx_window_win.c's window procedure calls these first. Windows only.
#pragma once
#include <windows.h>

HWND gfx_window_handle(void);   // gfx_window_win.c; NULL before gfx_window_start
int input_window_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res);
// The menu's Win32 adapter (ui/menu_win.cpp), called next: F1 / Esc open or close the menu; while it is open, keys,
// mouse and focus go to it. Returns 1 with *res set when the message is fully handled.
int menu_window_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res);
