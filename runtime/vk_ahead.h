// TOTK_SYNC_AHEAD=check (vk_ahead.c): measures what answering the frame wait one frame late without shadow storage
// would race, while the wait stays exact. Off: vk_ahead_on is 0 and every hook is one test of it. The same detector
// runs with the late wait itself (TOTK_SYNC_AHEAD=1, shadow off) under TOTK_SYNC_AHEAD_CHECK=1.
// TOTK_SYNC_AHEAD=1 without shadow storage: vk_ahead_g.lag, and the guards of that form (vk_ahead_init).
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
// The in-place form's switches, read once by vk_ahead_init; every guard is 0 without lag. A/B: TOTK_SYNC_AHEAD_<NAME>.
// msync: the mirror syncs copy the synced pages into staging on the CPU at submit (vk_memory.c vk_mem_mirror_prologue);
// wbmerge: every GPU write into a mirror goes back through the word merge, not only a shader's (vk_memory.c gpu_wrote);
// deswz: a texture re-uploaded after the CPU rewrote it takes the CPU deswizzle (vk_texture.c upload_gpu); reads: an
// invalidate of a range not read lately waits for the pending GPU write into it (vk_memory.c vk_mem_shadow_read);
// oneshot: a one-off small target is waited for, up to its value (vk_ahead_oneshot); queries (default 0): so is a
// samples-passed report.
typedef struct { int lag, msync, wbmerge, deswz, reads, oneshot, queries; } VkAheadGuards;
extern VkAheadGuards vk_ahead_g;
void vk_ahead_init(void);   // vk_memory.c mirror_mode, once at device start, shadow mode known: reads TOTK_SYNC_AHEAD
// The newest value in (lo, hi] whose GPU result the game reads right after the frame wait (0: none): the in-place
// frame wait waits up to it. Game thread.
uint64_t vk_ahead_oneshot(uint64_t lo, uint64_t hi);
void vk_ahead_query(uint64_t fence);   // a samples-passed report into CPU-visible memory, by the value being recorded
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
void vk_ahead_log_guards(void);  // at exit: what the in-place form's waits did
int vk_ahead_selftest(uint64_t addr, uint64_t size);   // --probe-vkmem: 0 passed
