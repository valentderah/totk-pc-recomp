// TOTK_SYNC_AHEAD=check (vk_ahead.c): measures what answering the frame wait one frame late without shadow storage
// would race, while the wait stays exact. Off: vk_ahead_on is 0 and every hook is one test of it.
#pragma once
#include <stdint.h>

// Classes of GPU-timeline accesses of guest memory a submission makes after its recording: in-place bindings of an
// imported pool without a mirror (vertex, index, uniform, storage, indirect, copy source), the VRAM mirror's sync copies,
// the GPU deswizzle of a texture's storage, and the mirror write-backs (plain copies, word merges). BOUND: a binding of
// a mirrored pool, the bytes the GPU reads from the mirror; not a race of its own, it narrows MSYNC to what N reads.
enum { AH_VB, AH_IB, AH_UBO, AH_SSBO, AH_IND, AH_COPY, AH_MSYNC, AH_DESWZ, AH_WB, AH_WBM, AH_BOUND, AH_N };
// Kinds of GPU results the CPU may read back: buffer writes, counter reports, samples-passed reports, event signals.
enum { AHW_WRITE, AHW_COUNTER, AHW_SAMPLES, AHW_EVENT, AHW_N };

extern int vk_ahead_on;
void vk_ahead_init(void);   // vk_memory.c mirror_mode, once at device start: reads TOTK_SYNC_AHEAD
// [addr, addr+size) of guest memory is read (or, for WB and WBM, written) on the GPU timeline by the submission of value
// fence (0: the one being recorded, else the next one).
void vk_ahead_read(uint64_t addr, uint64_t size, int cls, uint64_t fence);
// The GPU writes [addr, addr+size) of CPU-visible memory, or a report lands there, at the completion of value fence
// (0: as above).
void vk_ahead_gpu_write(uint64_t addr, uint64_t size, int kind, uint64_t fence);
void vk_ahead_invalidate(uint64_t addr, uint64_t size);   // nvnMemoryPoolInvalidateMappedRange
void vk_ahead_small(uint64_t addr, uint64_t gpu_fence);   // the fence write-back of a small target (vk_texture.c)
// The game's per-frame wait (nvn_api_core.c nvnSyncWait at frame_wait_lr): entry checks the previous frame's read set;
// lag before cpu_synced gives the value a one-frame-late wait would have waited for (lagged == asked: no lag), counted
// whether or not the GPU had finished asked (behind: it had not, the late wait would have returned early on this
// machine); done after it records this frame's read set. ok 0: the wait timed out.
void vk_ahead_frame_entry(void);
void vk_ahead_frame_lag(uint64_t asked, uint64_t lagged, int behind);
void vk_ahead_frame_done(int ok);
void vk_ahead_log_final(void);   // at exit
int vk_ahead_selftest(uint64_t addr, uint64_t size);   // --probe-vkmem: 0 passed
