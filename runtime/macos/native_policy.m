// The acceptance profile renders every game frame at 3840x2160. Diagnostics may
// opt out explicitly; those runs cannot satisfy the 4K/144 acceptance check.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "native_policy.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <pthread.h>
#include <pthread/qos.h>

extern void runtime_fail(const char *message);

static int strict_mode;
static id<NSObject> activity_token;
// Strict scene dimensions are constant; publish only the frame identity.
// Zero denotes an unused slot, so frame zero is stored as one.
static _Atomic uint64_t scene_frames[64];

int native_policy_strict(void) { return strict_mode; }

void native_frame_thread_init(void) {
    static _Thread_local int configured;
    if (configured) return;
    // Frame work includes loading and waits on guest workers. Its CPU cost has
    // no fixed deadline budget. QoS also propagates urgency to framework work.
    if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0))
        runtime_fail("set frame thread QoS");
    configured = 1;
}

void native_policy_init(void) {
    if (!activity_token) {
        activity_token = [[NSProcessInfo processInfo]
            beginActivityWithOptions:(NSActivityUserInitiated | NSActivityLatencyCritical |
                                      NSActivityIdleDisplaySleepDisabled | NSActivityIdleSystemSleepDisabled)
                              reason:@"TotK 4K 144Hz Gameplay"];
    }
    const char *setting = getenv("TOTK_NATIVE_4K144");
    strict_mode = !setting || strcmp(setting, "0");
    const struct { const char *key, *value; } defaults[] = {
        {"TOTK_GPU_KEEPALIVE", "0"}, {"TOTK_ARG_BIND_CACHE", "1"},
        {"TOTK_POOL_QUERY_CACHE", "1"}, {"TOTK_USNAP_INDEX", "1"},
        {"TOTK_PKG_FINAL_CACHE", "1"}, {"TOTK_ARG_EPOCH", "1"},
        {"TOTK_TEX_SLOTS", "1"}, {"TOTK_SHADOW_INDEX", "1"},
        {"TOTK_DRV_CACHE", "1"}, {"TOTK_ASYNC_REPLAY", "0"},
        {"TOTK_TEXTURE_PLAN", "1"}, {"TOTK_INLINE_BULK", "1"},
        {"TOTK_DIAG_ENV_ONCE", "1"}, {"TOTK_VERTEX_STREAMS", "1"},
        // Speculative pipeline scans compile mostly unused render states.
        // Keep the recorded startup cache and compile new states on demand.
        {"TOTK_PSO_PREDICT", "0"},
    };
    for (unsigned i=0; i<sizeof defaults/sizeof *defaults; ++i)
        if (setenv(defaults[i].key, defaults[i].value, 0)) runtime_fail("set native performance defaults");
    native_trace_init();
    if (!strict_mode) return;
    const struct { const char *key, *value; } settings[] = {
        {"TOTK_OUTPUT_RES", "3840x2160"}, {"TOTK_RENDER_RES", "3840x2160"},
        {"DM_PRESENT_HZ", "144"}, {"DM_PRESENT_LINK", "0"},
        {"TOTK_FPS60", "1"}, {"TOTK_UNCAPPED", "0"}, {"TOTK_REALTIME", "1"}, {"TOTK_CLOTH", "1"},
        {"TOTK_MFX", "0"}, {"TOTK_MFX_UPSCALE", "0"}, {"TOTK_MFX_OUTPUT", "0"},
        {"TOTK_GPU_ALLOW_SUBMIT", "1"}, {"TOTK_GPU_WIDEN", "1"},
        {"TOTK_GPU_BLOCKRT", "1"}, {"TOTK_GPU_MIDCLEAR", "1"},
        {"TOTK_GPU_NOCOMMIT", "0"}, {"TOTK_SKIP_INVISIBLE", "0"}, {"TOTK_EXP_SKIP_INVISIBLE", "0"},
        {"TOTK_SKIP_STORERS", "0"}, {"DM_FPS", "60"},
    };
    for (unsigned i = 0; i < sizeof settings / sizeof *settings; ++i)
        if (setenv(settings[i].key, settings[i].value, 1)) runtime_fail("set native render policy");
    unsetenv("TOTK_PRESENT_SKIP");
    fprintf(stderr, "[totk] native profile: scene/output 3840x2160, target 144 rendered fps, no scaling or interpolation\n");
}

int native_host_check(void) {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        struct utsname machine;
        uname(&machine);
        BOOL metal4 = NO;
        if (@available(macOS 26.0, *)) metal4 = device && [device supportsFamily:MTLGPUFamilyMetal4];
        BOOL native = !strcmp(machine.machine, "arm64");
        NSDictionary *report = @{@"target": @"macos-arm64", @"native_arm64": @(native),
            @"metal4": @(metal4), @"gpu": device.name ?: @"unavailable",
            @"unified_memory": @(device.hasUnifiedMemory),
            @"status": native && metal4 ? @"pass" : @"fail"};
        NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:nil];
        puts([[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding].UTF8String);
        return native && metal4 ? 0 : 1;
    }
}

void native_check_output(unsigned w, unsigned h) {
    if (!strict_mode) return;
    if (w != 3840 || h != 2160) runtime_fail("native 4K output target is not 3840x2160");
}

void native_check_scene(uint64_t frame, unsigned w, unsigned h) {
    if (!strict_mode) return;
    if (w != 3840 || h != 2160) {
        char message[160];
        snprintf(message, sizeof message, "native 4K scene is %ux%u at frame %llu; expected 3840x2160",
                 w, h, (unsigned long long)frame);
        runtime_fail(message);
    }
    if (frame == UINT64_MAX) runtime_fail("frame identity overflow");
    atomic_store_explicit(&scene_frames[frame % 64], frame + 1, memory_order_relaxed);
}

NativeFrame native_frame_capture(uint64_t frame, unsigned w, unsigned h, unsigned dw, unsigned dh) {
    NativeFrame result = {.frame=frame};
    if (frame != UINT64_MAX && atomic_load_explicit(&scene_frames[frame % 64], memory_order_relaxed) == frame + 1) {
        result.scene_width=3840; result.scene_height=2160;
    }
    result.output_width=w; result.output_height=h;
    result.drawable_width=dw; result.drawable_height=dh;
    return result;
}
