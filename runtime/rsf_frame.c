// Observe the game's update-to-draw handoff before assigning RSF source frames.
// Queue messages and guest thread identities are facts; they are not frame IDs.
#include "host.h"
#include "cfg.h"
#include "layout.h"
#include "rsf_frame.h"
#include "rsf_handoff.h"
#include "rsf_runtime.h"
#include "platform/platform.h"
#include <rescaleframe/native_latency_vulkan.h>
#include <rescaleframe/native_cpu.h>

#if __has_include("patches.h")
#include "patches.h"
#endif

static PlatMutex frame_lock = PLAT_MUTEX_INIT;
static struct FrameOwner { void *thread; uint64_t framework, current, completed, draw, draw_queue; int sampled; } frames[32];
static uint64_t next_source;
static PlatMutex latency_options_lock = PLAT_MUTEX_INIT;

static void latency_options(void) {
    static int applied = -1, last_result = -999;
    int requested = cfg_int("TOTK_RSF_LOW_LATENCY");
    __typeof__(&rsf_latencyvk_options) configure = rsf_runtime_symbol("rsf_latencyvk_options");
    if (!configure) return;
    plat_mutex_lock(&latency_options_lock);
    if (applied != requested) {
        int32_t result = configure((uint32_t)requested, 0);
        if (result != last_result || result == RSF_BACKEND_OK)
            rt_log("rsf: low latency requested %d, options result %d", requested, result);
        if (result == RSF_BACKEND_OK) applied = requested;
        last_result = result;
    }
    plat_mutex_unlock(&latency_options_lock);
}

static int tracking_enabled(void) {
    // These selections require restart. Queue initialization happens after
    // cfg_init, before any framework calculation opens a source.
    static int cached = -1;
    int value = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);
    if (value < 0) {
        value = !!(cfg_int("TOTK_RSF_ENABLED") || cfg_int("TOTK_RSF_FG") || cfg_int("TOTK_RSF_LOW_LATENCY"));
        __atomic_store_n(&cached, value, __ATOMIC_RELEASE);
    }
    return value;
}

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
        .render_expected = stage == RSF_GAME_CPU_FRAME_END && !(flags & RSF_GAME_CPU_RENDER_SKIPPED),
        .flags = RSF_GAME_CPU_INPUT_KEYED | flags};
    // The core may pace at INPUT_SAMPLE. No virtual guest core or frame-state
    // lock can span its driver sleep; rendering continues on the other threads.
    int away = kern_core_away();
    if (stage == RSF_GAME_CPU_INPUT_SAMPLE) latency_options();
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
        __typeof__(&rsf_latencyvk_get_status) latency = rsf_runtime_symbol("rsf_latencyvk_get_status");
        rsf_latencyvk_status status = {.struct_size = sizeof status, .abi_version = RSF_LATENCYVK_ABI_VERSION};
        if (latency && latency(&status) == RSF_BACKEND_OK)
            rt_log("rsf latency: source %llu owner %u available %u mode %u; sleeps %llu markers %llu refused %llu",
                   (unsigned long long)source, status.owner, status.low_latency_available, status.mode,
                   (unsigned long long)status.sleep_calls, (unsigned long long)status.marker_calls,
                   (unsigned long long)status.refused_calls);
    }
}

static uint64_t input_framework(const Cpu *c) {
    if (rt_game_version != 143 || (c->x[30] != MOD_MAIN_BASE + 0x47afa8 && c->x[30] != MOD_MAIN_BASE + 0x47b0f4)) return 0;
    // The pad collector saves the controller manager's frame. On a worker,
    // that frame saves the job's calc context in x19 at FP + 0x58. Its second
    // word is InputManager's configuration block at object + 0x480.
    uint64_t collector[2], manager[2], framework;
    uint64_t fp = c->x[29];
    if ((fp & 15) || !mem_guest_readable(fp, sizeof collector)) return 0;
    memcpy(collector, (const void *)(uintptr_t)fp, sizeof collector);
    if (collector[1] != MOD_MAIN_BASE + 0x479c88 || (collector[0] & 15) ||
        !mem_guest_readable(collector[0], 0x60)) return 0;
    memcpy(manager, (const void *)(uintptr_t)collector[0], sizeof manager);
    if (manager[1] != MOD_MAIN_BASE + 0x893e8c && manager[1] != MOD_MAIN_BASE + 0x893ef0 &&
        manager[1] != MOD_MAIN_BASE + 0x893f70 && manager[1] != MOD_MAIN_BASE + 0xea60fc &&
        manager[1] != MOD_MAIN_BASE + 0xea6160 && manager[1] != MOD_MAIN_BASE + 0xea6290) return 0;
    uint64_t context, config, task;
    memcpy(&context, (const void *)(uintptr_t)(collector[0] + 0x58), sizeof context);
    if (!mem_guest_readable(context, 16)) return 0;
    memcpy(&config, (const void *)(uintptr_t)(context + 8), sizeof config);
    if (config < 0x480 || !mem_guest_readable(config - 0x480 + 0xa8, 8)) return 0;
    memcpy(&task, (const void *)(uintptr_t)(config - 0x480 + 0xa8), sizeof task);
    if (!mem_guest_readable(task + 0x20, 8)) return 0;
    memcpy(&framework, (const void *)(uintptr_t)(task + 0x20), sizeof framework);
    return framework;
}

void rsf_frame_input(Cpu *c) {
    uint64_t source = 0;
    uint64_t framework = input_framework(c);
    plat_mutex_lock(&frame_lock);
    struct FrameOwner *owner = NULL;
    for (unsigned i = 0; i < 32; i++) if (frames[i].thread == c->thread && frames[i].current) owner = &frames[i];
    if (!owner && framework) {
        // Worker execution carries the same framework owner through the task's
        // calc context. Only one active traversal of that owner may match.
        for (unsigned i = 0; i < 32; i++) if (frames[i].current && frames[i].framework == framework) {
            if (owner) { owner = NULL; break; }
            owner = &frames[i];
        }
    }
    if (owner && owner->current && !owner->sampled) {
        source = owner->current;
        owner->sampled = 1;
    }
    plat_mutex_unlock(&frame_lock);
    cpu_fact(source, RSF_GAME_CPU_INPUT_SAMPLE, 0);
}

uint64_t rsf_frame_draw_source(Cpu *c) {
    uint64_t source = 0;
    plat_mutex_lock(&frame_lock);
    for (unsigned i = 0; i < 32; i++) if (frames[i].thread == c->thread) source = frames[i].draw;
    plat_mutex_unlock(&frame_lock);
    return source;
}

void rsf_frame_draw_done(Cpu *c) {
    plat_mutex_lock(&frame_lock);
    for (unsigned i = 0; i < 32; i++) if (frames[i].thread == c->thread) frames[i].draw = frames[i].draw_queue = 0;
    plat_mutex_unlock(&frame_lock);
}

static void retire_draw_queue(uint64_t queue) {
    rsf_handoff_finalize(queue);
    plat_mutex_lock(&frame_lock);
    for (unsigned i = 0; i < 32; i++) if (frames[i].draw_queue == queue) frames[i].draw = frames[i].draw_queue = 0;
    plat_mutex_unlock(&frame_lock);
}

static void publish_draw(Cpu *c, uint64_t queue, uint64_t message) {
    uint64_t framework = 0, source = 0, caller[2], draw_thread;
    // Framework pre-calc publishes the preceding completed calculation to its
    // draw thread. x19 still holds that framework at the Sead send call.
    if (rt_game_version == 143 && c->x[30] == MOD_MAIN_BASE + 0x59a6c8 &&
        !(c->x[29] & 15) && mem_guest_readable(c->x[29], sizeof caller)) {
        memcpy(caller, (const void *)(uintptr_t)c->x[29], sizeof caller);
        if (caller[1] == MOD_MAIN_BASE + 0x2db62c && mem_guest_readable(c->x[19] + 0x198, 8)) {
            memcpy(&draw_thread, (const void *)(uintptr_t)(c->x[19] + 0x198), sizeof draw_thread);
            if (queue >= 0x28 && draw_thread == queue - 0x28) framework = c->x[19];
        }
    }
    plat_mutex_lock(&frame_lock);
    for (unsigned i = 0; i < 32; i++) if (frames[i].thread == c->thread &&
        frames[i].framework == framework && !frames[i].current) source = frames[i].completed;
    plat_mutex_unlock(&frame_lock);
    if (!rsf_handoff_publish(queue, message, c->thread, framework, source)) return;
    plat_mutex_lock(&frame_lock);
    for (unsigned i = 0; i < 32; i++) if (frames[i].thread == c->thread && frames[i].completed == source)
        frames[i].completed = 0;
    plat_mutex_unlock(&frame_lock);
    cpu_fact(source, RSF_GAME_CPU_FRAME_END, 0);
    if ((rt_trace & RT_TRACE_PRESENT) && (source <= 16 || !(source % 600)))
        rt_log("rsf draw published: queue 0x%llx source %llu", (unsigned long long)queue, (unsigned long long)source);
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
        owner->framework = c->x[19];
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
    if (tracking_enabled()) publish_draw(c, queue, message);
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
    if (tracking_enabled() && (!trying || c->x[0]) && mem_guest_readable(output, sizeof(uint64_t))) {
        uint64_t message, source;
        memcpy(&message, (const void *)(uintptr_t)output, sizeof message);
        if (rsf_handoff_receive(queue, message, &source)) {
            plat_mutex_lock(&frame_lock);
            struct FrameOwner *owner = frame_owner(c->thread);
            if (owner) { owner->draw = source; owner->draw_queue = queue; }
            plat_mutex_unlock(&frame_lock);
            if ((rt_trace & RT_TRACE_PRESENT) && (source <= 16 || !(source % 600)))
                rt_log("rsf draw consumed: thread %p queue 0x%llx source %llu", c->thread,
                       (unsigned long long)queue, (unsigned long long)source);
        }
    }
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
    if (tracking_enabled()) rsf_handoff_foreign(queue);
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
    if (tracking_enabled()) rsf_handoff_foreign(c->x[0]);
    if (rt_trace & RT_TRACE_PRESENT) {
        int n = trace_count(c->thread, c->x[0], 1);
        if (n) rt_log("draw handoff: jam %d thread %p queue 0x%llx message 0x%llx caller 0x%llx ns %llu",
                      n, c->thread, (unsigned long long)c->x[0], (unsigned long long)c->x[1],
                      (unsigned long long)c->x[30], (unsigned long long)plat_now_ns());
    }
    rt_original(c);
}
HOST_OBSERVER("_ZN2nn2os20JamLightMessageQueueEPNS0_21LightMessageQueueTypeEm", observe_jam);
static void observe_try_jam(Cpu *c) { if (tracking_enabled()) rsf_handoff_foreign(c->x[0]); rt_original(c); }
HOST_OBSERVER("_ZN2nn2os23TryJamLightMessageQueueEPNS0_21LightMessageQueueTypeEm", observe_try_jam);
static void observe_initialize(Cpu *c) {
    uint64_t queue = c->x[0];
    rt_original(c);
    if (tracking_enabled()) { retire_draw_queue(queue); rsf_handoff_initialize(queue); }
}
static void observe_finalize(Cpu *c) { if (tracking_enabled()) retire_draw_queue(c->x[0]); rt_original(c); }
HOST_OBSERVER("_ZN2nn2os27InitializeLightMessageQueueEPNS0_21LightMessageQueueTypeEPmm", observe_initialize);
HOST_OBSERVER("_ZN2nn2os25FinalizeLightMessageQueueEPNS0_21LightMessageQueueTypeE", observe_finalize);
