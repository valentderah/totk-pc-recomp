// Adapted from MacNativeGames games/totk/runtime/services.c @ b75be8c (MIT, Copyright (c) 2026 MacNativeGames
// contributors), lines 304-382.
// The SVC entry of the lifted code: preemption checkpoint, trace, then the thread and
// synchronization SVCs (kernel.c), IPC (ipc.c), memory (memory.c), Break, OutputDebugString, GetInfo and ExitProcess. The 21 SVCs
// TotK has but never reaches on MNG's boot path stop with a named fatal, the caller and the guest backtrace.
#include <stdio.h>
#include <string.h>

#include "fs.h"
#include "host.h"
#include "kernel.h"
#include "vk.h"
#include "platform/platform.h"

#define R_OK 0u
#define ST_FREE 0u
#define ST_STACK 0xBu
#define PERM_RW 3u
#define PROGRAM_ID 0x0100f2c0115b6000ull

static uint64_t g_svcs;
void (*volatile svc_test_exit_hook)(unsigned code);

RtStats rt_stats(void) {
    return (RtStats){__atomic_load_n(&g_svcs, __ATOMIC_RELAXED), kern_threads_created(), hostfn_calls()};
}

static int readable(uint64_t a, uint64_t n) { return plat_mem_readable(a, n); }

void kern_backtrace(const Cpu *c) {
    char d[160];
    rt_logf("  lr %s\n", ldr_describe(c->x[30], d, sizeof d));
    uint64_t fp = c->x[29];
    for (int n = 0; n < 32 && fp && !(fp & 7) && readable(fp, 16); n++) {
        uint64_t next = LD64(fp), lr = LD64(fp + 8);
        rt_logf("  guest frame %2d: %s\n", n, ldr_describe(lr, d, sizeof d));
        if (next <= fp || next - fp > (64u << 20)) break;
        fp = next;
    }
}

static const struct { uint32_t imm; const char *name; } unimplemented[] = {
    {0x13, "MapSharedMemory"}, {0x14, "UnmapSharedMemory"},
    {0x23, "SendAsyncRequestWithUserBuffer"}, {0x28, "ReturnFromException"}, {0x2A, "FlushEntireDataCache"},
    {0x2F, "GetLastThreadInfo"}, {0x32, "SetThreadActivity"}, {0x33, "GetThreadContext3"},
    {0x36, "SynchronizePreemptionState"}, {0x40, "CreateSession"}, {0x41, "AcceptSession"},
    {0x43, "ReplyAndReceive"}, {0x44, "ReplyAndReceiveWithUserBuffer"}, {0x48, "MapPhysicalMemoryUnsafe"},
    {0x49, "UnmapPhysicalMemoryUnsafe"}, {0x4B, "CreateCodeMemory"}, {0x50, "CreateSharedMemory"},
    {0x51, "MapTransferMemory"}, {0x52, "UnmapTransferMemory"}, {0x70, "CreatePort"}, {0x72, "ConnectToPort"},
};

static __attribute__((noreturn)) void svc_fatal(Cpu *c, uint32_t imm, uint64_t pc, const char *why) {
    const char *name = "unknown";
    for (unsigned i = 0; i < sizeof unimplemented / sizeof *unimplemented; i++)
        if (unimplemented[i].imm == imm) name = unimplemented[i].name;
    char at[160];
    fflush(stdout);
    rt_logf("FATAL: svc #%#x %s at %s: %s\n  x0 %#llx x1 %#llx x2 %#llx x3 %#llx\n", imm, name,
            ldr_describe(pc, at, sizeof at), why, (unsigned long long)c->x[0], (unsigned long long)c->x[1],
            (unsigned long long)c->x[2], (unsigned long long)c->x[3]);
    kern_backtrace(c);
    rt_fatal("svc #%#x %s is not implemented", imm, name);
}

// QueryMemory sees the guest main thread's stack, which memory.c does not know: Stack inside it, free ranges
// around it clipped (the way memory.c clips moved stacks).
static void main_stack_query(uint64_t a, RtMemoryInfo *r) {
    uint64_t mb, ms;
    kern_main_stack(&mb, &ms);
    if (!ms) return;
    uint64_t me = mb + ms;
    if (a >= mb && a < me) {
        *r = (RtMemoryInfo){.base = mb, .size = ms, .state = ST_STACK, .perm = PERM_RW};
        return;
    }
    if (r->state != ST_FREE) return;
    uint64_t lo = r->base, hi = r->base + r->size;
    if (me <= a && me > lo) lo = me;
    if (mb > a && mb < hi) hi = mb;
    r->base = lo;
    r->size = hi - lo;
}

static int on_main_stack(uint64_t a, uint64_t n) {
    uint64_t mb, ms;
    kern_main_stack(&mb, &ms);
    return ms && a < mb + ms && mb < a + n;
}

static uint64_t user_exception_context(void) {
    static void *page;
    void *p = __atomic_load_n(&page, __ATOMIC_ACQUIRE);
    if (!p) {
        void *mine = mem_kernel_alloc(0x1000, 0x1000), *expected = NULL;
        if (__atomic_compare_exchange_n(&page, &expected, mine, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) p = mine;
        else {
            mem_kernel_free(mine, 0x1000);
            p = expected;
        }
    }
    return (uint64_t)(uintptr_t)p;
}

// GetInfo(w1 id, w2 handle, x3 sub id -> x1)
static void get_info(Cpu *c, uint64_t pc) {
    uint32_t id = (uint32_t)c->x[1];
    uint64_t v = 0;
    if (mem_info(id, &v)) {
        c->x[0] = R_OK;
        c->x[1] = v;
        return;
    }
    switch (id) {
    case 0: v = 7; break;                       // core mask
    case 1: v = 0x0ffffffff0000000ull; break;   // priority mask
    case 8: v = 0; break;                       // debugger attached
    case 10: v = 0; break;                      // idle tick count
    case 11:                                    // random entropy
        if (!plat_random(&v, sizeof v)) rt_fatal("GetInfo 11: no random bytes from the system");
        break;
    case 18: v = PROGRAM_ID; break;
    case 20: v = user_exception_context(); break;   // the SDK keeps e.g. the dying message at +0x1C0
    case 23: v = 1; break;
    default: {
        char why[64];
        snprintf(why, sizeof why, "GetInfo id %u (handle %#x, sub id %#llx)", id, (uint32_t)c->x[2],
                 (unsigned long long)c->x[3]);
        svc_fatal(c, 0x29, pc, why);
    }
    }
    c->x[0] = R_OK;
    c->x[1] = v;
}

static void svc_body(Cpu *c, uint32_t imm, uint64_t pc);

void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    kern_checkpoint();
    if (__builtin_expect(!kern_host_track || !c->thread, 1)) {
        svc_body(c, imm, pc);
        return;
    }
    void *t = c->thread;   // kern_host_track: the SVC counted in the thread's host depth (hostfn.c hostfn_tracked)
    int *d = kern_host_depth(t);
    __atomic_store_n(d, *d + 1, __ATOMIC_RELAXED);
    svc_body(c, imm, pc);
    int n = *d - 1;
    __atomic_store_n(d, n, __ATOMIC_RELAXED);
    if (!n && kern_preempt_pending(t)) kern_host_left(t);
}

static void svc_body(Cpu *c, uint32_t imm, uint64_t pc) {
    __atomic_fetch_add(&g_svcs, 1, __ATOMIC_RELAXED);
    if (rt_trace & RT_TRACE_SVC) {
        char at[160];
        rt_log("svc %#x at %s x0=%#llx x1=%#llx x2=%#llx x3=%#llx", imm, ldr_describe(pc, at, sizeof at),
               (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2],
               (unsigned long long)c->x[3]);
    }
    if (kern_svc(c, imm)) return;
    uint64_t *x = c->x;
    switch (imm) {
    case 0x15: case 0x1F: case 0x21: case 0x22:
        if (ipc_svc(c, imm)) return;
        svc_fatal(c, imm, pc, "not handled by ipc.c");
    case 0x01: {   // SetHeapSize(x1 size -> x1 address)
        uint64_t a = 0;
        uint32_t r = mem_set_heap_size(x[1], &a);
        x[0] = r;
        if (!r) x[1] = a;
        return;
    }
    case 0x02: x[0] = mem_set_permission(x[0], x[1], (uint32_t)x[2]); return;
    case 0x03: x[0] = mem_set_attribute(x[0], x[1], (uint32_t)x[2], (uint32_t)x[3]); return;
    case 0x04:   // MapMemory(x0 dst, x1 src, x2 size)
        x[0] = on_main_stack(x[0], x[2]) ? KERN_R_INVALID_STATE_MEM : mem_map(x[0], x[1], x[2]);
        return;
    case 0x05: x[0] = mem_unmap(x[0], x[1], x[2]); return;
    case 0x06: {   // QueryMemory(x0 MemoryInfo*, x2 address -> w1 page info)
        RtMemoryInfo mi;
        uint32_t r = mem_query(x[2], &mi);
        main_stack_query(x[2], &mi);
        if (!r && x[0]) memcpy((void *)(uintptr_t)x[0], &mi, sizeof mi);
        x[0] = r;
        x[1] = 0;
        return;
    }
    case 0x2C: x[0] = mem_map_physical(x[0], x[1]); return;
    case 0x2D: x[0] = mem_unmap_physical(x[0], x[1]); return;
    case 0x26: {   // Break(x0 reason, x1 info, x2 info size); bit 31 of the reason: notification only
        uint32_t result = 0;
        if (x[1] && x[2] >= 4 && readable(x[1], 4)) memcpy(&result, (const void *)(uintptr_t)x[1], 4);
        char at[160];
        rt_log("guest Break reason %#llx result %#x at %s", (unsigned long long)x[0], result,
               ldr_describe(pc, at, sizeof at));
        if (x[0] & 0x80000000ull) {
            x[0] = R_OK;
            return;
        }
        svc_fatal(c, imm, pc, "Break (the SDK aborted)");
    }
    case 0x27: {   // OutputDebugString(x0 text, x1 size)
        uint64_t n = x[1];
        if (n > 65536) svc_fatal(c, imm, pc, "debug string longer than 64 KiB");
        if (n) {
            fflush(stdout);
            rt_log_raw((const char *)(uintptr_t)x[0], n);
        }
        x[0] = R_OK;
        return;
    }
    case 0x29: get_info(c, pc); return;
    case 0x07: {   // ExitProcess, as closing the window (gfx_window_closed): pipeline cache, shader records and
                   // profile saved, logs flushed, exit code 0
        char at[160];
        rt_log("guest ExitProcess at %s; exiting", ldr_describe(pc, at, sizeof at));
        fs_save_flush();
        vk_pipeline_save();
        prof_write();
        fflush(NULL);
        if (svc_test_exit_hook) return svc_test_exit_hook(0);
        plat_exit(0);
    }
    }
    svc_fatal(c, imm, pc, "not implemented");
}
