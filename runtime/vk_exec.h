// The op-stream replay of the Vulkan back end. vk_exec.c walks the command
// handles of a submission, keeps render targets, passes, clears and fixed-function state; draws, binds and dispatches
// (vk_exec_draw.c) and copies, counters, events and conditional rendering (vk_exec_xfer.c) plug in through the two
// hooks at the bottom.
// Runs on the submitting thread under vk_sync_submit's lock; a chunked replay (TOTK_REPLAY_WORKERS, vk_exec.c) records
// on several threads, each with a VkExec of its own, and shares only the locked tables named where they are declared.
// The op loop follows MacNativeGames nvn_mtl.m (MIT, Copyright (c) 2026 jhaizhou) and the pass handling LAS Recomp
// (MIT, Copyright (c) 2026 Peiikai); vk_exec.c names the sources.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "nvn_api.h"
#include "vk.h"

// Dynamic state the next draw must record (VkExec.dirty). vk_exec_record sets them all at the start of each command
// buffer; the state ops set theirs; vk_exec_draw.c clears what it recorded.
enum {
    VK_EXEC_DIRTY_VIEWPORT = 1 << 0,     // viewports or depth range
    VK_EXEC_DIRTY_SCISSOR = 1 << 1,
    VK_EXEC_DIRTY_DEPTH_BIAS = 1 << 2,   // polygon offset
    VK_EXEC_DIRTY_BLEND_CONST = 1 << 3,
    VK_EXEC_DIRTY_STENCIL = 1 << 4,      // compare and write masks, references
    VK_EXEC_DIRTY_ALL = (1 << 5) - 1,
};

// One render target of OP_SET_RT, resolved to its vk_texture.c image for the current command buffer.
typedef struct {
    const tex_t *tex;                    // texture pin of ex->rt (nvn_pins_sweep keeps it); NULL: no target in this slot
    VkImage image;                       // VK_NULL_HANDLE: no target or not renderable (logged once)
    VkFormat format;                     // view format: the view's when it reinterprets a same-size format
    VkImageViewType view_type;           // 2D, or 2D_ARRAY for more than one layer
    VkImageAspectFlags aspect;
    uint32_t level, base_layer, layers;  // subresource of the view; of a 3D image: depth planes
    uint8_t is3d;                        // a 3D image (colour only), viewed through 2D views of its slices
    uint8_t blocks;                      // a BC image viewed as one texel per block (VkTexImage.block_views)
    uint32_t width, height, samples;     // extent of that level; with blocks, in blocks
    int32_t nvn_format;                  // NVNformat of the view (VkPipeState.color_fmt / depth_fmt)
    const VkTexImage *img;               // the image (fb_usage, fb_learned)
    uint8_t scale;                       // VkTexImage.scale
} VkExecTarget;

// TOTK_TRACE=gfx: replay counters of the current frame, logged and reset at each present (vk_exec_frame_log).
typedef struct {
    uint64_t passes, draws_seen, draws, clears, copies, dispatches, submits;
    uint64_t fb_draws, fb_barriers, fb_restarts, fb_layouts;   // feedback loops: draws sampling a written target of
                                                               // their pass, in-pass barriers, pass restarts, layout
                                                               // transitions
} VkExecFrame;

// A record the scan of a chunked replay planned before an op: a contact shadow march (vk_contact.c: vk_contact_plan
// decides it in replay order, vk_contact_exec records it), or DLSS's copy of the lit scene or its evaluation
// (vk_dlaa.c: vk_dlaa_plan, vk_dlaa_exec). The chunk that holds the op records it.
enum { VK_JOB_CONTACT, VK_JOB_DLSS_MASK, VK_JOB_DLSS };
typedef struct VkExecJob {
    const void *op;                      // vk_exec.c: the op it precedes in a scanned submission
    int kind;                            // VK_JOB_*
    int chunk;                           // vk_exec.c: the chunk that holds the op
    VkExecTarget scene, depth;           // contact: the scene colour and the linear depth; DLSS: the light target to copy
                                         // (mask) or the scaling pass's target (evaluation; no tex: in place)
    float pc[16];                        // the march's push constants
} VkExecJob;

// An OP_UPDATE_UB of the submission: [lo, hi) of guest memory takes data (vk_exec_draw.c ubo_ptr reads them).
typedef struct { uint64_t lo, hi; const uint8_t *data; } VkExecUpd;

struct VkExecDraw;   // vk_exec_draw.c: the memos and command buffer state of one recording context

// The replay state and one recording context. NVN state is queue state and carries over from one submission to the
// next, as in MacNativeGames' and LAS Recomp's replays. The "per command buffer" fields are reset by vk_exec_record.
// The fields from rt up to views are that queue state (VK_EXEC_STATE_BEGIN/END); the rest belongs to the context that
// records with it.
typedef struct VkExec {
    // ---- per command buffer
    VkCommandBuffer cb;                  // being recorded
    uint64_t fence;                      // timeline value the submission signals (vk_completion_add)
    int recorded;                        // Vulkan commands recorded so far; every handler adds the ones it records
    int in_pass;                         // inside vkCmdBeginRendering
    uint32_t dirty;                      // VK_EXEC_DIRTY_*
    // Pass marks (vk.h vk_pass_mark_begin): the open pass's record (NULL: none or marks off), vk_exec_frame.draws when
    // it began, and the passes and dispatches this submission marked so far.
    VkPassMark *mark;
    uint64_t mark_draws0;
    uint32_t mark_passes, mark_dispatches;

    // ---- render targets (OP_SET_RT) and the current pass
    p_set_rt rt;                         // last OP_SET_RT payload (a change ends the open pass); rt.n 0, no depth: none
    int rt_resolved;                   // color[] and depth hold this command buffer's images for rt
    VkExecTarget color[8], depth;        // resolved targets
    uint32_t pass_w, pass_h, pass_layers;            // render area and layer count of the open (or next) pass
    uint32_t pass_scale;                             // its targets' VkTexImage.scale: game viewports and scissors
                                                     // are multiplied by it
    int pass_noatt;                                   // the pass has no attachment: its size follows the draws (vk_exec_noatt_dims)
    VkFormat pass_color[8], pass_depth, pass_stencil; // attachment formats of the pass (VkPipelineRenderingCreateInfo);
                                                      // VK_FORMAT_UNDEFINED: none
    uint32_t pass_ncolor, pass_samples;               // colorAttachmentCount and rasterization samples of the pass

    // ---- fixed-function state (vk_exec.c keeps it; vk_exec_draw.c records it before draws)
    float vp[16][6];                     // OP_VIEWPORTS: x, y, w, h, near, far
    int has_vp;                          // a viewport was set; else the render area
    p_scissor scissor;                   // OP_SCISSOR (scissor 0)
    int has_scissor;                     // a scissor was set; else unbounded
    float depth_range[2];                // OP_DEPTH_RANGE: overrides the viewports' near and far
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];   // OP_STENCIL per face (0 front, 1 back): compare mask, write mask, ref
    float point_size;                    // OP_BIND_STATE kind 6: NVN point size for the support buffer; 0 = never set = 1
    p_poly_offset poly_offset;           // OP_POLY_OFFSET: depth bias factor, units, clamp
    p_prim_restart prim_restart;         // OP_PRIM_RESTART: applied to indexed strip and fan draws (op_draw_idx)
    float blend_color[4];                // OP_BLEND_COLOR
    int64_t copy_stride;                 // OP_COPY_STRIDE: row stride of buffer <-> texture copies (vk_exec_xfer.c)
    const dpool_t *tex_pool, *samp_pool; // OP_SET_TEX_POOL, OP_SET_SAMP_POOL
    // Pipeline inputs. vk_exec.c writes depth_clamp (OP_DEPTH_CLAMP), raster_discard (OP_RASTER_DISCARD) and, at pass
    // begin, color_fmt, depth_fmt and samples; vk_exec_draw.c owns every other field (shaders, prim, y_flip, state
    // objects, vertex input).
    VkPipeState pipe;

    // ---- vk_exec_draw.c slots (draws, binds, dispatch): left empty by vk_exec.c
    const prog_t *prog;                  // OP_BIND_PROGRAM
    int32_t prog_stages;
    NVNbufferRange vbuf[16];             // OP_BIND_VBUF
    p_bind_buf ubo[6][18], ssbo[6][16];  // OP_BIND_UBO / OP_BIND_SSBO per stage and index
    uint64_t tex[6][128], samp[6][128], img[6][32];   // OP_BIND_TEX / OP_BIND_SAMP / OP_BIND_IMG handles per stage

    // ---- vk_exec_xfer.c slots (render enable, conditional rendering): left empty by vk_exec.c
    int render_disabled;                 // OP_RENDER_ENABLE false or a failed OP_RENDER_COND: clears and draws skip
    p_render_cond render_cond;           // last OP_RENDER_COND
    int has_render_cond;
    // samples-passed counting (vk_exec_xfer.c): a reset since the last report, a report since the last reset, the game
    // reported twice without a reset (every draw counts from then on)
    int oq_counting, oq_reported, oq_keep;

    // ---- internal to vk_exec.c
    VkExecFrame fr;                      // counters of this context, added to vk_exec_frame at the end of a submission
    int split_draws, part_start;         // draws and dispatches since the last part; recorded at its start
    uint64_t part_t0;                    // replay time at its start
    void *views;                         // image views created for this command buffer (freed at its fence)
    int nviews, cap_views;
    int barrier_at;                      // recorded right after the last full barrier (-1: none yet)

    // Feedback loops (vk.h VkCtx.feedback, vk_exec_draw.c feedback_prepare). Learned targets are attached in
    // ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT; they stay in it across consecutive passes and go back to GENERAL at the next
    // full barrier that is not a pass begin.
    VkImage fbl_img[9];                  // images in the feedback layout now
    VkImageAspectFlags fbl_aspect[9];
    int nfbl;
    uint32_t fbl_mask;                   // attachments of the open pass in that layout: bit i colour i, bit 8 depth
    uint32_t fb_dirty;                   // attachments of the open pass written by a draw or clear since the pass began
                                         // or the last in-pass barrier
    const void *fb_fs;                   // feedback_scan's memo: fragment shader, texture bind serial, pass serial
    uint32_t fb_serial, fb_pass, fb_sampled;
    uint32_t tex_serial, pass_serial;    // bumped by every texture or image bind and pass begin

    // ---- vk_exec_xfer.c: the occlusion query of the open pass and the queries closed since the last report or reset
    int oq_open;                         // slot of the active query, -1 none
    uint32_t *oq_seg, oq_nseg, oq_capseg;
    int oq_lost;                         // the segment missed a draw (ring full)

    struct VkExecDraw *draw;             // vk_exec_draw.c, made on the context's first draw op

    // ---- a chunk of a scanned submission (vk_exec.c): the scan's answers, taken in op order
    int chunked;                         // records a chunk: what depends on earlier chunks comes from the scan
    const VkExecUpd *upd;                // the submission's inline uniform updates
    const uint8_t *cond;                 // its RENDER_COND results (1 renders)
    const VkExecJob *jobs;               // its planned records (contact shadows, DLSS)
    uint32_t njobs;
    uint32_t upd_n, cond_i, job_i;       // updates, results and records before the op being recorded
    uint32_t drop_n;                     // ops before it that may write uniform ranges (vk_usnap_drop_at)
    const uint32_t *wpos;                // the positions of the draws and dispatches among those ops (vk_exec_draw_writes)
    uint32_t wpos_i, wpos_end;           // the next of them, and the end of the chunk's
    uint32_t pos;                        // ops of the submission up to and with it (the scan's too)
} VkExec;

#define VK_EXEC_STATE_BEGIN offsetof(VkExec, rt)
#define VK_EXEC_STATE_END offsetof(VkExec, fr)

// Begins dynamic rendering with the current targets unless a pass is open (a full memory barrier precedes it). Call
// before a draw or an attachment clear.
void vk_exec_pass_begin(VkExec *ex);
// Ends the open pass (no-op without one) and marks its targets GPU-written. Call before copies and dispatches.
void vk_exec_pass_end(VkExec *ex);
// Full memory barrier (all commands, all memory); ends the open pass first. Copies and dispatches order themselves with
// it. Skipped when nothing was recorded since the last one.
void vk_exec_barrier(VkExec *ex);
// UpdateUniformBuffer overlays (vk_exec_draw.c): the copies into the real buffers that updates inside a pass left for
// the next barrier (barrier_to records them first), and a GPU write of another kind into [addr, addr+n) of this
// submission (a copy, a fill, a storage write), which retires the overlays it touches.
void vk_exec_ub_flush(VkExec *ex);
void vk_exec_ub_foreign(VkExec *ex, uint64_t addr, uint64_t n);
void vk_exec_ub_perf(void);
// vk_texture.c (TOTK_TEX_ALIAS): commands that must run at the replay's current position. vk_exec_inline_begin returns
// the command buffer being recorded after a full barrier (an open pass is ended; *reopen says so), or VK_NULL_HANDLE
// when this thread is not recording a replay. vk_exec_inline_end begins the pass again when it was open.
// vk_exec_inline_buffer: a device buffer for transfers, destroyed when the submission completes.
VkCommandBuffer vk_exec_inline_begin(int *reopen);
void vk_exec_inline_end(int reopen);
// vk_exec_draw.c: forget what the command buffer being recorded has bound (pipeline, sets, dynamic state, vertex
// buffers): the replay continues in a new one (vk_sync_split).
void vk_exec_draw_new_cb(VkExec *ex);
// The recording threads of the chunked replay as this run read TOTK_REPLAY_WORKERS (0 off), -1 before the first
// submission read it (the settings menu shows it).
int vk_exec_replay_workers(void);
// vk_exec_draw.c, for the state scan of a chunked replay: the storage ranges the shaders of a draw (dispatch 0) or a
// dispatch at state s write, as {address, size} into out; their count, or -1 when there are more than max or a stage's
// shader is not known yet (no context has translated it). Reads uniform blocks through s->upd like a chunk.
int vk_exec_draw_writes(const VkExec *s, int dispatch, uint64_t (*out)[2], int max);
// vk_exec_draw.c: the vertex and fragment shader of the last draw recorded into this command buffer (NULL: none yet).
void vk_exec_draw_shaders(const VkExec *ex, const GfxShader **vs, const GfxShader **fs);
VkBuffer vk_exec_inline_buffer(VkDeviceSize size);
// vk_exec.c: the stand-in a 3D BC render target is drawn through goes with its image (vk_exec_image_gone).
void vk_exec_bc_gone(VkImage image);
// vk_hdr.c: with TOTK_HDR=1, notes a draw into a presented window texture (vk_hdr_track: set by the first present).
extern int vk_hdr_track;
void vk_hdr_draw(VkExec *ex, const VkPipeInfo *info);
// Scissor 0 clipped to the render area of the open pass; extent 0: nothing to draw.
VkRect2D vk_exec_scissor(const VkExec *ex);
void vk_exec_noatt_dims(const VkExec *ex, uint32_t *w, uint32_t *h);   // pass size when no attachment is set
// 1: every target of the current pass (any 1: at least one, and not only the depth target while a colour target is
// rendered for the first time) was rendered in the previous presented frame (per-frame targets); 0 otherwise, or when
// the pass has none.
int vk_exec_targets_recent(VkExec *ex, int any);
// The current pass's targets with their formats, extents and the frames they were last rendered in (diagnostics).
void vk_exec_targets_describe(VkExec *ex, char *buf, size_t cap);

extern VkExecFrame vk_exec_frame;
void vk_exec_frame_written(const tex_t *t);                    // vk_texture_gpu_written: t rendered or copied into
uint64_t vk_exec_recording_fence(void);                        // fence of the submission this thread records, else 0
uint32_t vk_exec_replay_pos(void);                             // the op this thread records in a chunk, else UINT32_MAX
// A chunk that is about to make a texture entry whose content depends on the textures at its storage (vk_texture.c
// entry_of): whether a chunk before it is still being recorded, and the wait until none is. The caller holds no lock
// but the texture lock, which it releases for the wait.
int vk_exec_chunk_behind(void);
void vk_exec_chunk_settle(void);
void vk_exec_frame_log(const tex_t *presented, uint64_t frame);

// TOTK_DUMP=<frame> (vk_dump.c): between presents frame-1 and frame, every pass's targets and every dispatch's storage
// images are copied out and written as PNGs to TOTK_DUMP_DIR (default "dump"), with passes.txt (targets, pipelines,
// textures, samplers) and images.txt (per image value statistics). The hooks run only when vk_dump_armed is set.
extern int vk_dump_armed;
void vk_dump_init(void);                 // reads the environment; vk_exec_record's first call
void vk_dump_pass_end(VkExec *ex);       // after vkCmdEndRendering
void vk_dump_draw(VkExec *ex, const VkPipeState *s, VkPipeline p, const VkPipeInfo *info);   // after a draw's binds
void vk_dump_dispatch(VkExec *ex, const GfxShader *cs, VkPipeline p, const VkPipeInfo *info, const p_dispatch *d);
void vk_dump_copy(VkExec *ex, uint32_t op, const void *payload);   // before a copy or texture clear is replayed
void vk_dump_present(uint64_t frame);    // vk_be_present: waits for the dump frame's copies and closes the logs

// c0, the NVN driver constant bank of one stage (vk_exec_draw.c; TOTK_FIX_C0=0 binds zeros instead): bank is
// VK_C0_BYTES, stage an NVN stage (0 vertex, 1 fragment, 5 compute), ssbo and ubo the 16 bound slots of that stage.
#define VK_C0_BYTES 0x800u
void vk_c0_fill(uint8_t bank[VK_C0_BYTES], int stage, const p_bind_buf ssbo[16], const p_bind_buf ubo[16],
                const uint32_t dispatch[3], uint32_t base_instance);

// Handler hooks. vk_exec.c defines both WEAK: they stop with rt_fatal naming the op.
// vk_exec_draw.c: OP_DRAW, OP_DRAW_IDX, OP_DISPATCH, OP_BIND_* (program, state, vattr, vstream, vbuf, ubo, ssbo, tex,
// samp, img), OP_UPDATE_UB, OP_DRAW_IDX_IND, OP_DISPATCH_IND. payload: nvn_op_payload of the record.
void vk_exec_op_draw(VkExec *ex, uint32_t op, const void *payload);
int vk_exec_bind(VkExec *ex, uint32_t op, const void *payload);   // a bind op's writes into ex; 1: VkExecDraw.bind_gen
// The aspect a sampled view of img reads (vk_exec_draw.c get_view): stencil for a view in STENCIL depth-stencil mode.
VkImageAspectFlags vk_exec_view_aspect(const VkTexImage *img, const view_t *v, int hv);
VkImageView vk_exec_target_view(const VkTexImage *img, VkFormat format, VkImageViewType type, uint32_t aspect,
                                uint32_t level, uint32_t base, uint32_t layers, int blocks);   // kept for the image's life
// vk_exec_xfer.c: OP_COPY_B2B, OP_COPY_B2T, OP_COPY_T2B, OP_COPY_T2T, OP_CLEAR_TEX, OP_SIGNAL_EVENT, OP_REPORT_COUNTER,
// OP_RESET_COUNTER, OP_RENDER_ENABLE, OP_RENDER_COND, OP_CLEAR_BUF.
void vk_exec_op_xfer(VkExec *ex, uint32_t op, const void *payload);
// vk_dlaa.c: once DLSS has scaled the scene this frame, passes larger than the scene's light target vk_dlaa_swap_img
// sample DLSS's output vk_dlaa_swap_view in its place, so the game's scaling draw scales DLSS's picture.
extern const VkTexImage *vk_dlaa_swap_img;
extern VkImageView vk_dlaa_swap_view;
// The state scan's part of the transfer ops: render enable, conditional rendering (returns the condition: 1 renders),
// samples-passed counting state and the pending reports conditions read. Returns 1 for every other op.
// The state scan of a chunked replay (vk_exec.c): the textures op uses go live in replay order (vk_texture_revive); pos:
// the op's place in the submission. 1 when one of them has no entry yet.
int vk_exec_xfer_revive(uint32_t op, const void *payload, uint32_t pos);
int vk_exec_xfer_scan(VkExec *ex, uint32_t op, const void *payload);
// The guest memory a COPY_B2B, COPY_T2B or CLEAR_BUF op makes the GPU write, as far as the scan can tell without the
// texture's image: [*addr, *addr + return value).
uint64_t vk_exec_xfer_written(const VkExec *ex, uint32_t op, const void *payload, uint64_t *addr);
// vk_contact.c: a pass about to begin (its targets resolved, no pass open), and a dispatch or copy about to be recorded.
// vk_contact_plan is what they do in replay order (pass: the first; else the second), for the scan; 1: a march is due (j).
void vk_contact_pass_begin(VkExec *ex);
void vk_contact_flush(VkExec *ex);
int vk_contact_plan(const VkExec *ex, int pass, VkExecJob *j);
void vk_contact_exec(VkExec *ex, const VkExecJob *j);
// vk_exec_xfer.c, samples-passed counting: a draw about to be recorded in the open pass (begins an occlusion query
// when counting and none is active), and the open pass about to end (ends the active query).
void vk_exec_oq_draw(VkExec *ex);
void vk_exec_oq_pass_end(VkExec *ex);
void vk_exec_oq_forget(VkExec *ex);
void vk_exec_oq_flush(VkExec *ex);    // the end of a chunk: its closed queries counted as a segment without a report   // vk_exec_xfer_shutdown: the context's queries belonged to the old pool
// vk_exec.c: fn on every recording context (teardown, the device idle).
void vk_exec_contexts(void (*fn)(VkExec *ex));
