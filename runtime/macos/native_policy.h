// macOS runtime policy and measurements, independent of generated game tables.
#pragma once
#include <stdint.h>
#include <stddef.h>

void native_policy_init(void);
void native_frame_thread_init(void);
void native_patch_main(void *image, size_t size);
int native_policy_strict(void);
int native_host_check(void);
void native_check_output(unsigned width, unsigned height);
typedef struct {
    uint64_t frame;
    unsigned scene_width, scene_height, output_width, output_height;
    unsigned drawable_width, drawable_height;
} NativeFrame;
void native_check_scene(uint64_t frame, unsigned width, unsigned height);
NativeFrame native_frame_capture(uint64_t frame, unsigned w, unsigned h, unsigned dw, unsigned dh);
void native_presented(uint64_t ticket, NativeFrame frame, double seconds);
void native_trace_init(void);
void native_trace_finish(void);
int native_trace_active(void);
_Noreturn void native_exit(int status);
int native_run_probe(const char *name);
