// Internal header of the NVN front end, shared by nvn_api_core.c and the other
// slices (textures, samplers, programs, windows, state objects, command buffers). Adapted from MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c and nvn_impl.h @ b75be8cf
// (MIT, Copyright (c) 2026 jhaizhou; origin d2r-mobile).
#pragma once
#include <stdint.h>

#include "gfx.h"
#include "host.h"
#include "nvn_obj.h"

// NVN_API(nvnX) { body }: the strong nvnp_nvnX(Cpu *c); counts the call in gfx_counters, TOTK_TRACE=gfx logs the first.
#define NVN_API(n)                                                                                                   \
    static void nvn_impl_##n(Cpu *c);                                                                                \
    void nvnp_##n(Cpu *c) {                                                                                          \
        static uint8_t seen;                                                                                         \
        nvn_called(&seen, #n);                                                                                       \
        nvn_impl_##n(c);                                                                                             \
    }                                                                                                                \
    static void nvn_impl_##n(Cpu *c)
void nvn_called(uint8_t *seen, const char *name);          // the counting prologue of NVN_API
#define NVN_LOG(...) do { if (rt_trace & RT_TRACE_GFX) rt_log("nvn: " __VA_ARGS__); } while (0)   // TOTK_TRACE=gfx
// A wait for the GPU, the display or another thread gives the virtual core up until the end of the enclosing scope.
#define NVN_BLOCKING() __attribute__((cleanup(kern_core_back))) int nvn_away_ = kern_core_away()
static inline int nvn_arg_bool(const Cpu *c, int i) { return (int)(arg_x(c, i) & 0xff); }   // NVNboolean: low 8 bits only

#define NVN_POOL_MAGIC MAGIC('P', 'O', 'O', 'L')           // pool_t.magic of an initialized pool
#define NVN_VPOOL_PAGE 0x10000ull                          // NVN virtual mapping granularity (64 KiB)
static inline int nvn_gpu_only(uint64_t a) { return a - RT_GPU_VA_BASE < RT_GPU_VA_SIZE; }   // address in the GPU window
// A CPU-invisible physical or virtual pool lives in the GPU window: mem is its GPU address (never mapped on the host),
// game_mem the game's storage (never touched), span the reserved window bytes, host NULL. Every other pool: mem is the
// game's storage, the GPU address equals the guest address.

typedef struct { NVNdevice *dev; pool_t *pool; int64_t off; uint64_t size; } buf_builder_t;   FITS(buf_builder_t, NVNbufferBuilder);
typedef struct { uint32_t magic; pool_t *pool; int64_t off; uint64_t size; } buf_t;           FITS(buf_t, NVNbuffer);
typedef struct {                                           // NVNmappingRequest as main passes it (0x28 bytes)
    const NVNmemoryPool *physical_pool;                    // NULL unmaps
    int64_t physical_offset, virtual_offset;
    uint64_t size;
    int32_t storage_class;
    uint32_t padding;
} nvn_mapping_t;
_Static_assert(sizeof(nvn_mapping_t) == 0x28, "NVNmappingRequest stride");

const pool_t *nvn_pool_find(uint64_t addr, uint64_t len);  // GPU-visible pool holding [addr, addr+len], NULL if none
uint64_t nvn_new_serial(void);                             // next tex_t / prog_t serial, from 1
uint64_t nvn_fence_last(void);                             // fence value of the newest submit (0 before the first)
int nvn_depth_minus_w(void);                               // 1 after DeviceSetDepthMode(NEAR_IS_MINUS_W), else 0
void nvn_guest_written(uint64_t addr, uint64_t size);      // the host or the game wrote pool storage: back end re-reads
// TOTK_TRACE=perf: MemoryPoolFlushMappedRange [0] and BufferFlushMappedRange [1] calls [0] and bytes [1] by the pool's
// CPU class (0 CPU_CACHED, 1 CPU_UNCACHED, 2 other) since the previous call.
void nvn_flush_stats(uint64_t out[2][3][2]);
void nvn_submit_prepare(const uint64_t *handles, int n);   // weak no-op; the command-buffer slice reads deferred binds
// Asynchronous replay: as nvn_submit_prepare, writing the handles to replay to out (a clone where a still queued
// submission uses the same recording with other deferred binds)
void nvn_submit_prepare_async(const uint64_t *handles, uint64_t *out, int n);
int gfx_replay_async(void);                                // 1: the back end replays submissions on its own thread
uint64_t gfx_replayed_submits(void);                       // newest submit number (nvn_submit_count) replayed
#define NVN_HANDLE_TEX(h) ((int)((h) & 0xfffff))           // combined texture handle = texture id | sampler id << 20
#define NVN_HANDLE_SAMP(h) ((int)(((h) >> 20) & 0xfff))
extern GfxStats gfx_counters;                              // behind gfx_stats(); add with __atomic_fetch_add. frames:
                                                           // the back end's present; nvn_calls/nvn_distinct: NVN_API

// ---- nvn_api_obj.c: textures, samplers, programs, windows
#define NVN_TEX_MAGIC MAGIC('T', 'E', 'X', 0)              // tex_t.magic of an initialized texture
#define NVN_SAMP_MAGIC MAGIC('S', 'M', 'P', 0)             // samp_t.magic
#define NVN_PROG_MAGIC MAGIC('P', 'R', 'O', 'G')           // prog_t.magic
#define NVN_WIN_MAGIC MAGIC('W', 'I', 'N', 'D')            // win_t.magic
// The live texture whose storage starts at mem, copied into *out; 0 if none. The game re-initializes transient textures
// at the same pool memory without registering them again, so a registered serial that was finalized since resolves here.
int nvn_tex_live_at(const void *mem, tex_t *out);
// Byte offset of a view's first level and layer inside the texture storage (nvnTextureGetViewOffset); v may be NULL.
uint64_t nvn_tex_view_offset(const tex_t *t, const view_t *v);

// ---- nvn_api_cmd.c, nvn_ext.c: command buffers and their replay contract
// A command handle (what QueueSubmitCommands receives) points at a host copy of one recording: a flat sequence of
// 8-byte-aligned records {op_hdr, payload}, OP_BEGIN first and OP_END last. Called recordings are inlined, so a copy
// holds no OP_CALL or OP_JUMP. Texture pointers in payloads (p_set_rt, p_copy_b2t, p_copy_t2t, p_clear_tex) are
// immutable host copies that live for the run, one per tex_t.serial. Deferred binds (p_bind_handles.src != 0) hold the
// handles read at submit (nvn_submit_prepare). Payloads of the ops nvn_obj.h does not define:
typedef struct { volatile uint32_t *val; uint32_t mode, value; } p_signal_event;   // OP_SIGNAL_EVENT; mode: NVNeventSignalMode
typedef struct { uint64_t type, addr; } p_report_counter;  // OP_REPORT_COUNTER: NVNcounterType, NVNcounterData address
typedef struct { uint64_t type; } p_reset_counter;         // OP_RESET_COUNTER
typedef struct { uint64_t src, dst, size; } p_copy_b2b;    // OP_COPY_B2B: GPU addresses
// OP_UPDATE_UB: size bytes written at dst, within the uniform block [buf, buf+buf_size) of the call
typedef struct { uint64_t dst, size, buf, buf_size; uint8_t data[]; } p_update_ub;
typedef struct { int32_t bits, pad; } p_barrier;           // OP_BARRIER: NVNbarrierBits
typedef struct { uint64_t target; } p_jump;                // OP_JUMP / OP_CALL: next record / called recording
// OP_DRAW_IDX_IND: DrawElementsIndirect; ind = GPU address of {count, instances, first index, base vertex, base
// instance} (VkDrawIndexedIndirectCommand's layout), read by the GPU when the draw runs
typedef struct { int32_t prim, itype; uint64_t ibuf, ind; } p_draw_idx_ind;
typedef struct { uint64_t ind, caller; } p_dispatch_ind;   // OP_DISPATCH_IND: ind = GPU address of the 3 group counts
// OP_CLEAR_BUF: size bytes at addr filled with the 32-bit value in GPU order (ClearBuffer; ReportValue is a 4-byte one)
typedef struct { uint64_t addr, size; uint32_t value, pad; } p_clear_buf;
_Static_assert(sizeof(p_signal_event) == 16 && sizeof(p_report_counter) == 16 && sizeof(p_copy_b2b) == 24 &&
               sizeof(p_update_ub) == 32, "op payload layouts of hle_nvn.c");
static inline const op_hdr *nvn_op_first(uint64_t handle) { return (const op_hdr *)(uintptr_t)handle; }   // OP_BEGIN
static inline const op_hdr *nvn_op_next(const op_hdr *h) { return (const op_hdr *)(const void *)((const uint8_t *)h + h->size); }
static inline const void *nvn_op_payload(const op_hdr *h) { return h + 1; }
int nvn_handle_live(uint64_t handle);                      // 1 while the handle's copy exists (its control memory was
                                                           // not added again)
uint64_t nvn_handle_bytes(uint64_t handle);                // bytes of the copy, OP_BEGIN to OP_END; 0 if not live
uint64_t nvn_submit_count(void);                           // QueueSubmitCommands calls so far
const char *nvn_op_name(uint32_t op);                      // "DRAW_IDX", ...; "?" past OP_COUNT_
// MemoryPoolInvalidateMappedRange: the CPU is about to read pool storage the GPU wrote. Weak no-op in nvn_ext.c; a back
// end that renders into host-side copies defines it and writes them back.
void nvn_pool_invalidate(const pool_t *p, uint64_t off, uint64_t size);
// MemoryPoolFlushMappedRange: the CPU wrote [mem + off, mem + off + size). Weak no-op in nvn_ext.c; a back end that keeps
// device-side copies of pool memory (vk_memory.c's VRAM mirror) defines it and copies the range again.
void nvn_pool_flushed(const pool_t *p, uint64_t off, uint64_t size);
