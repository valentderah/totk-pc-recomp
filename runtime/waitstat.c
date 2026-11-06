// Wait accounting: every place the port blocks, and how long it blocked there. A frame that takes half a second has to be
// attributable to one of them, and the thread dump only shows where a thread is at one instant. Each site accumulates the
// number of calls and the nanoseconds spent blocked; dump() prints the table since the last dump, so a run with
// TOTK_WAIT_LOG=1 shows the whole budget.
//
// The site names are constants, so the table needs no allocation and the compare is on the pointer first.
#include <stdint.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

enum { WS_MAX = 64 };
static struct { const char *name; uint64_t calls, ns; } g_ws[WS_MAX];
static int g_ws_n;
static int g_ws_on = -1;

static int ws_on(void) {
    if (g_ws_on < 0) {
        const char *v = getenv("TOTK_WAIT_LOG");
        g_ws_on = v && *v == '1';
    }
    return g_ws_on;
}

void waitstat_add(const char *name, uint64_t ns) {
    if (!ws_on()) return;
    for (int i = 0; i < g_ws_n; i++)
        if (g_ws[i].name == name) {
            __atomic_add_fetch(&g_ws[i].calls, 1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&g_ws[i].ns, ns, __ATOMIC_RELAXED);
            return;
        }
    if (g_ws_n >= WS_MAX) return;
    g_ws[g_ws_n].name = name;
    __atomic_store_n(&g_ws[g_ws_n].calls, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ws[g_ws_n].ns, ns, __ATOMIC_RELAXED);
    g_ws_n++;
}

void waitstat_dump(const char *why) {
    if (!ws_on()) return;
    uint64_t total = 0;
    for (int i = 0; i < g_ws_n; i++) total += __atomic_load_n(&g_ws[i].ns, __ATOMIC_RELAXED);
    if (!total) return;
    rt_log("wait: %s: %.1f ms in blocking waits", why, (double)total / 1e6);
    for (int i = 0; i < g_ws_n; i++) {
        uint64_t c = __atomic_exchange_n(&g_ws[i].calls, 0, __ATOMIC_RELAXED);
        uint64_t n = __atomic_exchange_n(&g_ws[i].ns, 0, __ATOMIC_RELAXED);
        if (!c) continue;
        rt_log("wait:   %-24s %8llu calls %10.1f ms total %8.2f ms each", g_ws[i].name, (unsigned long long)c,
               (double)n / 1e6, (double)n / 1e6 / (double)c);
    }
}

uint64_t waitstat_now(void) { return ws_on() ? plat_now_ns() : 0; }
