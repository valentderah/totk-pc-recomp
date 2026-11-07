// Mutex ownership, for the case where a thread spins on a lock's "held" bit and the holder never releases it.
//
// nn::os::LockMutex is SDK code: it takes the lock in user mode and falls back to the kernel only when contended, so
// the kernel's ArbitrateLock cannot see an uncontended acquisition. These observers wrap the SDK entries instead and
// keep a small table of (mutex, owner, recursion depth). The SIGQUIT dump or crash dump then names the holder of every
// mutex the runtime still sees taken, which is what a spin on `bit31 == held` needs to be diagnosed.
//
// Off unless TOTK_MUTEX_TRACE=1. The table is a diagnostic: entries are claimed without a lock, because a torn or
// duplicate row is harmless and taking a lock here would perturb exactly what is being measured.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

#ifndef _WIN32
#include <signal.h>
#endif

enum { MT_MAX = 256, MT_SLOW_NS = 200000000ull };   // 200 ms counts as "this lock is stuck"

static struct {
    uint64_t mutex;
    uint32_t owner;        // guest thread handle that holds it, 0 = free
    uint32_t depth;
    uint64_t slowest_ns;   // longest a waiter waited for this mutex
} g_mt[MT_MAX];

static int g_mt_on = -1;

void mutex_trace_dump(void);

#ifndef _WIN32
static void on_sigquit(int sig) {
    (void)sig;
    rt_log("SIGQUIT: dumping mutex and synchronization diagnostics");
    mutex_trace_dump();
    kern_dump_waiters();
    signal(SIGQUIT, SIG_DFL);
    raise(SIGQUIT);
}

static void sigquit_setup(void) {
    static int done;
    if (done) return;
    done = 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigquit;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGQUIT, &sa, NULL);
}
#endif

static int mt_on(void) {
    if (g_mt_on < 0) {
        const char *v = getenv("TOTK_MUTEX_TRACE");
        g_mt_on = v && *v == '1';
#ifndef _WIN32
        if (g_mt_on || getenv("TOTK_SYNC_TRACE")) sigquit_setup();
#endif
    }
    return g_mt_on;
}

static int mt_slot(uint64_t mutex) {
    for (int i = 0; i < MT_MAX; i++)
        if (g_mt[i].mutex == mutex) return i;
    for (int i = 0; i < MT_MAX; i++)
        if (g_mt[i].mutex == 0) {
            g_mt[i].mutex = mutex;
            return i;
        }
    return -1;
}

static void obs_lock(Cpu *c) {
    if (!mt_on()) {
        rt_original(c);
        return;
    }
    uint64_t mutex = c->x[0];
    uint32_t self = kern_current_thread_handle();
    uint64_t t0 = plat_now_ns();
    rt_original(c);
    uint64_t dt = plat_now_ns() - t0;

    int i = mt_slot(mutex);
    if (i < 0) return;
    if (g_mt[i].owner == self) {
        g_mt[i].depth++;
    } else if (g_mt[i].owner == 0) {
        g_mt[i].owner = self;
        g_mt[i].depth = 1;
    } else {
        // Handed over by someone else: UnlockMutex in the same path should have cleared it. Record the new owner.
        g_mt[i].owner = self;
        g_mt[i].depth = 1;
    }
    if (dt > g_mt[i].slowest_ns) g_mt[i].slowest_ns = dt;
    if (dt > MT_SLOW_NS) rt_log("mutex: thread %#x waited %llu ms for mutex %#llx", self,
                                (unsigned long long)(dt / 1000000), (unsigned long long)mutex);
}

static void obs_unlock(Cpu *c) {
    if (!mt_on()) {
        rt_original(c);
        return;
    }
    uint64_t mutex = c->x[0];
    uint32_t self = kern_current_thread_handle();
    int i = mt_slot(mutex);
    if (i >= 0 && g_mt[i].owner == self) {
        if (g_mt[i].depth > 1) g_mt[i].depth--;
        else g_mt[i].owner = 0, g_mt[i].depth = 0;
    }
    rt_original(c);
}

void mutex_trace_dump(void) {
    if (!mt_on()) return;
    for (int i = 0; i < MT_MAX; i++)
        if (g_mt[i].mutex && g_mt[i].owner)
            rt_logf("mutex: %#llx held by thread %#x (depth %u, slowest wait %llu ms)\n",
                    (unsigned long long)g_mt[i].mutex, g_mt[i].owner, g_mt[i].depth,
                    (unsigned long long)(g_mt[i].slowest_ns / 1000000));
}

HOST_OBSERVER("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE", obs_lock);
HOST_OBSERVER("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE", obs_unlock);
