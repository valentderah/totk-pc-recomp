// Performance overlay numbers (runtime/perfmon.c): the game's frame rate and frame times, the render thread's waits
// from the perf counters, and the machine's CPU, memory and GPU load and temperatures. A value the machine does not give
// is negative.
//
// Adapted from SavageWindRecomp's runtime/perfmon.h (https://github.com/Revivedx/SavageWindRecomp @
// 575ad88e02f9a89a9a885d90039d48af33c982f4, MIT, Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors,
// Copyright (c) 2026 Peiikai).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PERF_HISTORY 120   // frame times kept for the graph

typedef struct {
    // the game's frames (presents), over the last second
    float fps, frame_ms_avg, frame_ms_max;
    // fps of the slowest 1% and 0.1% of the frames over the last ten seconds (-1: too few frames yet)
    float low1_fps, low01_fps;
    float frame_ms[PERF_HISTORY];   // oldest first
    // CPU: the whole machine and this process, percent of all logical processors
    char cpu_name[64];
    float cpu_total_pct, cpu_game_pct, cpu_temp_c;
    // memory, MB
    float ram_used_mb, ram_total_mb, ram_game_mb;
    // GPU: load and dedicated memory from the system's counters, temperature, power and clock from NVML (NVIDIA) or
    // ADL (AMD)
    char gpu_name[96];
    float gpu_pct, gpu_temp_c, gpu_power_w, gpu_clock_mhz, vram_used_mb, vram_total_mb;
    // per frame, averaged over the last half second: replay (NVN to Vulkan recording), GPU busy time (timestamps; -1
    // without them), the render thread's waits for pipeline builds, shader translation and texture uploads, and the
    // textures uploaded
    float replay_ms, gpu_ms, pipe_wait_ms, xlate_ms, tex_ms, tex_uploads;
} PerfStats;

// Overlay mode 0 off, 1 fps, 2 full (TOTK_PERF_OVERLAY), from the present path when the setting changes. Full starts
// the machine sampling thread (twice a second) and turns on the event counts and GPU timestamps; leaving full stops
// them. Off: perf_present skips perfmon_frame and nothing samples.
void perfmon_mode(int mode);
// The Vulkan device the game renders on: its name, its LUID (8 bytes; NULL when the driver gives none), its PCI bus
// (-1 unknown) and its device-local memory. Before the first perfmon_mode(2).
void perfmon_set_gpu(const char *name, const uint8_t *luid, int pci_bus, float vram_total_mb);
// A game frame was presented (perf_present, the render thread, only while the overlay is on): the time since the last
// one, the render thread's waits of this frame (PERF_N entries, host.h), its replay time and the running count of
// texture uploads.
void perfmon_frame(uint64_t now_ns, uint64_t interval_ns, const uint64_t *wait_ns, uint64_t replay_ns,
                   int64_t tex_uploads);
// The latest numbers (the overlay, present path).
void perfmon_snapshot(PerfStats *out);

#ifdef __cplusplus
}
#endif
