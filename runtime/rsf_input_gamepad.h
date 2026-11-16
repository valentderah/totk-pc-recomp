#pragma once
#include <SDL3/SDL.h>
// Called by the input owner; callback state has process lifetime and takes no input lock.
void rsf_input_gamepad_select(SDL_JoystickID id);
int rsf_input_gamepad_watch_start(void);

void rsf_input_sdl_dequeued(const SDL_Event *event, SDL_WindowID window);
