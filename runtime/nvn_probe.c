// totk.exe --probe-nvn: the NVN front end and the null back end, called the way the
// game calls them: through the real nvnp_* procs with a synthetic Cpu (AAPCS64 arguments in x0..x7 and v0..v7), on the
// guest main thread, on the null back end whatever TOTK_GPU says. Checks, in order:
//   - pool, buffer, texture, sampler, sync and event objects against MacNativeGames' semantics (hle_nvn.c), the
//     texture storage sizes against the original-SDK goldens of MNG tests/texture_auto_layout/probe.c;
//   - every op kind recorded through the CommandBuffer procs and parsed back with nvn_op_first/next/payload, and
//     CallCommands inlining a recording;
//   - the null back end's CPU-visible side effects (event write and add, counter report, buffer copy, uniform update,
//     in stream order across handles) landing in guest memory at submit, and not before;
//   - the proc table: every name of nvn_procs.inc resolves to its own synthetic address, an unknown name to NULL;
//   - named fatal stubs stop with their name, each in a child process (this executable again, TOTK_PROBE_NVN_CHILD).
// The 40 layout goldens themselves are run by --probe-nvn-tex against nvn_tex.c; here four of them go through the procs.
// Ported in part from MacNativeGames games/totk/tests/texture_auto_layout/probe.c @ b75be8cf (MIT, Copyright (c) 2026
// jhaizhou; origin d2r-mobile): the golden metadata, which holds no game bytes.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"

#define CHILD_ENV "TOTK_PROBE_NVN_CHILD"

#define NVN_PROC(n) void nvnp_##n(Cpu *c);
#include "nvn_procs.inc"
#undef NVN_PROC

typedef struct { const char *name; void (*fn)(Cpu *); } ProcRow;
static const ProcRow procs[] = {
#define NVN_PROC(n) {#n, nvnp_##n},
#include "nvn_procs.inc"
#undef NVN_PROC
};
#define NPROCS ((int)(sizeof procs / sizeof *procs))

// The named fatal stubs checked in child processes: proc names that no task implements (NvRm interop of the NVX
// extension), so the check does not go stale when the stub list shrinks.
static const char *const fatal_procs[] = {"nvnSyncGetNvRmSyncNVX", "nvnTextureGetNvRmSurfaceNVX"};
#define NFATAL ((int)(sizeof fatal_procs / sizeof *fatal_procs))

static int failures, checks, n_objects, n_ops, n_effects;
#define CHECK(cond, ...) \
    do { checks++; if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

// ---- calling a proc
static Cpu cpu;
static uint64_t call(void (*f)(Cpu *), const uint64_t a[8]) {
    memcpy(cpu.x, a, 8 * sizeof *a);
    f(&cpu);
    return cpu.x[0];
}
#define CALL(n, ...) call(nvnp_##n, (const uint64_t[8]){__VA_ARGS__})
#define U(p) ((uint64_t)(uintptr_t)(p))
#define I(x) ((uint64_t)(int64_t)(x))
static void fv(int i, float f) {   // a float argument: low 32 bits of vi
    uint32_t b;
    memcpy(&b, &f, 4);
    cpu.v[i] = b;
}

// ---- the objects, in place like the game's (each type is its NVN reserved size)
#define OBJ(T, n) static T n __attribute__((aligned(16)))
OBJ(NVNdevice, dev);
OBJ(NVNdeviceBuilder, devb);
OBJ(NVNmemoryPool, pool);        // CPU-visible, game storage
OBJ(NVNmemoryPool, gpool);       // physical, CPU_NO_ACCESS: GPU window
OBJ(NVNmemoryPool, vpool);       // virtual
OBJ(NVNmemoryPoolBuilder, poolb);
OBJ(NVNqueue, queue);
OBJ(NVNqueueBuilder, queueb);
static uint8_t *pmem, *gmem;
static const uint64_t PROBE_POOL_SIZE = 0x40000, PROBE_GPOOL_SIZE = 0x20000;

static pool_t *P(void) { return (pool_t *)(void *)&pool; }
static pool_t *G(void) { return (pool_t *)(void *)&gpool; }

// ---- pools
static uint64_t make_pool(NVNmemoryPool *p, void *storage, uint64_t size, uint32_t flags) {
    CALL(nvnMemoryPoolBuilderSetDefaults, U(&poolb));
    CALL(nvnMemoryPoolBuilderSetDevice, U(&poolb), U(&dev));
    CALL(nvnMemoryPoolBuilderSetStorage, U(&poolb), U(storage), size);
    CALL(nvnMemoryPoolBuilderSetFlags, U(&poolb), flags);
    return (uint8_t)CALL(nvnMemoryPoolInitialize, U(p), U(&poolb));
}

static void probe_device_and_pools(void) {
    CALL(nvnDeviceBuilderSetDefaults, U(&devb));
    CHECK((uint8_t)CALL(nvnDeviceInitialize, U(&dev), U(&devb)) == 1, "DeviceInitialize");
    int abi[2] = {0, 0};
    CALL(nvnDeviceGetInteger, U(&dev), 0, U(&abi[0]));
    CALL(nvnDeviceGetInteger, U(&dev), 1, U(&abi[1]));
    CHECK(abi[0] == 55 && abi[1] >= 15, "device ABI %d.%d", abi[0], abi[1]);

    pmem = mem_kernel_alloc(PROBE_POOL_SIZE, 0x1000);
    gmem = mem_kernel_alloc(PROBE_GPOOL_SIZE, 0x1000);
    CHECK((uint8_t)make_pool(&pool, pmem, PROBE_POOL_SIZE, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED) == 1,
          "CPU-visible pool");
    CHECK(P()->magic == NVN_POOL_MAGIC && P()->mem == pmem && P()->size == PROBE_POOL_SIZE, "pool fields");
    CHECK(CALL(nvnMemoryPoolGetFlags, U(&pool)) == (NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED),
          "pool flags");
    CHECK(CALL(nvnMemoryPoolMap, U(&pool)) == U(pmem) && CALL(nvnMemoryPoolGetBufferAddress, U(&pool)) == U(pmem),
          "pool map and address are the guest storage");
    CHECK(CALL(nvnMemoryPoolGetSize, U(&pool)) == PROBE_POOL_SIZE, "pool size");
    CHECK(nvn_pool_find(U(pmem) + 0x100, 0x200) == P() && !nvn_pool_find(U(pmem) + PROBE_POOL_SIZE - 8, 16), "pool index");
    n_objects += 6;

    // refused: no storage, PHYSICAL | VIRTUAL, a size that is not a 64 KiB multiple (MNG totk_vpool.c)
    static NVNmemoryPool bad;
    CHECK(make_pool(&bad, NULL, 0x10000, NVN_MEMORY_POOL_FLAGS_CPU_CACHED) == 0, "a pool without storage is refused");
    CHECK(make_pool(&bad, gmem, PROBE_GPOOL_SIZE, NVN_MEMORY_POOL_FLAGS_PHYSICAL | NVN_MEMORY_POOL_FLAGS_VIRTUAL) == 0,
          "PHYSICAL | VIRTUAL is refused");
    CHECK(make_pool(&bad, gmem, 0x1800, NVN_MEMORY_POOL_FLAGS_PHYSICAL | NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) == 0,
          "a physical pool of 0x1800 bytes is refused");
    n_objects += 3;

    // a CPU-invisible physical pool lives in the GPU window, has no CPU view, and is not in the host address space
    CHECK((uint8_t)make_pool(&gpool, gmem, PROBE_GPOOL_SIZE, NVN_MEMORY_POOL_FLAGS_PHYSICAL | NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS |
                                                       NVN_MEMORY_POOL_FLAGS_GPU_CACHED) == 1, "physical pool");
    CHECK(nvn_gpu_only(U(G()->mem)) && G()->game_mem == gmem && G()->span == PROBE_GPOOL_SIZE, "physical pool placed in the GPU window");
    CHECK(CALL(nvnMemoryPoolMap, U(&gpool)) == 0, "a CPU_NO_ACCESS pool has no CPU view");
    CHECK(CALL(nvnMemoryPoolGetBufferAddress, U(&gpool)) == U(G()->mem), "GPU address of the physical pool");

    // a virtual pool: requests are validated before any mapping changes
    CHECK((uint8_t)make_pool(&vpool, NULL, PROBE_POOL_SIZE, NVN_MEMORY_POOL_FLAGS_VIRTUAL | NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) == 1,
          "virtual pool");
    nvn_mapping_t map[2] = {{(const NVNmemoryPool *)&gpool, 0, 0x10000, 0x10000}, {NULL, 0, 0x10000, 0x10000}};
    CHECK((uint8_t)CALL(nvnMemoryPoolMapVirtual, U(&vpool), 1, U(map)) == 1, "map 64 KiB into the virtual pool");
    CHECK((uint8_t)CALL(nvnMemoryPoolMapVirtual, U(&vpool), 2, U(map)) == 1, "map then unmap");
    nvn_mapping_t bad_map[3] = {{(const NVNmemoryPool *)&gpool, 0, 0x100, 0x10000},          // unaligned
                                {(const NVNmemoryPool *)&gpool, 0, 0x30000, 0x20000},        // past the virtual pool
                                {(const NVNmemoryPool *)&pool, 0, 0, 0x10000}};              // source is not physical
    for (int i = 0; i < 3; i++)
        CHECK((uint8_t)CALL(nvnMemoryPoolMapVirtual, U(&vpool), 1, U(&bad_map[i])) == 0, "invalid mapping request %d accepted", i);
    n_objects += 9;

    // Finalize returns a GPU window range, so the pool can be initialized again
    uint64_t gva = U(((pool_t *)(void *)&vpool)->mem);
    CALL(nvnMemoryPoolFinalize, U(&vpool));
    CHECK(((pool_t *)(void *)&vpool)->magic == 0 && !nvn_gpu_only(U(((pool_t *)(void *)&vpool)->mem)), "virtual pool finalized");
    CHECK((uint8_t)make_pool(&vpool, NULL, PROBE_POOL_SIZE, NVN_MEMORY_POOL_FLAGS_VIRTUAL | NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) == 1 &&
              U(((pool_t *)(void *)&vpool)->mem) == gva, "the freed window range is reused");
    CALL(nvnMemoryPoolFinalize, U(&vpool));
    n_objects += 2;
}

// ---- buffers
OBJ(NVNbufferBuilder, bufb);
OBJ(NVNbuffer, buf);
OBJ(NVNbuffer, gbuf);

static void probe_buffers(void) {
    CALL(nvnBufferBuilderSetDevice, U(&bufb), U(&dev));
    CALL(nvnBufferBuilderSetDefaults, U(&bufb));
    CALL(nvnBufferBuilderSetStorage, U(&bufb), U(&pool), 0x100, 0x200);
    CHECK((uint8_t)CALL(nvnBufferInitialize, U(&buf), U(&bufb)) == 1, "BufferInitialize");
    CHECK(CALL(nvnBufferGetAddress, U(&buf)) == U(pmem) + 0x100 && CALL(nvnBufferMap, U(&buf)) == U(pmem) + 0x100,
          "buffer address and map are pool storage + offset (MNG hle_nvn.c:237, 248)");
    CHECK(CALL(nvnBufferGetSize, U(&buf)) == 0x200 && CALL(nvnBufferGetMemoryOffset, U(&buf)) == 0x100 &&
              CALL(nvnBufferGetMemoryPool, U(&buf)) == U(&pool), "buffer size, offset and pool");
    CALL(nvnBufferFlushMappedRange, U(&buf), 0, 0x200);

    CALL(nvnBufferBuilderSetStorage, U(&bufb), U(&pool), I(PROBE_POOL_SIZE - 0x100), 0x200);
    CHECK((uint8_t)CALL(nvnBufferInitialize, U(&buf), U(&bufb)) == 0, "a buffer past the end of its pool is refused");
    CALL(nvnBufferBuilderSetStorage, U(&bufb), U(&pool), 0x100, 0x200);
    CALL(nvnBufferInitialize, U(&buf), U(&bufb));
    CALL(nvnBufferFinalize, U(&buf));
    CHECK(((const uint32_t *)(const void *)&buf)[0] == 0, "BufferFinalize clears the object");

    CALL(nvnBufferBuilderSetStorage, U(&bufb), U(&gpool), 0x1000, 0x400);
    CHECK((uint8_t)CALL(nvnBufferInitialize, U(&gbuf), U(&bufb)) == 1, "buffer in a GPU-window pool");
    CHECK(CALL(nvnBufferMap, U(&gbuf)) == 0 && CALL(nvnBufferGetAddress, U(&gbuf)) == U(G()->mem) + 0x1000,
          "a GPU-window buffer has no CPU view and a window address");
    n_objects += 7;
}

// ---- textures and views
OBJ(NVNtextureBuilder, texb);
OBJ(NVNtexture, tex1);
OBJ(NVNtexture, tex2);
OBJ(NVNtexture, tex3);
OBJ(NVNtextureView, view);

static void set_tex(int fmt, int w, int h, int levels, uint64_t off) {
    CALL(nvnTextureBuilderSetDevice, U(&texb), U(&dev));
    CALL(nvnTextureBuilderSetDefaults, U(&texb));
    CALL(nvnTextureBuilderSetTarget, U(&texb), NVN_TEXTURE_TARGET_2D);
    CALL(nvnTextureBuilderSetSize2D, U(&texb), I(w), I(h));
    CALL(nvnTextureBuilderSetLevels, U(&texb), I(levels));
    CALL(nvnTextureBuilderSetFormat, U(&texb), I(fmt));
    CALL(nvnTextureBuilderSetStorage, U(&texb), U(&pool), off);
}

// Four of the 40 original-SDK goldens of MNG texture_auto_layout/probe.c: {format, width, height, levels, size}
static const struct { int format, width, height, levels; uint64_t size; } layout_goldens[] = {
    {0x25, 40, 9, 6, 6144}, {0x25, 40, 128, 8, 37888}, {0x48, 160, 160, 8, 57344}, {0x48, 160, 516, 10, 222208}};

static void probe_textures(void) {
    for (size_t i = 0; i < sizeof layout_goldens / sizeof *layout_goldens; i++) {
        set_tex(layout_goldens[i].format, layout_goldens[i].width, layout_goldens[i].height, layout_goldens[i].levels, 0);
        uint64_t size = CALL(nvnTextureBuilderGetStorageSize, U(&texb));
        CHECK(size == layout_goldens[i].size, "storage size of %#x %dx%d: %llu, expected %llu", layout_goldens[i].format,
              layout_goldens[i].width, layout_goldens[i].height, (unsigned long long)size,
              (unsigned long long)layout_goldens[i].size);
        CHECK(CALL(nvnTextureBuilderGetStorageAlignment, U(&texb)) == 512, "storage alignment");
        n_objects += 2;
    }

    set_tex(NVN_FORMAT_RGBA8, 40, 128, 8, 0x1000);
    CHECK((uint8_t)CALL(nvnTextureInitialize, U(&tex1), U(&texb)) == 1, "TextureInitialize");
    const tex_t *t1 = (const tex_t *)(const void *)&tex1;
    CHECK(t1->magic == NVN_TEX_MAGIC && t1->serial && t1->mem == pmem + 0x1000 && t1->L.size == 37888, "texture fields");
    CHECK(CALL(nvnTextureGetWidth, U(&tex1)) == 40 && CALL(nvnTextureGetHeight, U(&tex1)) == 128 &&
              CALL(nvnTextureGetDepth, U(&tex1)) == 1 && CALL(nvnTextureGetStorageSize, U(&tex1)) == 37888, "texture queries");
    CHECK(CALL(nvnTextureGetStorageClass, U(&tex1)) == CALL(nvnTextureBuilderGetStorageClass, U(&texb)), "storage class");
    tex_t live;
    CHECK(nvn_tex_live_at(pmem + 0x1000, &live) && live.serial == t1->serial, "the live texture table resolves the storage");

    // a second texture and a third, in the other pools of the layout
    set_tex(NVN_FORMAT_RGBA_DXT5_SRGB, 160, 160, 8, 0x10000);
    CHECK((uint8_t)CALL(nvnTextureInitialize, U(&tex2), U(&texb)) == 1, "BC3 texture");
    CHECK(CALL(nvnTextureGetStorageSize, U(&tex2)) == 57344, "BC3 160x160 storage size");
    CHECK(CALL(nvnTextureCompare, U(&tex1), U(&tex1)) == 1 && CALL(nvnTextureCompare, U(&tex1), U(&tex2)) == 0, "TextureCompare");

    // packaged texture data is copied into the pool storage at Initialize
    set_tex(NVN_FORMAT_RGBA8, 4, 4, 1, 0x20000);
    uint64_t need = CALL(nvnTextureBuilderGetStorageSize, U(&texb));
    static uint8_t pkg[4096];
    CHECK(need > 0 && need <= sizeof pkg, "packaged texture size %llu", (unsigned long long)need);
    for (size_t i = 0; i < sizeof pkg; i++) pkg[i] = (uint8_t)(i * 7 + 3);
    CALL(nvnTextureBuilderSetPackagedTextureData, U(&texb), U(pkg));
    CHECK(CALL(nvnTextureBuilderGetPackagedTextureData, U(&texb)) == U(pkg), "packaged data pointer");
    CHECK((uint8_t)CALL(nvnTextureInitialize, U(&tex3), U(&texb)) == 1, "packaged texture");
    CHECK(!memcmp(pmem + 0x20000, pkg, (size_t)need), "packaged data copied into the pool storage");

    // views: levels and layers, and the byte offset of a level (hand-derived in nvn_tex_probe.c)
    CALL(nvnTextureViewSetDefaults, U(&view));
    CALL(nvnTextureViewSetLevels, U(&view), 1, 3);
    CALL(nvnTextureViewSetLayers, U(&view), 2, 4);
    int layers[2] = {0, 0};
    CHECK((uint8_t)CALL(nvnTextureViewGetLayers, U(&view), U(&layers[0]), U(&layers[1])) == 1 && layers[0] == 2 && layers[1] == 4,
          "view layers");
    CALL(nvnTextureViewSetLayers, U(&view), 0, 1);
    CHECK(CALL(nvnTextureGetViewOffset, U(&tex1), U(&view)) == 24576, "offset of level 1 of the RGBA8 40x128 texture");
    CHECK(CALL(nvnTextureGetViewOffset, U(&tex1), 0) == 0, "no view, no offset");
    // depth-stencil mode: unset reads DEPTH and false; STENCIL is kept, read back and compared
    OBJ(NVNtextureView, sview);
    int dsm = -1;
    CALL(nvnTextureViewSetDefaults, U(&sview));
    CHECK((uint8_t)CALL(nvnTextureViewGetDepthStencilMode, U(&sview), U(&dsm)) == 0 && dsm == NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH,
          "default view depth-stencil mode");
    CHECK((uint8_t)CALL(nvnTextureViewCompare, U(&sview), U(&view)) == 0, "views with other levels compare unequal");
    OBJ(NVNtextureView, sview2);
    CALL(nvnTextureViewSetDefaults, U(&sview2));
    CALL(nvnTextureViewSetDepthStencilMode, U(&sview), NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL);
    CHECK((uint8_t)CALL(nvnTextureViewGetDepthStencilMode, U(&sview), U(&dsm)) == 1 && dsm == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL,
          "view depth-stencil mode STENCIL");
    CHECK((uint8_t)CALL(nvnTextureViewCompare, U(&sview), U(&sview2)) == 0, "a STENCIL view differs from a default one");
    CALL(nvnTextureViewSetDepthStencilMode, U(&sview2), NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL);
    CHECK((uint8_t)CALL(nvnTextureViewCompare, U(&sview), U(&sview2)) == 1, "two STENCIL views compare equal");
    CHECK(((const view_t *)(const void *)&sview)->set_dsmode && ((const view_t *)(const void *)&sview)->dsmode == 1 &&
              !((const view_t *)(const void *)&sview)->set_levels, "STENCIL view bits");

    // descriptor pools
    OBJ(NVNtexturePool, tp);
    CHECK((uint8_t)CALL(nvnTexturePoolInitialize, U(&tp), U(&pool), 0x30000, 16) == 1, "texture pool");
    CALL(nvnTexturePoolRegisterTexture, U(&tp), 3, U(&tex1), 0);

    CALL(nvnTextureFinalize, U(&tex3));
    CHECK(((const tex_t *)(const void *)&tex3)->magic == 0 && !nvn_tex_live_at(pmem + 0x20000, &live), "TextureFinalize");
    n_objects += 15;
}

// ---- samplers
OBJ(NVNsamplerBuilder, sampb);
OBJ(NVNsampler, samp);

static void probe_samplers(void) {
    CALL(nvnSamplerBuilderSetDevice, U(&sampb), U(&dev));
    CALL(nvnSamplerBuilderSetDefaults, U(&sampb));
    uint32_t mn = 99, mg = 99, ws = 99, wt = 99, wr = 99;
    CALL(nvnSamplerBuilderGetMinMagFilter, U(&sampb), U(&mn), U(&mg));
    CALL(nvnSamplerBuilderGetWrapMode, U(&sampb), U(&ws), U(&wt), U(&wr));
    CHECK(mn == NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR && mg == NVN_MAG_FILTER_LINEAR && ws == NVN_WRAP_MODE_REPEAT &&
              wt == NVN_WRAP_MODE_REPEAT && wr == NVN_WRAP_MODE_REPEAT, "sampler defaults are the GL ones");

    CALL(nvnSamplerBuilderSetMinMagFilter, U(&sampb), NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR, NVN_MAG_FILTER_NEAREST);
    CALL(nvnSamplerBuilderSetWrapMode, U(&sampb), NVN_WRAP_MODE_CLAMP, NVN_WRAP_MODE_MIRROR_CLAMP, NVN_WRAP_MODE_REPEAT);
    fv(0, 0.5f);
    fv(1, 7.f);
    CALL(nvnSamplerBuilderSetLodClamp, U(&sampb));
    fv(0, -1.5f);
    CALL(nvnSamplerBuilderSetLodBias, U(&sampb));
    fv(0, 4.f);
    CALL(nvnSamplerBuilderSetMaxAnisotropy, U(&sampb));
    float border[4] = {0.25f, 0.5f, 0.75f, 1.f};
    CALL(nvnSamplerBuilderSetBorderColor, U(&sampb), U(border));
    CALL(nvnSamplerBuilderSetReductionFilter, U(&sampb), 1);
    CHECK((uint8_t)CALL(nvnSamplerInitialize, U(&samp), U(&sampb)) == 1, "SamplerInitialize");
    const samp_t *s = (const samp_t *)(const void *)&samp;
    CHECK(s->magic == NVN_SAMP_MAGIC && s->p.minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR && s->p.magf == NVN_MAG_FILTER_NEAREST &&
              s->p.ws == NVN_WRAP_MODE_CLAMP && s->p.wt == NVN_WRAP_MODE_MIRROR_CLAMP && s->p.wr == NVN_WRAP_MODE_REPEAT,
          "sampler filters and wrap modes");
    CHECK(s->p.lod_min == 0.5f && s->p.lod_max == 7.f && s->p.lod_bias == -1.5f && s->p.aniso == 4.f &&
              !memcmp(s->p.border, border, 16), "sampler LOD, anisotropy and border");
    CHECK(CALL(nvnSamplerGetReductionFilter, U(&samp)) == 1 && CALL(nvnSamplerBuilderGetReductionFilter, U(&sampb)) == 1,
          "reduction filter");
    OBJ(NVNsamplerPool, sp);
    CHECK((uint8_t)CALL(nvnSamplerPoolInitialize, U(&sp), U(&pool), 0x31000, 8) == 1, "sampler pool");
    CALL(nvnSamplerPoolRegisterSampler, U(&sp), 2, U(&samp));
    CALL(nvnSamplerFinalize, U(&samp));
    CHECK(((const samp_t *)(const void *)&samp)->magic == 0, "SamplerFinalize");
    CALL(nvnSamplerInitialize, U(&samp), U(&sampb));
    n_objects += 6;
}

// ---- sync and events
OBJ(NVNsync, syncs);
OBJ(NVNsync, sync_future);
OBJ(NVNevent, ev);
OBJ(NVNevent, ev2);
OBJ(NVNeventBuilder, evb);

static void probe_sync_events(void) {
    CHECK((uint8_t)CALL(nvnSyncInitialize, U(&syncs), U(&dev)) == 1, "SyncInitialize");
    CHECK(CALL(nvnSyncWait, U(&syncs), 1000) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED, "a sync never fenced times out at once");
    CALL(nvnSyncInitialize, U(&sync_future), U(&dev));
    sync_t *sf = (sync_t *)(void *)&sync_future;
    sf->fenced = 1;
    sf->fence = nvn_fence_last() + 100;
    CHECK(CALL(nvnSyncWait, U(&sync_future), 1000000) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED,
          "a fence value that was never submitted is never reached");
    CALL(nvnSyncFinalize, U(&sync_future));
    CHECK(sf->magic == 0, "SyncFinalize");

    CALL(nvnEventBuilderSetDefaults, U(&evb));
    CALL(nvnEventBuilderSetStorage, U(&evb), U(&pool), 0);
    CHECK((uint8_t)CALL(nvnEventInitialize, U(&ev), U(&evb)) == 1, "EventInitialize");
    CHECK(((const ev_t *)(const void *)&ev)->val == (volatile uint32_t *)pmem, "the event value lives in pool storage");
    CHECK((uint32_t)CALL(nvnEventGetValue, U(&ev)) == 0, "a new event reads 0");
    CALL(nvnEventSignal, U(&ev), NVN_EVENT_SIGNAL_MODE_WRITE, 7);
    CHECK((uint32_t)CALL(nvnEventGetValue, U(&ev)) == 7 && *(uint32_t *)pmem == 7, "EventSignal WRITE");
    CALL(nvnEventSignal, U(&ev), NVN_EVENT_SIGNAL_MODE_ADD, 3);
    CHECK((uint32_t)CALL(nvnEventGetValue, U(&ev)) == 10, "EventSignal ADD");
    CALL(nvnEventSignal, U(&ev), NVN_EVENT_SIGNAL_MODE_WRITE, 0);

    CALL(nvnEventBuilderSetStorage, U(&evb), U(&gpool), 0);
    CHECK((uint8_t)CALL(nvnEventInitialize, U(&ev2), U(&evb)) == 0, "an event in a GPU-window pool is refused");
    CALL(nvnEventFinalize, U(&ev2));

    // QueueBuilder defaults, then the queue
    CALL(nvnQueueBuilderSetDevice, U(&queueb), U(&dev));
    CALL(nvnQueueBuilderSetDefaults, U(&queueb));
    CHECK(CALL(nvnQueueBuilderGetQueueMemorySize, U(&queueb)) == ((65536u + 262144u + 16384u + 4095) & ~4095u), "queue memory size");
    CHECK((uint8_t)CALL(nvnQueueInitialize, U(&queue), U(&queueb)) == 1, "QueueInitialize");

    OBJ(NVNwindow, win);
    uint8_t before[sizeof win];
    memcpy(before, &win, sizeof win);
    CALL(nvnWindowFinalize, U(&win));
    CHECK(!memcmp(before, &win, sizeof win), "WindowFinalize is a no-op");
    n_objects += 11;
}

// ---- recording every op kind and parsing it back
OBJ(NVNcommandBuffer, cmd);
OBJ(NVNcommandBuffer, cmd2);
OBJ(NVNprogram, prog);
static uint8_t *ctlmem1, *ctlmem2, *ctlmem3;

static uint32_t expect[128];
static int nexpect;
#define REC(op, call) do { call; expect[nexpect++] = (op); } while (0)

static void begin(NVNcommandBuffer *c, uint8_t *ctl, uint64_t cmd_off) {
    CHECK((uint8_t)CALL(nvnCommandBufferInitialize, U(c), U(&dev)) == 1, "CommandBufferInitialize");
    CALL(nvnCommandBufferAddCommandMemory, U(c), U(&pool), cmd_off, 0x4000);
    CALL(nvnCommandBufferAddControlMemory, U(c), U(ctl), 0x1000);
    CALL(nvnCommandBufferBeginRecording, U(c));
}

static uint64_t record_all(void) {
    NVNcommandBuffer *c = &cmd;
    begin(c, ctlmem1, 0x32000);
    static const float rgba[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    static const uint8_t ub_data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const NVNtexture *colors[1] = {&tex1};
    NVNcopyRegion rg = {1, 2, 3, 4, 5, 1}, rg2 = {6, 7, 8, 9, 10, 1};
    float vp[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint32_t attr[2] = {0x11223344, 0x55667788};
    struct { uint32_t stride, divisor; } stream[2] = {{16, 0}, {32, 1}};
    NVNbufferRange vb[2] = {{0x1000, 0x100}, {0x2000, 0x200}};
    uint8_t blend[8] = {1, 2, 3, 4, 5, 6, 7, 8}, poly[4] = {9, 10, 11, 12};
    uint64_t handles[3] = {0x300005, 0x400006, 0x500007};
    cpu.x[30] = 0x7000001234ull;
    OBJ(NVNtexturePool, tp2);
    OBJ(NVNsamplerPool, sp2);
    CALL(nvnTexturePoolInitialize, U(&tp2), U(&pool), 0x30000, 16);
    CALL(nvnSamplerPoolInitialize, U(&sp2), U(&pool), 0x31000, 8);
    CALL(nvnProgramInitialize, U(&prog), U(&dev));
    CALL(nvnTextureViewSetDefaults, U(&view));
    CALL(nvnTextureViewSetLevels, U(&view), 1, 3);

    REC(OP_SIGNAL_EVENT, CALL(nvnCommandBufferSignalEvent, U(c), U(&ev), NVN_EVENT_SIGNAL_MODE_ADD, NVN_EVENT_SIGNAL_LOCATION_BOTTOM, 0, 0x55));
    REC(OP_REPORT_COUNTER, CALL(nvnCommandBufferReportCounter, U(c), NVN_COUNTER_TYPE_SAMPLES_PASSED, 0xaabb00));
    REC(OP_RESET_COUNTER, CALL(nvnCommandBufferResetCounter, U(c), NVN_COUNTER_TYPE_SAMPLES_PASSED));
    REC(OP_COPY_B2B, CALL(nvnCommandBufferCopyBufferToBuffer, U(c), 0x1000, 0x2000, 0x40, 0));
    REC(OP_UPDATE_UB, CALL(nvnCommandBufferUpdateUniformBuffer, U(c), 0x3000, 0x100, 0x10, 8, U(ub_data)));
    REC(OP_SET_RT, CALL(nvnCommandBufferSetRenderTargets, U(c), 1, U(colors), 0, U(&tex2), 0));
    REC(OP_CLEAR_COLOR, CALL(nvnCommandBufferClearColor, U(c), 2, U(rgba), 0xf));
    fv(0, 0.25f);
    REC(OP_CLEAR_DS, CALL(nvnCommandBufferClearDepthStencil, U(c), 1, 5, 0xff));
    REC(OP_VIEWPORTS, CALL(nvnCommandBufferSetViewports, U(c), 1, 2, U(vp)));
    REC(OP_SCISSOR, CALL(nvnCommandBufferSetScissor, U(c), 1, 2, 3, 4));
    fv(0, 0.125f);
    fv(1, 0.875f);
    REC(OP_DEPTH_RANGE, CALL(nvnCommandBufferSetDepthRange, U(c)));
    REC(OP_BIND_PROGRAM, CALL(nvnCommandBufferBindProgram, U(c), U(&prog), 3));
    REC(OP_BIND_STATE, CALL(nvnCommandBufferBindBlendState, U(c), U(blend)));
    REC(OP_BIND_STATE, CALL(nvnCommandBufferBindPolygonState, U(c), U(poly)));
    REC(OP_BIND_VATTR, CALL(nvnCommandBufferBindVertexAttribState, U(c), 2, U(attr)));
    REC(OP_BIND_VSTREAM, CALL(nvnCommandBufferBindVertexStreamState, U(c), 2, U(stream)));
    REC(OP_BIND_VBUF, CALL(nvnCommandBufferBindVertexBuffers, U(c), 1, 2, U(vb)));
    REC(OP_BIND_UBO, CALL(nvnCommandBufferBindUniformBuffer, U(c), NVN_SHADER_STAGE_FRAGMENT, 3, 0x6000, 0x80));
    REC(OP_BIND_SSBO, CALL(nvnCommandBufferBindStorageBuffer, U(c), NVN_SHADER_STAGE_COMPUTE, 1, 0x7000, 0x90));
    REC(OP_BIND_TEX, CALL(nvnCommandBufferBindTexture, U(c), NVN_SHADER_STAGE_FRAGMENT, 2, 0x300005));
    REC(OP_BIND_TEX, CALL(nvnCommandBufferBindTexturesDeferred, U(c), NVN_SHADER_STAGE_VERTEX, 0, 3, U(handles)));
    REC(OP_BIND_SAMP, CALL(nvnCommandBufferBindSeparateSampler, U(c), NVN_SHADER_STAGE_FRAGMENT, 4, 0x100000));
    REC(OP_BIND_IMG, CALL(nvnCommandBufferBindImage, U(c), NVN_SHADER_STAGE_COMPUTE, 1, 7));
    REC(OP_STENCIL, CALL(nvnCommandBufferSetStencilRef, U(c), NVN_FACE_FRONT, 9));
    fv(0, 1.5f);
    fv(1, 2.5f);
    fv(2, 3.5f);
    REC(OP_POLY_OFFSET, CALL(nvnCommandBufferSetPolygonOffsetClamp, U(c)));
    REC(OP_DRAW, CALL(nvnCommandBufferDrawArrays, U(c), NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 6));
    REC(OP_DRAW_IDX, CALL(nvnCommandBufferDrawElementsInstanced, U(c), NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 12,
                          0x8000, I(-3), 2, 5));
    REC(OP_DISPATCH, CALL(nvnCommandBufferDispatchCompute, U(c), 8, 4, 2));
    REC(OP_BARRIER, CALL(nvnCommandBufferBarrier, U(c), 3));
    REC(OP_SET_TEX_POOL, CALL(nvnCommandBufferSetTexturePool, U(c), U(&tp2)));
    REC(OP_SET_SAMP_POOL, CALL(nvnCommandBufferSetSamplerPool, U(c), U(&sp2)));
    REC(OP_COPY_B2T, CALL(nvnCommandBufferCopyBufferToTexture, U(c), 0x9000, U(&tex1), U(&view), U(&rg), 0));
    REC(OP_COPY_STRIDE, CALL(nvnCommandBufferSetCopyRowStride, U(c), 256));
    REC(OP_BLEND_COLOR, CALL(nvnCommandBufferSetBlendColor, U(c), U(rgba)));
    REC(OP_COPY_T2B, CALL(nvnCommandBufferCopyTextureToBuffer, U(c), U(&tex1), 0, U(&rg), 0xa000, 0));
    REC(OP_COPY_T2T, CALL(nvnCommandBufferCopyTextureToTexture, U(c), U(&tex1), 0, U(&rg), U(&tex2), 0, U(&rg2), 0));
    REC(OP_CLEAR_TEX, CALL(nvnCommandBufferClearTexture, U(c), U(&tex2), 0, U(&rg2), U(rgba), 0xf));
    REC(OP_DEPTH_CLAMP, CALL(nvnCommandBufferSetDepthClamp, U(c), 1));
    REC(OP_RENDER_ENABLE, CALL(nvnCommandBufferSetRenderEnable, U(c), 0));
    REC(OP_RASTER_DISCARD, CALL(nvnCommandBufferSetRasterizerDiscard, U(c), 1));
    REC(OP_RENDER_COND, CALL(nvnCommandBufferSetRenderEnableConditional, U(c), 1, 0xb000));
    REC(OP_PRIM_RESTART, CALL(nvnCommandBufferSetPrimitiveRestart, U(c), 1, 0xffff));
    REC(OP_DRAW_IDX_IND, CALL(nvnCommandBufferDrawElementsIndirect, U(c), NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_INT,
                              0x8000, 0xc000));
    REC(OP_DISPATCH_IND, CALL(nvnCommandBufferDispatchComputeIndirect, U(c), 0xc100));
    REC(OP_CLEAR_BUF, CALL(nvnCommandBufferClearBuffer, U(c), 0xc200, 0x40, 0xdeadbeef));
    REC(OP_CLEAR_BUF, CALL(nvnCommandBufferReportValue, U(c), 0x1234, 0xc300));
    static const int thresholds[4] = {1, 2, 3, 4};
    CALL(nvnCommandBufferSetColorReductionEnable, U(c), 1);   // hints: nothing recorded
    CALL(nvnCommandBufferSetColorReductionThresholds, U(c), 0, U(thresholds));
    REC(OP_COPY_T2T, CALL(nvnCommandBufferDownsample, U(c), U(&tex1), U(&tex2)));
    CHECK((uint8_t)CALL(nvnCommandBufferIsRecording, U(c)) == 1, "IsRecording while recording");
    uint64_t h = CALL(nvnCommandBufferEndRecording, U(c));
    CHECK((uint8_t)CALL(nvnCommandBufferIsRecording, U(c)) == 0, "IsRecording after EndRecording");
    return h;
}

static const float *as_f(const void *p) { return p; }

static void parse_all(uint64_t h) {
    CHECK(h && nvn_handle_live(h), "the recording handle is live");
    const op_hdr *o = nvn_op_first(h);
    CHECK(o->op == OP_BEGIN, "a recording starts with OP_BEGIN");
    uint64_t bytes = 0;
    int idx = 0, seen_state = 0, seen_tex = 0;
    uint8_t kinds[OP_COUNT_] = {0};
    const tex_t *t1 = (const tex_t *)(const void *)&tex1, *t2 = (const tex_t *)(const void *)&tex2;
    for (; o->op != OP_END; o = nvn_op_next(o)) {
        bytes += o->size;
        CHECK(o->size >= 8 && !(o->size & 7) && o->op < OP_COUNT_, "op header op %u size %u", o->op, o->size);
        if (o->op == OP_BEGIN) continue;
        CHECK(o->op != OP_JUMP && o->op != OP_CALL, "a handle holds no OP_JUMP or OP_CALL");
        if (idx >= nexpect || o->op != expect[idx]) {
            CHECK(0, "op %d is %s, expected %s", idx, nvn_op_name(o->op), idx < nexpect ? nvn_op_name(expect[idx]) : "(end)");
            return;
        }
        idx++;
        kinds[o->op]++;
        const void *a = nvn_op_payload(o);
        switch (o->op) {
        case OP_SIGNAL_EVENT: {
            const p_signal_event *p = a;
            CHECK(p->val == (volatile uint32_t *)pmem && p->mode == NVN_EVENT_SIGNAL_MODE_ADD && p->value == 0x55, "SIGNAL_EVENT payload");
            break;
        }
        case OP_REPORT_COUNTER: {
            const p_report_counter *p = a;
            CHECK(p->type == NVN_COUNTER_TYPE_SAMPLES_PASSED && p->addr == 0xaabb00, "REPORT_COUNTER payload");
            break;
        }
        case OP_RESET_COUNTER: CHECK(((const p_reset_counter *)a)->type == NVN_COUNTER_TYPE_SAMPLES_PASSED, "RESET_COUNTER payload"); break;
        case OP_COPY_B2B: {
            const p_copy_b2b *p = a;
            CHECK(p->src == 0x1000 && p->dst == 0x2000 && p->size == 0x40, "COPY_B2B payload");
            break;
        }
        case OP_UPDATE_UB: {
            const p_update_ub *p = a;
            static const uint8_t want[8] = {1, 2, 3, 4, 5, 6, 7, 8};
            CHECK(p->dst == 0x3010 && p->size == 8 && !memcmp(p->data, want, 8) && o->size == 8 + 16 + 8, "UPDATE_UB payload");
            break;
        }
        case OP_SET_RT: {
            const p_set_rt *p = a;
            CHECK(p->n == 1 && p->color[0] && p->color[0] != t1 && p->color[0]->serial == t1->serial && p->depth &&
                      p->depth != t2 && p->depth->serial == t2->serial && !p->has_cview[0] && !p->has_dview,
                  "SET_RT payload: textures are pinned host copies");
            break;
        }
        case OP_CLEAR_COLOR: {
            const p_clear_color *p = a;
            CHECK(p->index == 2 && p->mask == 0xf && p->rgba[0] == 0.1f && p->rgba[3] == 0.4f, "CLEAR_COLOR payload");
            break;
        }
        case OP_CLEAR_DS: {
            const p_clear_ds *p = a;
            CHECK(p->depth == 0.25f && p->depth_mask == 1 && p->stencil == 5 && p->stencil_mask == 0xff, "CLEAR_DS payload");
            break;
        }
        case OP_VIEWPORTS: {
            const p_viewports *p = a;
            CHECK(p->first == 1 && p->n == 2 && p->v[0][0] == 1 && p->v[0][3] == 4 && p->v[1][0] == 5 && p->v[1][3] == 8, "VIEWPORTS payload");
            break;
        }
        case OP_SCISSOR: {
            const p_scissor *p = a;
            CHECK(p->x == 1 && p->y == 2 && p->w == 3 && p->h == 4, "SCISSOR payload");
            break;
        }
        case OP_DEPTH_RANGE: CHECK(as_f(a)[0] == 0.125f && as_f(a)[1] == 0.875f, "DEPTH_RANGE payload"); break;
        case OP_BIND_PROGRAM: {
            const p_bind_program *p = a;
            CHECK(p->prog == (const prog_t *)(const void *)&prog && p->stages == 3, "BIND_PROGRAM payload");
            break;
        }
        case OP_BIND_STATE: {
            const p_bind_state *p = a;
            if (seen_state++ == 0) CHECK(p->kind == 0 && p->bytes[0] == 1 && p->bytes[7] == 8, "BIND_STATE blend payload");
            else CHECK(p->kind == 4 && p->bytes[0] == 9 && p->bytes[3] == 12, "BIND_STATE polygon payload");
            break;
        }
        case OP_BIND_VATTR: {
            const p_bind_vattr *p = a;
            CHECK(p->first == 0 && p->n == 2 && p->st[0] == 0x11223344 && p->st[1] == 0x55667788, "BIND_VATTR payload");
            break;
        }
        case OP_BIND_VSTREAM: {
            const p_bind_vstream *p = a;
            CHECK(p->n == 2 && p->st[0].stride == 16 && p->st[1].stride == 32 && p->st[1].divisor == 1, "BIND_VSTREAM payload");
            break;
        }
        case OP_BIND_VBUF: {
            const p_bind_vbuf *p = a;
            CHECK(p->first == 1 && p->n == 2 && p->r[0].address == 0x1000 && p->r[1].size == 0x200, "BIND_VBUF payload");
            break;
        }
        case OP_BIND_UBO: {
            const p_bind_buf *p = a;
            CHECK(p->stage == NVN_SHADER_STAGE_FRAGMENT && p->index == 3 && p->addr == 0x6000 && p->size == 0x80, "BIND_UBO payload");
            break;
        }
        case OP_BIND_SSBO: {
            const p_bind_buf *p = a;
            CHECK(p->stage == NVN_SHADER_STAGE_COMPUTE && p->index == 1 && p->addr == 0x7000 && p->size == 0x90, "BIND_SSBO payload");
            break;
        }
        case OP_BIND_TEX: {
            const p_bind_handles *p = a;
            if (seen_tex++ == 0)
                CHECK(p->stage == NVN_SHADER_STAGE_FRAGMENT && p->first == 2 && p->n == 1 && p->h[0] == 0x300005 && p->src == 0,
                      "BIND_TEX immediate payload");
            else
                CHECK(p->stage == NVN_SHADER_STAGE_VERTEX && p->first == 0 && p->n == 3 && p->h[2] == 0x500007 && p->src != 0,
                      "BIND_TEX deferred payload keeps the game's array pointer");
            break;
        }
        case OP_BIND_SAMP: {
            const p_bind_handles *p = a;
            CHECK(p->first == 4 && p->n == 1 && p->h[0] == 0x100000, "BIND_SAMP payload");
            break;
        }
        case OP_BIND_IMG: {
            const p_bind_handles *p = a;
            CHECK(p->stage == NVN_SHADER_STAGE_COMPUTE && p->first == 1 && p->n == 1 && p->h[0] == 7, "BIND_IMG payload");
            break;
        }
        case OP_STENCIL: {
            const p_stencil *p = a;
            CHECK(p->face == NVN_FACE_FRONT && p->ref == 9 && p->which == 2, "STENCIL payload");
            break;
        }
        case OP_POLY_OFFSET: {
            const p_poly_offset *p = a;
            CHECK(p->factor == 1.5f && p->units == 2.5f && p->clamp == 3.5f, "POLY_OFFSET payload");
            break;
        }
        case OP_DRAW: {
            const p_draw *p = a;
            CHECK(p->prim == NVN_DRAW_PRIMITIVE_TRIANGLES && p->first == 0 && p->count == 6 && p->instances == 1, "DRAW payload");
            break;
        }
        case OP_DRAW_IDX: {
            const p_draw_idx *p = a;
            CHECK(p->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT && p->count == 12 && p->base_vertex == -3 && p->base_instance == 2 &&
                      p->instances == 5 && p->ibuf == 0x8000, "DRAW_IDX payload");
            break;
        }
        case OP_DISPATCH: {
            const p_dispatch *p = a;
            CHECK(p->x == 8 && p->y == 4 && p->z == 2 && p->caller == 0x7000001234ull, "DISPATCH payload");
            break;
        }
        case OP_BARRIER: CHECK(((const p_barrier *)a)->bits == 3, "BARRIER payload"); break;
        case OP_SET_TEX_POOL:
        case OP_SET_SAMP_POOL: CHECK(((const p_set_pool *)a)->pool != NULL, "SET_*_POOL payload"); break;
        case OP_COPY_B2T: {
            const p_copy_b2t *p = a;
            CHECK(p->src == 0x9000 && p->tex && p->tex->serial == t1->serial && p->has_view && p->view.set_levels && p->view.base_level == 1 &&
                      p->r.xoffset == 1 && p->r.width == 4 && p->r.height == 5, "COPY_B2T payload");
            break;
        }
        case OP_COPY_STRIDE: CHECK(((const p_copy_stride *)a)->stride == 256, "COPY_STRIDE payload"); break;
        case OP_BLEND_COLOR: CHECK(((const p_blend_color *)a)->rgba[2] == 0.3f, "BLEND_COLOR payload"); break;
        case OP_COPY_T2B: {
            const p_copy_b2t *p = a;
            CHECK(p->src == 0xa000 && p->tex && p->tex->serial == t1->serial && !p->has_view && p->r.width == 4, "COPY_T2B payload");
            break;
        }
        case OP_COPY_T2T: {
            const p_copy_t2t *p = a;
            if (p->flags == NVN_COPY_FLAGS_LINEAR_FILTER_BIT) {   // Downsample: whole level 0 of each, filtered
                CHECK(p->src && p->src->serial == t1->serial && p->dst && p->dst->serial == t2->serial && !p->has_sv && !p->has_dv &&
                          p->sr.width == 40 && p->sr.height == 128 && p->dr.width == 160 && p->dr.height == 160 && p->sr.depth == 1,
                      "Downsample payload");
                break;
            }
            CHECK(p->src && p->src->serial == t1->serial && p->dst && p->dst->serial == t2->serial && p->sr.width == 4 && p->dr.width == 9,
                  "COPY_T2T payload");
            break;
        }
        case OP_CLEAR_TEX: {
            const p_clear_tex *p = a;
            CHECK(p->tex && p->tex->serial == t2->serial && !p->is_uint && p->mask == 0xf && p->c.f[1] == 0.2f && p->r.xoffset == 6, "CLEAR_TEX payload");
            break;
        }
        case OP_DEPTH_CLAMP: CHECK(((const p_flag *)a)->value == 1, "DEPTH_CLAMP payload"); break;
        case OP_RENDER_ENABLE: CHECK(((const p_flag *)a)->value == 0, "RENDER_ENABLE payload"); break;
        case OP_RASTER_DISCARD: CHECK(((const p_flag *)a)->value == 1, "RASTER_DISCARD payload"); break;
        case OP_RENDER_COND: {
            const p_render_cond *p = a;
            CHECK(p->mode == 1 && p->addr == 0xb000, "RENDER_COND payload");
            break;
        }
        case OP_PRIM_RESTART: {
            const p_prim_restart *p = a;
            CHECK(p->enable == 1 && p->index == 0xffff, "PRIM_RESTART payload");
            break;
        }
        case OP_DRAW_IDX_IND: {
            const p_draw_idx_ind *p = a;
            CHECK(p->prim == NVN_DRAW_PRIMITIVE_TRIANGLES && p->itype == NVN_INDEX_TYPE_UNSIGNED_INT && p->ibuf == 0x8000 && p->ind == 0xc000,
                  "DRAW_IDX_IND payload");
            break;
        }
        case OP_DISPATCH_IND: CHECK(((const p_dispatch_ind *)a)->ind == 0xc100, "DISPATCH_IND payload"); break;
        case OP_CLEAR_BUF: {
            const p_clear_buf *p = a;
            CHECK((p->addr == 0xc200 && p->size == 0x40 && p->value == 0xdeadbeef) ||
                      (p->addr == 0xc300 && p->size == 4 && p->value == 0x1234), "CLEAR_BUF payload (ClearBuffer, ReportValue)");
            break;
        }
        default: CHECK(0, "unexpected op %s", nvn_op_name(o->op)); break;
        }
    }
    bytes += o->size;   // OP_END
    CHECK(idx == nexpect, "%d of %d ops parsed", idx, nexpect);
    CHECK(bytes == nvn_handle_bytes(h), "the copy holds %llu bytes, the walk %llu", (unsigned long long)nvn_handle_bytes(h),
          (unsigned long long)bytes);
    int missing = 0;
    for (uint32_t k = OP_SIGNAL_EVENT; k < OP_COUNT_; k++) missing += !kinds[k];
    CHECK(!missing, "%d op kinds were not recorded", missing);
    n_ops = OP_COUNT_ - OP_SIGNAL_EVENT;
}

static void probe_commands(void) {
    ctlmem1 = mem_kernel_alloc(0x1000, 16);
    ctlmem2 = mem_kernel_alloc(0x1000, 16);
    ctlmem3 = mem_kernel_alloc(0x1000, 16);
    uint64_t h = record_all();
    parse_all(h);

    // CallCommands inlines the called recording at the call position
    begin(&cmd2, ctlmem2, 0x34000);
    CALL(nvnCommandBufferBarrier, U(&cmd2), 1);
    CALL(nvnCommandBufferCallCommands, U(&cmd2), 1, U(&h));
    CALL(nvnCommandBufferBarrier, U(&cmd2), 2);
    uint64_t h2 = CALL(nvnCommandBufferEndRecording, U(&cmd2));
    int n = 0, barriers = 0, inner = 0, forbidden = 0;
    for (const op_hdr *o = nvn_op_first(h2); o->op != OP_END; o = nvn_op_next(o)) {
        forbidden += o->op == OP_CALL || o->op == OP_JUMP;
        if (o->op != OP_BEGIN) n++;
        if (o->op == OP_BARRIER) barriers++;
        inner++;
    }
    CHECK(!forbidden && n == nexpect + 2 && barriers == 3 && inner == nexpect + 3, "CallCommands inlined: %d ops (expected %d), %d barriers",
          n, nexpect + 2, barriers);
    CHECK(nvn_op_first(h2)->op == OP_BEGIN && nvn_op_next(nvn_op_first(h2))->op == OP_BARRIER, "the first op before the call stays first");

    // adding the control memory again retires the recordings made in it
    CALL(nvnCommandBufferAddControlMemory, U(&cmd2), U(ctlmem2), 0x1000);
    CHECK(!nvn_handle_live(h2) && nvn_handle_live(h), "reusing control memory retires only the handles recorded in it");
    CALL(nvnCommandBufferFinalize, U(&cmd2));
    n_objects += 5;
}

// ---- the null back end's side effects, at submit and in stream order
static void probe_null_effects(void) {
    // layout in the CPU-visible pool; offsets are bytes from pmem
    enum { OFF_COUNTER_A = 0x100, OFF_COUNTER_B = 0x200, OFF_UB = 0x300, OFF_DST = 0x400, OFF_B2B_DST = 0x500 };
    uint8_t *ev_val = pmem;
    static const uint8_t d1[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
    static const uint8_t d2[8] = {0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7};
    uint8_t keep[16];
    memset(pmem + OFF_COUNTER_B, 0xaa, 16);
    memset(pmem + OFF_B2B_DST, 0x5c, 16);
    memcpy(keep, pmem + OFF_B2B_DST, 16);
    *(uint32_t *)ev_val = 0;
    uint64_t base = U(pmem), gbase = U(G()->mem);

    NVNcommandBuffer *c = &cmd;
    begin(c, ctlmem3, 0x36000);
    CALL(nvnCommandBufferResetCounter, U(c), NVN_COUNTER_TYPE_SAMPLES_PASSED);
    CALL(nvnCommandBufferSignalEvent, U(c), U(&ev), NVN_EVENT_SIGNAL_MODE_WRITE, NVN_EVENT_SIGNAL_LOCATION_TOP, 0, 0x11);
    CALL(nvnCommandBufferSignalEvent, U(c), U(&ev), NVN_EVENT_SIGNAL_MODE_ADD, NVN_EVENT_SIGNAL_LOCATION_BOTTOM, 0, 5);
    CALL(nvnCommandBufferUpdateUniformBuffer, U(c), base + OFF_UB, 64, 4, 16, U(d1));
    CALL(nvnCommandBufferCopyBufferToBuffer, U(c), base + OFF_UB + 4, base + OFF_DST, 16, 0);
    CALL(nvnCommandBufferReportCounter, U(c), NVN_COUNTER_TYPE_TIMESTAMP, base + OFF_COUNTER_A);
    CALL(nvnCommandBufferReportCounter, U(c), NVN_COUNTER_TYPE_SAMPLES_PASSED, base + OFF_COUNTER_B);
    // GPU-window addresses hold no host memory: these have no effect and must not stop the run
    CALL(nvnCommandBufferCopyBufferToBuffer, U(c), base + OFF_UB + 4, gbase + 0x10, 16, 0);
    CALL(nvnCommandBufferCopyBufferToBuffer, U(c), gbase + 0x10, base + OFF_B2B_DST, 16, 0);
    CALL(nvnCommandBufferReportCounter, U(c), NVN_COUNTER_TYPE_TIMESTAMP, gbase + 0x20);
    CALL(nvnCommandBufferUpdateUniformBuffer, U(c), gbase, 64, 8, 8, U(d2));
    CALL(nvnCommandBufferDrawArrays, U(c), NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);   // ignored
    CALL(nvnCommandBufferBarrier, U(c), 1);
    uint64_t ha = CALL(nvnCommandBufferEndRecording, U(c));

    begin(&cmd2, ctlmem2, 0x38000);
    CALL(nvnCommandBufferUpdateUniformBuffer, U(&cmd2), base + OFF_DST, 64, 0, 8, U(d2));   // after the first copy
    CALL(nvnCommandBufferSignalEvent, U(&cmd2), U(&ev), NVN_EVENT_SIGNAL_MODE_WRITE, NVN_EVENT_SIGNAL_LOCATION_TOP, 0, 0x99);
    uint64_t hb = CALL(nvnCommandBufferEndRecording, U(&cmd2));

    CHECK(*(uint32_t *)ev_val == 0 && !pmem[OFF_UB + 4] && !pmem[OFF_DST] && pmem[OFF_COUNTER_B] == 0xaa,
          "recording has no side effects");

    uint64_t t0 = rt_ticks(), subs = nvn_submit_count(), fence0 = nvn_fence_last();
    uint64_t hs[2] = {ha, hb};
    CALL(nvnQueueSubmitCommands, U(&queue), 2, U(hs));
    uint64_t t1 = rt_ticks();
    CALL(nvnQueueFenceSync, U(&queue), U(&syncs), NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    uint64_t wait = CALL(nvnSyncWait, U(&syncs), 1000000000ull);
    CHECK(wait == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || wait == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED, "SyncWait after the submit: %llu",
          (unsigned long long)wait);
    CHECK(nvn_submit_count() == subs + 1 && nvn_fence_last() == fence0 + 1 && gfx_backend()->fence_done() == nvn_fence_last(),
          "one submit advances the fence once (%llu -> %llu)", (unsigned long long)fence0, (unsigned long long)nvn_fence_last());

    // event: WRITE 0x11, ADD 5, then handle 2's WRITE 0x99
    CHECK((uint32_t)CALL(nvnEventGetValue, U(&ev)) == 0x99, "event value %#x after the stream, expected 0x99", *(uint32_t *)ev_val);
    // uniform update lands at buffer + offset, nothing around it moves
    CHECK(!memcmp(pmem + OFF_UB + 4, d1, 16) && !pmem[OFF_UB + 3] && !pmem[OFF_UB + 20], "uniform update");
    // buffer copy in stream order: handle 1's copy, then handle 2's update over its first 8 bytes
    CHECK(!memcmp(pmem + OFF_DST, d2, 8) && !memcmp(pmem + OFF_DST + 8, d1 + 8, 8) && !pmem[OFF_DST + 16],
          "buffer copy and later update in stream order");
    // counters: NVNcounterData {counter, timestamp}; the null GPU reports 0 and a timestamp in 19.2 MHz ticks
    uint64_t ca[2], cb[2];
    memcpy(ca, pmem + OFF_COUNTER_A, 16);
    memcpy(cb, pmem + OFF_COUNTER_B, 16);
    CHECK(ca[0] == 0 && ca[1] + 1 >= t0 && ca[1] <= t1 + 1, "timestamp counter {%llu, %llu} outside [%llu, %llu]", (unsigned long long)ca[0],
          (unsigned long long)ca[1], (unsigned long long)t0, (unsigned long long)t1);
    CHECK(cb[0] == 0 && cb[1] + 1 >= t0 && cb[1] <= t1 + 1, "samples-passed counter {%llu, %llu}", (unsigned long long)cb[0],
          (unsigned long long)cb[1]);
    CHECK(!memcmp(pmem + OFF_B2B_DST, keep, 16), "a copy out of the GPU window left the target alone");
    n_effects = 6;

    CALL(nvnCommandBufferFinalize, U(&cmd2));
    CALL(nvnCommandBufferFinalize, U(&cmd));
    n_objects += 2;
}

static void probe_main(Cpu *unused) {
    (void)unused;
    probe_device_and_pools();
    probe_buffers();
    probe_textures();
    probe_samplers();
    probe_sync_events();
    probe_commands();
    probe_null_effects();
    CALL(nvnQueueFinalize, U(&queue));
    CALL(nvnDeviceFinalize, U(&dev));
}

// ---- the proc table
static void probe_proc_table(void) {
    int dup = 0;
    uint64_t addr[NPROCS];
    for (int i = 0; i < NPROCS; i++) {
        addr[i] = nvn_proc_address(procs[i].name);
        const char *reg = addr[i] ? hostfn_name(addr[i]) : NULL;
        char want[160];
        snprintf(want, sizeof want, "nvn:%s", procs[i].name);
        CHECK(addr[i] && reg && !strcmp(reg, want), "proc %s resolves to %s", procs[i].name, reg ? reg : "nothing");
    }
    for (int i = 0; i < NPROCS; i++)
        for (int k = i + 1; k < NPROCS; k++) dup += addr[i] && addr[i] == addr[k];
    CHECK(dup == 0, "%d procs share a synthetic address", dup);
    CHECK(NPROCS >= 534, "the proc list has %d names, the loader asks for 534", NPROCS);
    CHECK(nvn_proc_address("nvnNoSuchProc") == 0 && nvn_proc_address("") == 0 && nvn_proc_address(NULL) == 0,
          "an unknown name returns NULL");
    // nvnDeviceGetProcAddress(device, name), the way the loader calls it
    CHECK(CALL(nvnDeviceGetProcAddress, U(&dev), U("nvnBufferGetSize")) == nvn_proc_address("nvnBufferGetSize") &&
              CALL(nvnDeviceGetProcAddress, U(&dev), U("nvnNoSuchProc")) == 0, "nvnDeviceGetProcAddress");
}

// ---- named fatal stubs, each in a child process
static uint32_t run_child(const char *mode, char *out, size_t cap) {
    int code = plat_run_self(CHILD_ENV, mode, out, cap, 0);
    if (code < 0) rt_fatal("probe-nvn: cannot start the %s child", mode);
    return (uint32_t)code;
}

static int run_as_child(const char *name) {
    for (int i = 0; i < NPROCS; i++)
        if (!strcmp(procs[i].name, name)) {
            procs[i].fn(&cpu);
            printf("child %s: the call returned\n", name);
            return 0;
        }
    rt_fatal("probe-nvn: no proc %s", name);
}

static void probe_children(void) {
    static char out[16384];
    for (int i = 0; i < NFATAL; i++) {
        uint32_t code = run_child(fatal_procs[i], out, sizeof out);
        char want[160];
        snprintf(want, sizeof want, "NVN %s is not implemented", fatal_procs[i]);
        CHECK(code == 70 && strstr(out, "FATAL: ") && strstr(out, want), "child %s: exit %u, output: %s", fatal_procs[i], code, out);
        const char *line = strstr(out, "FATAL: ");
        int len = line ? (int)strcspn(line, "\r\n") : 0;
        printf("fatal stub %-30s child exit %u: %.*s\n", fatal_procs[i], code, len, line ? line : "(no FATAL line)");
    }
}

int probe_nvn(void) {
    const char *child = getenv(CHILD_ENV);
    if (child && *child) return run_as_child(child);
    // The probe checks the null back end whatever TOTK_GPU says; the choice is made once, at the first object call.
    ((RtConfig *)rt_config())->gpu = "null";
    nvn_procs_register();
    hostfn_seal();
    kern_run_on_guest_main(probe_main);
    probe_proc_table();
    probe_children();
    if (failures) {
        printf("probe-nvn: FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("probe-nvn: passed (%d checks: %d object cases, %d op kinds recorded and parsed, %d procs resolved, %d side effects on the %s "
           "back end, %d fatal stubs in child processes)\n", checks, n_objects, n_ops, NPROCS, n_effects, gfx_backend()->name, NFATAL);
    return 0;
}
