// Observe the game's update-to-draw handoff before assigning RSF source frames.
// Queue messages and guest thread identities are facts; they are not frame IDs.
#include "host.h"
#include "platform/platform.h"

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
