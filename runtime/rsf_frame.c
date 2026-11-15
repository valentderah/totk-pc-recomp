// Observe the game's update-to-draw handoff before assigning RSF source frames.
// Queue messages and guest thread identities are facts; they are not frame IDs.
#include "host.h"
#include "cfg.h"
#include "layout.h"
#include "rsf_frame.h"
#include "rsf_runtime.h"
#include "platform/platform.h"
#include <rescaleframe/native_latency_vulkan.h>
#include <rescaleframe/native_cpu.h>

#if __has_include("patches.h")
#include "patches.h"
#endif

static PlatMutex frame_lock = PLAT_MUTEX_INIT;
static struct FrameOwner { void *thread; uint64_t current, completed; int sampled; } frames[32];
static uint64_t next_source;

static struct FrameOwner *frame_owner(void *thread) {
    if (!thread) return NULL;
    for (unsigned i = 0; i < 32; i++) if (!frames[i].thread || frames[i].thread == thread) {
        frames[i].thread = thread;
        return &frames[i];
    }
    return NULL;
}

static void cpu_fact(uint64_t source, uint32_t stage, uint32_t flags) {
    if (!source) return;
    __typeof__(&rsf_latencyvk_cpu) sink = rsf_runtime_symbol("rsf_latencyvk_cpu");
    if (!sink) return;
    rsf_game_cpu_event event = {.struct_size = sizeof event, .stage = stage,
        .session_id = 1, .source_frame_id = source, .timestamp_qpc = plat_counter(),
        .qpc_frequency = plat_counter_hz(), .event_version = RSF_GAME_CPU_EVENT_VERSION,
        .flags = RSF_GAME_CPU_INPUT_KEYED | flags};
    // The core may pace at INPUT_SAMPLE. No virtual guest core or frame-state
    // lock can span its driver sleep; rendering continues on the other threads.
    int away = kern_core_away();
    sink(&event);
    kern_core_back(&away);
    if ((rt_trace & RT_TRACE_PRESENT) && (source <= 16 || !(source % 600)))
        rt_log("rsf source: %llu stage %u flags %#x", (unsigned long long)source, stage, event.flags);
    if ((rt_trace & RT_TRACE_PRESENT) && stage == RSF_GAME_CPU_SIMULATION_END &&
        (source <= 16 || !(source % 600))) {
        __typeof__(&rsf_native_cpu_read) read = rsf_runtime_symbol("rsf_native_cpu_read");
        __typeof__(&rsf_native_cpu_counters_get) counters = rsf_runtime_symbol("rsf_native_cpu_counters_get");
        rsf_native_cpu_frame accepted = {.struct_size = sizeof accepted};
        rsf_native_cpu_counters totals = {.struct_size = sizeof totals};
        if (read && counters && read(1, source, &accepted) && counters(&totals))
            rt_log("rsf source accepted: %llu stages %#x failed %u flags %#x; %llu frames, %llu order violations, %llu input substitutes",
                   (unsigned long long)source, accepted.stage_mask, accepted.failed, accepted.flags,
                   (unsigned long long)totals.frames, (unsigned long long)totals.order_violations,
                   (unsigned long long)totals.substitutes);
    }
}

void rsf_frame_input(Cpu *c) {
    uint64_t source = 0;
    plat_mutex_lock(&frame_lock);
    struct FrameOwner *owner = frame_owner(c->thread);
    if (owner && owner->current && !owner->sampled) {
        source = owner->current;
        owner->sampled = 1;
    }
    plat_mutex_unlock(&frame_lock);
    cpu_fact(source, RSF_GAME_CPU_INPUT_SAMPLE, 0);
}

#ifdef RT_PATCH_PAGE
static void calculation_begin(Cpu *c) {
    // Displaced ADRP from the verified 1.4.3 framework calculation entry.
    c->x[8] = MOD_MAIN_BASE + 0x396f000;
    if (!cfg_int("TOTK_RSF_ENABLED") && !cfg_int("TOTK_RSF_FG") && !cfg_int("TOTK_RSF_LOW_LATENCY")) return;
    uint64_t source = 0, abandoned = 0;
    plat_mutex_lock(&frame_lock);
    struct FrameOwner *owner = frame_owner(c->thread);
    if (owner) {
        abandoned = owner->current;
        source = owner->current = ++next_source;
        owner->sampled = 0;
    }
    plat_mutex_unlock(&frame_lock);
    cpu_fact(abandoned, RSF_GAME_CPU_FRAME_END, RSF_GAME_CPU_RENDER_SKIPPED);
    cpu_fact(source, RSF_GAME_CPU_FRAME_BEGIN, 0);
    // The input manager is itself a task inside this calculation traversal.
    // The core accepts its later INPUT_SAMPLE fact without moving either event.
    cpu_fact(source, RSF_GAME_CPU_SIMULATION_BEGIN, 0);
}

static void calculation_end(Cpu *c) {
    // Displaced load after the task calculation traversal, before its guard is released.
    memcpy(&c->x[0], (const void *)(uintptr_t)(c->x[19] + 0x98), sizeof c->x[0]);
    uint64_t source = 0;
    plat_mutex_lock(&frame_lock);
    struct FrameOwner *owner = frame_owner(c->thread);
    if (owner) {
        source = owner->current;
        owner->completed = source;
        owner->current = 0;
    }
    plat_mutex_unlock(&frame_lock);
    cpu_fact(source, RSF_GAME_CPU_SIMULATION_END, 0);
}

__attribute__((constructor)) static void register_frame_hooks(void) {
    if (rt_game_version != 143) return;
    rt_register_host(RT_PATCH_PAGE + 0xf50, calculation_begin);
    rt_register_host(RT_PATCH_PAGE + 0xf40, calculation_end);
}
#endif

static int trace_count(void *thread, uint64_t queue, int send) {
    static PlatMutex lock = PLAT_MUTEX_INIT;
    static struct { void *thread; uint64_t queue; unsigned count[2]; } owners[1024];
    int result = 0;
    plat_mutex_lock(&lock);
    for (unsigned i = 0; i < 1024; i++) if (!owners[i].thread || (owners[i].thread == thread && owners[i].queue == queue)) {
        owners[i].thread = thread;
        owners[i].queue = queue;
        unsigned n = ++owners[i].count[send];
        if (n <= 16) result = (int)n;
        break;
    }
    plat_mutex_unlock(&lock);
    return result;
}

static void observe_send(Cpu *c) {
    uint64_t queue = c->x[0], message = c->x[1], caller = c->x[30];
    if (rt_trace & RT_TRACE_PRESENT) {
        int n = trace_count(c->thread, queue, 1);
        if (n)
            rt_log("draw handoff: send %d thread %p queue 0x%llx message 0x%llx caller 0x%llx ns %llu",
                   n, c->thread, (unsigned long long)queue, (unsigned long long)message,
                   (unsigned long long)caller, (unsigned long long)plat_now_ns());
        if (n == 1) {
            uint64_t fp = c->x[29];
            for (unsigned depth = 0; depth < 8 && !(fp & 15) && mem_guest_readable(fp, 16); depth++) {
                uint64_t pair[2];
                memcpy(pair, (const void *)(uintptr_t)fp, sizeof pair);
                rt_log("handoff caller: queue 0x%llx depth %u return 0x%llx", (unsigned long long)queue,
                       depth, (unsigned long long)pair[1]);
                if (pair[0] <= fp || pair[0] - fp > 0x100000) break;
                fp = pair[0];
            }
        }
    }
    rt_original(c);
}

static void observe_receive_impl(Cpu *c, int trying) {
    uint64_t output = c->x[0], queue = c->x[1], caller = c->x[30];
    rt_original(c);
    if ((rt_trace & RT_TRACE_PRESENT) && (!trying || c->x[0])) {
        int n = trace_count(c->thread, queue, 0);
        if (n && mem_guest_readable(output, sizeof(uint64_t))) {
            uint64_t message;
            memcpy(&message, (const void *)(uintptr_t)output, sizeof message);
            rt_log("draw handoff: receive %d thread %p queue 0x%llx message 0x%llx caller 0x%llx ns %llu",
                   n, c->thread, (unsigned long long)queue, (unsigned long long)message,
                   (unsigned long long)caller, (unsigned long long)plat_now_ns());
        }
    }
}

static void observe_receive(Cpu *c) { observe_receive_impl(c, 0); }
static void observe_try_receive(Cpu *c) { observe_receive_impl(c, 1); }

static void observe_try_send(Cpu *c) {
    uint64_t queue = c->x[0], message = c->x[1], caller = c->x[30];
    rt_original(c);
    if ((rt_trace & RT_TRACE_PRESENT) && c->x[0]) {
        int n = trace_count(c->thread, queue, 1);
        if (n) rt_log("draw handoff: try-send %d thread %p queue 0x%llx message 0x%llx caller 0x%llx ns %llu",
                      n, c->thread, (unsigned long long)queue, (unsigned long long)message,
                      (unsigned long long)caller, (unsigned long long)plat_now_ns());
    }
}

HOST_OBSERVER("_ZN2nn2os21SendLightMessageQueueEPNS0_21LightMessageQueueTypeEm", observe_send);
HOST_OBSERVER("_ZN2nn2os24ReceiveLightMessageQueueEPmPNS0_21LightMessageQueueTypeE", observe_receive);
HOST_OBSERVER("_ZN2nn2os24TrySendLightMessageQueueEPNS0_21LightMessageQueueTypeEm", observe_try_send);
HOST_OBSERVER("_ZN2nn2os27TryReceiveLightMessageQueueEPmPNS0_21LightMessageQueueTypeE", observe_try_receive);
// Jam inserts at the front. Keep the distinction visible in its own trace.
static void observe_jam(Cpu *c) {
    if (rt_trace & RT_TRACE_PRESENT) {
        int n = trace_count(c->thread, c->x[0], 1);
        if (n) rt_log("draw handoff: jam %d thread %p queue 0x%llx message 0x%llx caller 0x%llx ns %llu",
                      n, c->thread, (unsigned long long)c->x[0], (unsigned long long)c->x[1],
                      (unsigned long long)c->x[30], (unsigned long long)plat_now_ns());
    }
    rt_original(c);
}
HOST_OBSERVER("_ZN2nn2os20JamLightMessageQueueEPNS0_21LightMessageQueueTypeEm", observe_jam);
