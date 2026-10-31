// Sampling profiler (TOTK_PROFILE=<path>) and per-frame timing (TOTK_TRACE=perf). tools/prof_report.py reads the profile.
// A host thread wakes every 1 ms and, for every other thread, suspends it, reads Rip and resumes it. The sampler never
// allocates or takes a lock between SuspendThread and ResumeThread and never runs guest code. The profile is text:
//   totk-prof 1 / base <hex> / period_us / samples N / thread <tid> <samples> <in-exe samples> / module <base> <size> <path> /
//   rip <hex> <count> (the top PROF_TOP Rips) / stack <count> <rip> <caller> ... (TOTK_PROFILE_RENDER or
//   TOTK_PROFILE_CHAINS only, the top PROF_STACKS call chains)
// With TOTK_PROFILE_RENDER (the render thread) or TOTK_PROFILE_CHAINS=1 (every thread, chains of all threads summed: for
// short frame ranges, as each sample copies a stack) the sampler also copies up to STK_COPY bytes of the suspended
// thread's stack and, after
// ResumeThread, unwinds the copy with the .pdata tables (RtlVirtualUnwind), so it takes no lock while the thread is
// suspended. The unwind stops at the first frame outside the copy; deeper callers are cut.
// TOTK_PROFILE_SLOW=<ms> keeps every thread's chains of the last half second in a ring (a thread outside the exe, mostly
// waiting, copies 8 KiB of stack, one in it STK_COPY; a thread that ran no cycle since its previous sample is not
// suspended and repeats its previous chain; a sample that took over 2 ms to stop its thread, which happens while the
// thread is in a long kernel call, is written as `tstall <tid> <ms into the window> <ms stopping> <rip>...` with the
// chain where the thread stopped) and, after a present at least <ms> after the previous one, writes
// that interval's samples (plus the 20 ms before) to <TOTK_PROFILE>.f<frame>: the profile format, one `stack 1` line per
// sample, and `tstack <tid> <ms into the window> <rip>...` lines that keep the thread and the time. At most 64 dumps.
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "flight.h"
#include "gfx.h"
#include "host.h"
#include "perfmon.h"
#include "prof.h"

#define render_tid prof_render_tid
#define main_tid prof_main_tid
#define replay_tid prof_replay_tid

#define PROF_SLOTS (1u << 16)   // Rip hash table, open addressing; a full table drops new Rips
#define PROF_TOP 4000           // more than the 200 asked for: the report sums per function
#define PROF_THREADS 512
#define STK_SLOTS (1u << 15)      // call-chain hash table, open addressing; a full table drops new chains
#define PROF_STACKS 6000
#define STK_COPY (64u << 10)      // stack bytes copied per sample
#define STK_SLACK (64u << 10)     // zeros after the copy: a frame reaching past it reads zeros, and a zero Rip ends the chain

typedef struct { uint64_t rip, n; } RipCount;
#define PROF_DEPTH 24             // frames kept per call chain, the sampled Rip first
// cycles, depth, last: TOTK_PROFILE_SLOW only, the thread's cycle count and chain at its previous sample
typedef struct { DWORD tid; HANDLE h; uint64_t n, in_exe, cycles; uint32_t depth; uint64_t last[PROF_DEPTH]; } ThreadSlot;

static RipCount table[PROF_SLOTS];
typedef struct { uint64_t hash, n; uint32_t depth; uint64_t rip[PROF_DEPTH]; } StackCount;
static StackCount stacks[STK_SLOTS];
static uint64_t stacks_dropped;
static uint8_t stk_buf[STK_COPY + STK_SLACK] __attribute__((aligned(16)));   // the slack is never written
static ThreadSlot threads[PROF_THREADS];
static int n_threads;
static uint64_t total, dropped;
static const char *out_path;
static HANDLE sampler;
static volatile LONG stop, written;
static uintptr_t exe_lo, exe_hi;
static int only_render;             // TOTK_PROFILE_RENDER=1: sample the render thread only
static int only_main;               // TOTK_PROFILE_MAIN=1: sample the guest main thread only
static int chains;                  // call chains are kept: TOTK_PROFILE_RENDER or TOTK_PROFILE_CHAINS
static volatile uint64_t cur_frame; // last presented frame
static uint64_t frame_lo, frame_hi; // TOTK_PROFILE_FRAMES=<lo>-<hi>: sample only while presents lo..hi-1 are pending
static int64_t world_lo, world_hi;  // TOTK_PROFILE_FRAMES=w<lo>-<hi>: only while the world clock is in [lo, hi) s (us)

#define SLOW_RING (1u << 15)        // TOTK_PROFILE_SLOW: about 0.5 s of samples of every thread
#define SLOW_DUMPS 64
typedef struct { uint64_t t; DWORD tid; uint32_t depth; uint64_t rip[PROF_DEPTH]; } Sample;
static Sample ring[SLOW_RING];
static uint64_t ring_n;
#define STALL_RING 1024
static Sample stalls[STALL_RING];   // t: when the stop began; rip[PROF_DEPTH - 1] holds the stop's ns
static uint64_t stall_n;
static uint64_t slow_ns;            // TOTK_PROFILE_SLOW in ns, 0 off
static volatile LONG64 dump_lo, dump_hi, dump_frame;   // the render thread asks for a window, the sampler writes it

static void count_rip(uint64_t rip) {
    uint32_t i = (uint32_t)((rip * 0x9E3779B97F4A7C15ull) >> 48);
    for (uint32_t k = 0; k < 64; k++, i = (i + 1) & (PROF_SLOTS - 1)) {
        if (table[i].rip == rip || !table[i].rip) {
            table[i].rip = rip;
            table[i].n++;
            return;
        }
    }
    dropped++;
}

static void count_stack(const uint64_t *rip, uint32_t depth) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t d = 0; d < depth; d++) h = (h ^ rip[d]) * 0x100000001b3ull;
    h |= 1;   // 0 marks a free slot
    uint32_t i = (uint32_t)(h >> 49);
    for (uint32_t k = 0; k < 64; k++, i = (i + 1) & (STK_SLOTS - 1)) {
        StackCount *s = &stacks[i];
        if (!s->hash) {
            s->hash = h, s->depth = depth;
            memcpy(s->rip, rip, depth * sizeof *rip);
        } else if (s->hash != h || s->depth != depth || memcmp(s->rip, rip, depth * sizeof *rip)) {
            continue;
        }
        s->n++;
        return;
    }
    stacks_dropped++;
}

// Unwinds the stack copied to stk_buf from real address lo (n bytes). Registers that point into the copied range are
// moved into the copy, so frame-pointer frames read the copy too. Returns the number of frames.
static uint32_t unwind_copy(CONTEXT *c, uintptr_t lo, size_t n, uint64_t *rip) {
    uintptr_t buf = (uintptr_t)stk_buf, end = buf + n;
    DWORD64 *regs[] = {&c->Rbx, &c->Rbp, &c->Rsi, &c->Rdi, &c->R12, &c->R13, &c->R14, &c->R15};
    for (unsigned i = 0; i < sizeof regs / sizeof *regs; i++)
        if (*regs[i] >= lo && *regs[i] < lo + n) *regs[i] = *regs[i] - lo + buf;
    c->Rsp = buf;
    uint32_t d = 0;
    while (d < PROF_DEPTH && c->Rip && c->Rsp >= buf && c->Rsp + 8 <= end) {
        rip[d++] = c->Rip;
        DWORD64 base;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c->Rip, &base, NULL);
        if (f) {
            void *hd;
            DWORD64 est;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c->Rip, f, c, &hd, &est, NULL);
        } else {   // a leaf function: the return address is on top
            c->Rip = *(DWORD64 *)c->Rsp;
            c->Rsp += 8;
        }
    }
    return d;
}

// Re-lists the process's threads: keeps the handle of a thread seen before, opens new ones, closes the gone ones.
static void refresh_threads(DWORD self) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    static ThreadSlot next[PROF_THREADS];
    int n = 0;
    THREADENTRY32 te = {.dwSize = sizeof te};
    DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self || n == PROF_THREADS) continue;
        ThreadSlot *old = NULL;
        for (int i = 0; i < n_threads; i++)
            if (threads[i].tid == te.th32ThreadID) old = &threads[i];
        if (old) {
            next[n++] = *old;
            old->h = NULL;
        } else {
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (h) next[n++] = (ThreadSlot){.tid = te.th32ThreadID, .h = h};
        }
    }
    CloseHandle(snap);
    for (int i = 0; i < n_threads; i++)
        if (threads[i].h) CloseHandle(threads[i].h);   // exited; its counts are lost with the slot
    memcpy(threads, next, sizeof(ThreadSlot) * n);
    n_threads = n;
}

static void write_modules(FILE *f) {
    HMODULE mods[1024];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; i++) {
        MODULEINFO mi;
        char path[MAX_PATH];
        if (GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi) && GetModuleFileNameA(mods[i], path, sizeof path))
            fprintf(f, "module %llx %lx %s\n", (unsigned long long)(uintptr_t)mi.lpBaseOfDll, mi.SizeOfImage, path);
    }
}

// The sampler, between ticks: the samples of the window the render thread asked for.
static void slow_dump(void) {
    uint64_t lo = (uint64_t)dump_lo, hi = (uint64_t)dump_hi, frame = (uint64_t)dump_frame;
    static int dumps;
    char path[1100];
    snprintf(path, sizeof path, "%s.f%llu", out_path, (unsigned long long)frame);
    FILE *f = dumps < SLOW_DUMPS ? fopen(path, "w") : NULL;
    if (f) {
        dumps++;
        fprintf(f, "totk-prof 1\nbase %llx\nperiod_us 1000\nframe %llu interval_ms %.1f\nmain %u render %u replay %u\n",
                (unsigned long long)(uintptr_t)GetModuleHandleW(NULL), (unsigned long long)frame, (hi - lo) / 1e6 - 20,
                main_tid, render_tid, replay_tid);
        write_modules(f);
        uint64_t first = ring_n > SLOW_RING ? ring_n - SLOW_RING : 0;
        for (int pass = 0; pass < 2; pass++)
            for (uint64_t k = first; k < ring_n; k++) {
                const Sample *s = &ring[k & (SLOW_RING - 1)];
                if (s->t < lo || s->t > hi) continue;
                if (pass) fprintf(f, "tstack %lu %.1f", s->tid, (s->t - lo) / 1e6);
                else fprintf(f, "stack 1");
                for (uint32_t d = 0; d < s->depth; d++) fprintf(f, " %llx", (unsigned long long)s->rip[d]);
                fputc('\n', f);
            }
        for (uint64_t k = stall_n > STALL_RING ? stall_n - STALL_RING : 0; k < stall_n; k++) {
            const Sample *s = &stalls[k & (STALL_RING - 1)];
            if (s->t + s->rip[PROF_DEPTH - 1] < lo || s->t > hi) continue;
            fprintf(f, "tstall %lu %.1f %.1f", s->tid, ((double)s->t - (double)lo) / 1e6, s->rip[PROF_DEPTH - 1] / 1e6);
            for (uint32_t d = 0; d < s->depth; d++) fprintf(f, " %llx", (unsigned long long)s->rip[d]);
            fputc('\n', f);
        }
        fclose(f);
        rt_log("prof: slow frame %llu (%.1f ms) -> %s", (unsigned long long)frame, (hi - lo) / 1e6 - 20, path);
    }
    InterlockedExchange64(&dump_hi, 0);
}

static DWORD WINAPI sampler_main(void *arg) {
    (void)arg;
    DWORD self = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    static CONTEXT ctx __attribute__((aligned(16)));
    for (unsigned tick = 0; !stop; tick++) {
        if (timer) {
            LARGE_INTEGER due = {.QuadPart = -10000};   // 1 ms
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, 10);
        } else {
            Sleep(1);
        }
        if (tick % 64 == 0) refresh_threads(self);
        if (dump_hi) slow_dump();
        if (frame_hi && (cur_frame < frame_lo || cur_frame >= frame_hi)) continue;
        if (world_hi) {   // -1 before the world clock starts
            int64_t w = frame_step_world_us();
            if (w < world_lo || w >= world_hi) continue;
        }
        uint64_t now = slow_ns ? kern_now_ns() : 0;
        for (int i = 0; i < n_threads; i++) {
            ThreadSlot *t = &threads[i];
            if ((only_render && t->tid != render_tid) || (only_main && t->tid != main_tid)) continue;
            uint64_t cyc = 0;
            if (slow_ns && t->depth && QueryThreadCycleTime(t->h, &cyc) && cyc == t->cycles) {   // still waiting
                Sample *s = &ring[ring_n++ & (SLOW_RING - 1)];
                s->t = now, s->tid = t->tid, s->depth = t->depth;
                memcpy(s->rip, t->last, t->depth * sizeof *t->last);
                continue;
            }
            ctx.ContextFlags = chains ? CONTEXT_CONTROL | CONTEXT_INTEGER : CONTEXT_CONTROL;
            uint64_t stop0 = slow_ns ? kern_now_ns() : 0;
            if (SuspendThread(t->h) == (DWORD)-1) continue;
            BOOL ok = GetThreadContext(t->h, &ctx);   // returns once the thread has stopped in user mode
            uint64_t stop_ns = slow_ns ? kern_now_ns() - stop0 : 0;
            size_t n = 0;
            MEMORY_BASIC_INFORMATION mb;
            // a thread stopped between its sub rsp and the stack probe has Rsp in the guard page: touching it would fault
            if (ok && chains && VirtualQuery((void *)ctx.Rsp, &mb, sizeof mb) && mb.State == MEM_COMMIT &&
                !(mb.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
                n = (uintptr_t)mb.BaseAddress + mb.RegionSize - ctx.Rsp;   // the committed stack above Rsp
                size_t cap = slow_ns && (ctx.Rip < exe_lo || ctx.Rip >= exe_hi) ? 8u << 10 : STK_COPY;
                if (n > cap) n = cap;
                memcpy(stk_buf, (void *)ctx.Rsp, n);
            }
            ResumeThread(t->h);
            if (!ok) continue;
            count_rip(ctx.Rip);
            uint64_t chain[PROF_DEPTH] = {ctx.Rip};
            uint32_t d = n ? unwind_copy(&ctx, ctx.Rsp, n, chain) : 0;
            if (d) count_stack(chain, d);
            if (slow_ns) {
                Sample *s = &ring[ring_n++ & (SLOW_RING - 1)];
                s->t = now, s->tid = t->tid, s->depth = d ? d : 1;
                memcpy(s->rip, chain, s->depth * sizeof *chain);
                if (stop_ns > 2000000) {
                    Sample *w = &stalls[stall_n++ & (STALL_RING - 1)];
                    *w = *s;
                    w->t = stop0;
                    if (w->depth > PROF_DEPTH - 1) w->depth = PROF_DEPTH - 1;
                    w->rip[PROF_DEPTH - 1] = stop_ns;
                }
                QueryThreadCycleTime(t->h, &t->cycles);   // after ResumeThread: the cycles the sample itself cost
                t->depth = s->depth;
                memcpy(t->last, chain, s->depth * sizeof *chain);
            }
            total++;
            t->n++;
            if (ctx.Rip >= exe_lo && ctx.Rip < exe_hi) t->in_exe++;
        }
    }
    return 0;
}

static int cmp_desc(const void *a, const void *b) {
    uint64_t x = ((const RipCount *)a)->n, y = ((const RipCount *)b)->n;
    return (x < y) - (x > y);
}

static int cmp_stack_desc(const void *a, const void *b) {
    uint64_t x = ((const StackCount *)a)->n, y = ((const StackCount *)b)->n;
    return (x < y) - (x > y);
}

void prof_sampler_write(void) {
    if (!out_path || InterlockedExchange(&written, 1)) return;
    InterlockedExchange(&stop, 1);
    if (WaitForSingleObject(sampler, 2000) != WAIT_OBJECT_0) return;   // sampler stuck: no half-written counts
    FILE *f = fopen(out_path, "w");
    if (!f) {
        rt_log("prof: cannot write %s", out_path);
        return;
    }
    fprintf(f, "totk-prof 1\nbase %llx\nperiod_us 1000\nsamples %llu\ndropped %llu\n", (unsigned long long)(uintptr_t)GetModuleHandleW(NULL),
            (unsigned long long)total, (unsigned long long)dropped);
    for (int i = 0; i < n_threads; i++)
        fprintf(f, "thread %lu %llu %llu\n", threads[i].tid, (unsigned long long)threads[i].n, (unsigned long long)threads[i].in_exe);
    write_modules(f);
    int cnt = 0;
    for (uint32_t i = 0; i < PROF_SLOTS; i++)
        if (table[i].rip) table[cnt++] = table[i];
    qsort(table, cnt, sizeof *table, cmp_desc);
    for (int i = 0; i < cnt && i < PROF_TOP; i++) fprintf(f, "rip %llx %llu\n", (unsigned long long)table[i].rip, (unsigned long long)table[i].n);
    int ns = 0;
    for (uint32_t i = 0; i < STK_SLOTS; i++)
        if (stacks[i].hash) stacks[ns++] = stacks[i];
    qsort(stacks, ns, sizeof *stacks, cmp_stack_desc);
    for (int i = 0; i < ns && i < PROF_STACKS; i++) {
        fprintf(f, "stack %llu", (unsigned long long)stacks[i].n);
        for (uint32_t d = 0; d < stacks[i].depth; d++) fprintf(f, " %llx", (unsigned long long)stacks[i].rip[d]);
        fputc('\n', f);
    }
    if (stacks_dropped) fprintf(f, "stacks_dropped %llu\n", (unsigned long long)stacks_dropped);
    fclose(f);
    rt_log("prof: %llu samples, %d Rips -> %s", (unsigned long long)total, cnt, out_path);
}

int prof_sampler_start(void) {
    out_path = cfg_get("TOTK_PROFILE");
    if (!out_path || !*out_path) {
        out_path = NULL;
        return 0;
    }
    only_render = cfg_get("TOTK_PROFILE_RENDER") != NULL;
    only_main = !only_render && cfg_get("TOTK_PROFILE_MAIN") != NULL;
    const char *ch = cfg_get("TOTK_PROFILE_CHAINS");
    const char *sl = cfg_get("TOTK_PROFILE_SLOW");
    slow_ns = sl && atoi(sl) > 0 ? (uint64_t)atoi(sl) * 1000000ull : 0;
    chains = only_render || only_main || slow_ns || (ch && *ch && strcmp(ch, "0"));
    const char *fr = cfg_get("TOTK_PROFILE_FRAMES");
    double wl, wh;
    if (fr && fr[0] == 'w' && sscanf(fr + 1, "%lf-%lf", &wl, &wh) == 2 && wl >= 0 && wl < wh)
        world_lo = (int64_t)(wl * 1e6), world_hi = (int64_t)(wh * 1e6);
    else if (fr && sscanf(fr, "%llu-%llu", (unsigned long long *)&frame_lo, (unsigned long long *)&frame_hi) != 2) frame_hi = 0;
    MODULEINFO mi;
    if (GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(NULL), &mi, sizeof mi)) {
        exe_lo = (uintptr_t)mi.lpBaseOfDll;
        exe_hi = exe_lo + mi.SizeOfImage;
    }
    sampler = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
    if (!sampler) {
        rt_log("prof: cannot start the sampler (error %lu)", GetLastError());
        out_path = NULL;
        return 0;
    }
    return 1;
}

// perf_present, every present: the frame the sampler's TOTK_PROFILE_FRAMES window counts, and a TOTK_PROFILE_SLOW
// dump of a present at least slow_ns after the previous one (d 0 on the first present).
void prof_sampler_frame(uint64_t frame, uint64_t now, uint64_t d) {
    cur_frame = frame;
    if (slow_ns && d >= slow_ns && !dump_hi) {
        dump_lo = (LONG64)(now - d - 20000000ull);
        dump_frame = (LONG64)frame;
        InterlockedExchange64(&dump_hi, (LONG64)now);
    }
}
