#include "rsf_input_gamepad.h"
#include "rsf_input.h"
static SDL_JoystickID selected;
static bool SDLCALL received(void *user,SDL_Event *event) {
    (void)user;
    SDL_JoystickID id=0;
    switch (event->type) {
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: id=event->gaxis.which; break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: id=event->gbutton.which; break;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN: case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION: case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
        id=event->gtouchpad.which; break;
    default: return true;
    }
    SDL_JoystickID active=__atomic_load_n(&selected,__ATOMIC_ACQUIRE);
    if (id && id==active) rsf_input_received(RSF_INPUT_CONTROLLER);
    return true; // Observes delivery; never filters or removes SDL's event.
}
void rsf_input_gamepad_select(SDL_JoystickID id) { __atomic_store_n(&selected,id,__ATOMIC_RELEASE); }
int rsf_input_gamepad_watch_start(void) {
    // Input startup is serialized by its owner. No borrowed pad/user pointer is
    // captured; SDL may invoke this process-lifetime callback on another thread.
    static int installed;
    if (!installed) installed=SDL_AddEventWatch(received,NULL);
    return installed;
}

void rsf_input_sdl_dequeued(const SDL_Event *e,SDL_WindowID window) {
    if (!window) return;
    switch(e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP:
        if(e->key.windowID==window) rsf_input_received(RSF_INPUT_KEYBOARD); break;
    case SDL_EVENT_MOUSE_MOTION:
        if(e->motion.windowID==window) rsf_input_received(RSF_INPUT_MOUSE); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        if(e->button.windowID==window) rsf_input_received(RSF_INPUT_MOUSE); break;
    case SDL_EVENT_MOUSE_WHEEL:
        if(e->wheel.windowID==window) rsf_input_received(RSF_INPUT_MOUSE); break;
    }
}
