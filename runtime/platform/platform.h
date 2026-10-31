// Platform layer: threads, time, locks, atomics, files. Runtime code calls this instead of the OS; one implementation
// per OS (win32.c, linux.c). The FP control register lives in the per-architecture fp_*.h headers, inline, because
// lifted FP code touches it on every operation.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#if defined(__x86_64__) || defined(_M_X64)
#include "fp_x86.h"
#include <immintrin.h>
#endif

// ---- threads
typedef struct PlatThread PlatThread;
typedef void (*PlatThreadFn)(void *arg);

// Starts fn(arg) on a new thread named name (shown in debuggers and profilers; may be NULL). NULL on failure.
PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name);
// The same with the stack size given: stack bytes reserved for the thread (0: PLAT_STACK_DEFAULT). The default is the
// same on every OS, so a thread that fits on Windows fits elsewhere.
#define PLAT_STACK_DEFAULT (16u << 20)
PlatThread *plat_thread_new_ex(PlatThreadFn fn, void *arg, const char *name, size_t stack);
// Starts a thread nobody joins (plat_thread_new_ex + plat_thread_detach). 1 on success.
int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack);
void plat_thread_join(PlatThread *t);     // waits for fn to return, then frees t
// Waits at most ns for fn to return: 1 when it did (t is freed), 0 when it still runs (t stays valid).
int plat_thread_join_ns(PlatThread *t, uint64_t ns);
void plat_thread_detach(PlatThread *t);   // frees t; the thread runs on
// Restricts t (NULL: the calling thread) to the logical processors in mask (bit n = processor n of the first group).
// Returns the previous mask, 0 on failure.
uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask);
uint32_t plat_cpu_count(void);            // logical processors of the machine (system info)
uint32_t plat_cpu_usable(void);           // the ones this process may run on (its affinity mask): size thread pools by it
uint64_t plat_thread_cpu_ns(uint32_t tid);  // CPU time of the thread with id tid (plat_thread_id) so far; 0 unknown
// The logical processors the process may use (bit n = processor n, the first 64), into *all, and the lowest of each
// physical core among them into *first; returns the number of physical cores, 0 when unknown.
int plat_cpu_cores(uint64_t *all, uint64_t *first);
int plat_process_affinity(uint64_t mask);   // every thread of the process to the processors in mask; 1 on success

// Interrupting a thread (kernel.c's asynchronous preemption). A thread makes its own handle; another thread stops it
// wherever it runs, reads where it is, may send it elsewhere, and lets it go on. While it is stopped the caller must not
// wait for anything the stopped thread could hold.
typedef struct PlatIntr PlatIntr;
PlatIntr *plat_intr_self(void);           // NULL on failure
void plat_intr_free(PlatIntr *t);
// Stops t and reads its instruction and stack pointers: 1 when stopped, 0 when it did not stop in time (a thread
// blocked in the OS); t then runs on untouched.
int plat_intr_stop(PlatIntr *t, uint64_t *rip, uint64_t *rsp);
// Lets a stopped t go on: at to when to is nonzero, else where it stopped. 0 when the redirection failed (t then goes
// on where it stopped).
int plat_intr_resume(PlatIntr *t, uint64_t to);

// Fibers: execution contexts with stacks of their own that threads switch between; a fiber may resume on another
// thread. Each switch keeps the callee-saved registers and the floating-point control state.
typedef struct PlatFiber PlatFiber;
PlatFiber *plat_fiber_self(void);         // the calling thread's own context as a fiber, once before its first switch
void plat_fiber_unself(void);             // the calling thread stops being a fiber (it is on its own context again)
// A fiber that runs fn(arg) on a stack of stack bytes (reserved, committed as it grows); fn never returns.
PlatFiber *plat_fiber_new(size_t stack, void (*fn)(void *arg), void *arg);
void plat_fiber_switch(PlatFiber *to);    // from the fiber the calling thread runs
void plat_fiber_free(PlatFiber *f);       // a fiber nobody runs
// Scheduling priority, Windows' seven levels: of the calling thread, or of t. On Linux a thread lowered without the
// right to raise its priority again (CAP_SYS_NICE, or an RLIMIT_NICE of 20) could never return to normal, and the
// runtime lowers threads that later take shared locks at normal priority; without that right every change is skipped
// and threads keep the normal priority. Levels above normal need the same right.
enum { PLAT_PRIO_IDLE = -15, PLAT_PRIO_LOWEST = -2, PLAT_PRIO_BELOW_NORMAL = -1, PLAT_PRIO_NORMAL = 0,
       PLAT_PRIO_ABOVE_NORMAL = 1, PLAT_PRIO_HIGHEST = 2, PLAT_PRIO_TIME_CRITICAL = 15 };
void plat_thread_priority(int prio);
void plat_thread_set_priority(PlatThread *t, int prio);
int plat_thread_priority_get(void);       // the calling thread's level
void plat_yield(void);                    // gives the rest of the time slice to another ready thread
static inline void plat_pause(void) {     // spin-wait hint
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#endif
}

// ---- time
uint64_t plat_now_ns(void);               // monotonic, high resolution, arbitrary epoch
uint64_t plat_counter(void);              // the OS's raw monotonic counter (QPC on Windows), plat_counter_hz per second
uint64_t plat_counter_hz(void);
void plat_timer_1ms(void);                // asks the OS for timed waits accurate to 1 ms or better (call before threads)
// t * num / den for a fixed num / den without a 128-bit division per call (__udivti3 took 5% of the game's running
// samples through plat_now_ns and rt_ticks): t * q + (t * f >> 64) with q = num / den and f = (num % den) * 2^64 / den.
// Monotonic in t and at most one below the exact quotient while t * (num % den) / den fits 64 bits.
typedef struct { uint64_t q, f; } PlatScale;
static inline PlatScale plat_scale(uint64_t num, uint64_t den) {
    return (PlatScale){num / den, (uint64_t)(((unsigned __int128)(num % den) << 64) / den)};
}
static inline uint64_t plat_scaled(PlatScale s, uint64_t t) {
    return t * s.q + (uint64_t)(((unsigned __int128)t * s.f) >> 64);
}
void plat_sleep_ns(uint64_t ns);          // returns no earlier than ns later; overshoot well under 1 ms
// Sleeps about ns on the OS's high-resolution timer without spinning: wakes up to PLAT_WAIT_LATE_NS late, never early.
void plat_wait_ns(uint64_t ns);
#ifdef _WIN32
#define PLAT_WAIT_LATE_NS 500000u   // high-resolution waitable timer
#else
#define PLAT_WAIT_LATE_NS 60000u    // clock_nanosleep with a 1 us timer slack (plat_timer_1ms), scheduler wakeup
#endif
static inline uint64_t plat_now_ms(void) { return plat_now_ns() / 1000000u; }
void plat_sleep_ms(uint32_t ms);          // coarse sleep: the scheduler's granularity, no spinning

// ---- locks and condition variables. A PlatMutex is a reader-writer lock taken exclusively (plat_mutex_lock) or
// shared (plat_mutex_lock_shared), not recursive, like Windows' SRW lock it is on Windows. Zero-initialised storage is
// a valid unlocked lock and an empty condition variable (PLAT_MUTEX_INIT / PLAT_COND_INIT spell it).
typedef struct { void *p; } PlatMutex;
typedef struct { void *p; } PlatCond;
#define PLAT_MUTEX_INIT {0}
#define PLAT_COND_INIT {0}
void plat_mutex_init(PlatMutex *m);
void plat_mutex_lock(PlatMutex *m);
int plat_mutex_trylock(PlatMutex *m);     // 1 when acquired
void plat_mutex_unlock(PlatMutex *m);
void plat_mutex_lock_shared(PlatMutex *m);
int plat_mutex_trylock_shared(PlatMutex *m);   // 1 when acquired
void plat_mutex_unlock_shared(PlatMutex *m);
void plat_cond_init(PlatCond *c);
void plat_cond_wait(PlatCond *c, PlatMutex *m);   // m held exclusively
int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns);   // 0 on timeout (spurious wakeups return 1 too)
static inline int plat_cond_wait_ms(PlatCond *c, PlatMutex *m, uint32_t ms) {
    return plat_cond_wait_ns(c, m, (uint64_t)ms * 1000000u);
}
void plat_cond_signal(PlatCond *c);
void plat_cond_broadcast(PlatCond *c);

// A mutex the thread that holds it may take again; unlocked as many times as locked. Zero-initialised is unlocked.
typedef struct { PlatMutex m; uint32_t owner, depth; } PlatRMutex;
#define PLAT_RMUTEX_INIT {PLAT_MUTEX_INIT, 0, 0}
void plat_rmutex_lock(PlatRMutex *m);
void plat_rmutex_unlock(PlatRMutex *m);

// One-time initialisation: the first caller runs fn(arg), every other caller waits until it returned. Zero-initialised
// (PLAT_ONCE_INIT) has not run.
typedef struct { volatile int32_t state; } PlatOnce;
#define PLAT_ONCE_INIT {0}
void plat_once(PlatOnce *o, void (*fn)(void *arg), void *arg);

// An event: set wakes its waiters; an auto-reset event lets one waiter through and clears itself, a manual-reset one
// stays set until reset.
typedef struct { PlatMutex m; PlatCond c; int set, manual; } PlatEvent;
void plat_event_init(PlatEvent *e, int manual, int initially_set);
void plat_event_set(PlatEvent *e);
void plat_event_reset(PlatEvent *e);
void plat_event_wait(PlatEvent *e);
int plat_event_wait_ns(PlatEvent *e, uint64_t ns);   // 1 when the event was set, 0 on timeout

// ---- atomics: full barriers, explicit widths (Windows' Interlocked* operate on 32-bit LONG). The value returned is
// noted per function: new for inc/dec/add, previous for xadd/xchg/cas/or.
#define PLAT_SC __ATOMIC_SEQ_CST
static inline int32_t plat_inc32(volatile int32_t *p) { return __atomic_add_fetch(p, 1, PLAT_SC); }
static inline int32_t plat_dec32(volatile int32_t *p) { return __atomic_sub_fetch(p, 1, PLAT_SC); }
static inline int32_t plat_add32(volatile int32_t *p, int32_t v) { return __atomic_add_fetch(p, v, PLAT_SC); }
static inline int32_t plat_xadd32(volatile int32_t *p, int32_t v) { return __atomic_fetch_add(p, v, PLAT_SC); }
static inline int32_t plat_xchg32(volatile int32_t *p, int32_t v) { return __atomic_exchange_n(p, v, PLAT_SC); }
static inline int32_t plat_cas32(volatile int32_t *p, int32_t xchg, int32_t cmp) {
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, PLAT_SC, PLAT_SC);
    return cmp;
}
static inline int64_t plat_inc64(volatile int64_t *p) { return __atomic_add_fetch(p, 1, PLAT_SC); }
static inline int64_t plat_dec64(volatile int64_t *p) { return __atomic_sub_fetch(p, 1, PLAT_SC); }
static inline int64_t plat_add64(volatile int64_t *p, int64_t v) { return __atomic_add_fetch(p, v, PLAT_SC); }
static inline int64_t plat_xadd64(volatile int64_t *p, int64_t v) { return __atomic_fetch_add(p, v, PLAT_SC); }
static inline int64_t plat_xchg64(volatile int64_t *p, int64_t v) { return __atomic_exchange_n(p, v, PLAT_SC); }
static inline int64_t plat_or64(volatile int64_t *p, int64_t v) { return __atomic_fetch_or(p, v, PLAT_SC); }
static inline int64_t plat_cas64(volatile int64_t *p, int64_t xchg, int64_t cmp) {
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, PLAT_SC, PLAT_SC);
    return cmp;
}
static inline void *plat_xchgp(void *volatile *p, void *v) { return __atomic_exchange_n(p, v, PLAT_SC); }
static inline void *plat_casp(void *volatile *p, void *xchg, void *cmp) {
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, PLAT_SC, PLAT_SC);
    return cmp;
}

// ---- virtual memory: ranges reserved at fixed addresses below 2^39, their pages committed, decommitted and protected
// at 4 KiB granularity.
enum { PLAT_PROT_NONE, PLAT_PROT_READ, PLAT_PROT_RW };
enum { PLAT_MEM_FREE = -2, PLAT_MEM_RESERVED = -1 };   // plat_mem_state besides PLAT_PROT_* for committed pages
// Reserves [addr, addr + n) inaccessible, exactly there: 0 when any of it is taken. watch: writes to its pages are
// tracked for plat_mem_written.
int plat_mem_reserve(uint64_t addr, uint64_t n, int watch);
// n bytes of read-write memory anywhere, zero, backed only as pages are first written (plat_mem_protect may seal them);
// NULL on failure. Kept until exit.
void *plat_mem_alloc(size_t n);
int plat_mem_commit(uint64_t addr, size_t n);             // read-write pages inside a reserved range; 1 on success
int plat_mem_decommit(uint64_t addr, uint64_t n);          // contents dropped: committed again, the pages read zero
int plat_mem_protect(uint64_t addr, uint64_t n, int prot); // committed pages only; 1 on success
// The page at addr: PLAT_PROT_* when committed, else PLAT_MEM_RESERVED or PLAT_MEM_FREE (not reserved by us). *end
// gets the end of the run of pages in the same state, at most limit.
int plat_mem_state(uint64_t addr, uint64_t limit, uint64_t *end);
// The pages of a watched reservation written since the last reset, up to max page addresses into pages, in address
// order; reset: those reported start over as unwritten. UINT64_MAX when writes to the range are not tracked.
uint64_t plat_mem_written(uint64_t addr, uint64_t n, uint64_t *pages, uint64_t max, int reset);
// A write that faulted on a read-only page at addr goes to fn first; fn returns 1 when it made the page writable and
// the write should run again, 0 to let the fault go on (to the crash handler). One hook per process.
void plat_mem_fault_hook(int (*fn)(uint64_t addr));
// Starts this program again with the same arguments and ends this process with the new one's exit code (Windows), or
// replaces this process with it (Linux). Environment changes made before the call are passed on.
_Noreturn void plat_reexec(void);

// A crash in this executable's own code (an access violation, an illegal instruction, a division by zero, a stack
// overflow) goes to fn, which reports it and must end the process. Faults in other modules (drivers) are left to the
// OS. The write-fault hook above runs first. On Windows abort() (a failed assert, std::terminate) and an invalid
// parameter to a C runtime function go there too, with the host frames in this executable: the C runtime ends the
// process for both with 0xc0000409 and no exception.
typedef struct {
    const char *what;          // "access violation", "illegal instruction", ...
    uint32_t code;             // the Windows exception code or the signal number
    uintptr_t rip, offset;     // the faulting instruction, and its offset in the executable
    int access;                // the data access that faulted: 0 read, 1 write, 8 execute; -1 when not an access
    uint64_t addr;             // its address when access >= 0
    int nframes;               // abort and invalid parameter: return addresses in this executable, innermost first
    uintptr_t frames[16];      // (rip is frames[0]); 0 for a fault, whose rip is exact
} PlatCrash;
void plat_crash_hook(void (*fn)(const PlatCrash *c));
int plat_mem_read(uint64_t addr, void *out, size_t n);    // copies n bytes, 0 instead of faulting on unmapped memory
int plat_mem_readable(uint64_t addr, uint64_t n);         // every page of [addr, addr + n) can be read; no fault
void *plat_aligned_alloc(size_t size, size_t align);      // free with plat_aligned_free; NULL on failure
void plat_aligned_free(void *p);

// ---- process
_Noreturn void plat_exit(int code);       // ends the process at once: other threads stop, no atexit handlers run
int plat_setenv(const char *name, const char *value);    // value NULL removes it; 1 on success
uint32_t plat_process_id(void);
// CPU time of the process since it started (user and kernel), and wall time since it started, in ns; 0 when unknown.
uint64_t plat_process_cpu_ns(void);
uint64_t plat_process_uptime_ns(void);
uint64_t plat_unix_time_ns(void);         // wall clock, ns since 1970
void plat_local_time(struct tm *out);      // the wall clock now in the local time zone
int plat_localtime(int64_t unix_s, struct tm *out);   // a POSIX time in the local time zone; 0 out of range
int plat_gmtime(int64_t unix_s, struct tm *out);      // the same in UTC
long plat_tz_west(void);                  // the local zone's standard offset, seconds west of UTC (UTC-5: 18000)

// Runs this executable again with the same arguments and env_name=env_value added to its environment, for probes that
// expect the child to stop with a fatal. Its stdout and stderr go into out (NUL-terminated, the rest discarded); waits
// at most timeout_ms (0: no limit) and kills it after that. Returns the exit code, -1 when it could not start.
int plat_run_self(const char *env_name, const char *env_value, char *out, size_t cap, uint32_t timeout_ms);

// ---- randomness
int plat_random(void *out, size_t n);     // cryptographically random bytes; 1 on success

// ---- files. Paths are UTF-8 with either separator; each implementation converts to what the OS wants.
int plat_file_exists(const char *path);                   // 1 for a file (not a directory)
int plat_dir_exists(const char *path);                    // 1 for a directory
// path made absolute against the working directory (not resolved further); 0 when it does not fit cap.
int plat_full_path(const char *path, char *out, size_t cap);
void plat_file_mkdir_p(const char *path);                 // creates every missing directory of path, itself included
// Whole file into a malloc'd buffer with a NUL after the last byte; NULL when missing, empty or over 1 GiB.
uint8_t *plat_file_read(const char *path, uint64_t *size);
int plat_file_write(const char *path, const void *data, size_t n);   // create or truncate; 1 when all n bytes landed
int plat_file_replace(const char *from, const char *to);  // rename, replacing to (write-through); 1 on success
// Writes path.<pid>.<tid>.tmp, flushes it to the disk and renames it over path: a crash leaves the old file or the new
// one, never a torn or zero-filled one. 1 on success.
int plat_file_write_atomic(const char *path, const void *data, size_t n);
int plat_file_flush(const char *path);                   // flushes a written file's data to the disk; 1 on success
int plat_file_sync(FILE *f);                              // fflush, then the file's data to the disk; 1 on success
// fseek and ftell with 64-bit offsets (a long is 32 bits on Windows).
#ifdef _WIN32
static inline int plat_fseek64(FILE *f, int64_t off, int whence) { return _fseeki64(f, off, whence); }
static inline int64_t plat_ftell64(FILE *f) { return _ftelli64(f); }
#else
static inline int plat_fseek64(FILE *f, int64_t off, int whence) { return fseeko(f, (off_t)off, whence); }
static inline int64_t plat_ftell64(FILE *f) { return (int64_t)ftello(f); }
#endif
void plat_file_delete(const char *path);
int plat_file_truncate(const char *path, uint64_t size);  // 1 on success
int64_t plat_file_size(const char *path);                 // bytes of a file, -1 when missing or unreadable
int plat_file_set_readonly(const char *path, int on);     // sets or clears the read-only attribute; 1 on success
// A file that only grows and other processes may read meanwhile: each write lands whole at its end, from any thread,
// and reaches the file even if the process dies right after (no user-space buffer, no flush to the disk).
typedef struct PlatAppend PlatAppend;
PlatAppend *plat_append_open(const char *path);           // creates or truncates path; NULL on failure
int plat_append_write(PlatAppend *f, const void *data, size_t n);   // 1 when all n bytes landed
// Calls cb(name, ctx) for each file in dir whose name ends in ext (".in"); name has no directory part. cb returns 0 to
// stop. Returns 0 when dir cannot be listed.
int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *name, void *ctx), void *ctx);
// Open files for positional reads: any thread, in parallel on one handle, no file pointer.
typedef struct PlatFile PlatFile;
PlatFile *plat_file_open(const char *path);               // read-only; NULL when missing or unreadable
// The same without the OS's file cache (reads go to the disk into the buffer): offsets, sizes and buffers aligned to
// the sector size.
PlatFile *plat_file_open_unbuffered(const char *path);
void plat_file_close(PlatFile *f);
// Bytes read at off (0 at or past the end of the file), -1 on an I/O error, -2 when the OS refused to write into buf
// (a read-only page).
int64_t plat_file_pread(PlatFile *f, void *buf, uint32_t n, uint64_t off);
// What changes when the file is replaced or written: its volume, file id, size, last write and creation (on Linux,
// status change) times in the OS's units. 0 on failure.
typedef struct { uint64_t volume, file_id, size, write_time, create_time; } PlatFileId;
int plat_file_id(PlatFile *f, PlatFileId *out);
const void *plat_file_map(PlatFile *f);                   // a read-only view of the whole file, kept until exit; NULL

// ---- the host file system under the game's save data (fs_host.c). Calls that can fail return PLAT_FS_OK (0) or the
// reason; other processes may open the same files meanwhile.
enum { PLAT_FS_OK, PLAT_FS_NOT_FOUND, PLAT_FS_EXISTS, PLAT_FS_NOT_EMPTY, PLAT_FS_BUSY, PLAT_FS_ERROR };
enum { PLAT_FS_MISSING = -1, PLAT_FS_FILE, PLAT_FS_DIR, PLAT_FS_LINK };   // plat_fs_kind; LINK: a symbolic link or
                                                                         // reparse point, never followed
int plat_fs_kind(const char *path);
int plat_fs_mkdir(const char *path);                      // PLAT_FS_EXISTS when something is there
int plat_fs_rmdir(const char *path);                      // an empty directory, or a link to one
int plat_fs_unlink(const char *path);                     // a file or a link; a read-only one too
// Renames a file or directory; never replaces (PLAT_FS_EXISTS). durable: the change is on the disk at return.
int plat_fs_rename(const char *from, const char *to, int durable);
int plat_fs_link(const char *existing, const char *path); // a hard link
int plat_fs_links(const char *path);                      // the file's hard link count, 0 when unknown
void plat_fs_sync_dir(const char *path);                  // a directory's entries to the disk, as far as the FS allows
// Why dir could not be renamed, for the log: into out "no permission to rename it" when this process may not delete
// it, else the programs holding a file under it ("explorer.exe 1234, ..."), else an empty string. Windows refuses to
// rename a directory with any file open under it; elsewhere out is always empty.
void plat_fs_busy_reason(const char *dir, char *out, size_t cap);
// cb(name, kind, size, ctx) for each entry of dir but "." and ".."; cb returns 0 to stop. The listing's error.
int plat_fs_list(const char *dir, int (*cb)(const char *name, int kind, uint64_t size, void *ctx), void *ctx);
// An existing file for reading and, with write, writing; *err (may be NULL) gets the reason on failure.
PlatFile *plat_fs_open(const char *path, int write, int *err);
PlatFile *plat_fs_create(const char *path, int *err);     // a new file for writing; PLAT_FS_EXISTS when there
int64_t plat_file_pwrite(PlatFile *f, const void *buf, uint32_t n, uint64_t off);   // bytes written, -1 on error
int plat_file_set_size(PlatFile *f, uint64_t size);       // PLAT_FS_*
int plat_file_fsync(PlatFile *f);                         // the file's data to the disk; 1 on success
int plat_temp_dir(char *out, size_t cap);                 // the user's temporary directory, no trailing separator
int plat_exe_path(char *out, size_t cap);                 // path of the running executable; 0 on failure
uintptr_t plat_exe_base(void);                            // where the executable is loaded
// The module (executable or library) holding code address addr: its file name without directories into name, its load
// address into *base. 0 when none.
int plat_module_of(uintptr_t addr, char *name, size_t cap, uintptr_t *base);
#define PLAT_PATH_MAX 1024   // path buffers of the runtime
#ifdef _WIN32
#define PLAT_EXE_SUFFIX ".exe"   // of the programs the runtime starts (totk_xlate)
#else
#define PLAT_EXE_SUFFIX ""
#endif
// The last directory separator in path ('/' or '\\'), NULL when there is none.
static inline char *plat_last_sep(const char *path) {
    char *last = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') last = (char *)p;
    return last;
}
// Environment variable as UTF-8 into out; its length, or 0 when unset, empty or too long for cap.
size_t plat_getenv(const char *name, char *out, size_t cap);
// The user's UI language as a BCP-47 tag ("ru-RU", "en-US"); "" when the system gives none. Valid for the process.
const char *plat_ui_language(void);
// The user's regional format (locale) as a BCP-47 tag; "" when the system gives none. Valid for the process.
const char *plat_user_locale(void);
uint32_t plat_thread_id(void);                            // id of the calling thread, for unique file names
// A system font file with the glyphs of a catalog language ("ja", "ko", "zh-Hans", "zh-Hant"): the menu shows each
// language's name in its own script. Copies the UTF-8 path into out; 0 when the system has none.
int plat_font_path(const char *lang, char *out, size_t cap);
// The operating system and its build ("Windows 10.0.28000") into out; 0 when unknown.
int plat_os_version(char *out, size_t cap);
// The file the process's stderr (the log) goes to, as a UTF-8 path; 0 when stderr is not a file (a console, a pipe).
int plat_stderr_path(char *out, size_t cap);

// ---- desktop: the in-game menu's "Copy system info" and "Open log folder"
int plat_clipboard_set(const char *utf8);                 // replaces the clipboard's text; 1 on success
int plat_open_folder(const char *path);                   // shows the folder in the file manager; 1 on success

// ---- dynamic libraries: a library some machines lack (a GPU vendor's), loaded at run time. A bare name is searched
// in the system directories only. NULL when missing.
void *plat_lib_open(const char *name);
void *plat_lib_sym(void *lib, const char *name);

// ---- machine load for the performance overlay (perfmon.c's sampling thread; each call can take milliseconds). A number
// the system does not give is -1.
typedef struct PlatLoad PlatLoad;
// Opens the counters: CPU times, the thermal zones, and the GPU engine and memory counters of the adapter whose LUID is
// gpu_luid (8 bytes, Vulkan's deviceLUID; NULL: the busiest adapter) or, where the system names GPUs by PCI address,
// on PCI bus pci_bus (-1: unknown). Never NULL.
PlatLoad *plat_load_open(const uint8_t *gpu_luid, int pci_bus);
void plat_load_close(PlatLoad *l);
// Since the previous call on l (the first call gives -1): CPU use of the machine and of this process, percent of all
// logical processors, and the hottest ACPI thermal zone in degrees Celsius.
void plat_cpu_load(PlatLoad *l, float *total_pct, float *proc_pct, float *temp_c);
// Since the previous call on l: the busiest GPU engine type in percent (as Task Manager shows it), and the adapter's
// dedicated memory in use in MB. Any GPU vendor.
void plat_gpu_load(PlatLoad *l, float *load_pct, float *vram_used_mb);
// Physical memory in MB: in use and installed on the machine, and this process's working set.
void plat_mem_info(float *used_mb, float *total_mb, float *proc_mb);
// Commit charge in MB: this process's private bytes, the system's commit in use and its limit (RAM plus page files).
// An allocation fails when the system's reaches the limit, however much physical memory is free. -1 when unknown.
void plat_commit_info(double *proc_mb, double *used_mb, double *limit_mb);
// This process's memory in bytes: resident (the working set) and private (Windows: committed private bytes; Linux: the
// resident anonymous pages). 0 when unknown.
void plat_process_memory(uint64_t *resident, uint64_t *private_bytes);

// ---- child processes: a program with stdin and stdout piped to us (stderr discarded), no console window.
typedef struct PlatChild PlatChild;
// Starts "exe" followed by arg (arg may be NULL). NULL on failure; *err (may be NULL) gets the OS error code.
// Children start one at a time so none inherits another pipe ends.
PlatChild *plat_child_start(const char *exe, const char *arg, uint32_t *err);
void plat_child_stop(PlatChild *c);                       // kills the child and frees c
int plat_child_write(PlatChild *c, const void *data, size_t n);   // 1 when all n bytes were written
long plat_child_read(PlatChild *c, void *buf, size_t cap);        // bytes read (at least 1); 0 or negative: pipe broke
