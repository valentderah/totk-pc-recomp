#ifndef TOTK_MOUSE_INPUT_H
#define TOTK_MOUSE_INPUT_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t buttons;
    int32_t x, y;
} NativeMouseSample;

typedef struct { double yaw, pitch; } NativeMouseTurn;

// The input owner serializes events and samples with its existing input lock.
typedef struct {
    double dx, dy, sensitivity;
    uint64_t held, pressed, frame, sampled_ns, moved_ns, camera_frame;
    NativeMouseSample sample;
    bool captured, invert_y, has_frame, direct, has_camera, stick_override;
} NativeMouseInput;

void native_mouse_configure(NativeMouseInput *, double sensitivity, bool invert_y);
void native_mouse_clear(NativeMouseInput *, uint64_t now_ns);
void native_mouse_capture(NativeMouseInput *, bool captured, uint64_t now_ns);
void native_mouse_camera(NativeMouseInput *, uint64_t frame, bool active);
bool native_mouse_motion(NativeMouseInput *, double dx, double dy, uint64_t now_ns);
bool native_mouse_button(NativeMouseInput *, unsigned button, bool down);
NativeMouseSample native_mouse_sample(NativeMouseInput *, uint64_t frame, uint64_t now_ns);
NativeMouseTurn native_mouse_turn(NativeMouseInput *, uint64_t now_ns);

// Native window adapter; positive Y means up, as in the Switch right stick.
void runtime_input_mouse_configure(double sensitivity, int invert_y);
void runtime_input_mouse_capture(int captured, int user);
void runtime_input_mouse_motion(double dx, double dy, int user);
void runtime_input_mouse_button(unsigned button, int down, int user);
void runtime_input_mouse_direct(int enabled);
NativeMouseTurn runtime_input_mouse_turn(int normal_camera_active);
#endif
