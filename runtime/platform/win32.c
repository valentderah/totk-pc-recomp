// Windows implementation of platform.h: CreateThread, QueryPerformanceCounter, high-resolution waitable timers,
// SRW locks and condition variables.
#include <io.h>
#include <stdio.h>
#include <windows.h>
#include <bcrypt.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <restartmanager.h>
#include <shellapi.h>
#include <timeapi.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "platform.h"

_Static_assert(sizeof(PlatMutex) == sizeof(SRWLOCK), "PlatMutex holds an SRWLOCK");
_Static_assert(sizeof(PlatCond) == sizeof(CONDITION_VARIABLE), "PlatCond holds a CONDITION_VARIABLE");

// ---- threads

struct PlatThread {
    HANDLE h;
    PlatThreadFn fn;
    void *arg;
};

static DWORD WINAPI thread_main(LPVOID p) {
    PlatThread *t = p;
    t->fn(t->arg);
    return 0;
}

PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    return plat_thread_new_ex(fn, arg, name, 0);
}

PlatThread *plat_thread_new_ex(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    t->h = CreateThread(NULL, stack ? stack : PLAT_STACK_DEFAULT, thread_main, t,
                        CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t->h) { free(t); return NULL; }
    if (name) {
        wchar_t w[64];
        if (MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 64)) SetThreadDescription(t->h, w);
    }
    ResumeThread(t->h);
    return t;
}

void plat_thread_join(PlatThread *t) {
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

int plat_thread_join_ns(PlatThread *t, uint64_t ns) {
    uint64_t ms = (ns + 999999) / 1000000;
    if (WaitForSingleObject(t->h, ms >= INFINITE ? INFINITE - 1 : (DWORD)ms) != WAIT_OBJECT_0) return 0;
    CloseHandle(t->h);
    free(t);
    return 1;
}

// The thread keeps using t->fn and t->arg, so the struct stays allocated (a few bytes per detached thread).
void plat_thread_detach(PlatThread *t) {
    CloseHandle(t->h);
    t->h = NULL;
}

int plat_cpu_cores(uint64_t *all, uint64_t *first) {
    DWORD_PTR proc, sys;
    *all = *first = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)) return 0;
    *all = proc;
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *buf = len ? malloc(len) : NULL;
    int cores = 0;
    if (buf && GetLogicalProcessorInformationEx(RelationProcessorCore, buf, &len))
        for (DWORD off = 0; off < len;) {
            SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *e = (void *)((char *)buf + off);
            KAFFINITY m = e->Processor.GroupMask[0].Mask & proc;
            if (e->Processor.GroupMask[0].Group == 0 && m) *first |= m & -m, cores++;   // the lowest of the core's
            off += e->Size;
        }
    free(buf);
    return cores;
}

int plat_process_affinity(uint64_t mask) { return SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)mask) != 0; }

uint64_t plat_thread_cpu_ns(uint32_t tid) {
    HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    FILETIME a, b, k, u;
    uint64_t ns = 0;
    if (th && GetThreadTimes(th, &a, &b, &k, &u))
        ns = (((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime)) *
             100;
    if (th) CloseHandle(th);
    return ns;
}

// ---- thread interruption: SuspendThread, Get/SetThreadContext, ResumeThread.
struct PlatIntr {
    CONTEXT ctx;   // first: CONTEXT wants 16-byte alignment
    HANDLE h;
};

PlatIntr *plat_intr_self(void) {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                          FALSE, GetCurrentThreadId());
    PlatIntr *t = h ? _aligned_malloc(sizeof *t, 16) : NULL;
    if (!t) {
        if (h) CloseHandle(h);
        return NULL;
    }
    t->h = h;
    return t;
}

void plat_intr_free(PlatIntr *t) {
    if (!t) return;
    CloseHandle(t->h);
    _aligned_free(t);
}

int plat_intr_stop(PlatIntr *t, uint64_t *rip, uint64_t *rsp) {
    if (SuspendThread(t->h) == (DWORD)-1) return 0;
    memset(&t->ctx, 0, sizeof t->ctx);
    t->ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;   // set back whole, so the kernel restores every GPR
    if (!GetThreadContext(t->h, &t->ctx)) {
        ResumeThread(t->h);
        return 0;
    }
    *rip = t->ctx.Rip;
    *rsp = t->ctx.Rsp;
    return 1;
}

int plat_intr_resume(PlatIntr *t, uint64_t to) {
    int ok = 1;
    if (to) {
        t->ctx.Rip = to;
        ok = SetThreadContext(t->h, &t->ctx) != 0;
    }
    ResumeThread(t->h);
    return ok;
}

// ---- fibers: Windows fibers with FIBER_FLAG_FLOAT_SWITCH (the MXCSR and x87 control word switch with them).
struct PlatFiber {
    void *h;
    void (*fn)(void *arg);
    void *arg;
};

static __thread PlatFiber self_fiber;

PlatFiber *plat_fiber_self(void) {
    self_fiber.h = ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH);
    return self_fiber.h ? &self_fiber : NULL;
}

void plat_fiber_unself(void) {
    ConvertFiberToThread();
    self_fiber.h = NULL;
}

static void WINAPI fiber_start(void *p) {
    PlatFiber *f = p;
    f->fn(f->arg);
}

PlatFiber *plat_fiber_new(size_t stack, void (*fn)(void *arg), void *arg) {
    PlatFiber *f = malloc(sizeof *f);
    if (!f) return NULL;
    f->fn = fn, f->arg = arg;
    f->h = CreateFiberEx(0x10000, stack, FIBER_FLAG_FLOAT_SWITCH, fiber_start, f);
    if (!f->h) {
        free(f);
        return NULL;
    }
    return f;
}

void plat_fiber_switch(PlatFiber *to) { SwitchToFiber(to->h); }

void plat_fiber_free(PlatFiber *f) {
    if (!f) return;
    DeleteFiber(f->h);
    free(f);
}

int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = plat_thread_new_ex(fn, arg, name, stack);
    if (t) plat_thread_detach(t);
    return t != NULL;
}

uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask) {
    return (uint64_t)SetThreadAffinityMask(t ? t->h : GetCurrentThread(), (DWORD_PTR)mask);
}

uint32_t plat_cpu_count(void) {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? (uint32_t)n : 1;
}

// The processors of the process's affinity mask: what `start /affinity`, Task Manager's "Set affinity" or a launcher
// leaves this process. A process that spans processor groups (more than 64 logical processors) gets no mask from
// GetProcessAffinityMask (both masks 0): every active processor counts then.
uint32_t plat_cpu_usable(void) {
    DWORD_PTR proc = 0, sys = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && proc && sys) {
        uint32_t n = (uint32_t)__builtin_popcountll((unsigned long long)proc);
        if (n) return n;
    }
    return plat_cpu_count();
}

// PLAT_PRIO_* are the THREAD_PRIORITY_* values.
void plat_thread_priority(int prio) { SetThreadPriority(GetCurrentThread(), prio); }
void plat_thread_set_priority(PlatThread *t, int prio) { SetThreadPriority(t->h, prio); }
int plat_thread_priority_get(void) { return GetThreadPriority(GetCurrentThread()); }

void plat_yield(void) { SwitchToThread(); }

// ---- time

static uint64_t qpc_freq(void) {
    static uint64_t freq;
    if (!freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = (uint64_t)f.QuadPart;
    }
    return freq;
}

uint64_t plat_counter(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

uint64_t plat_counter_hz(void) { return qpc_freq(); }

// The default timer tick is 15.6 ms; a 1 ms wait would cost a full tick.
void plat_timer_1ms(void) { timeBeginPeriod(1); }

static PlatScale qpc_ns;   // set before main, so no thread reads it half written

__attribute__((constructor)) static void qpc_ns_init(void) { qpc_ns = plat_scale(1000000000u, qpc_freq()); }

uint64_t plat_now_ns(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return plat_scaled(qpc_ns, (uint64_t)t.QuadPart);
}

// A high-resolution waitable timer per sleeping thread (Windows 10 1803+; one handle per thread, kept until exit)
// wakes within about 0.1-0.5 ms; the last PLAT_SPIN_NS are spent yielding so the deadline is met closely.
// Without that timer kind, Sleep(1) granularity applies.
#define PLAT_SPIN_NS 300000u

static __thread HANDLE sleep_timer;
static __thread int sleep_timer_tried;

void plat_wait_ns(uint64_t ns) {
    if (!sleep_timer_tried) {
        sleep_timer_tried = 1;
        sleep_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    LARGE_INTEGER due = {.QuadPart = -(LONGLONG)(ns / 100)};   // relative, 100 ns units
    if (!due.QuadPart) due.QuadPart = -1;
    if (sleep_timer && SetWaitableTimerEx(sleep_timer, &due, 0, NULL, NULL, NULL, 0))
        WaitForSingleObject(sleep_timer, INFINITE);
    else Sleep((DWORD)((ns + 999999) / 1000000));
}

void plat_sleep_ns(uint64_t ns) {
    uint64_t end = plat_now_ns() + ns;
    if (ns > PLAT_SPIN_NS) plat_wait_ns(ns - PLAT_SPIN_NS);
    while (plat_now_ns() < end) SwitchToThread();
}

void plat_sleep_ms(uint32_t ms) { Sleep(ms); }

// ---- locks and condition variables

void plat_mutex_init(PlatMutex *m) { InitializeSRWLock((PSRWLOCK)m); }
void plat_mutex_lock(PlatMutex *m) { AcquireSRWLockExclusive((PSRWLOCK)m); }
int plat_mutex_trylock(PlatMutex *m) { return TryAcquireSRWLockExclusive((PSRWLOCK)m) != 0; }
void plat_mutex_unlock(PlatMutex *m) { ReleaseSRWLockExclusive((PSRWLOCK)m); }

void plat_mutex_lock_shared(PlatMutex *m) { AcquireSRWLockShared((PSRWLOCK)m); }
int plat_mutex_trylock_shared(PlatMutex *m) { return TryAcquireSRWLockShared((PSRWLOCK)m) != 0; }
void plat_mutex_unlock_shared(PlatMutex *m) { ReleaseSRWLockShared((PSRWLOCK)m); }

void plat_cond_init(PlatCond *c) { InitializeConditionVariable((PCONDITION_VARIABLE)c); }
void plat_cond_wait(PlatCond *c, PlatMutex *m) { SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, INFINITE, 0); }

int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    uint64_t ms = (ns + 999999) / 1000000;
    DWORD to = ms >= INFINITE ? INFINITE - 1 : (DWORD)ms;
    return SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, to, 0) ? 1 : GetLastError() != ERROR_TIMEOUT;
}

void plat_cond_signal(PlatCond *c) { WakeConditionVariable((PCONDITION_VARIABLE)c); }
void plat_cond_broadcast(PlatCond *c) { WakeAllConditionVariable((PCONDITION_VARIABLE)c); }

// ---- files

static int widen(const char *s, wchar_t *w, int cap) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap);
    if (!n) w[0] = 0;
    return n;
}

#define WPATH 2048

int plat_file_exists(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int plat_dir_exists(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int plat_full_path(const char *path, char *out, size_t cap) {
    wchar_t w[WPATH], full[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    DWORD n = GetFullPathNameW(w, WPATH, full, NULL);
    if (!n || n >= WPATH) return 0;
    return WideCharToMultiByte(CP_UTF8, 0, full, -1, out, (int)cap, NULL, NULL) != 0;
}

void plat_file_mkdir_p(const char *path) {
    wchar_t tmp[WPATH];
    if (!widen(path, tmp, WPATH)) return;
    for (wchar_t *p = tmp + 1; *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            wchar_t c = *p;
            *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = c;
        }
    }
    CreateDirectoryW(tmp, NULL);
}

uint8_t *plat_file_read(const char *path, uint64_t *size) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    uint8_t *buf = NULL;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1ll << 30)) {
        buf = malloc((size_t)sz.QuadPart + 1);
        DWORD got = 0;
        if (buf && (!ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL) || got != sz.QuadPart)) {
            free(buf);
            buf = NULL;
        }
        if (buf) {
            buf[sz.QuadPart] = 0;
            *size = (uint64_t)sz.QuadPart;
        }
    }
    CloseHandle(h);
    return buf;
}

int plat_file_write(const char *path, const void *data, size_t n) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    int ok = WriteFile(h, data, (DWORD)n, &put, NULL) && put == n;
    CloseHandle(h);
    return ok;
}

PlatAppend *plat_append_open(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    return h == INVALID_HANDLE_VALUE ? NULL : (PlatAppend *)h;
}

int plat_append_write(PlatAppend *f, const void *data, size_t n) {
    // Offset and OffsetHigh all ones: WriteFile appends at the end of the file, atomically against other writers.
    OVERLAPPED o = {.Offset = 0xffffffff, .OffsetHigh = 0xffffffff};
    DWORD put = 0;
    return WriteFile((HANDLE)f, data, (DWORD)n, &put, &o) && put == n;
}

int plat_file_replace(const char *from, const char *to) {
    wchar_t a[WPATH], b[WPATH];
    if (!widen(from, a, WPATH) || !widen(to, b, WPATH)) return 0;
    return MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    char tmp[WPATH + 48];
    wchar_t a[WPATH], b[WPATH];
    int len = snprintf(tmp, sizeof tmp, "%s.%u.%u.tmp", path, (unsigned)GetCurrentProcessId(), (unsigned)GetCurrentThreadId());
    if (len < 0 || len >= (int)sizeof tmp || !widen(tmp, a, WPATH) || !widen(path, b, WPATH)) return 0;
    HANDLE h = CreateFileW(a, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    int ok = WriteFile(h, data, (DWORD)n, &put, NULL) && put == n && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 1;
    DeleteFileW(a);
    return 0;
}

int plat_file_flush(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    return ok;
}

int plat_file_sync(FILE *f) { return !fflush(f) && !_commit(_fileno(f)); }

void plat_file_delete(const char *path) {
    wchar_t w[WPATH];
    if (widen(path, w, WPATH)) DeleteFileW(w);
}

int plat_file_truncate(const char *path, uint64_t size) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER at = {.QuadPart = (LONGLONG)size};
    int ok = SetFilePointerEx(h, at, NULL, FILE_BEGIN) && SetEndOfFile(h);
    CloseHandle(h);
    return ok;
}

int64_t plat_file_size(const char *path) {
    wchar_t w[WPATH];
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!widen(path, w, WPATH) || !GetFileAttributesExW(w, GetFileExInfoStandard, &d)) return -1;
    return (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
}

int plat_file_set_readonly(const char *path, int on) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    return SetFileAttributesW(w, on ? a | FILE_ATTRIBUTE_READONLY : a & ~(DWORD)FILE_ATTRIBUTE_READONLY) != 0;
}

int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *name, void *ctx), void *ctx) {
    wchar_t wd[WPATH], we[64], pat[WPATH + 64];
    if (!widen(dir, wd, WPATH) || !widen(ext, we, 64)) return 0;
    swprintf(pat, WPATH + 64, L"%ls\\*%ls", wd, we);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char name[1024];
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
        if (!cb(name, ctx)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 1;
}

// ---- virtual memory: VirtualAlloc reservations, MEM_WRITE_WATCH for the watched ones.

int plat_mem_reserve(uint64_t addr, uint64_t n, int watch) {
    void *p = VirtualAlloc((void *)(uintptr_t)addr, n, MEM_RESERVE | (watch ? MEM_WRITE_WATCH : 0), PAGE_NOACCESS);
    if (p && p != (void *)(uintptr_t)addr) {
        VirtualFree(p, 0, MEM_RELEASE);
        p = NULL;
    }
    return p != NULL;
}

void *plat_mem_alloc(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }

int plat_mem_commit(uint64_t addr, size_t n) {
    return VirtualAlloc((void *)(uintptr_t)addr, n, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

// Decommitted pages come back zeroed when committed again.
int plat_mem_decommit(uint64_t addr, uint64_t n) { return VirtualFree((void *)(uintptr_t)addr, n, MEM_DECOMMIT) != 0; }

int plat_mem_protect(uint64_t addr, uint64_t n, int prot) {
    DWORD old;
    DWORD p = prot == PLAT_PROT_RW ? PAGE_READWRITE : prot == PLAT_PROT_READ ? PAGE_READONLY : PAGE_NOACCESS;
    return VirtualProtect((void *)(uintptr_t)addr, n, p, &old) != 0;
}

int plat_mem_state(uint64_t addr, uint64_t limit, uint64_t *end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void *)(uintptr_t)addr, &mbi, sizeof mbi)) {
        *end = limit;
        return PLAT_MEM_FREE;
    }
    uint64_t e = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    *end = e < limit ? e : limit;
    if (mbi.State == MEM_RESERVE) return PLAT_MEM_RESERVED;
    if (mbi.State != MEM_COMMIT) return PLAT_MEM_FREE;
    DWORD p = mbi.Protect & 0xff;
    return p == PAGE_READWRITE ? PLAT_PROT_RW : p == PAGE_READONLY ? PLAT_PROT_READ : PLAT_PROT_NONE;
}

// Adapted from LAS Recomp runtime/platform/windows.c:40-63 @ da35b83 (MIT, Copyright (c) 2026 Peiikai): GetWriteWatch
// with the reset flag.
uint64_t plat_mem_written(uint64_t addr, uint64_t n, uint64_t *pages, uint64_t max, int reset) {
    if (!max || !n) return 0;
    ULONG_PTR got = (ULONG_PTR)max;
    DWORD gran;
    _Static_assert(sizeof(PVOID) == sizeof(uint64_t), "page addresses are written straight into a uint64_t array");
    if (GetWriteWatch(reset ? WRITE_WATCH_FLAG_RESET : 0, (void *)(uintptr_t)addr, (SIZE_T)n, (PVOID *)pages, &got,
                      &gran))
        return UINT64_MAX;
    return got;
}

static int (*fault_hook)(uint64_t addr);

// A write access violation (ExceptionInformation[0] == 1) goes to the hook; registered first, before the crash handler.
static LONG CALLBACK fault_veh(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || er->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    return fault_hook((uint64_t)er->ExceptionInformation[1]) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

void plat_mem_fault_hook(int (*fn)(uint64_t addr)) {
    fault_hook = fn;
    AddVectoredExceptionHandler(1, fault_veh);
}

static void (*crash_fn)(const PlatCrash *c);

static LONG CALLBACK crash_veh(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    const char *what = code == EXCEPTION_ACCESS_VIOLATION ? "access violation"
                     : code == EXCEPTION_ILLEGAL_INSTRUCTION ? "illegal instruction"
                     : code == EXCEPTION_INT_DIVIDE_BY_ZERO ? "integer division by zero"
                     : code == EXCEPTION_PRIV_INSTRUCTION ? "privileged instruction"
                     : code == EXCEPTION_STACK_OVERFLOW ? "stack overflow"
                     : code == EXCEPTION_IN_PAGE_ERROR ? "in-page error" : NULL;
    if (!what) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
    MODULEINFO mi;
    if (!GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(NULL), &mi, sizeof mi) ||
        rip - (uintptr_t)mi.lpBaseOfDll >= mi.SizeOfImage)
        return EXCEPTION_CONTINUE_SEARCH;   // not our code: leave it to the handler that expects it
    PlatCrash c = {what, (uint32_t)code, rip, rip - (uintptr_t)mi.lpBaseOfDll, -1, 0};
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        c.access = (int)ep->ExceptionRecord->ExceptionInformation[0], c.addr = ep->ExceptionRecord->ExceptionInformation[1];
    crash_fn(&c);
    return EXCEPTION_CONTINUE_SEARCH;
}

// abort() and an invalid parameter to a C runtime function raise no exception: the C runtime ends the process with
// 0xc0000409 after calling the SIGABRT or invalid-parameter handler. The report names the host frames in this
// executable; the innermost is the caller of abort (assert) or of the C runtime function. Skipped frames: this function
// and the handler that called it.
__attribute__((noinline)) static void crash_from_runtime(const char *what, uint32_t code) {
    PlatCrash c = {what, code, 0, 0, -1, 0};
    void *f[62];
    USHORT n = RtlCaptureStackBackTrace(2, 62, f, NULL);
    MODULEINFO mi;
    if (GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(NULL), &mi, sizeof mi)) {
        uintptr_t base = (uintptr_t)mi.lpBaseOfDll;
        for (USHORT i = 0; i < n && c.nframes < (int)(sizeof c.frames / sizeof *c.frames); i++)
            if ((uintptr_t)f[i] - base < mi.SizeOfImage) c.frames[c.nframes++] = (uintptr_t)f[i];
        if (c.nframes) c.rip = c.frames[0], c.offset = c.rip - base;
    }
    crash_fn(&c);
}

static void abort_signal(int sig) { crash_from_runtime("abort", (uint32_t)sig); }

static void invalid_parameter(const wchar_t *expr, const wchar_t *func, const wchar_t *file, unsigned line,
                              uintptr_t reserved) {
    (void)expr, (void)func, (void)file, (void)line, (void)reserved;   // NULL and 0 in the release C runtime
    crash_from_runtime("invalid parameter to a C runtime function", 0xc0000417u);   // STATUS_INVALID_CRUNTIME_PARAMETER
}

void plat_crash_hook(void (*fn)(const PlatCrash *c)) {
    crash_fn = fn;
    AddVectoredExceptionHandler(0, crash_veh);
    signal(SIGABRT, abort_signal);
    _set_invalid_parameter_handler(invalid_parameter);
}

// A fresh process gets a fresh layout: Windows places DLLs, heaps and thread stacks at random addresses. The parent
// waits and exits with the child's code.
void plat_reexec(void) {
    fflush(stdout);
    fflush(stderr);
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    wchar_t *cmd = _wcsdup(GetCommandLineW());   // CreateProcessW may write its command line
    if (!cmd || !CreateProcessW(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "FATAL: re-running the process failed (error %lu)\n", GetLastError());
        ExitProcess(70);
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 70;
    GetExitCodeProcess(pi.hProcess, &code);
    ExitProcess(code);
}

int plat_mem_read(uint64_t addr, void *out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)addr, out, n, &got) && got == n;
}

int plat_mem_readable(uint64_t addr, uint64_t n) {
    for (uint64_t a = addr, end = addr + n; a < end || a == addr;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        a = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (a >= end) return 1;
    }
    return 1;
}

void *plat_aligned_alloc(size_t size, size_t align) { return _aligned_malloc(size, align); }
void plat_aligned_free(void *p) { _aligned_free(p); }

// ---- process

void plat_exit(int code) {
    fflush(stdout);
    fflush(stderr);
    ExitProcess((UINT)code);
}

// _putenv_s, not SetEnvironmentVariableA: getenv reads the C runtime's copy of the environment, which only the CRT's
// own calls update. An empty value removes the variable.
int plat_setenv(const char *name, const char *value) { return _putenv_s(name, value ? value : "") == 0; }

uint32_t plat_process_id(void) { return GetCurrentProcessId(); }

static uint64_t ft64(FILETIME f) { return (uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime; }

uint64_t plat_process_cpu_ns(void) {
    FILETIME cr, ex, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &cr, &ex, &k, &u)) return 0;
    return (ft64(k) + ft64(u)) * 100;
}

uint64_t plat_process_uptime_ns(void) {
    FILETIME cr, ex, k, u, now;
    if (!GetProcessTimes(GetCurrentProcess(), &cr, &ex, &k, &u)) return 0;
    GetSystemTimeAsFileTime(&now);
    return ft64(now) > ft64(cr) ? (ft64(now) - ft64(cr)) * 100 : 0;
}

uint64_t plat_unix_time_ns(void) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return (ft64(now) - 116444736000000000ull) * 100;   // FILETIME counts 100 ns from 1601
}

void plat_local_time(struct tm *out) {
    time_t t = time(NULL);
    localtime_s(out, &t);
}

int plat_localtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return !localtime_s(out, &t);
}

int plat_gmtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return !gmtime_s(out, &t);
}

long plat_tz_west(void) {
    long west = 0;
    _tzset();
    _get_timezone(&west);
    return west;
}

int plat_run_self(const char *env_name, const char *env_value, char *out, size_t cap, uint32_t timeout_ms) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = GetStdHandle(STD_INPUT_HANDLE),
                       .hStdOutput = wr, .hStdError = wr};
    PROCESS_INFORMATION pi;
    wchar_t *cmdline = _wcsdup(GetCommandLineW());
    SetEnvironmentVariableA(env_name, env_value);
    BOOL ok = cmdline && CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    SetEnvironmentVariableA(env_name, NULL);
    free(cmdline);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    size_t len = 0;
    DWORD got;
    char sink[512];
    while (len + 1 < cap && ReadFile(rd, out + len, (DWORD)(cap - 1 - len), &got, NULL) && got) len += got;
    if (cap) out[len] = 0;
    while (ReadFile(rd, sink, sizeof sink, &got, NULL) && got) {}
    CloseHandle(rd);
    if (WaitForSingleObject(pi.hProcess, timeout_ms ? timeout_ms : INFINITE) != WAIT_OBJECT_0)
        TerminateProcess(pi.hProcess, 1), WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

int plat_random(void *out, size_t n) {
    return BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

// FILE_FLAG_OVERLAPPED handles: a read names its offset, and each thread waits on an event of its own, so concurrent
// reads on one handle do not share the file pointer or the event.
PlatFile *plat_file_open(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    return h == INVALID_HANDLE_VALUE ? NULL : (PlatFile *)h;
}

PlatFile *plat_file_open_unbuffered(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, NULL);
    return h == INVALID_HANDLE_VALUE ? NULL : (PlatFile *)h;
}

void plat_file_close(PlatFile *f) {
    if (f) CloseHandle((HANDLE)f);
}

int64_t plat_file_pread(PlatFile *f, void *buf, uint32_t n, uint64_t off) {
    static __thread HANDLE ev;
    if (!ev && !(ev = CreateEventW(NULL, TRUE, FALSE, NULL))) return -1;
    OVERLAPPED ov = {.Offset = (DWORD)off, .OffsetHigh = (DWORD)(off >> 32), .hEvent = ev};
    DWORD got = 0;
    if (ReadFile((HANDLE)f, buf, n, &got, &ov)) return got;
    DWORD e = GetLastError();
    if (e == ERROR_IO_PENDING) {
        if (GetOverlappedResult((HANDLE)f, &ov, &got, TRUE)) return got;
        e = GetLastError();
    }
    return e == ERROR_HANDLE_EOF ? 0 : e == ERROR_NOACCESS ? -2 : -1;
}

int plat_file_id(PlatFile *f, PlatFileId *out) {
    BY_HANDLE_FILE_INFORMATION i;
    if (!GetFileInformationByHandle((HANDLE)f, &i)) return 0;
    *out = (PlatFileId){i.dwVolumeSerialNumber, (uint64_t)i.nFileIndexHigh << 32 | i.nFileIndexLow,
                        (uint64_t)i.nFileSizeHigh << 32 | i.nFileSizeLow,
                        (uint64_t)i.ftLastWriteTime.dwHighDateTime << 32 | i.ftLastWriteTime.dwLowDateTime,
                        (uint64_t)i.ftCreationTime.dwHighDateTime << 32 | i.ftCreationTime.dwLowDateTime};
    return 1;
}

const void *plat_file_map(PlatFile *f) {
    HANDLE map = CreateFileMappingW((HANDLE)f, NULL, PAGE_READONLY, 0, 0, NULL);
    const void *v = map ? MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (map) CloseHandle(map);   // the view keeps the mapping alive
    return v;
}

static int fs_err(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_DIRECTORY: return PLAT_FS_NOT_FOUND;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return PLAT_FS_EXISTS;
    case ERROR_DIR_NOT_EMPTY: return PLAT_FS_NOT_EMPTY;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: return PLAT_FS_BUSY;
    default: return PLAT_FS_ERROR;
    }
}

int plat_fs_kind(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return PLAT_FS_MISSING;
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES) return PLAT_FS_MISSING;
    return a & FILE_ATTRIBUTE_REPARSE_POINT ? PLAT_FS_LINK : a & FILE_ATTRIBUTE_DIRECTORY ? PLAT_FS_DIR : PLAT_FS_FILE;
}

int plat_fs_mkdir(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return PLAT_FS_ERROR;
    return CreateDirectoryW(w, NULL) ? PLAT_FS_OK : fs_err(GetLastError());
}

int plat_fs_rmdir(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return PLAT_FS_ERROR;
    return RemoveDirectoryW(w) ? PLAT_FS_OK : fs_err(GetLastError());
}

int plat_fs_unlink(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return PLAT_FS_ERROR;
    DWORD a = GetFileAttributesW(w);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(w, a & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    return DeleteFileW(w) ? PLAT_FS_OK : fs_err(GetLastError());
}

int plat_fs_rename(const char *from, const char *to, int durable) {
    wchar_t a[WPATH], b[WPATH];
    if (!widen(from, a, WPATH) || !widen(to, b, WPATH)) return PLAT_FS_ERROR;
    return MoveFileExW(a, b, durable ? MOVEFILE_WRITE_THROUGH : 0) ? PLAT_FS_OK : fs_err(GetLastError());
}

int plat_fs_link(const char *existing, const char *path) {
    wchar_t a[WPATH], b[WPATH];
    if (!widen(existing, a, WPATH) || !widen(path, b, WPATH)) return PLAT_FS_ERROR;
    return CreateHardLinkW(b, a, NULL) ? PLAT_FS_OK : fs_err(GetLastError());
}

int plat_fs_links(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    BY_HANDLE_FILE_INFORMATION fi;
    int links = f != INVALID_HANDLE_VALUE && GetFileInformationByHandle(f, &fi) ? (int)fi.nNumberOfLinks : 0;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    return links;
}

// NTFS journals directory entries; flushing the directory handle is as far as Windows goes.
void plat_fs_sync_dir(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(h);
        CloseHandle(h);
    }
}

// Restart Manager (rstrtmgr.dll, loaded on first use) names the processes that hold a file open.
typedef DWORD(WINAPI *RmStartSession_t)(DWORD *, DWORD, WCHAR *);
typedef DWORD(WINAPI *RmRegisterResources_t)(DWORD, UINT, LPCWSTR *, UINT, RM_UNIQUE_PROCESS *, UINT, LPCWSTR *);
typedef DWORD(WINAPI *RmGetList_t)(DWORD, UINT *, UINT *, RM_PROCESS_INFO *, LPDWORD);
typedef DWORD(WINAPI *RmEndSession_t)(DWORD);
static struct {
    INIT_ONCE once;
    RmStartSession_t start;
    RmRegisterResources_t add;
    RmGetList_t list;
    RmEndSession_t end;
} rm = {INIT_ONCE_STATIC_INIT};

static BOOL CALLBACK rm_load(INIT_ONCE *o, void *p, void **c) {
    (void)o, (void)p, (void)c;
    HMODULE l = plat_lib_open("rstrtmgr.dll");
    if (!l) return TRUE;
    rm.add = (RmRegisterResources_t)(void *)GetProcAddress(l, "RmRegisterResources");
    rm.list = (RmGetList_t)(void *)GetProcAddress(l, "RmGetList");
    rm.end = (RmEndSession_t)(void *)GetProcAddress(l, "RmEndSession");
    RmStartSession_t start = (RmStartSession_t)(void *)GetProcAddress(l, "RmStartSession");
    if (rm.add && rm.list && rm.end) rm.start = start;   // start set: all of them are there
    return TRUE;
}

// The files under dir (links not followed), at most BUSY_FILES of them, each a malloc'd path.
#define BUSY_FILES 256
typedef struct { LPCWSTR p[BUSY_FILES]; UINT n; } BusyFiles;

static void busy_files(const wchar_t *dir, BusyFiles *f) {
    wchar_t pat[WPATH + 4];
    swprintf(pat, WPATH + 4, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        DWORD a = fd.dwFileAttributes;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || (a & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        size_t n = wcslen(dir) + wcslen(fd.cFileName) + 2;
        wchar_t *p = n <= WPATH ? malloc(n * sizeof *p) : NULL;
        if (!p) continue;
        swprintf(p, n, L"%ls\\%ls", dir, fd.cFileName);
        if (a & FILE_ATTRIBUTE_DIRECTORY) busy_files(p, f), free(p);
        else f->p[f->n++] = p;
    } while (f->n < BUSY_FILES && FindNextFileW(h, &fd));
    FindClose(h);
}

void plat_fs_busy_reason(const char *dir, char *out, size_t cap) {
    if (!cap) return;
    *out = 0;
    wchar_t w[WPATH];
    if (!widen(dir, w, WPATH)) return;
    for (wchar_t *q = w; *q; q++)
        if (*q == L'/') *q = L'\\';
    // a rename needs DELETE access to the directory: a folder made by an elevated run can refuse it
    HANDLE h = CreateFileW(w, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_ACCESS_DENIED) snprintf(out, cap, "no permission to rename it");
        return;
    }
    CloseHandle(h);
    InitOnceExecuteOnce(&rm.once, rm_load, NULL, NULL);
    DWORD session;
    WCHAR key[CCH_RM_SESSION_KEY + 1] = {0};
    if (!rm.start || rm.start(&session, 0, key) != ERROR_SUCCESS) return;
    BusyFiles f = {.n = 0};
    busy_files(w, &f);
    RM_PROCESS_INFO info[8];
    UINT need = 0, n = 8;
    DWORD reasons = 0, e = f.n ? rm.add(session, f.n, f.p, 0, NULL, 0, NULL) : ERROR_SUCCESS;
    if (f.n && e == ERROR_SUCCESS) e = rm.list(session, &need, &n, info, &reasons);
    else n = 0;
    rm.end(session);
    for (UINT i = 0; i < f.n; i++) free((void *)f.p[i]);
    if (e == ERROR_MORE_DATA) snprintf(out, cap, "%u programs hold files in it", need);   // info is not filled then
    if (e != ERROR_SUCCESS) return;
    size_t at = 0;
    for (UINT i = 0; i < n && at < cap; i++) {   // the program's display name ("Windows Explorer") and process id
        char name[CCH_RM_MAX_APP_NAME + 1];
        if (!WideCharToMultiByte(CP_UTF8, 0, info[i].strAppName, -1, name, sizeof name, NULL, NULL)) strcpy(name, "?");
        int k = snprintf(out + at, cap - at, "%s%s %lu", i ? ", " : "", name, info[i].Process.dwProcessId);
        if (k < 0) break;
        at += (size_t)k;
    }
}

int plat_fs_list(const char *dir, int (*cb)(const char *name, int kind, uint64_t size, void *ctx), void *ctx) {
    wchar_t wd[WPATH], pat[WPATH + 4];
    if (!widen(dir, wd, WPATH)) return PLAT_FS_ERROR;
    swprintf(pat, WPATH + 4, L"%ls\\*", wd);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return fs_err(GetLastError());
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        char name[1024];
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
        DWORD a = fd.dwFileAttributes;
        int kind = a & FILE_ATTRIBUTE_REPARSE_POINT ? PLAT_FS_LINK : a & FILE_ATTRIBUTE_DIRECTORY ? PLAT_FS_DIR
                                                                                                   : PLAT_FS_FILE;
        if (!cb(name, kind, (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow, ctx)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return PLAT_FS_OK;
}

static PlatFile *fs_open(const char *path, DWORD access, DWORD share, DWORD how, int *err) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) {
        if (err) *err = PLAT_FS_ERROR;
        return NULL;
    }
    HANDLE h = CreateFileW(w, access, share, NULL, how, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = fs_err(GetLastError());
        return NULL;
    }
    return (PlatFile *)h;
}

PlatFile *plat_fs_open(const char *path, int write, int *err) {
    return fs_open(path, GENERIC_READ | (write ? GENERIC_WRITE : 0),
                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, err);
}

PlatFile *plat_fs_create(const char *path, int *err) { return fs_open(path, GENERIC_WRITE, 0, CREATE_NEW, err); }

int64_t plat_file_pwrite(PlatFile *f, const void *buf, uint32_t n, uint64_t off) {
    static __thread HANDLE ev;
    if (!ev && !(ev = CreateEventW(NULL, TRUE, FALSE, NULL))) return -1;
    OVERLAPPED ov = {.Offset = (DWORD)off, .OffsetHigh = (DWORD)(off >> 32), .hEvent = ev};
    DWORD put = 0;
    if (WriteFile((HANDLE)f, buf, n, &put, &ov)) return put;
    if (GetLastError() == ERROR_IO_PENDING && GetOverlappedResult((HANDLE)f, &ov, &put, TRUE)) return put;
    return -1;
}

int plat_file_set_size(PlatFile *f, uint64_t size) {
    FILE_END_OF_FILE_INFO eof = {.EndOfFile.QuadPart = (LONGLONG)size};
    return SetFileInformationByHandle((HANDLE)f, FileEndOfFileInfo, &eof, sizeof eof) ? PLAT_FS_OK
                                                                                      : fs_err(GetLastError());
}

int plat_file_fsync(PlatFile *f) { return FlushFileBuffers((HANDLE)f) != 0; }

int plat_temp_dir(char *out, size_t cap) {
    wchar_t w[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, w);
    if (!n || n > MAX_PATH) return 0;
    while (n && (w[n - 1] == L'\\' || w[n - 1] == L'/')) w[--n] = 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) != 0;
}

uintptr_t plat_exe_base(void) { return (uintptr_t)GetModuleHandleW(NULL); }

int plat_module_of(uintptr_t addr, char *name, size_t cap, uintptr_t *base) {
    HMODULE m = NULL;
    wchar_t w[WPATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)addr, &m) ||
        !m || !GetModuleFileNameW(m, w, WPATH))
        return 0;
    const wchar_t *n = wcsrchr(w, L'\\') ? wcsrchr(w, L'\\') + 1 : w;
    *base = (uintptr_t)m;
    return WideCharToMultiByte(CP_UTF8, 0, n, -1, name, (int)cap, NULL, NULL) != 0;
}

int plat_exe_path(char *out, size_t cap) {
    wchar_t w[WPATH];
    DWORD n = GetModuleFileNameW(NULL, w, WPATH);
    if (!n || n >= WPATH) return 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) != 0;
}

size_t plat_getenv(const char *name, char *out, size_t cap) {
    wchar_t wn[256], v[WPATH];
    if (!widen(name, wn, 256)) return 0;
    DWORD n = GetEnvironmentVariableW(wn, v, WPATH);
    if (!n || n >= WPATH) return 0;
    int m = WideCharToMultiByte(CP_UTF8, 0, v, -1, out, (int)cap, NULL, NULL);
    return m ? (size_t)m - 1 : 0;
}

uint32_t plat_thread_id(void) { return GetCurrentThreadId(); }

const char *plat_ui_language(void) {
    static char tag[LOCALE_NAME_MAX_LENGTH];
    static int done;
    if (!done) {
        wchar_t w[LOCALE_NAME_MAX_LENGTH];
        LCID lcid = GetUserDefaultUILanguage();
        int n = LCIDToLocaleName(lcid, w, LOCALE_NAME_MAX_LENGTH, 0);
        if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, tag, sizeof tag, NULL, NULL)) tag[0] = 0;
        done = 1;
    }
    return tag;
}

const char *plat_user_locale(void) {
    static char tag[LOCALE_NAME_MAX_LENGTH];
    static int done;
    if (!done) {
        wchar_t w[LOCALE_NAME_MAX_LENGTH];
        size_t i = 0;
        if (GetUserDefaultLocaleName(w, LOCALE_NAME_MAX_LENGTH))
            for (; w[i] && w[i] < 0x80 && i + 1 < sizeof tag; i++) tag[i] = (char)w[i];
        tag[i] = 0;
        done = 1;
    }
    return tag;
}

int plat_font_path(const char *lang, char *out, size_t cap) {
    static const struct { const char *lang; const wchar_t *files[3]; } fonts[] = {
        {"ja", {L"YuGothM.ttc", L"meiryo.ttc", L"msgothic.ttc"}},
        {"ko", {L"malgun.ttf", L"gulim.ttc", NULL}},
        {"zh-Hans", {L"msyh.ttc", L"simsun.ttc", NULL}},
        {"zh-Hant", {L"msjh.ttc", L"mingliu.ttc", NULL}},
    };
    wchar_t dir[MAX_PATH], w[WPATH];
    UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    for (size_t i = 0; i < sizeof fonts / sizeof *fonts; i++) {
        if (strcmp(fonts[i].lang, lang)) continue;
        for (int k = 0; k < 3 && fonts[i].files[k]; k++) {
            swprintf(w, WPATH, L"%ls\\Fonts\\%ls", dir, fonts[i].files[k]);
            DWORD a = GetFileAttributesW(w);
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
                return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) != 0;
        }
    }
    return 0;
}

int plat_os_version(char *out, size_t cap) {
    // GetVersionEx reports the version the manifest asks for; ntdll's RtlGetVersion the real one
    typedef LONG(WINAPI * RtlGetVersion_t)(OSVERSIONINFOW *);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersion_t get = nt ? (RtlGetVersion_t)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    OSVERSIONINFOW v = {.dwOSVersionInfoSize = sizeof v};
    if (!get || get(&v) != 0) return 0;
    DWORD ubr = 0, size = sizeof ubr;   // the update build revision, the number after the build
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD,
                     NULL, &ubr, &size) != ERROR_SUCCESS)
        ubr = 0;
    snprintf(out, cap, ubr ? "Windows %lu.%lu.%lu.%lu" : "Windows %lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion,
             v.dwBuildNumber, ubr);
    return 1;
}

int plat_stderr_path(char *out, size_t cap) {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    wchar_t w[WPATH];
    if (!h || h == INVALID_HANDLE_VALUE || GetFileType(h) != FILE_TYPE_DISK) return 0;
    DWORD n = GetFinalPathNameByHandleW(h, w, WPATH, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n || n >= WPATH) return 0;
    const wchar_t *p = wcsncmp(w, L"\\\\?\\", 4) ? w : w + 4;   // the \\?\ prefix
    return WideCharToMultiByte(CP_UTF8, 0, p, -1, out, (int)cap, NULL, NULL) != 0;
}

// ---- desktop

int plat_clipboard_set(const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0) return 0;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n * sizeof(wchar_t));
    if (!mem) return 0;
    wchar_t *w = GlobalLock(mem);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    GlobalUnlock(mem);
    // A clipboard opened without a window gets no owner, and SetClipboardData may then fail: a message-only window of
    // the calling thread owns it for the call.
    HWND owner = CreateWindowExW(0, L"STATIC", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    int ok = 0;
    if (OpenClipboard(owner)) {
        ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, mem) != NULL;
        CloseClipboard();
    }
    if (owner) DestroyWindow(owner);
    if (!ok) GlobalFree(mem);   // after a successful SetClipboardData the system owns it
    return ok;
}

static DWORD WINAPI open_folder_main(void *arg) {
    ShellExecuteW(NULL, L"open", arg, NULL, NULL, SW_SHOWNORMAL);
    free(arg);
    return 0;
}

int plat_open_folder(const char *path) {
    wchar_t *w = malloc(WPATH * sizeof *w);
    if (!w || !widen(path, w, WPATH)) return free(w), 0;
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return free(w), 0;
    // the shell can take a while to answer: not on the caller's thread (the present path)
    HANDLE t = CreateThread(NULL, 0, open_folder_main, w, 0, NULL);
    if (!t) return free(w), 0;
    CloseHandle(t);
    return 1;
}

// ---- dynamic libraries

void *plat_lib_open(const char *name) {
    wchar_t w[WPATH];
    if (!widen(name, w, WPATH)) return NULL;
    int bare = !wcschr(w, L'\\') && !wcschr(w, L'/');
    return (void *)LoadLibraryExW(w, NULL, bare ? LOAD_LIBRARY_SEARCH_SYSTEM32 : 0);
}

void *plat_lib_sym(void *lib, const char *name) { return lib ? (void *)GetProcAddress((HMODULE)lib, name) : NULL; }

// ---- machine load: GetSystemTimes for the CPU, PDH (pdh.dll, loaded on first use) for the thermal zones and the GPU
// engine and adapter memory counters that Task Manager reads.

typedef PDH_STATUS(WINAPI *PdhOpenQueryW_t)(LPCWSTR, DWORD_PTR, PDH_HQUERY *);
typedef PDH_STATUS(WINAPI *PdhAddEnglishCounterW_t)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER *);
typedef PDH_STATUS(WINAPI *PdhCollectQueryData_t)(PDH_HQUERY);
typedef PDH_STATUS(WINAPI *PdhGetFormattedCounterArrayW_t)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD,
                                                           PPDH_FMT_COUNTERVALUE_ITEM_W);
typedef PDH_STATUS(WINAPI *PdhCloseQuery_t)(PDH_HQUERY);
static struct {
    INIT_ONCE once;
    PdhOpenQueryW_t open;
    PdhAddEnglishCounterW_t add;
    PdhCollectQueryData_t collect;
    PdhGetFormattedCounterArrayW_t get;
    PdhCloseQuery_t close;
} pdh = {INIT_ONCE_STATIC_INIT};

static BOOL CALLBACK pdh_load(INIT_ONCE *o, void *p, void **c) {
    (void)o, (void)p, (void)c;
    HMODULE l = plat_lib_open("pdh.dll");
    if (!l) return TRUE;
    pdh.add = (PdhAddEnglishCounterW_t)(void *)GetProcAddress(l, "PdhAddEnglishCounterW");
    pdh.collect = (PdhCollectQueryData_t)(void *)GetProcAddress(l, "PdhCollectQueryData");
    pdh.get = (PdhGetFormattedCounterArrayW_t)(void *)GetProcAddress(l, "PdhGetFormattedCounterArrayW");
    pdh.close = (PdhCloseQuery_t)(void *)GetProcAddress(l, "PdhCloseQuery");
    PdhOpenQueryW_t open = (PdhOpenQueryW_t)(void *)GetProcAddress(l, "PdhOpenQueryW");
    if (pdh.add && pdh.collect && pdh.get && pdh.close) pdh.open = open;   // open set: all of them are there
    return TRUE;
}

// One PDH query with its wildcard counters ("\Object(*)\Counter"). A query whose open failed stays NULL.
typedef struct { PDH_HQUERY q; PDH_HCOUNTER c[2]; } PdhQuery;

static void pdh_query(PdhQuery *pq, const wchar_t *c0, const wchar_t *c1) {
    InitOnceExecuteOnce(&pdh.once, pdh_load, NULL, NULL);
    pq->q = NULL;
    if (!pdh.open || pdh.open(NULL, 0, &pq->q) != ERROR_SUCCESS) {
        pq->q = NULL;
        return;
    }
    if (pdh.add(pq->q, c0, 0, &pq->c[0]) != ERROR_SUCCESS) pq->c[0] = NULL;
    if (!c1 || pdh.add(pq->q, c1, 0, &pq->c[1]) != ERROR_SUCCESS) pq->c[1] = NULL;
    if (!pq->c[0] && !pq->c[1]) {
        pdh.close(pq->q);
        pq->q = NULL;
    }
}

// The counter's instances as a malloc'd array (free it), *count of them; NULL when there is no valid data yet (a rate
// counter needs two collections).
static PDH_FMT_COUNTERVALUE_ITEM_W *pdh_items(PDH_HCOUNTER c, DWORD *count) {
    DWORD size = 0;
    *count = 0;
    if (!c || pdh.get(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, count, NULL) != PDH_MORE_DATA || !size) return NULL;
    PDH_FMT_COUNTERVALUE_ITEM_W *items = malloc(size);
    if (items && pdh.get(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, count, items) == ERROR_SUCCESS) return items;
    free(items);
    *count = 0;
    return NULL;
}

struct PlatLoad {
    uint64_t idle, kernel, user, proc;   // the previous call's times; kernel 0 before the first
    PdhQuery thermal, gpu;               // gpu: engine utilisation, adapter dedicated memory
    wchar_t luid[40];                    // "luid_0x<high>_0x<low>" of the adapter, lower case; empty for any
};

PlatLoad *plat_load_open(const uint8_t *gpu_luid, int pci_bus) {
    (void)pci_bus;   // the LUID names the adapter
    PlatLoad *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    if (gpu_luid) {   // Vulkan's deviceLUID is the LUID struct's memory: LowPart, then HighPart
        uint32_t lo, hi;
        memcpy(&lo, gpu_luid, 4);
        memcpy(&hi, gpu_luid + 4, 4);
        swprintf(l->luid, sizeof l->luid / sizeof *l->luid, L"luid_0x%08x_0x%08x", hi, lo);
    }
    pdh_query(&l->thermal, L"\\Thermal Zone Information(*)\\Temperature", NULL);
    pdh_query(&l->gpu, L"\\GPU Engine(*)\\Utilization Percentage", L"\\GPU Adapter Memory(*)\\Dedicated Usage");
    return l;
}

void plat_load_close(PlatLoad *l) {
    if (!l) return;
    if (l->thermal.q) pdh.close(l->thermal.q);
    if (l->gpu.q) pdh.close(l->gpu.q);
    free(l);
}

void plat_cpu_load(PlatLoad *l, float *total_pct, float *proc_pct, float *temp_c) {
    *total_pct = *proc_pct = *temp_c = -1;
    if (!l) return;
    FILETIME idle, kern, user, cr, ex, pk, pu;
    if (GetSystemTimes(&idle, &kern, &user) && GetProcessTimes(GetCurrentProcess(), &cr, &ex, &pk, &pu)) {
        uint64_t i = ft64(idle), k = ft64(kern), u = ft64(user), p = ft64(pk) + ft64(pu);
        uint64_t total = (k - l->kernel) + (u - l->user);   // kernel time includes idle time
        if (l->kernel && total) {
            *total_pct = 100.0f * (float)(total - (i - l->idle)) / (float)total;
            *proc_pct = 100.0f * (float)(p - l->proc) / (float)total;
        }
        l->idle = i, l->kernel = k, l->user = u, l->proc = p;
    }
    // the firmware's ACPI zones, near the CPU on most boards; the exact package temperature needs a kernel driver
    DWORD n;
    PDH_FMT_COUNTERVALUE_ITEM_W *it;
    if (l->thermal.q && pdh.collect(l->thermal.q) == ERROR_SUCCESS && (it = pdh_items(l->thermal.c[0], &n))) {
        for (DWORD i = 0; i < n; i++) {   // the hottest zone, in kelvin
            double c = it[i].FmtValue.doubleValue - 273.15;
            if (it[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA && c > *temp_c && c < 150) *temp_c = (float)c;
        }
        free(it);
    }
}

// The adapter part of an instance name, "luid_0x..._0x...", lower case, into out (40 wide characters).
static void inst_luid(const wchar_t *name, wchar_t *out) {
    const wchar_t *p = wcsstr(name, L"luid_");
    size_t n = 0;
    while (p && p[n] && n < 26 && n < 39) out[n] = (wchar_t)towlower(p[n]), n++;   // luid_0x%08x_0x%08x: 26
    out[n] = 0;
}

void plat_gpu_load(PlatLoad *l, float *load_pct, float *vram_used_mb) {
    *load_pct = *vram_used_mb = -1;
    if (!l || !l->gpu.q || pdh.collect(l->gpu.q) != ERROR_SUCCESS) return;
    // Engine instances: "pid_<n>_luid_0x.._0x.._phys_<n>_eng_<n>_engtype_<type>". Per adapter and engine type the
    // processes' shares add up; the load is the busiest engine type.
    enum { TYPES = 32 };
    struct { wchar_t luid[40], type[32]; double sum; } t[TYPES];
    int nt = 0;
    DWORD n;
    PDH_FMT_COUNTERVALUE_ITEM_W *it = pdh_items(l->gpu.c[0], &n);
    for (DWORD i = 0; it && i < n; i++) {
        if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
        wchar_t lu[40];
        inst_luid(it[i].szName, lu);
        if (l->luid[0] && wcscmp(lu, l->luid)) continue;
        const wchar_t *ty = wcsstr(it[i].szName, L"engtype_");
        ty = ty ? ty + 8 : L"";
        int k = 0;
        while (k < nt && (wcscmp(t[k].luid, lu) || wcsncmp(t[k].type, ty, 31))) k++;
        if (k == nt) {
            if (nt == TYPES) continue;
            wcscpy(t[k].luid, lu);
            wcsncpy(t[k].type, ty, 31);
            t[k].type[31] = 0;
            t[k].sum = 0;
            nt++;
        }
        t[k].sum += it[i].FmtValue.doubleValue;
    }
    free(it);
    for (int k = 0; k < nt; k++)
        if (t[k].sum > *load_pct) *load_pct = (float)(t[k].sum > 100 ? 100 : t[k].sum);
    // Adapter memory instances: "luid_0x.._0x.._phys_<n>", bytes. Without a LUID: the adapter that uses the most.
    it = pdh_items(l->gpu.c[1], &n);
    double sum = 0, best = -1;
    for (DWORD i = 0; it && i < n; i++) {
        if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
        wchar_t lu[40];
        inst_luid(it[i].szName, lu);
        if (l->luid[0] ? !wcscmp(lu, l->luid) : 0) sum += it[i].FmtValue.doubleValue;
        if (it[i].FmtValue.doubleValue > best) best = it[i].FmtValue.doubleValue;
    }
    if (it && (l->luid[0] ? sum > 0 : best >= 0)) *vram_used_mb = (float)((l->luid[0] ? sum : best) / 1048576.0);
    free(it);
}

void plat_mem_info(float *used_mb, float *total_mb, float *proc_mb) {
    *used_mb = *total_mb = *proc_mb = -1;
    MEMORYSTATUSEX ms = {.dwLength = sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) {
        *total_mb = (float)(ms.ullTotalPhys >> 20);
        *used_mb = (float)((ms.ullTotalPhys - ms.ullAvailPhys) >> 20);
    }
    PROCESS_MEMORY_COUNTERS pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) *proc_mb = (float)(pmc.WorkingSetSize >> 20);
}

void plat_commit_info(double *proc_mb, double *used_mb, double *limit_mb) {
    *proc_mb = *used_mb = *limit_mb = -1;
    MEMORYSTATUSEX ms = {.dwLength = sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) {
        *limit_mb = (double)(ms.ullTotalPageFile >> 20);
        *used_mb = (double)((ms.ullTotalPageFile - ms.ullAvailPageFile) >> 20);
    }
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc))
        *proc_mb = (double)(pmc.PrivateUsage >> 20);
}

void plat_process_memory(uint64_t *resident, uint64_t *private_bytes) {
    PROCESS_MEMORY_COUNTERS_EX pm = {.cb = sizeof pm};
    *resident = *private_bytes = 0;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm))
        *resident = pm.WorkingSetSize, *private_bytes = pm.PrivateUsage;
}

// ---- child processes

struct PlatChild {
    HANDLE proc, in, out;                                  // in: our write end, out: our read end
};

PlatChild *plat_child_start(const char *exe, const char *arg, uint32_t *err) {
    wchar_t we[WPATH], wa[256] = L"", cmd[WPATH + 300];
    if (err) *err = 0;
    if (!widen(exe, we, WPATH) || (arg && !widen(arg, wa, 256))) return NULL;
    swprintf(cmd, WPATH + 300, arg ? L"\"%ls\" %ls" : L"\"%ls\"", we, wa);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE in_r, in_w, out_r, out_w;
    // Pipes are created and handed out one child at a time: a child started meanwhile would inherit the other pipe
    // ends and keep them open after their owner closed them.
    static SRWLOCK spawn = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&spawn);
    if (!CreatePipe(&in_r, &in_w, &sa, 0)) return ReleaseSRWLockExclusive(&spawn), NULL;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        CloseHandle(in_r), CloseHandle(in_w);
        return ReleaseSRWLockExclusive(&spawn), NULL;
    }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    STARTUPINFOW si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = in_r, .hStdOutput = out_w,
                       .hStdError = nul != INVALID_HANDLE_VALUE ? nul : out_w};
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    DWORD e = GetLastError();
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    ReleaseSRWLockExclusive(&spawn);
    if (!ok) {
        CloseHandle(in_w), CloseHandle(out_r);
        if (err) *err = e;
        return NULL;
    }
    CloseHandle(pi.hThread);
    PlatChild *c = malloc(sizeof *c);
    if (!c) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess), CloseHandle(in_w), CloseHandle(out_r);
        return NULL;
    }
    c->proc = pi.hProcess, c->in = in_w, c->out = out_r;
    return c;
}

void plat_child_stop(PlatChild *c) {
    if (!c) return;
    CloseHandle(c->in);
    CloseHandle(c->out);
    TerminateProcess(c->proc, 1);
    CloseHandle(c->proc);
    free(c);
}

int plat_child_write(PlatChild *c, const void *data, size_t n) {
    DWORD put = 0;
    return WriteFile(c->in, data, (DWORD)n, &put, NULL) && put == n;
}

long plat_child_read(PlatChild *c, void *buf, size_t cap) {
    DWORD got = 0;
    if (!ReadFile(c->out, buf, (DWORD)cap, &got, NULL)) return -1;
    return (long)got;
}
