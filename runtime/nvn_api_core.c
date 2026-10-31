// NVN front end, first slice: device, queue, memory pool, buffer, sync and
// event objects. Ported from MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c @ b75be8cf (MIT,
// Copyright (c) 2026 jhaizhou; origin d2r-mobile), lines 137-253, 534-645 and 1975-2001; Metal, mach, the translation
// thread and Apple diagnostics dropped. Calls that reached Metal go to gfx_backend() (gfx.h). nvnQueuePresentTexture
// belongs to the window slice; nvnDeviceGetProcAddress lives in nvn_procs.c.
//
// Objects are stored in place in the NVN object memory the game provides (nvn_obj.h); every argument is read from the
// AAPCS64 registers (host.h arg_x / arg_p) and every result returned with ret_x.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_pool_index.h"
#include "platform/platform.h"

#define DEV_MAGIC MAGIC('D', 'E', 'V', 0)
#define QUEUE_MAGIC MAGIC('Q', 'U', 'E', 'U')
#define BUF_MAGIC MAGIC('B', 'U', 'F', 0)
#define SYNC_MAGIC MAGIC('S', 'Y', 'N', 'C')
#define EVENT_MAGIC MAGIC('E', 'V', 'N', 'T')

// ---------- shared helpers (nvn_api.h)
// The call count goes to the shared counter 1024 at a time: the main thread and the job workers all record command
// buffers, and one locked add per call kept the counter's cache line moving between their cores (0.8k samples a
// fight). The count lags by at most 1023 calls per thread.
void nvn_called(uint8_t *seen, const char *name) {
    static _Thread_local uint32_t calls;
    if (++calls == 1024) {
        __atomic_fetch_add(&gfx_counters.nvn_calls, calls, __ATOMIC_RELAXED);
        calls = 0;
    }
    if (__atomic_load_n(seen, __ATOMIC_RELAXED) || __atomic_exchange_n(seen, 1, __ATOMIC_RELAXED)) return;
    uint64_t n = __atomic_add_fetch(&gfx_counters.nvn_distinct, 1, __ATOMIC_RELAXED);
    NVN_LOG("first call %llu: %s", (unsigned long long)n, name);
}

uint64_t nvn_new_serial(void) {
    static uint64_t n;
    return __atomic_add_fetch(&n, 1, __ATOMIC_RELAXED);
}

static uint64_t last_fence;
uint64_t nvn_fence_last(void) { return __atomic_load_n(&last_fence, __ATOMIC_ACQUIRE); }

static int depth_minus_w;
int nvn_depth_minus_w(void) { return __atomic_load_n(&depth_minus_w, __ATOMIC_RELAXED); }

void nvn_guest_written(uint64_t addr, uint64_t size) {
    if (size) gfx_backend()->guest_written(addr, size);
}

// memcpy of pool storage the GPU rendered into must copy what the GPU wrote: TotK moves its generated grass maps
// between pools with memcpy. Copies below 1 KiB skip the check; the copy itself is memmove, as overlapping
// arguments are undefined for memcpy anyway.
static void ov_memcpy(Cpu *c) {
    uint64_t dst = arg_x(c, 0), src = arg_x(c, 1), n = arg_x(c, 2);
    if (n >= 1024) gfx_backend()->guest_read(src, n);
    memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n);
    ret_x(c, dst);
}
HOST_OVERRIDE("memcpy", ov_memcpy);

__attribute__((weak)) void nvn_submit_prepare(const uint64_t *handles, int n) { (void)handles, (void)n; }
__attribute__((weak)) void nvn_submit_prepare_async(const uint64_t *handles, uint64_t *out, int n) {
    memcpy(out, handles, sizeof *out * (size_t)n);
}
__attribute__((weak)) int gfx_replay_async(void) { return 0; }
__attribute__((weak)) uint64_t gfx_replayed_submits(void) { return nvn_submit_count(); }

// ---------- pools: GPU window (section 3.1) and the address index
static PlatMutex pool_lock = PLAT_MUTEX_INIT;   // guards gva and index
typedef struct { uint64_t lo, hi; } Span;
enum { GVA_MAX = 1024 };
static Span gva[GVA_MAX];   // ranges handed out in the GPU window, sorted by lo
static int ngva;
static pidx pool_index;

// First fit in [RT_GPU_VA_BASE, +RT_GPU_VA_SIZE); size is a multiple of 64 KiB, so every range stays 64 KiB aligned.
static uint64_t gva_alloc(uint64_t size) {
    plat_mutex_lock(&pool_lock);
    uint64_t at = RT_GPU_VA_BASE, r = 0;
    int i = 0;
    for (; i < ngva && gva[i].lo - at < size; i++) at = gva[i].hi;
    if (ngva < GVA_MAX && size <= RT_GPU_VA_BASE + RT_GPU_VA_SIZE - at) {
        memmove(gva + i + 1, gva + i, (size_t)(ngva - i) * sizeof *gva);
        gva[i] = (Span){at, at + size};
        ngva++;
        r = at;
    }
    plat_mutex_unlock(&pool_lock);
    return r;
}

static void gva_free(uint64_t lo) {
    plat_mutex_lock(&pool_lock);
    for (int i = 0; i < ngva; i++)
        if (gva[i].lo == lo) {
            memmove(gva + i, gva + i + 1, (size_t)(ngva - i - 1) * sizeof *gva);
            ngva--;
            break;
        }
    plat_mutex_unlock(&pool_lock);
}

static void index_add(const pool_t *o) {
    uintptr_t lo = (uintptr_t)o->mem;
    pidx_ent e = {.lo = lo, .hi = lo + o->size, .base = lo, .gpu = lo, .key = o};
    plat_mutex_lock(&pool_lock);
    pidx_remove(&pool_index, o, NULL);   // a pool object initialized again without Finalize
    int ok = pidx_insert(&pool_index, &e);
    plat_mutex_unlock(&pool_lock);
    if (!ok) rt_fatal("nvn: out of memory for the pool index");
}

static void index_del(const pool_t *o) {
    plat_mutex_lock(&pool_lock);
    pidx_remove(&pool_index, o, NULL);
    plat_mutex_unlock(&pool_lock);
}

const pool_t *nvn_pool_find(uint64_t addr, uint64_t len) {
    plat_mutex_lock_shared(&pool_lock);
    const pidx_ent *e = pidx_find(&pool_index, (uintptr_t)addr, len);
    const pool_t *p = e ? e->key : NULL;
    plat_mutex_unlock_shared(&pool_lock);
    return p;
}

// Physical and virtual pools (MacNativeGames totk_vpool.c checks). A CPU-invisible one gets a GPU window range.
// A CPU-visible physical pool (1.4.3: flags 0x18c, GPU_NO_ACCESS) keeps the game's storage for the CPU; the back end
// gives it device memory as any physical pool (vk_memory.c vk_pool_init), which the GPU reaches through virtual pools.
static int pool_place(pool_t *o) {
    int phys = (o->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL) != 0, virt = (o->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) != 0;
    if (!phys && !virt) return 1;
    const char *kind = phys ? "physical" : "virtual";
    if (phys && virt) { rt_log("nvn: pool flags %#x are both PHYSICAL and VIRTUAL", o->flags); return 0; }
    if (!o->size || o->size % NVN_VPOOL_PAGE) {
        rt_log("nvn: %s pool size %#llx is not a multiple of 64 KiB", kind, (unsigned long long)o->size);
        return 0;
    }
    if (virt && o->mem) { rt_log("nvn: virtual pool given storage %p", (void *)o->mem); return 0; }
    if (phys && !o->mem) { rt_log("nvn: physical pool without storage"); return 0; }
    if (!(o->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) {
        if (virt) {
            rt_log("nvn: CPU-visible virtual pool (flags %#x, size %#llx) refused", o->flags, (unsigned long long)o->size);
            return 0;
        }
        rt_log("nvn: CPU-visible physical pool %p (flags %#x, size %#llx) keeps the game's storage", (void *)o->mem,
               o->flags, (unsigned long long)o->size);
        return 1;
    }
    uint64_t a = gva_alloc(o->size);
    if (!a) {
        rt_log("nvn: GPU address window full, %s pool of %#llx bytes refused", kind, (unsigned long long)o->size);
        return 0;
    }
    o->mem = (uint8_t *)(uintptr_t)a;
    o->span = o->size;
    return 1;
}

static int pool_gpu_visible(const pool_t *o) { return !(o->flags & NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS); }

// ---------- Device
static const int devinfo[] = {   // Tegra X1 values (nnheaders nvn_DeviceConstantsNX.h, measured on SMO)
    [0x00] = 55, [0x01] = 15,    // ABI: TOTK main requires major 55, minor >= 15
    [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 16, [0x07] = 4, [0x08] = 16, [0x09] = 32,
    [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 16, [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256,
    [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2, [0x18] = 256, [0x19] = 4096, [0x1A] = 131072,
    [0x1B] = 16, [0x1C] = 1536, [0x1D] = 1024, [0x1E] = 64, [0x1F] = 1536, [0x20] = 65535, [0x21] = 65535, [0x22] = 65535,
    [0x23] = 8, [0x24] = 1048576, [0x25] = 4096, [0x26] = 16, [0x27] = 4096, [0x28] = 1, [0x29] = 1, [0x2A] = 1,
    [0x2B] = 262144, [0x2C] = 15, [0x2D] = 2048, [0x2E] = 1, [0x2F] = 1, [0x30] = 14, [0x31] = 5, [0x32] = 1, [0x33] = 8,
    [0x34] = 8, [0x35] = 4, [0x36] = 32, [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 1, [0x3C] = 4,
    [0x3D] = 16384, [0x3E] = 134217728, [0x3F] = 2048, [0x40] = 16384, [0x41] = 16384, [0x42] = 1, [0x43] = 1, [0x44] = 1,
    [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4A] = 1, [0x4B] = 1, [0x4C] = 1, [0x4D] = 16,
    [0x4E] = 1024, [0x4F] = 32, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536, [0x53] = 256, [0x54] = 16384,
    [0x55] = 262144, [0x56] = 4096, [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096, [0x5C] = 16384,
    [0x5D] = 4096, [0x5E] = 128, [0x5F] = 4096,   // 0x5F (separate sampler bindings) from the sampler pool limit, unverified
};

// void nvnDeviceBuilderSetDefaults(NVNdeviceBuilder *b)
NVN_API(nvnDeviceBuilderSetDefaults) { memset(arg_p(c, 0), 0, sizeof(dev_builder_t)); }
// void nvnDeviceBuilderSetFlags(NVNdeviceBuilder *b, int flags)
NVN_API(nvnDeviceBuilderSetFlags) { ((dev_builder_t *)arg_p(c, 0))->flags = (uint32_t)arg_x(c, 1); }
// NVNboolean nvnDeviceInitialize(NVNdevice *d, const NVNdeviceBuilder *b)
NVN_API(nvnDeviceInitialize) {
    dev_t_ *o = arg_p(c, 0);
    const dev_builder_t *b = arg_p(c, 1);
    memset(o, 0, sizeof *o);
    o->magic = DEV_MAGIC;
    o->flags = b->flags;
    NVN_LOG("DeviceInitialize %p flags %#x, back end %s", (void *)o, o->flags, gfx_backend()->name);
    ret_x(c, 1);
}
// void nvnDeviceFinalize(NVNdevice *d)
NVN_API(nvnDeviceFinalize) { ((dev_t_ *)arg_p(c, 0))->magic = 0; }
// void nvnDeviceGetInteger(const NVNdevice *d, NVNdeviceInfo what, int *out)
NVN_API(nvnDeviceGetInteger) {
    unsigned w = (unsigned)arg_x(c, 1);
    int *out = arg_p(c, 2);
    if (w >= sizeof devinfo / sizeof *devinfo) rt_fatal("nvn: unsupported NVN device property %#x", w);
    *out = devinfo[w];
    NVN_LOG("DeviceGetInteger %#x -> %d", w, *out);
}
// NVNboolean nvnDeviceRegisterFastClearColor{,i,ui}(NVNdevice *d, const T *color, NVNformat format), ...Depth(d, float):
// fast-clear registration is an optimisation hint; "not registered" keeps the normal clear path.
NVN_API(nvnDeviceRegisterFastClearColor) { ret_x(c, 0); }
NVN_API(nvnDeviceRegisterFastClearColori) { ret_x(c, 0); }
NVN_API(nvnDeviceRegisterFastClearColorui) { ret_x(c, 0); }
NVN_API(nvnDeviceRegisterFastClearDepth) { ret_x(c, 0); }
// NVNimageHandle nvnDeviceGetImageHandle(const NVNdevice *d, int id)
NVN_API(nvnDeviceGetImageHandle) { ret_x(c, (uint64_t)(int64_t)(int)arg_x(c, 1)); }
// NVNtextureHandle nvnDeviceGetTextureHandle(const NVNdevice *d, int texture_id, int sampler_id):
// texture id (20 bits) | sampler id << 20, the encoding the replay decodes (NVN_HANDLE_TEX / NVN_HANDLE_SAMP)
NVN_API(nvnDeviceGetTextureHandle) {
    int tid = (int)arg_x(c, 1), sid = (int)arg_x(c, 2);
    if (tid < 0 || tid >= (1 << 20) || sid < 0 || sid >= (1 << 12)) rt_fatal("nvn: texture handle ids out of range %d/%d", tid, sid);
    ret_x(c, (uint64_t)tid | (uint64_t)sid << 20);
}
// NVNtextureHandle nvnDeviceGetTexelFetchHandle(const NVNdevice *d, int texture_id)
NVN_API(nvnDeviceGetTexelFetchHandle) {
    int tid = (int)arg_x(c, 1);
    if (tid < 0 || tid >= (1 << 20)) rt_fatal("nvn: texel fetch id out of range %d", tid);
    ret_x(c, (uint64_t)tid);
}
// NVNseparateTextureHandle / NVNseparateSamplerHandle nvnDeviceGetSeparate{Texture,Sampler}Handle(const NVNdevice *d,
// int id): the descriptor index; a sampler index sits at bit 20 as in combined handles. One-word structs return in x0.
NVN_API(nvnDeviceGetSeparateTextureHandle) { ret_x(c, (uint64_t)(int64_t)(int)arg_x(c, 1)); }
NVN_API(nvnDeviceGetSeparateSamplerHandle) { ret_x(c, (uint64_t)(int64_t)(int)arg_x(c, 1) << 20); }
// void nvnDeviceInstallDebugCallback(NVNdevice *d, PFNNVNDEBUGCALLBACKPROC cb, void *data, NVNboolean enable)
NVN_API(nvnDeviceInstallDebugCallback) {
    dev_t_ *o = arg_p(c, 0);
    o->dbg_cb = nvn_arg_bool(c, 3) ? arg_p(c, 1) : NULL;
    o->dbg_data = arg_p(c, 2);
}
// void nvnDeviceSetWindowOriginMode(NVNdevice *d, NVNwindowOriginMode mode)
NVN_API(nvnDeviceSetWindowOriginMode) {
    uint32_t m = (uint32_t)arg_x(c, 1);
    ((dev_t_ *)arg_p(c, 0))->origin = m;
    NVN_LOG("window origin %s", m ? "upper left (as D3D and Vulkan)" : "lower left (GL)");
}
// void nvnDeviceSetDepthMode(NVNdevice *d, NVNdepthMode mode)
NVN_API(nvnDeviceSetDepthMode) {
    uint32_t m = (uint32_t)arg_x(c, 1);
    ((dev_t_ *)arg_p(c, 0))->depth_mode = m;
    __atomic_store_n(&depth_minus_w, m == NVN_DEPTH_MODE_NEAR_IS_MINUS_W, __ATOMIC_RELAXED);
    NVN_LOG("depth mode %s", m ? "near is 0 (as D3D and Vulkan)" : "near is -w (GL): vertex shaders map z to (z+w)/2");
}
// void nvnDeviceFinalizeCommandHandle(NVNdevice *d, NVNcommandHandle h)
NVN_API(nvnDeviceFinalizeCommandHandle) { (void)c; }

// ---------- Memory pools
// void nvnMemoryPoolBuilderSetDefaults(NVNmemoryPoolBuilder *b)
NVN_API(nvnMemoryPoolBuilderSetDefaults) { memset(arg_p(c, 0), 0, sizeof(pool_builder_t)); }
// void nvnMemoryPoolBuilderSetDevice(NVNmemoryPoolBuilder *b, NVNdevice *d)
NVN_API(nvnMemoryPoolBuilderSetDevice) { ((pool_builder_t *)arg_p(c, 0))->dev = arg_p(c, 1); }
// void nvnMemoryPoolBuilderSetStorage(NVNmemoryPoolBuilder *b, void *memory, size_t size)
NVN_API(nvnMemoryPoolBuilderSetStorage) {
    pool_builder_t *b = arg_p(c, 0);
    b->mem = arg_p(c, 1);
    b->size = arg_x(c, 2);
}
// void nvnMemoryPoolBuilderSetFlags(NVNmemoryPoolBuilder *b, int flags)
NVN_API(nvnMemoryPoolBuilderSetFlags) { ((pool_builder_t *)arg_p(c, 0))->flags = (uint32_t)arg_x(c, 1); }
// NVNboolean nvnMemoryPoolInitialize(NVNmemoryPool *p, const NVNmemoryPoolBuilder *b)
// Pool setup imports, mirrors and maps memory in the back end (the driver, VirtualAlloc): up to 20 ms on the game's
// loader threads, which held a virtual core through it while job workers waited (TOTK_CORE_DIAG). The thread gives
// the core up, as for any blocking host call.
NVN_API(nvnMemoryPoolInitialize) {
    NVN_BLOCKING();
    pool_t *o = arg_p(c, 0);
    const pool_builder_t *s = arg_p(c, 1);
    memset(o, 0, sizeof *o);
    o->magic = NVN_POOL_MAGIC;
    o->flags = s->flags;
    o->mem = o->game_mem = s->mem;
    o->size = s->size;
    int ok = pool_place(o);
    if (ok && !o->mem) {
        rt_log("nvn: MemoryPoolInitialize: pool without storage, flags %#x size %#llx", o->flags,
               (unsigned long long)o->size);
        ok = 0;
    }
    if (ok && !gfx_backend()->pool_init(o)) {
        rt_log("nvn: MemoryPoolInitialize: the %s back end refused pool %p (flags %#x, size %#llx)", gfx_backend()->name,
               (void *)o->mem, o->flags, (unsigned long long)o->size);
        ok = 0;
    }
    if (ok && pool_gpu_visible(o)) index_add(o);
    if (!ok) {
        if (nvn_gpu_only((uintptr_t)o->mem)) gva_free((uintptr_t)o->mem);
        o->magic = 0;
    }
    NVN_LOG("MemoryPool %p mem %p size %#llx flags %#x storage %p%s", (void *)o, (void *)o->mem,
            (unsigned long long)o->size, o->flags, (void *)o->game_mem, ok ? "" : " failed");
    ret_x(c, ok);
}
// void nvnMemoryPoolFinalize(NVNmemoryPool *p)
NVN_API(nvnMemoryPoolFinalize) {
    NVN_BLOCKING();   // as nvnMemoryPoolInitialize
    pool_t *o = arg_p(c, 0);
    if (o->magic != NVN_POOL_MAGIC) return;
    if (pool_gpu_visible(o)) index_del(o);
    gfx_backend()->pool_fini(o);
    if (nvn_gpu_only((uintptr_t)o->mem)) gva_free((uintptr_t)o->mem);
    o->mem = o->game_mem;
    o->span = 0;
    o->magic = 0;
}
static uint64_t flush_stat[2][3][2];   // nvn_flush_stats

static void flush_note(int kind, const pool_t *p, uint64_t size) {
    uint32_t f = p ? p->flags : 0;
    int k = f & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS ? 2 : f & NVN_MEMORY_POOL_FLAGS_CPU_CACHED ? 0
          : f & NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED ? 1 : 2;
    __atomic_fetch_add(&flush_stat[kind][k][0], 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&flush_stat[kind][k][1], size, __ATOMIC_RELAXED);
}

void nvn_flush_stats(uint64_t out[2][3][2]) {
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 3; k++)
            for (int j = 0; j < 2; j++) out[i][k][j] = __atomic_exchange_n(&flush_stat[i][k][j], 0, __ATOMIC_RELAXED);
}

// void nvnMemoryPoolFlushMappedRange(const NVNmemoryPool *p, ptrdiff_t offset, size_t size): storage is coherent; a
// back end with device-side copies hears of it (nvn_pool_flushed)
NVN_API(nvnMemoryPoolFlushMappedRange) {
    const pool_t *p = arg_p(c, 0);
    int64_t off = (int64_t)arg_x(c, 1);
    uint64_t size = arg_x(c, 2);
    if (rt_trace & RT_TRACE_PERF) flush_note(0, p, size);
    if (p && off >= 0 && (uint64_t)off < p->size) nvn_pool_flushed(p, (uint64_t)off, size < p->size - (uint64_t)off ? size : p->size - (uint64_t)off);
}
// NVNmemoryPoolFlags nvnMemoryPoolGetFlags(const NVNmemoryPool *p)
NVN_API(nvnMemoryPoolGetFlags) { ret_x(c, ((const pool_t *)arg_p(c, 0))->flags); }
// void *nvnMemoryPoolMap(const NVNmemoryPool *p): NULL for CPU_NO_ACCESS
NVN_API(nvnMemoryPoolMap) {
    const pool_t *o = arg_p(c, 0);
    if (o->magic != NVN_POOL_MAGIC || (o->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) { ret_x(c, 0); return; }
    ret_x(c, (uintptr_t)o->mem);
}
// MacNativeGames totk_vpool.c: every request is checked before any mapping changes.
static int check_request(const pool_t *v, const nvn_mapping_t *r, int i) {
    uint64_t end = (uint64_t)r->virtual_offset + r->size;
    if (r->virtual_offset < 0 || r->physical_offset < 0 || !r->size) {
        rt_log("nvn: MapVirtual request %d has a negative or empty range", i);
        return 0;
    }
    if (((uint64_t)r->virtual_offset | (uint64_t)r->physical_offset | r->size) % NVN_VPOOL_PAGE) {
        rt_log("nvn: MapVirtual request %d not 64 KiB aligned (v %#llx p %#llx n %#llx)", i,
               (unsigned long long)r->virtual_offset, (unsigned long long)r->physical_offset, (unsigned long long)r->size);
        return 0;
    }
    if (end < r->size || end > v->size) {
        rt_log("nvn: MapVirtual request %d exceeds the virtual pool (%#llx > %#llx)", i, (unsigned long long)end,
               (unsigned long long)v->size);
        return 0;
    }
    if (!r->physical_pool) return 1;   // unmap
    const pool_t *p = (const pool_t *)(const void *)r->physical_pool;
    if (p->magic != NVN_POOL_MAGIC || !(p->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL)) {
        rt_log("nvn: MapVirtual request %d source %p is not a physical pool", i, (const void *)p);
        return 0;
    }
    uint64_t pend = (uint64_t)r->physical_offset + r->size;
    if (pend < r->size || pend > p->size) {
        rt_log("nvn: MapVirtual request %d exceeds the physical pool (%#llx > %#llx)", i, (unsigned long long)pend,
               (unsigned long long)p->size);
        return 0;
    }
    return 1;
}
// NVNboolean nvnMemoryPoolMapVirtual(NVNmemoryPool *p, int n, const NVNmappingRequest *requests)
NVN_API(nvnMemoryPoolMapVirtual) {
    NVN_BLOCKING();   // as nvnMemoryPoolInitialize
    pool_t *v = arg_p(c, 0);
    int n = (int)arg_x(c, 1);
    const nvn_mapping_t *r = arg_p(c, 2);
    if (v->magic != NVN_POOL_MAGIC || !(v->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) || !v->mem) {
        rt_log("nvn: MapVirtual target %p is not a virtual pool", (void *)v);
        ret_x(c, 0);
        return;
    }
    if (n < 0 || (n && !r)) { rt_log("nvn: MapVirtual with %d requests at %p", n, (const void *)r); ret_x(c, 0); return; }
    for (int i = 0; i < n; i++)
        if (!check_request(v, &r[i], i)) { ret_x(c, 0); return; }
    if (!gfx_backend()->pool_map_virtual(v, n, r))
        rt_fatal("nvn: MapVirtual of %d requests failed after validation; virtual pool %p is inconsistent", n, (void *)v);
    NVN_LOG("MapVirtual pool %p, %d requests", (void *)v->mem, n);
    ret_x(c, 1);
}
// NVNbufferAddress nvnMemoryPoolGetBufferAddress(const NVNmemoryPool *p)
NVN_API(nvnMemoryPoolGetBufferAddress) { ret_x(c, (uintptr_t)((const pool_t *)arg_p(c, 0))->mem); }

// ---------- Buffers: they reference their pool and own no storage
// void nvnBufferBuilderSetDevice(NVNbufferBuilder *b, NVNdevice *d)
NVN_API(nvnBufferBuilderSetDevice) { ((buf_builder_t *)arg_p(c, 0))->dev = arg_p(c, 1); }
// void nvnBufferBuilderSetDefaults(NVNbufferBuilder *b): keeps the device
NVN_API(nvnBufferBuilderSetDefaults) {
    buf_builder_t *b = arg_p(c, 0);
    NVNdevice *d = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = d;
}
// void nvnBufferBuilderSetStorage(NVNbufferBuilder *b, NVNmemoryPool *p, ptrdiff_t offset, size_t size)
NVN_API(nvnBufferBuilderSetStorage) {
    buf_builder_t *b = arg_p(c, 0);
    b->pool = arg_p(c, 1);
    b->off = (int64_t)arg_x(c, 2);
    b->size = arg_x(c, 3);
}
// NVNboolean nvnBufferInitialize(NVNbuffer *b, const NVNbufferBuilder *builder)
NVN_API(nvnBufferInitialize) {
    buf_t *o = arg_p(c, 0);
    const buf_builder_t *s = arg_p(c, 1);
    if (!s->pool || s->pool->magic != NVN_POOL_MAGIC || s->off < 0 || (uint64_t)s->off > s->pool->size ||
        s->size > s->pool->size - (uint64_t)s->off) {
        ret_x(c, 0);
        return;
    }
    *o = (buf_t){BUF_MAGIC, s->pool, s->off, s->size};
    ret_x(c, 1);
}
// void nvnBufferFinalize(NVNbuffer *b)
NVN_API(nvnBufferFinalize) { memset(arg_p(c, 0), 0, sizeof(buf_t)); }
static uint64_t buf_address(const buf_t *o) { return o->pool && o->pool->mem ? (uintptr_t)o->pool->mem + (uint64_t)o->off : 0; }
// void *nvnBufferMap(const NVNbuffer *b): NULL in a GPU-window pool, which has no CPU view
NVN_API(nvnBufferMap) {
    uint64_t a = buf_address(arg_p(c, 0));
    ret_x(c, nvn_gpu_only(a) ? 0 : a);
}
// NVNbufferAddress nvnBufferGetAddress(const NVNbuffer *b)
NVN_API(nvnBufferGetAddress) { ret_x(c, buf_address(arg_p(c, 0))); }
// NVNmemoryPool *nvnBufferGetMemoryPool(const NVNbuffer *b)
NVN_API(nvnBufferGetMemoryPool) { ret_x(c, (uintptr_t)((const buf_t *)arg_p(c, 0))->pool); }
// ptrdiff_t nvnBufferGetMemoryOffset(const NVNbuffer *b)
NVN_API(nvnBufferGetMemoryOffset) { ret_x(c, (uint64_t)((const buf_t *)arg_p(c, 0))->off); }
// size_t nvnBufferGetSize(const NVNbuffer *b)
NVN_API(nvnBufferGetSize) { ret_x(c, ((const buf_t *)arg_p(c, 0))->size); }
// void nvnBufferFlushMappedRange(const NVNbuffer *b, ptrdiff_t offset, size_t size): the back end re-reads the range
NVN_API(nvnBufferFlushMappedRange) {
    const buf_t *o = arg_p(c, 0);
    int64_t off = (int64_t)arg_x(c, 1);
    uint64_t size = arg_x(c, 2);
    if (off < 0 || (uint64_t)off > o->size || size > o->size - (uint64_t)off)
        rt_fatal("nvn: BufferFlushMappedRange %#llx+%#llx outside the buffer (%#llx bytes)", (unsigned long long)off,
                 (unsigned long long)size, (unsigned long long)o->size);
    if (rt_trace & RT_TRACE_PERF) flush_note(1, o->pool, size);
    uint64_t a = buf_address(o);
    if (a && !nvn_gpu_only(a)) nvn_guest_written(a + (uint64_t)off, size);
}

// ---------- Sync and events
// A sync is a fence value: fenced = 1 after QueueFenceSync (the newest submit), 3 after WindowAcquireTexture (fence 0).
// NVNboolean nvnSyncInitialize(NVNsync *s, NVNdevice *d)
NVN_API(nvnSyncInitialize) {
    *(sync_t *)arg_p(c, 0) = (sync_t){SYNC_MAGIC, 0, 0};
    ret_x(c, 1);
}
// void nvnSyncFinalize(NVNsync *s)
NVN_API(nvnSyncFinalize) { ((sync_t *)arg_p(c, 0))->magic = 0; }
// void nvnQueueFenceSync(NVNqueue *q, NVNsync *s, NVNsyncCondition condition, int flags): "every command submitted to
// the queue so far has completed"
NVN_API(nvnQueueFenceSync) {
    sync_t *s = arg_p(c, 1);
    s->fenced = 1;
    s->fence = nvn_fence_last();
}
// NVNsyncWaitResult nvnSyncWait(const NVNsync *s, uint64_t timeout_ns): a sync never fenced reports a timeout at once;
// a fenced one is released only after the back end completed that fence value
NVN_API(nvnSyncWait) {
    const sync_t *s = arg_p(c, 0);
    uint64_t timeout = arg_x(c, 1);
    if (!s->fenced) { ret_x(c, NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED); return; }
    const GfxBackend *b = gfx_backend();
    if (b->fence_done() >= s->fence) { b->cpu_synced(s->fence); ret_x(c, NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED); return; }
    int ok;
    {
        NVN_BLOCKING();
        ok = b->fence_wait(s->fence, timeout);
    }
    if (ok) b->cpu_synced(s->fence);
    ret_x(c, ok ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
}
// void nvnEventBuilderSetDefaults(NVNeventBuilder *b)
NVN_API(nvnEventBuilderSetDefaults) { memset(arg_p(c, 0), 0, sizeof(ev_builder_t)); }
// void nvnEventBuilderSetStorage(NVNeventBuilder *b, const NVNmemoryPool *p, int64_t offset)
NVN_API(nvnEventBuilderSetStorage) { *(ev_builder_t *)arg_p(c, 0) = (ev_builder_t){arg_p(c, 1), (int64_t)arg_x(c, 2)}; }
// NVNboolean nvnEventInitialize(NVNevent *e, const NVNeventBuilder *b): the value lives in pool storage
NVN_API(nvnEventInitialize) {
    const ev_builder_t *s = arg_p(c, 1);
    if (!s->pool || !s->pool->mem || nvn_gpu_only((uintptr_t)s->pool->mem)) {
        rt_log("nvn: EventInitialize without CPU-visible storage (pool %p)", (const void *)s->pool);
        ret_x(c, 0);
        return;
    }
    *(ev_t *)arg_p(c, 0) = (ev_t){EVENT_MAGIC, (volatile uint32_t *)(s->pool->mem + s->off)};
    ret_x(c, 1);
}
// void nvnEventFinalize(NVNevent *e)
NVN_API(nvnEventFinalize) { ((ev_t *)arg_p(c, 0))->magic = 0; }
// uint32_t nvnEventGetValue(const NVNevent *e)
NVN_API(nvnEventGetValue) { ret_x(c, *((const ev_t *)arg_p(c, 0))->val); }
// void nvnEventSignal(NVNevent *e, NVNeventSignalMode mode, uint32_t value)
NVN_API(nvnEventSignal) {
    const ev_t *e = arg_p(c, 0);
    uint32_t v = (uint32_t)arg_x(c, 2);
    if ((uint32_t)arg_x(c, 1) == NVN_EVENT_SIGNAL_MODE_ADD) *e->val += v;
    else *e->val = v;
}

// ---------- Queue
// void nvnQueueBuilderSetDefaults(NVNqueueBuilder *b): keeps the device; the device's default memory sizes
NVN_API(nvnQueueBuilderSetDefaults) {
    queue_builder_t *b = arg_p(c, 0);
    NVNdevice *d = b->dev;
    memset(b, 0, sizeof *b);
    b->dev = d;
    b->cmd_sz = 65536;
    b->comp_sz = 262144;
    b->ctl_sz = 16384;
    b->flush = 4096;
}
// void nvnQueueBuilderSetDevice(NVNqueueBuilder *b, NVNdevice *d)
NVN_API(nvnQueueBuilderSetDevice) { ((queue_builder_t *)arg_p(c, 0))->dev = arg_p(c, 1); }
// void nvnQueueBuilderSetFlags(NVNqueueBuilder *b, int flags)
NVN_API(nvnQueueBuilderSetFlags) { ((queue_builder_t *)arg_p(c, 0))->flags = (uint32_t)arg_x(c, 1); }
// void nvnQueueBuilderSet{Command,Compute,Control}MemorySize / SetCommandFlushThreshold(NVNqueueBuilder *b, size_t n)
NVN_API(nvnQueueBuilderSetCommandMemorySize) { ((queue_builder_t *)arg_p(c, 0))->cmd_sz = arg_x(c, 1); }
NVN_API(nvnQueueBuilderSetComputeMemorySize) { ((queue_builder_t *)arg_p(c, 0))->comp_sz = arg_x(c, 1); }
NVN_API(nvnQueueBuilderSetControlMemorySize) { ((queue_builder_t *)arg_p(c, 0))->ctl_sz = arg_x(c, 1); }
NVN_API(nvnQueueBuilderSetCommandFlushThreshold) { ((queue_builder_t *)arg_p(c, 0))->flush = arg_x(c, 1); }
// size_t nvnQueueBuilderGetQueueMemorySize(const NVNqueueBuilder *b): command + compute + control rounded to 4 KiB (the
// host does not use it; the game only allocates it)
NVN_API(nvnQueueBuilderGetQueueMemorySize) {
    const queue_builder_t *b = arg_p(c, 0);
    ret_x(c, (b->cmd_sz + b->comp_sz + b->ctl_sz + 4095) & ~(uint64_t)4095);
}
// void nvnQueueBuilderSetQueueMemory(NVNqueueBuilder *b, void *memory, size_t size)
NVN_API(nvnQueueBuilderSetQueueMemory) {
    queue_builder_t *b = arg_p(c, 0);
    b->mem = arg_p(c, 1);
    b->mem_sz = arg_x(c, 2);
}
// NVNboolean nvnQueueInitialize(NVNqueue *q, const NVNqueueBuilder *b)
NVN_API(nvnQueueInitialize) {
    queue_t *q = arg_p(c, 0);
    *q = (queue_t){QUEUE_MAGIC, ((const queue_builder_t *)arg_p(c, 1))->dev, 0};
    NVN_LOG("QueueInitialize %p", (void *)q);
    ret_x(c, 1);
}
// void nvnQueueFinalize(NVNqueue *q)
NVN_API(nvnQueueFinalize) { ((queue_t *)arg_p(c, 0))->magic = 0; }
// NVNqueueGetErrorResult nvnQueueGetError(NVNqueue *q, NVNqueueErrorInfo *info)
NVN_API(nvnQueueGetError) {
    NVNqueueErrorInfo *info = arg_p(c, 1);
    if (info) memset(info, 0, sizeof *info);
    ret_x(c, NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR);
}
// void nvnQueueFlush(NVNqueue *q): submits are already handed to the back end
NVN_API(nvnQueueFlush) { (void)c; }
// void nvnQueueFinish(NVNqueue *q)
NVN_API(nvnQueueFinish) {
    {
        NVN_BLOCKING();
        gfx_backend()->finish();
    }
    gfx_backend()->cpu_synced(gfx_backend()->fence_done());
}
// ---------- Submit latency guard for TotK 1.2.1's per-object uniform double buffer
// NVN starts the GPU when QueueSubmitCommands returns, within microseconds; here the replay into Vulkan takes 15-30 ms
// first, and the GPU reads uniform memory only after it. The game's producer f_70008c6460 (thread 0x103e, p56)
// rewrites the half of its per-object uniform blocks that was front until the last flip, 14-23 ms after the submit
// that still reads it, with no fence: on hardware the GPU has read that half by then. Here the rewrite lands inside
// the replay, and that frame draws other objects' matrices (phantom rocks, flat far field).
// The guard makes the producer wait, at its commit point, until the newest submit has been replayed and completed.
// lod_entered / lod_issued count submits that entered QueueSubmitCommands / reached the back end; lod_fence holds
// the fence of the last LOD_RING. TOTK_LOD_GUARD=0 turns the wait off.
#define LOD_RING 64
static uint64_t lod_entered, lod_issued, lod_fence[LOD_RING];

// void nvnQueueSubmitCommands(NVNqueue *q, int n, const NVNcommandHandle *handles): replayed on this thread with its
// core given up (section 2.2), or queued for the replay thread (TOTK_REPLAY_ASYNC, vk_backend.c); the newest fence
// value is what QueueFenceSync records. With the queue the fence covers replay and GPU, so the LOD guard below waits
// for both.
NVN_API(nvnQueueSubmitCommands) {
    queue_t *q = arg_p(c, 0);
    int n = (int)arg_x(c, 1);
    const uint64_t *h = arg_p(c, 2);
    if (n < 0 || (n && !h)) rt_fatal("nvn: QueueSubmitCommands with %d handles at %p", n, (const void *)h);
    NVN_BLOCKING();
    uint64_t no = __atomic_add_fetch(&lod_entered, 1, __ATOMIC_SEQ_CST);
    q->submits++;
    uint64_t f;
    if (gfx_replay_async()) {
        // Prepare and queue under one lock: submit numbers follow queue order (gfx_replayed_submits).
        static PlatMutex submit_lock = PLAT_MUTEX_INIT;
        uint64_t local[32], *hs = n <= 32 ? local : malloc(sizeof *hs * (size_t)n);
        if (!hs) rt_fatal("nvn: out of memory (submit of %d handles)", n);
        plat_mutex_lock(&submit_lock);
        nvn_submit_prepare_async(h, hs, n);
        f = gfx_backend()->submit(hs, n);
        plat_mutex_unlock(&submit_lock);
        if (hs != local) free(hs);
    } else {
        nvn_submit_prepare(h, n);
        f = gfx_backend()->submit(h, n);
    }
    uint64_t prev = __atomic_load_n(&last_fence, __ATOMIC_RELAXED);
    while (f > prev && !__atomic_compare_exchange_n(&last_fence, &prev, f, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {}
    __atomic_store_n(&lod_fence[no % LOD_RING], f, __ATOMIC_RELAXED);
    __atomic_store_n(&lod_issued, no, __ATOMIC_SEQ_CST);
}

// The producer (1.2.1 main f_70008c6460) releases the read lock on obj+0x538 at lr 0x70008c70c8; when
// [obj+0x529] == [obj+0x528] (x19 = obj) it goes on to rewrite the back half. Waits off the core, at most ~400 ms.
static void obs_release_read(Cpu *c) {
    uint64_t lr = c->x[30], obj = c->x[19];
    rt_original(c);
    if (lr != 0x70008c70c8ull) return;
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("TOTK_LOD_GUARD");
    }
    if (!on || *(volatile uint8_t *)(uintptr_t)(obj + 0x528) != *(volatile uint8_t *)(uintptr_t)(obj + 0x529)) return;
    uint64_t e = __atomic_load_n(&lod_entered, __ATOMIC_SEQ_CST);
    if (!e) return;
    NVN_BLOCKING();
    for (int i = 0; i < 200 && __atomic_load_n(&lod_issued, __ATOMIC_SEQ_CST) < e; i++) plat_sleep_ms(1);
    uint64_t f = __atomic_load_n(&lod_fence[e % LOD_RING], __ATOMIC_RELAXED);
    if (gfx_backend()->fence_done() < f) gfx_backend()->fence_wait(f, 200000000ull);
}
HOST_OBSERVER("_ZN2nn2os15ReleaseReadLockEPNS0_20ReaderWriterLockTypeE", obs_release_read);
