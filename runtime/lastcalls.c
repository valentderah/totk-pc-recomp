// The last lifted functions each thread entered: a small ring per thread, recorded from FN_ENTER while
// TOTK_LASTCALLS=1, and printed on SIGQUIT (or from a crash path) so a thread that is stuck, or a fault's caller, can be
// named without a debugger. One entry is a guest address and the link register, which ldr_describe turns into "main+0x…".
//
// Off by default: recording costs a thread-local slot lookup per lifted call, and the ring is 64 entries per thread.
#include <signal.h>
#include <stdint.h>
#include <string.h>

#include "cpu.h"
#include "host.h"
#include "platform/platform.h"

enum { LC_THREADS = 256, LC_DEPTH = 64 };

typedef struct {
    uint64_t addr[LC_DEPTH], lr[LC_DEPTH];
    unsigned pos;         // next slot to write
    int used;
    uint64_t tid;         // plat_thread_id, for the dump
} LastCallRing;

static LastCallRing g_lc[LC_THREADS];
static int g_lc_n;
static volatile int g_lc_on;

static _Thread_local int g_lc_slot = -1;

// The printing is done from a signal handler, so it must not take locks: rt_logf writes to stderr, and the rings are
// only read here.
static void dump_ring(int slot, int n) {
    LastCallRing *r = &g_lc[slot];
    if (!r->used) return;
    char d[96];
    unsigned have = r->pos < LC_DEPTH ? r->pos : LC_DEPTH;
    if ((unsigned)n > have) n = (int)have;
    rt_logf("lastcalls: thread %llu: %u calls recorded, newest %d:\n", (unsigned long long)r->tid, have, n);
    for (int i = 1; i <= n; i++) {
        unsigned k = (r->pos + LC_DEPTH - (unsigned)i) % LC_DEPTH;
        rt_logf("lastcalls:   #%-2d at %s (lr %s)\n", i, ldr_describe(r->addr[k], d, sizeof d),
                ldr_describe(r->lr[k], d, sizeof d));
    }
}

void lastcall_dump(int n) {
    if (!g_lc_on) return;
    for (int i = 0; i < g_lc_n; i++) dump_ring(i, n);
}

static void on_sigquit(int sig) {
    rt_logf("lastcalls: SIGQUIT: %d thread(s) with a ring\n", g_lc_n);
    lastcall_dump(16);
    signal(sig, SIG_DFL);
    raise(sig);
}

void lastcall_note(uint64_t addr, uint64_t lr) {
    if (!g_lc_on) return;
    int slot = g_lc_slot;
    if (slot < 0) {
        slot = __atomic_fetch_add(&g_lc_n, 1, __ATOMIC_RELAXED);
        if (slot >= LC_THREADS) { g_lc_slot = LC_THREADS; return; }   // more threads than rings: this one is not recorded
        g_lc_slot = slot;
        g_lc[slot].tid = plat_thread_id();
        g_lc[slot].used = 1;
    }
    if (slot >= LC_THREADS) return;
    LastCallRing *r = &g_lc[slot];
    r->addr[r->pos] = addr;
    r->lr[r->pos] = lr;
    r->pos = (r->pos + 1) % LC_DEPTH;
}

// Called once, early: TOTK_LASTCALLS=1 starts recording and installs the SIGQUIT dump.
void lastcall_setup(void) {
    const char *v = getenv("TOTK_LASTCALLS");
    if (!v || !*v || !strcmp(v, "0")) return;
    g_lc_on = 1;
    g_lastcall = 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigquit;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_ONSTACK;
    sigaction(SIGQUIT, &sa, NULL);
    rt_log("lastcalls: recording the last %d lifted calls of each thread; SIGQUIT dumps them", (int)LC_DEPTH);
}
