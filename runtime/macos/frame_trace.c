// Presentation callbacks only enqueue records. Formatting and disk I/O belong
// to the writer; a full queue fails the capture instead of losing evidence.
#include "native_policy.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern void runtime_fail(const char *message);
enum { TRACE_CAPACITY = 4096, TRACE_BATCH = 256 };
typedef struct { uint64_t ticket; NativeFrame frame; double seconds; int strict; } TraceRecord;
static struct {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_t thread;
    TraceRecord *records;
    FILE *file;
    uint64_t read, write;
    int stopping;
} trace = {.mutex=PTHREAD_MUTEX_INITIALIZER, .ready=PTHREAD_COND_INITIALIZER};
static _Atomic int active;

static void writer_failure(void) {
    // runtime_fail uses exit(), whose trace finalizer would join this thread.
    fputs("[totk] FAIL: frame trace I/O failed; capture is incomplete\n", stderr);
    _Exit(1);
}

int native_trace_active(void) { return atomic_load_explicit(&active, memory_order_relaxed); }

static void *write_trace(void *unused) {
    (void)unused;
    TraceRecord batch[TRACE_BATCH];
    uint64_t flushed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    for (;;) {
        pthread_mutex_lock(&trace.mutex);
        while (trace.read == trace.write && !trace.stopping) {
            struct timespec timeout = {1, 0};
            if (pthread_cond_timedwait_relative_np(&trace.ready, &trace.mutex, &timeout) == ETIMEDOUT) break;
        }
        size_t n = (size_t)(trace.write - trace.read);
        if (n > TRACE_BATCH) n = TRACE_BATCH;
        for (size_t i=0; i<n; ++i) batch[i] = trace.records[trace.read++ % TRACE_CAPACITY];
        int finished = trace.stopping && trace.read == trace.write;
        pthread_mutex_unlock(&trace.mutex);
        for (size_t i=0; i<n; ++i) {
            const TraceRecord *r = &batch[i];
            NativeFrame f = r->frame;
            if (fprintf(trace.file, "%llu,%llu,%.9f,%u,%u,%u,%u,%u,%u,%d\n",
                (unsigned long long)r->ticket, (unsigned long long)f.frame, r->seconds,
                f.scene_width, f.scene_height, f.output_width, f.output_height,
                f.drawable_width, f.drawable_height, r->strict) < 0) writer_failure();
        }
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (finished || now-flushed >= UINT64_C(1000000000)) {
            if (fflush(trace.file)) writer_failure();
            flushed = now;
        }
        if (finished) return NULL;
    }
}

void native_trace_finish(void) {
    if (!atomic_exchange_explicit(&active, 0, memory_order_relaxed)) return;
    pthread_mutex_lock(&trace.mutex);
    trace.stopping = 1;
    pthread_cond_signal(&trace.ready);
    pthread_mutex_unlock(&trace.mutex);
    if (pthread_join(trace.thread, NULL) || fclose(trace.file)) runtime_fail("close frame trace");
    free(trace.records);
    trace.records = NULL;
}

_Noreturn void native_exit(int status) {
    native_trace_finish();
    _exit(status);
}

void native_trace_init(void) {
    const char *path = getenv("TOTK_FRAME_TRACE");
    if (!path || !*path || native_trace_active()) return;
    trace.file = fopen(path, "w");
    trace.records = calloc(TRACE_CAPACITY, sizeof *trace.records);
    if (!trace.file || !trace.records) runtime_fail("open frame trace");
    trace.read = trace.write = 0;
    trace.stopping = 0;
    if (setvbuf(trace.file, NULL, _IOFBF, 65536) ||
        fputs("ticket,frame,present_seconds,scene_width,scene_height,output_width,output_height,drawable_width,drawable_height,strict\n", trace.file) < 0)
        runtime_fail("initialize frame trace");
    if (pthread_create(&trace.thread, NULL, write_trace, NULL)) runtime_fail("start frame trace writer");
    atomic_store_explicit(&active, 1, memory_order_relaxed);
    if (atexit(native_trace_finish)) runtime_fail("register frame trace shutdown");
}

void native_presented(uint64_t ticket, NativeFrame frame, double seconds) {
    if (!native_trace_active()) return;
    pthread_mutex_lock(&trace.mutex);
    if (trace.stopping) { pthread_mutex_unlock(&trace.mutex); return; }
    if (trace.write - trace.read == TRACE_CAPACITY) {
        pthread_mutex_unlock(&trace.mutex);
        runtime_fail("frame trace writer fell behind; capture is incomplete");
        return;
    }
    int wake = trace.write == trace.read;
    trace.records[trace.write++ % TRACE_CAPACITY] = (TraceRecord){ticket, frame, seconds, native_policy_strict()};
    if (wake) pthread_cond_signal(&trace.ready);
    pthread_mutex_unlock(&trace.mutex);
}
