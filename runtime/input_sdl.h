// The SDL3 window's input seams (Linux and other hosts without Win32): gfx_window_sdl.c owns the window and its event
// loop, input_sdl.c the keyboard, the mouse and the cursor, ui/menu_sdl.cpp the menu's events. Window thread, except
// where noted.
#pragma once
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

// gfx_window_sdl.c
SDL_Window *gfx_window_sdl(void);         // the game window, NULL before gfx_window_start; any thread
double gfx_window_sdl_density(void);      // pixels per window unit (mouse positions are in units); any thread

// input_sdl.c
void input_sdl_window(SDL_Window *w);     // the game window was created
void input_sdl_event(const SDL_Event *e); // every event, first
void input_sdl_idle(void);                // no event for a while: a cursor change asked for from another thread

// ui/menu_sdl.cpp
void menu_sdl_event(const SDL_Event *e);  // every event, after input_sdl_event

#ifdef __cplusplus
}
#endif
