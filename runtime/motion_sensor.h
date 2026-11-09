#pragma once
#include <stdint.h>
#include <SDL3/SDL.h>

typedef struct {
    SDL_JoystickID id;
    uint64_t retry_at, zero_since, retry_delay;
    int ready, unsupported, zero_pending;
} MotionSensor;

// Called with the input lock held. Returns one when both samples are usable.
int motion_sensor_read(MotionSensor *s, SDL_Gamepad *pad, uint64_t now, float acc[3], float gyro[3]);
void motion_sensor_reset(MotionSensor *s);
