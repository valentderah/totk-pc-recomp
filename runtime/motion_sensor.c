#include <string.h>

#include "host.h"
#include "motion_sensor.h"

#define NS_SECOND 1000000000ull
#define MAX_RETRY (8 * NS_SECOND)

void motion_sensor_reset(MotionSensor *s) {
    memset(s, 0, sizeof *s);
}

static void retry(MotionSensor *s, SDL_Gamepad *pad, uint64_t now) {
    if (SDL_GamepadSensorEnabled(pad, SDL_SENSOR_GYRO) &&
        !SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, false))
        rt_log("input: could not disable gyro (%s)", SDL_GetError());
    if (SDL_GamepadSensorEnabled(pad, SDL_SENSOR_ACCEL) &&
        !SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, false))
        rt_log("input: could not disable accelerometer (%s)", SDL_GetError());
    s->ready = 0;
    s->zero_pending = 0;
    if (!s->retry_delay) s->retry_delay = NS_SECOND;
    s->retry_at = now + s->retry_delay;
    if (s->retry_delay < MAX_RETRY) s->retry_delay *= 2;
}

int motion_sensor_read(MotionSensor *s, SDL_Gamepad *pad, uint64_t now, float acc[3], float gyro[3]) {
    SDL_JoystickID id = SDL_GetGamepadID(pad);
    if (id != s->id) {
        motion_sensor_reset(s);
        s->id = id;
        if (!SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) || !SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) {
            s->unsupported = 1;
            rt_log("input: \"%s\" has no gyro and accelerometer: the six-axis sensor reads a controller at rest",
                   SDL_GetGamepadName(pad));
        }
    }
    if (s->unsupported || (!s->ready && now < s->retry_at)) return 0;
    if (!s->ready) {
        if (!SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true)) {
            rt_log("input: could not enable gyro (%s); retrying", SDL_GetError());
            retry(s, pad, now);
            return 0;
        }
        if (!SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true)) {
            rt_log("input: could not enable accelerometer (%s); retrying", SDL_GetError());
            retry(s, pad, now);
            return 0;
        }
        s->ready = 1;
        rt_log("input: motion sensors on (gyro %.0f Hz, accelerometer %.0f Hz)",
               (double)SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_GYRO),
               (double)SDL_GetGamepadSensorDataRate(pad, SDL_SENSOR_ACCEL));
    }
    if (!SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, acc, 3) ||
        !SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, gyro, 3)) {
        rt_log("input: could not read motion sensors (%s); retrying", SDL_GetError());
        retry(s, pad, now);
        return 0;
    }
    if (acc[0] == 0 && acc[1] == 0 && acc[2] == 0 && gyro[0] == 0 && gyro[1] == 0 && gyro[2] == 0) {
        if (!s->zero_pending) {
            s->zero_pending = 1;
            s->zero_since = now;
        } else if (now - s->zero_since >= 2 * NS_SECOND) {
            rt_log("input: motion sensors report only zeros; restarting them");
            retry(s, pad, now);
        }
        return 0;
    }
    s->zero_pending = 0;
    s->retry_delay = 0;
    return 1;
}
