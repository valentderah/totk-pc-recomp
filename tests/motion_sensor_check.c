// Build with SDL3 and runtime/motion_sensor.c; no game files are needed.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <SDL3/SDL.h>

#include "motion_sensor.h"

#define SECOND 1000000000ull

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

typedef struct { int fail, enable_calls, disable_calls; } VirtualSensor;

static bool SDLCALL enable_sensors(void *userdata, bool enabled) {
    VirtualSensor *v = userdata;
    if (enabled) {
        v->enable_calls++;
        if (v->fail) return SDL_SetError("simulated sensor failure");
    } else v->disable_calls++;
    return true;
}

static SDL_JoystickID attach(VirtualSensor *v, int sensors) {
    static const SDL_VirtualJoystickSensorDesc descs[] = {
        {SDL_SENSOR_ACCEL, 250}, {SDL_SENSOR_GYRO, 250},
    };
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.name = "Virtual motion controller";
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.nsensors = sensors ? 2 : 0;
    desc.sensors = sensors ? descs : NULL;
    desc.userdata = v;
    desc.SetSensorsEnabled = enable_sensors;
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    assert(id);
    return id;
}

static void send(SDL_Joystick *joy, SDL_SensorType type, const float data[3]) {
    assert(SDL_SendJoystickVirtualSensorData(joy, type, 0, data, 3));
    SDL_UpdateGamepads();
}

int main(void) {
    assert(SDL_Init(SDL_INIT_GAMEPAD));
    SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_SENSOR_UPDATE, false);
    VirtualSensor v = {.fail = 1};
    SDL_JoystickID id = attach(&v, 1);
    SDL_Gamepad *pad = SDL_OpenGamepad(id);
    SDL_Joystick *joy = SDL_OpenJoystick(id);
    assert(pad && joy);
    MotionSensor s = {0};
    float a[3], g[3];
    assert(!motion_sensor_read(&s, pad, SECOND, a, g));
    assert(v.enable_calls == 1 && s.retry_at == 2 * SECOND);
    assert(!motion_sensor_read(&s, pad, 2 * SECOND - 1, a, g));
    assert(v.enable_calls == 1);
    assert(!motion_sensor_read(&s, pad, 2 * SECOND, a, g));
    assert(s.retry_at == 4 * SECOND && s.retry_delay == 4 * SECOND);
    assert(!motion_sensor_read(&s, pad, 4 * SECOND, a, g));
    assert(s.retry_at == 8 * SECOND && s.retry_delay == 8 * SECOND);
    assert(!motion_sensor_read(&s, pad, 8 * SECOND, a, g));
    assert(s.retry_at == 16 * SECOND && s.retry_delay == 8 * SECOND);
    v.fail = 0;
    assert(!motion_sensor_read(&s, pad, 16 * SECOND, a, g));
    assert(s.ready && s.zero_pending);
    assert(!motion_sensor_read(&s, pad, 18 * SECOND, a, g));
    assert(!s.ready && s.retry_at == 26 * SECOND && v.disable_calls == 1);
    assert(!motion_sensor_read(&s, pad, 26 * SECOND, a, g));
    const float gravity[3] = {0, SDL_STANDARD_GRAVITY, 0}, turn[3] = {1, 2, 3};
    send(joy, SDL_SENSOR_ACCEL, gravity);
    send(joy, SDL_SENSOR_GYRO, turn);
    assert(motion_sensor_read(&s, pad, 26 * SECOND + 1, a, g));
    assert(a[1] == gravity[1] && g[0] == 1 && g[1] == 2 && g[2] == 3 && !s.retry_delay);
    assert(motion_sensor_read(&s, pad, 27 * SECOND, a, g));

    SDL_CloseJoystick(joy);
    SDL_CloseGamepad(pad);
    assert(SDL_DetachVirtualJoystick(id));
    VirtualSensor no_motion = {0};
    id = attach(&no_motion, 0);
    pad = SDL_OpenGamepad(id);
    assert(pad);
    assert(!motion_sensor_read(&s, pad, 28 * SECOND, a, g));
    assert(s.id == id && s.unsupported && !no_motion.enable_calls);
    SDL_CloseGamepad(pad);
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    puts("motion sensor retry, zero reports, recovery and reconnect passed");
    return 0;
}
