// The null graphics back end: no GPU. Every submit completes at
// once and advances the fence value, pools always initialise, presents are only counted, texel reads return zeros.
// A submit performs the CPU-visible side effects of its op streams in order: event signals, counter reports (timestamp
// ticks, counter 0), buffer-to-buffer copies and uniform updates.
// It is the CPU-side gate for the NVN front end. Also the back-end choice (gfx_backend) and the statistics (gfx_stats).
#include <stdint.h>
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "nvn_api.h"

GfxStats gfx_counters;

GfxStats gfx_stats(void) {
    GfxStats s;
    s.frames = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    s.nvn_calls = __atomic_load_n(&gfx_counters.nvn_calls, __ATOMIC_RELAXED);
    s.nvn_distinct = __atomic_load_n(&gfx_counters.nvn_distinct, __ATOMIC_RELAXED);
    s.unimplemented = __atomic_load_n(&gfx_counters.unimplemented, __ATOMIC_RELAXED);
    s.validation_errors = __atomic_load_n(&gfx_counters.validation_errors, __ATOMIC_RELAXED);
    s.shader_failures = __atomic_load_n(&gfx_counters.shader_failures, __ATOMIC_RELAXED);
    s.draws_skipped = __atomic_load_n(&gfx_counters.draws_skipped, __ATOMIC_RELAXED);
    return s;
}

// TOTK_GPU=null|vulkan, default vulkan (rt_config); chosen once.
const GfxBackend *gfx_backend(void) {
    static const GfxBackend *chosen;
    const GfxBackend *b = __atomic_load_n(&chosen, __ATOMIC_ACQUIRE);
    if (b) return b;
    const char *gpu = rt_config()->gpu;
    b = gpu && !strcmp(gpu, "null") ? &gfx_null_backend : &gfx_vulkan_backend;
    __atomic_store_n(&chosen, b, __ATOMIC_RELEASE);
    return b;
}

static uint64_t fence;

static int null_pool_init(pool_t *p) { (void)p; return 1; }
static void null_pool_fini(pool_t *p) { (void)p; }
static int null_pool_map_virtual(pool_t *v, int n, const void *requests) { (void)v, (void)n, (void)requests; return 1; }
static void null_texture_register(int id, const tex_t *t, const view_t *v) { (void)id, (void)t, (void)v; }
static void null_sampler_register(int id, const samp_par *s) { (void)id, (void)s; }
static void null_texture_release(const tex_t *t) { (void)t; }
static void null_program_release(const prog_t *p) { (void)p; }

// The CPU-visible side effects of one op stream, in stream order (MNG nvn_mtl.m mtl_exec and the commit handler:
// 10618-10642, 1677-1678, 1960-1985). Everything else (draws, dispatches, clears, texture copies) has no effect here.
// Addresses of CPU-invisible pools (the GPU window) hold no host memory, so those writes are dropped.
static void *null_cpu_addr(uint64_t addr, uint64_t size, const char *what) {
    if (nvn_gpu_only(addr)) return NULL;
    if (!nvn_pool_find(addr, size ? size : 1)) rt_fatal("gfx: null back end: %s at %#llx (%llu bytes) is in no memory pool", what,
                                                        (unsigned long long)addr, (unsigned long long)size);
    return (void *)(uintptr_t)addr;
}

// NVNcounterData = {counter, timestamp}. Timestamps are Tegra 19.2 MHz ticks, the base of GetSystemTick; every counter
// type the null GPU cannot measure (all but the timestamps, samples passed included) reports 0.
static void null_report(uint64_t addr) {
    NVNcounterData *d = null_cpu_addr(addr, sizeof *d, "counter report");
    if (!d) return;
    uint64_t rec[2] = {0, (uint64_t)((unsigned __int128)kern_now_ns() * 12 / 625)};
    memcpy(d, rec, sizeof rec);
}

static void null_replay(uint64_t handle) {
    for (const op_hdr *h = nvn_op_first(handle);; h = nvn_op_next(h)) {
        const void *a = nvn_op_payload(h);
        switch (h->op) {
        case OP_END: return;
        case OP_SIGNAL_EVENT: {
            const p_signal_event *e = a;
            if (e->mode == NVN_EVENT_SIGNAL_MODE_ADD) __atomic_fetch_add(e->val, e->value, __ATOMIC_RELEASE);
            else __atomic_store_n(e->val, e->value, __ATOMIC_RELEASE);
            break;
        }
        case OP_REPORT_COUNTER: null_report(((const p_report_counter *)a)->addr); break;
        case OP_COPY_B2B: {
            const p_copy_b2b *b = a;
            void *s = null_cpu_addr(b->src, b->size, "buffer copy source"), *d = null_cpu_addr(b->dst, b->size, "buffer copy target");
            if (s && d) memmove(d, s, b->size);
            break;
        }
        case OP_CLEAR_BUF: {
            const p_clear_buf *f = a;
            uint32_t *d = null_cpu_addr(f->addr, f->size, "buffer fill");
            for (uint64_t i = 0; d && i < f->size / 4; i++) d[i] = f->value;
            break;
        }
        case OP_UPDATE_UB: {
            const p_update_ub *u = a;
            void *d = null_cpu_addr(u->dst, u->size, "uniform update");
            if (d) memcpy(d, u->data, u->size);
            break;
        }
        default: break;
        }
    }
}

static uint64_t null_submit(const uint64_t *handles, int n) {
    nvn_pins_sweep(nvn_submit_count(), NULL, 0);   // no replay state outlives a submission here
    for (int i = 0; i < n; i++) null_replay(handles[i]);
    return __atomic_add_fetch(&fence, 1, __ATOMIC_ACQ_REL);
}

static int null_fence_wait(uint64_t value, uint64_t timeout_ns) {
    (void)timeout_ns;
    return value <= __atomic_load_n(&fence, __ATOMIC_ACQUIRE);   // a value never submitted is never reached
}

static uint64_t null_fence_done(void) { return __atomic_load_n(&fence, __ATOMIC_ACQUIRE); }

static void null_present(const tex_t *t, const int crop[4], uint64_t at_ns) {
    (void)t, (void)crop, (void)at_ns;
    __atomic_fetch_add(&gfx_counters.frames, 1, __ATOMIC_RELAXED);
}

static void null_finish(void) {}

static void null_read_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row,
                             uint64_t img) {
    (void)t, (void)v;
    int h = r->height > 0 ? r->height : 1, d = r->depth > 0 ? r->depth : 1;
    for (int z = 0; z < d; z++) memset(dst + (uint64_t)z * img, 0, row * (uint64_t)h);
}

static void null_write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src,
                              uint64_t row, uint64_t img) {
    (void)t, (void)v, (void)r, (void)src, (void)row, (void)img;
}

static void null_guest_written(uint64_t addr, uint64_t size) { (void)addr, (void)size; }
static void null_guest_read(uint64_t addr, uint64_t size) { (void)addr, (void)size; }
static void null_cpu_synced(uint64_t fence) { (void)fence; }

const GfxBackend gfx_null_backend = {
    .name = "null",
    .pool_init = null_pool_init,
    .pool_fini = null_pool_fini,
    .pool_map_virtual = null_pool_map_virtual,
    .texture_register = null_texture_register,
    .sampler_register = null_sampler_register,
    .texture_release = null_texture_release,
    .program_release = null_program_release,
    .submit = null_submit,
    .fence_wait = null_fence_wait,
    .fence_done = null_fence_done,
    .present = null_present,
    .finish = null_finish,
    .read_texels = null_read_texels,
    .write_texels = null_write_texels,
    .guest_written = null_guest_written,
    .guest_read = null_guest_read,
    .cpu_synced = null_cpu_synced,
};
