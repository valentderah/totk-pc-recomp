// Game-free probes are compiled only into diagnostic builds.
#include "native_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef TOTK_PROBE_BUILD
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <time.h>
#include "nvn_pool_index.h"
extern void runtime_fail(const char *message);
extern void runtime_gpu_path_probe(const char *name);
extern void runtime_save_probe(void);
extern void runtime_swkbd_probe(void);
extern void mtl_probe_warm_pipelines(void);
extern void mtl_probe_attachment_ledger(void);
extern void mtl_probe_attachment_feedback(void);
extern uint64_t runtime_now_ns(void);

static void *probe_frame_thread(void *unused) {
    (void)unused;
    if (pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0)) runtime_fail("set initial probe QoS");
    native_frame_thread_init();
    native_frame_thread_init();
    qos_class_t qos = QOS_CLASS_UNSPECIFIED;
    int relative = 0;
    if (pthread_get_qos_class_np(pthread_self(), &qos, &relative) || qos != QOS_CLASS_USER_INTERACTIVE)
        runtime_fail("frame thread lost interactive QoS");
    thread_time_constraint_policy_data_t policy;
    mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t is_default = FALSE;
    if (thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                          (thread_policy_t)&policy, &count, &is_default) != KERN_SUCCESS || !is_default)
        runtime_fail("frame thread has an unbounded real-time policy");
    return NULL;
}

static mach_timebase_info_data_t reference_timebase;
static pthread_once_t reference_once = PTHREAD_ONCE_INIT;
static void reference_init(void) { mach_timebase_info(&reference_timebase); }
static uint64_t reference_clock(void) {
    pthread_once(&reference_once, reference_init);
    return (uint64_t)((__uint128_t)mach_absolute_time()*reference_timebase.numer/reference_timebase.denom);
}

static int probe_clock(void) {
    for (unsigned i=0; i<100000; ++i) {
        uint64_t lo=reference_clock(), actual=runtime_now_ns(), hi=reference_clock();
        if (actual < lo || actual > hi) runtime_fail("native clock changed uptime semantics");
    }
    uint64_t (*clocks[])(void) = {reference_clock, runtime_now_ns};
    double times[2];
    volatile uint64_t sink=0;
    for (unsigned j=0; j<2; ++j) {
        uint64_t start=runtime_now_ns();
        for (unsigned i=0; i<1000000; ++i) sink+=clocks[j]();
        times[j]=(runtime_now_ns()-start)/1000000.0;
    }
    (void)sink;
    printf("{\"probe\":\"clock\",\"status\":\"pass\",\"checks\":100000,\"reference_ns\":%.3f,\"native_ns\":%.3f}\n", times[0], times[1]);
    return 0;
}

static int probe_pool_cache(void) {
    pidx index={0}; pidx_query_cache cache={0};
    enum { POOLS=3229, QUERIES=1000000 };
    for (unsigned i=0; i<POOLS; ++i) {
        uintptr_t start=UINT64_C(0x7000000000)+(uint64_t)i*0x100000;
        pidx_ent entry={.lo=start,.hi=start+0x80000,.key=(void *)(uintptr_t)(i+1),.gpu=start};
        if (!pidx_insert(&index,&entry)) runtime_fail("pool cache fixture");
        pidx_query_invalidate(&cache);
    }
    for (unsigned round=0; round<20000; ++round) {
        uintptr_t address=UINT64_C(0x7000000000)+(uint64_t)(round%POOLS)*0x100000;
        // Boundary, zero-length, miss, overflow, and cache collisions.
        const uint64_t lengths[]={0,64,0x80000,0x80001,UINT64_MAX};
        for (unsigned j=0; j<5; ++j)
            if (pidx_find_cached(&index,&cache,address,lengths[j]) != pidx_find(&index,address,lengths[j]))
                runtime_fail("pool cache differs from authoritative lookup");
        const void *key=(void *)(uintptr_t)(round%POOLS+1);
        pidx_ent removed;
        if (!pidx_remove(&index,key,&removed)) runtime_fail("remove pool fixture");
        pidx_query_invalidate(&cache);
        if (pidx_find_cached(&index,&cache,address,64)) runtime_fail("pool cache retained a retired allocation");
        if (!pidx_insert(&index,&removed)) runtime_fail("restore pool fixture");
        pidx_query_invalidate(&cache);
    }
    double ns[2][2]; uint64_t sums[2][2]={{0}};
    for (unsigned cold=0; cold<2; ++cold) for (unsigned cached=0; cached<2; ++cached) {
        uint64_t start=runtime_now_ns();
        for (unsigned i=0; i<QUERIES; ++i) {
            unsigned pool=cold ? (unsigned)((uint64_t)i*1223%POOLS) : (i/32)%POOLS;
            uintptr_t address=UINT64_C(0x7000000000)+(uint64_t)pool*0x100000+(i%8)*256;
            const pidx_ent *entry=cached ? pidx_find_cached(&index,&cache,address,64) : pidx_find(&index,address,64);
            sums[cold][cached]+=entry ? entry->gpu : 0;
        }
        ns[cold][cached]=(runtime_now_ns()-start)/(double)QUERIES;
    }
    if (sums[0][0]!=sums[0][1] || sums[1][0]!=sums[1][1]) runtime_fail("pool cache benchmark differs");
    free(index.e); free(index.maxhi);
    printf("{\"probe\":\"pool-cache\",\"status\":\"pass\",\"checks\":120000,\"hot_reference_ns\":%.3f,\"hot_cached_ns\":%.3f,\"cold_reference_ns\":%.3f,\"cold_cached_ns\":%.3f}\n",
           ns[0][0],ns[0][1],ns[1][0],ns[1][1]);
    return 0;
}

static void *trace_producer(void *arg) {
    uint64_t first=(uintptr_t)arg*256;
    for (uint64_t i=first; i<first+256; ++i) {
        NativeFrame frame={.frame=i,.scene_width=3840,.scene_height=2160,
                           .output_width=3840,.output_height=2160,.drawable_width=3840,.drawable_height=2160};
        native_presented(i,frame,(double)i/144.0);
    }
    return NULL;
}

static int probe_trace(int close_app) {
    native_policy_init();
    if (!native_trace_active()) runtime_fail("probe-trace requires TOTK_FRAME_TRACE");
    pthread_t threads[4];
    for (uintptr_t i=0; i<4; ++i)
        if (pthread_create(&threads[i],NULL,trace_producer,(void *)i)) runtime_fail("start trace producer");
    for (unsigned i=0; i<4; ++i)
        if (pthread_join(threads[i],NULL)) runtime_fail("join trace producer");
    if (close_app) native_exit(0);
    native_trace_finish();
    puts("{\"probe\":\"trace\",\"status\":\"pass\",\"records\":1024}");
    return 0;
}
#endif

int native_run_probe(const char *name) {
    if (!strcmp(name, "--selftest") || !strcmp(name, "--host-check")) return native_host_check();
#ifdef TOTK_PROBE_BUILD
    if (!strcmp(name, "--probe-clock")) return probe_clock();
    if (!strcmp(name, "--probe-pool-cache")) return probe_pool_cache();
    if (!strcmp(name, "--probe-warm-pipelines")) { mtl_probe_warm_pipelines(); return 0; }
    if (!strcmp(name, "--probe-attachment-ledger") || !strcmp(name, "--probe-attachment-feedback")) {
        setenv("TOTK_NATIVE_4K144", "0", 1);
        setenv("TOTK_HEADLESS", "1", 1);
        setenv("TOTK_PROBE_GPU", "1", 1);
        native_policy_init();
        if (!strcmp(name, "--probe-attachment-feedback")) mtl_probe_attachment_feedback();
        else mtl_probe_attachment_ledger();
        return 0;
    }
    if (!strcmp(name, "--probe-trace")) return probe_trace(0);
    if (!strcmp(name, "--probe-trace-exit")) return probe_trace(1);
    if (!strcmp(name, "--probe-policy")) {
        extern int mfx_enabled(void), mfxup_enabled(void), mfxup_output_enabled(void);
        extern void mfx_set(int), mfxup_set(int), mfxup_output_set(int);
        setenv("TOTK_NATIVE_4K144", "1", 1);
        native_policy_init();
        pthread_t frame_threads[2];
        for (unsigned i=0; i<2; ++i)
            if (pthread_create(&frame_threads[i], NULL, probe_frame_thread, NULL)) runtime_fail("start frame policy probe");
        for (unsigned i=0; i<2; ++i)
            if (pthread_join(frame_threads[i], NULL)) runtime_fail("join frame policy probe");
        if (mfx_enabled() || mfxup_enabled() || mfxup_output_enabled()) runtime_fail("native policy initial scaling");
        mfx_set(1); mfxup_set(1); mfxup_output_set(1);
        if (mfx_enabled() || mfxup_enabled() || mfxup_output_enabled()) runtime_fail("native policy runtime scaling");
        native_check_scene(7,3840,2160);
        NativeFrame f=native_frame_capture(7,3840,2160,3840,2160);
        if(f.scene_width!=3840 || native_frame_capture(8,3840,2160,3840,2160).scene_width)
            runtime_fail("frame scene identity");
        native_check_scene(71,3840,2160);
        if(native_frame_capture(7,3840,2160,3840,2160).scene_width || f.scene_width!=3840)
            runtime_fail("frame metadata lifetime");
        puts("{\"probe\":\"policy\",\"status\":\"pass\"}");
        return 0;
    }
    // A closed list prevents unknown names from selecting the donor's default probe.
    const char *gpu[] = {"midclear", "ds-clear", "copy-alias", "dispatch-chain", "storage-view",
        "texture-retire", "pool-retire", "ubo-snapshot", "linear-rt", "depth-output", "srgb-view",
        "vertex-streams", "arg-epoch", "tex-slots", "layer-merge", "cube-merge", "compressed-alias",
        "upload-batch", "ub-rename", "drv-c0", "widen", "winding", "tile", "blockrt"};
    for (unsigned i = 0; i < sizeof gpu / sizeof *gpu; ++i) {
        char mode[80];
        snprintf(mode, sizeof mode, "--probe-gpu-%s", gpu[i]);
        if (!strcmp(name, mode)) {
            if (native_host_check()) return 1;
            setenv("TOTK_NATIVE_4K144", "0", 1);
            native_policy_init();
            extern void dm_gfx_set_flag(const char *, int);
            dm_gfx_set_flag("pool_query_check", 1);
            dm_gfx_set_flag("usnap_index_check", 1);
            setenv("TOTK_PROBE_GPU", "1", 1);
            setenv("TOTK_HEADLESS", "1", 1);
            runtime_gpu_path_probe(mode + 2);
            return 0;
        }
    }
    if (!strcmp(name, "--probe-save")) { runtime_save_probe(); return 0; }
    if (!strcmp(name, "--probe-swkbd")) { runtime_swkbd_probe(); return 0; }
#endif
    return -1;
}
