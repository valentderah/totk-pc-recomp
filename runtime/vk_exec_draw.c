// Draws, binds and dispatches of the Vulkan op-stream replay: the strong
// vk_exec_op_draw behind vk_exec.c's weak hook. The bind ops fill the VkExec slots; a draw opens the pass, resolves the
// bound program to translated shaders (the translator's answers come from the bound state), asks vk_pipeline.c for the
// pipeline, pushes (or allocates) set 0 as VkPipeInfo.bindings says and records the dynamic state, vertex and index
// buffers and the draw. A dispatch ends the pass and does the same for the compute shader. UPDATE_UB is a buffer copy
// from transient memory between full barriers.
//
// Per command buffer the replay keeps transient memory (support buffers, copied uniform ranges, generated indices),
// descriptor pools, image views and the storage-buffer ranges the GPU may write; all of it is freed, and the ranges are
// reported with vk_gpu_written, from one completion callback once the submission's fence is done (the write watch
// does not see GPU writes). The memos and per-command-buffer state belong to one recording context (VkExecDraw); the
// tables the contexts of a chunked replay share (stand-ins, depth twins, views) take their own locks.
//
// Provenance: the op semantics (bind ops, program and slot conventions, draw and dispatch flow, vertex streams with a
// zeroed buffer for unbound ones) follow MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m:7409-7460,
// 9365-9488, 9489-9921, 10013-10079 @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou); the uniform-buffer spans, texture and
// sampler descriptor lookups and the dispatch flow follow LAS Recomp runtime/nvn/vulkan/exec.c:1851-1908, 2300-2516
// @ da35b83a (MIT, Copyright (c) 2026 Peiikai). The support buffer layout is Ryujinx's SupportBuffer.cs (MIT).
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "vk.h"
#include "vk_exec.h"
#include "platform/platform.h"
#include "vk_dlaa.h"
#include "memx.h"

#define MAX_BINDINGS 128
#define MAX_DESC 1024
#define CHUNK_BYTES (4u << 20)
#define ZERO_BYTES 65536u
#define SUPPORT_BYTES 1456u   // Ryujinx SupportBuffer (std140): alpha_test, is_bgra[8], viewport_inverse, viewport_size, ...;
                              // 1408 pcss and 1424 shadow_spread (translator patches 0010, 0011), 1440 HDR (support_fill)
#define NVN_STAGE_VS 0
#define NVN_STAGE_FS 1
#define NVN_STAGE_CS 5
#define ONESHOT_WAIT_MS 30000u   // prepare_draw: how long a one-shot draw waits for its pipeline before it is skipped

enum { W_PROGRAM, W_SHADER, W_PIPELINE, W_NO_VS, W_INDEX, W_UB, W_BIND, W_SSBO_ALIGN, W_SSBO_SLOT, W_RESTART, W_INDIRECT, W_COUNT };

static void warn_once(int key, const char *fmt, ...) {
    static volatile int32_t seen[W_COUNT];
    if (__atomic_exchange_n(&seen[key], 1, __ATOMIC_RELAXED)) return;
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    rt_log("vk: %s (logged once)", msg);
}

static void *grow(void *p, int *cap, size_t elem) {
    *cap = *cap ? *cap * 2 : 8;
    p = realloc(p, elem * (size_t)*cap);
    if (!p) rt_fatal("vk: out of memory");
    return p;
}

// ---- resources of one command buffer, freed by its completion callback

typedef struct { VkBuffer buf; VmaAllocation alloc; uint8_t *map; uint64_t size, used; } Chunk;
typedef struct {
    VkImage image;
    VkFormat format;
    VkImageViewType type;
    uint32_t aspect, level0, levels, layer0, layers, swz;
} ViewKey;
_Static_assert(sizeof(ViewKey) == 40, "ViewKey is hashed as bytes: no padding");
typedef struct { uint64_t addr, size; } Range;
typedef struct {
    Chunk *chunks;
    int nchunks, cchunks;
    VkDescriptorPool *pools;
    int npools, cpools;
    Range *written;
    int nwritten, cwritten;
} CbRes;

// Descriptor pools and full-size transient chunks go back to these lists when their command buffer completes, instead
// of being destroyed and created again for the next one: each creation is a driver memory allocation (on Windows an
// allocation, a map and a paging-fence wait on the CPU), about ten of them per frame inside the replay.
#define SPARE_POOLS 64
#define SPARE_CHUNKS 16
static PlatMutex spare_lock = PLAT_MUTEX_INIT;
static VkDescriptorPool spare_pool[SPARE_POOLS];
static Chunk spare_chunk[SPARE_CHUNKS];
static int nspare_pool, nspare_chunk;

// State of the command buffer being recorded that the Vulkan command buffer itself holds.
typedef struct {
    VkPipeline pipe[2];                  // bound per bind point: 0 graphics, 1 compute
    VkPipeState st;                      // the state vk_pipeline_set_dynamic last recorded
    int st_valid;
    int st_pm;                           // st equals PipeMemo.s (set where both were the draw's state, cleared at
                                         // every write of PipeMemo): TOTK_BIND_INC skips the compare of st
    int dyn_valid;                       // vk_pipeline_set_dynamic_delta's record of this command buffer's dynamic state
                                         // holds (TOTK_DYN_DELTA); pipeline binds keep it, the memset clears it
    uint32_t vmask;                      // the vertex bindings st's vertex input reads (valid with st_valid)
    uint32_t vres;                       // the streams whose vb entry was resolved from vin (others: the zero buffer)
    VkBuffer vb[17];                     // vertex buffers last bound
    VkDeviceSize vo[17], vsz[17], vstr[17];
    int vcls[17];                        // their pool class and pool record, for the census
    void *vpool[17];
    NVNbufferRange vin[16];              // the ranges they were resolved from, under vk_mem_gen vgen
    uint64_t vgen;
    int vb_valid;
    uint8_t support[SUPPORT_BYTES];      // last support buffer and where it lives
    float sup_in[3];                     // the support_fill arguments it was filled from
    VkBuffer sup_buf;
    VkDeviceSize sup_off;
    int sup_valid;
    uint8_t c0[3][VK_C0_BYTES];          // last c0 bank per stage (vertex, fragment, compute) and where it lives
    VkBuffer c0_buf[3];
    VkDeviceSize c0_off[3];
    int c0_valid[3];
    struct { p_bind_buf ssbo[16], ubo[16]; uint32_t dim[3], binst; } c0_in[3];   // what vk_c0_fill read for c0[k]
    VkPipelineLayout ds_layout;          // graphics layout the sets below were last pushed or bound with
    VkDescriptorSet ds_bound[2];         // allocated set bound per set number (0: none or pushed since)
    VkDescriptorSet ds_last[2];          // last allocated and written set per set number, contents in DS
    VkDescriptorSetLayout ds_last_layout[2];
    int ds_pushed[2];                    // set number holds the push of DS.psig under ds_layout
    int res_valid;                       // the graphics sets bound hold what the inputs in RS give (bind_resources)
    int bi_valid;                        // TOTK_BIND_INC: VkExecDraw.BL holds this command buffer's last prepared draw
} CbState;

// Contents of CbState.ds_last: the set's packed descriptors (VkDescSlot), which with its layout determine every write. A draw
// whose allocated set would hold the same reuses the set instead of allocating and writing one (a per-draw
// cost). psig: the same for the last push per set number; a push equal to it under the same layout is not recorded
// again (the descriptors stay bound across pipeline binds of that layout).
#define DS_SIG_BYTES (MAX_DESC * sizeof(VkDescSlot))
typedef struct { uint32_t n[2], pn[2]; uint8_t sig[2][DS_SIG_BYTES], psig[2][DS_SIG_BYTES]; } DescSigs;

// TOTK_BIND_STATS counters (bind_stats).
typedef struct {
    uint64_t draws, bind[2], desc[2], push, push_skip, set_new, set_reuse, ac_miss, same_prog, same_all, bind_skip;
} BindStats;

// TOTK_DYN_DELTA (prepare_draw): the mode and, for the perf line, the set_dynamic calls, the commands they recorded
// (a full call counts 1) and the calls that recorded none.
static struct { int mode; uint64_t calls, cmds, empty; } DD = {-1};

// The inputs of the last graphics bind_resources (bind_sig) and what its build counted for the traffic census, which a
// skipped build counts again. RS_BYTES bounds the bound-slot bytes a pipeline reads (RsList): per binding at most one
// c0 storage table, otherwise one bound range or handle per descriptor.
#define RS_BYTES (MAX_BINDINGS * sizeof(((VkExec *)0)->ssbo[0]) + MAX_DESC * sizeof(p_bind_buf))
typedef struct { int cls, use; uint64_t bytes; } RsCount;
typedef struct {
    uint32_t hn;                         // words of h the last signature filled; 0: none
    uint64_t h[24];                      // generations, feedback images, support inputs, base instance
    const VkPipeInfo *info;              // the pipeline whose RsList ranges body holds, as of bind op generation gen
    uint64_t gen;
    uint8_t body[RS_BYTES];
    int rec;                             // a graphics build is running: count_use records, scratch copies mark volatile
    int volatile_;                       // the build copied guest bytes into transient memory: a rebuild differs
    RsCount cnt[2 * VK_MCLS_N], pcnt[2 * VK_MCLS_N];
    int ncnt, pncnt, cnt_over;
    int cmds;                            // check mode: pushes, binds and set writes the build recorded
    uint32_t ckn;                        // check mode: the last build's packed descriptors (bytes in ck)
    uint8_t ck[DS_SIG_BYTES];
} RsState;

// UpdateUniformBuffer overlays of the submission being replayed (op_update_ub, TOTK_UB_OVERLAY). Here because
// bind_sig reads them (ov_at).
#define OV_N 2048
#define OV_FOREIGN 4096
static struct {
    int mode;                  // rt_knob: 0 off, 1 on, 2 ab; -1 unread
    uint64_t fence;            // the submission the lists belong to
    uint64_t serial;           // numbers the overlays, so bind_sig tells one from the next at the same range
    uint64_t gen;              // bumped by every change of the list o (TOTK_BIND_INC: a binding may read other bytes)
    uint64_t lo, hi;           // the span of the overlays: most bindings lie outside it
    uint32_t n, np, nf;
    int f_over;                // more foreign writes than f holds: no more overlays in this submission
    struct { uint64_t lo, hi, serial; VkBuffer buf; VkDeviceSize off; } o[OV_N];   // oldest first, no two equal
    struct { uint64_t dst; VkBuffer src; VkDeviceSize soff; VkMemSpan sp; uint64_t size; } p[OV_N];   // to copy
    struct { uint64_t lo, hi; } f[OV_FOREIGN];   // GPU writes of other kinds
    // since the last perf line (vk_exec_ub_perf)
    uint64_t c_update, c_overlaid, c_overlaid_out, c_flush, c_bind, c_merged, c_mixed, c_inpass, c_outside, c_early,
        c_order;
} OV = {.mode = -1};

// A binding without a size reaches to the end of its backing: as far as the overlays go, for ov_at.
static uint64_t ov_rest(uint64_t addr) { return addr < OV.hi ? OV.hi - addr : 1; }

// The serial of the newest overlay a binding of [addr, addr+n) reads from, 0 for none (bind_sig).
static uint64_t ov_at(const VkExec *ex, uint64_t addr, uint64_t n) {
    if (!OV.n || OV.fence != ex->fence || addr >= OV.hi || addr + n <= OV.lo) return 0;
    for (uint32_t i = OV.n; i-- > 0;)
        if (addr < OV.o[i].hi && addr + n > OV.o[i].lo) return OV.o[i].serial;
    return 0;
}

// The bound-slot bytes of VkExec a pipeline's descriptors read, as ranges merged where they touch, built once per
// pipeline info: c0's storage table, the uniform and storage ranges of the bound slots and the texture and image
// handles. sup and c0: the support inputs and the base instance count too. p: storage buffers whose address the shader
// loads from a user uniform block; the address read from the block counts too. ok 0: never skipped (too many
// descriptors or such buffers).
#define RS_PTRS 4
typedef struct { uint32_t off, len; } RsRange;
typedef struct { int st; const GfxShaderRes *res; } RsPtr;
// TOTK_BIND_INC: the bound slots a graphics pipeline's descriptors read, per stage (0 vertex, 1 fragment). always:
// descriptors rebuilt on every draw (written storage buffers and those whose address a uniform block holds).
typedef struct {
    uint32_t ubo[2];                     // uniform slots 3..20 (bit slot - 3: the OP_BIND_UBO index)
    uint32_t ssbo[2];                    // storage slots read through c0's table
    uint64_t tex[2][2];                  // texture handle slots 0..127
    uint64_t ftex[2];                    // every fragment texture slot, buffer textures too (feedback_scan reads them)
    uint32_t img[2];                     // image handle slots 0..31
    uint8_t c0[2], c1[2];                // reads c0 (every storage slot and the base instance), the compiler constants
    uint8_t gfx;                         // every binding is a vertex or fragment one
    uint16_t always, ndesc;
    uint8_t wr;                          // a storage binding the shaders write: its build has side effects, nothing is kept
} BiMask;
// TOTK_BIND_INC: per binding of a graphics pipeline, the marks and generations whose change makes bind_resources build
// it again (bi_tests). f: BT_* flags; st: the stage the masks are of; the masks: the slots of that stage it reads.
enum { BT_ALWAYS = 1, BT_C0 = 2, BT_C1 = 4, BT_BUF = 8, BT_TEX = 16 };
typedef struct { uint8_t f, st, dep; uint32_t ubo, ssbo, img; uint64_t tex[2]; } BiTest;
typedef struct { uint32_t n, bytes; uint8_t sup, c0, ok, np; RsPtr p[RS_PTRS]; BiMask m; BiTest *t; RsRange r[]; } RsList;
// (program, stage, answers) -> shader, 4-way set-associative, most recently used first. A miss costs a SHA-256 of the
// whole shader code (gfx_shader.c req_init), slow on CPUs without SHA instructions: a field frame looks up ~3000 and
// the 2048-slot direct-mapped table this replaced missed 3% of them on conflicts.
#define SHADER_SETS 2048
#define SHADER_WAYS 4
typedef struct { uint64_t serial, h; int stage; int used; GfxShaderAnswers a; const GfxShader *s; } ShaderEnt;   // h: hash of a

// Which shader of a program serves each NVN stage, and its code length, per program serial (new at every
// nvnProgramSetShaders): the scan reads two words of every stage's control block, cold guest cache lines, and runs
// several times a draw. Direct-mapped; a collision only scans again.
#define STAGEM_N 512
typedef struct { uint64_t serial; int8_t idx[6]; uint32_t len[6]; } StageMemo;

// Texture and image types of a stage's first 32 slots, each kept with the handle and the generation and slot token it
// was looked up under: a slot is looked up again, a hash probe, only when the handle changed or its slot may resolve
// otherwise (slot_still).
typedef struct { uint64_t tex[32], img[32], ttok[32], itok[32]; uint32_t tgen[32], igen[32]; uint8_t tt[32], it[32]; } AcSlots;

// The slots of (program, stage) whose types its translation reads (gfx_shader_slot_mask), learned from its first
// shader; ~0u while unknown. The types of other slots select the same shader, so answers_for leaves them zero instead
// of looking up all 64 handles. Direct-mapped by serial and stage; a collision only relearns.
#define SLOTM_N 4096
typedef struct { uint64_t serial; int st; uint32_t m; } SlotMask;

// The inline uniform updates of the submission being replayed (upd_note).
#define UPD_N 4096
typedef struct {
    uint64_t fence;                      // the submission the entries belong to
    uint32_t n;                          // updates noted in it
    VkExecUpd u[UPD_N];
} UpdRing;

// The image and the levels [l0, l1) and layers [a0, a1) a texture's view covers.
typedef struct { VkImage image; uint32_t l0, l1, a0, a1; } SubRange;

// The subresource a texture handle samples (feedback_scan).
#define FM_N 8192
typedef struct { uint64_t h, tok; uint32_t gen; SubRange r; } FbMemo;

// Sampled-texture descriptors per (handle, resource type, separate) (tex_info_lookup).
#define TD_N 8192
typedef struct {
    uint64_t h, tok;
    uint32_t gen, epoch, sgen;
    uint16_t type;
    uint8_t separate, valid;
    VkImage image;
    VkDescriptorImageInfo out;
} TexMemo;

// The previous draw's shaders and what they were looked up from (prepare_draw).
typedef struct {
    const prog_t *prog;
    uint64_t serial;
    uint32_t gen, tex_serial;
    uint64_t pipe_gen;                   // VkExec.pipe_gen when pipe was copied
    uint64_t tex[2][32], img[2][32];
    VkPipeState pipe;
} MemoKey;
typedef struct { int valid, has_fs; MemoKey k; const GfxShader *vs, *fs; } ShaderMemo;

// The previous draw's pipeline and the state it was found for (prepare_draw). gen: VkExec.pipe_gen when s was built
// from VkExec.pipe.
typedef struct { int valid; VkPipeState s; const VkPipeInfo *info; uint64_t gen; } PipeMemo;
// The pipelines found before, by state: vk_pipeline_graphics_async takes the pipeline cache's lock, which the contexts
// of a chunked replay would take on every draw whose state differs from the draw before. Direct-mapped.
#define PC_N 1024
typedef struct { const VkPipeInfo *info; VkPipeState s; } PipeCache;

// TOTK_BIND_INC (section before prepare_draw): the last prepared graphics draw of the command buffer, as far as the
// next draw's decisions compare it (valid with CbState.bi_valid), and the census of those decisions.
#define BI_MV 10
typedef struct {
    const VkPipeInfo *info;
    const prog_t *prog;
    uint64_t serial, pipe_gen;
    uint64_t gt[4], gb[4];               // bi_gt, bi_gb before its build
    uint32_t binst, slots_gen, pass_serial, tex_serial;
    const GfxShader *vs, *fs;
    uint8_t sx[4];                       // the state fields a draw sets: prim, restart, feedback, depth_minus_w
    uint8_t sm_ok;                       // the shader memo (SM) holds this draw's shaders and lookup inputs
    uint8_t fb_ok;                       // VkExec's feedback scan memo (fb_fs ...) holds this draw's scan
    uint32_t vmask, vused;               // vused: the streams its bind_vertex_buffers resolved
    uint64_t vgen;
} BiLast;
typedef struct {
    uint64_t frame;                      // gfx_counters.frames at the last draw counted
    uint32_t frames;                     // frames since the last line
    uint64_t draws, cls[4], b_gen, desc, rec[4], always, mv[BI_MV], slots[5];
    uint64_t sh_keep, sh_memo, fb_n, fb_keep, fb_memo, vb_skip, pg_hit;
    uint64_t mism, mism_total, checked;
    uint32_t mism_frame, frame_lines;
} BiCensus;
enum { BIM_PIPE, BIM_DYN, BIM_SHADER, BIM_FB, BIM_VB, BIM_DESC, BIM_N };   // check mode's kinds of difference
static int bind_inc_mode(void);
static void bi_mismatch(struct VkExec *ex, int kind, const char *fmt, ...);

// TOTK_BIND_INC: what building one descriptor counted (count_use; n 0: nothing, more than 1: never kept) and whether it
// copied guest bytes into transient memory (vol: a rebuild may differ, so it is never kept).
typedef struct { uint8_t vol, n; int16_t cls; int32_t use; uint64_t bytes; } BiMeta;
// The packed descriptors of the last graphics build (bind_resources writes them here while TOTK_BIND_INC is set), the
// pipeline they are for (valid with CbState.bi_valid) and, per descriptor, BiMeta.
typedef struct {
    const VkPipeInfo *info;
    BiMeta *cur;                         // count_use and the transient copies record here while a descriptor is built
    VkDescSlot d[MAX_DESC];
    BiMeta meta[MAX_DESC];
    VkDescSlot old[MAX_DESC];            // check mode: the descriptors before the build
    BiMeta mold[MAX_DESC];
    uint8_t vol[MAX_BINDINGS];           // per binding: a descriptor of its last build has BiMeta.vol
} BiDesc;

// TOTK_BIND_INC on: graphics descriptor sets allocated in batches (alloc_set_batched). Adapted from Savage Wind Recomp
// runtime/nvn/vulkan/exec.c vk_alloc_set (https://github.com/Revivedx/SavageWindRecomp @
// 740b18025fe11ba03c117dbb56668db304b5043e, MIT, Copyright (c) 2026 Revivedx and the Savage Wind Recomp contributors,
// Copyright (c) 2026 Peiikai).
#define ASET_CACHE 16
#define ASET_BATCH 16
typedef struct { VkDescriptorSetLayout layout; uint64_t fence; int n, next; VkDescriptorSet set[ASET_BATCH]; } ASetCache;

// The memos and command buffer state of one recording context (VkExec.draw). Nothing in it is shared: the context's
// thread reads and writes it without a lock.
typedef struct VkExecDraw {
    CbRes *cur;                          // resources of the submission being recorded
    uint64_t cur_fence;
    CbState B;
    DescSigs DS;
    BindStats BS;
    RsState RS;
    BiLast BL;
    BiCensus BC;
    BiDesc BI;
    ASetCache aset[ASET_CACHE];
    nvn_hmap rs_lists;                   // VkPipeInfo pointer -> RsList, until shutdown
    const VkPipeInfo *rs_last;           // the last lookup
    const RsList *rs_last_l;
    uint32_t c0_dim[3], c0_binst;        // the dispatch size and base instance of the draw or dispatch being bound
    uint64_t bind_gen;                   // bumped by every bind op that writes ex->prog, ubo, ssbo, tex or img
    int twin_dispatch;                   // op_dispatch*: the resources being bound are a dispatch's
    uint64_t wait_frame, wait_left_ns;   // prepare_draw: full-screen pipeline waits left in this frame
    ShaderEnt shader_tab[SHADER_SETS][SHADER_WAYS];
    StageMemo stagem[STAGEM_N];
    AcSlots AC[6];
    SlotMask slotm[SLOTM_N];
    FbMemo FM[FM_N];
    TexMemo TD[TD_N];
    ShaderMemo SM;
    PipeMemo PM;
    PipeCache PC[PC_N];
    UpdRing UPD;
    VkWriteDescriptorSet w[MAX_BINDINGS];   // bind_resources' scratch
    VkDescSlot d[MAX_DESC];
    VkBufferView bv[MAX_DESC];
    RsRange rs_tmp[MAX_DESC];               // rs_list's
} VkExecDraw;

static VkExecDraw *draw_new(VkExec *ex) {
    VkExecDraw *D = calloc(1, sizeof *D);
    if (!D) rt_fatal("vk: out of memory (replay context)");
    D->bind_gen = 1;
    return ex->draw = D;
}

static void cb_done(void *arg) {   // completion thread
    CbRes *r = arg;
    VkCtx *c = vk_ctx();
    for (int i = 0; i < r->nwritten; i++) vk_gpu_written(r->written[i].addr, r->written[i].size);
    for (int i = 0; i < r->npools; i++) {
        vkResetDescriptorPool(c->device, r->pools[i], 0);
        plat_mutex_lock(&spare_lock);
        int kept = nspare_pool < SPARE_POOLS;
        if (kept) spare_pool[nspare_pool++] = r->pools[i];
        plat_mutex_unlock(&spare_lock);
        if (!kept) vkDestroyDescriptorPool(c->device, r->pools[i], NULL);
    }
    for (int i = 0; i < r->nchunks; i++) {
        plat_mutex_lock(&spare_lock);
        int kept = r->chunks[i].size == CHUNK_BYTES && nspare_chunk < SPARE_CHUNKS;
        if (kept) spare_chunk[nspare_chunk++] = r->chunks[i];
        plat_mutex_unlock(&spare_lock);
        if (!kept) vmaDestroyBuffer(c->vma, r->chunks[i].buf, r->chunks[i].alloc);
    }
    free(r->written), free(r->pools), free(r->chunks), free(r);
}

// Also after the bound state was replaced (a chunk's start): bind_sig compares the bound slots again.
void vk_exec_draw_new_cb(VkExec *ex) {
    vk_exec_dirty_all(ex);
    if (!ex->draw) return;
    memset(&ex->draw->B, 0, sizeof ex->draw->B);
    ex->draw->bind_gen++;
}

void vk_exec_draw_shaders(const VkExec *ex, const GfxShader **vs, const GfxShader **fs) {
    int valid = ex->draw && ex->draw->B.st_valid;
    *vs = valid ? ex->draw->B.st.vs : NULL;
    *fs = valid ? ex->draw->B.st.fs : NULL;
}

// Marks a dispatch about to be recorded (vk.h vk_pass_mark_begin); the caller ends it after the dispatch.
static VkPassMark *dispatch_mark(VkExec *ex, const GfxShader *cs, uint32_t x, uint32_t y, uint32_t z) {
    VkPassMark *m = vk_pass_mark_begin(ex->cb, VK_MARK_DISPATCH, ex->fence);
    if (!m) return NULL;
    m->step = ex->mark_dispatches++;
    m->w = x, m->h = y, m->d = z, m->draws = 0, m->ncolor = 0;
    m->first[0] = cs, m->first[1] = m->last[0] = m->last[1] = NULL;
    return m;
}

static CbRes *cb_res(VkExec *ex) {
    VkExecDraw *D = ex->draw;
    if (D->cur && D->cur_fence == ex->fence) return D->cur;
    D->cur = calloc(1, sizeof *D->cur);
    if (!D->cur) rt_fatal("vk: out of memory");
    D->cur_fence = ex->fence;
    memset(&D->B, 0, sizeof D->B);
    vk_completion_add(ex->fence, cb_done, D->cur);
    return D->cur;
}

typedef struct { const VkBufferCreateInfo *bi; const VmaAllocationCreateInfo *ai; Chunk *c; VmaAllocationInfo info;
                 VkResult r; } ChunkTry;
static int chunk_try(void *arg) {
    ChunkTry *t = arg;
    vk_mem_collect();   // the fence thread's collect waits while the caller holds cmd_lock
    t->r = rt_oom_forced(RT_OOM_CHUNK) ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                                       : vmaCreateBuffer(vk_ctx()->vma, t->bi, t->ai, &t->c->buf, &t->c->alloc, &t->info);
    return t->r == VK_SUCCESS;
}

static uint8_t *scratch(VkExec *ex, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    CbRes *r = cb_res(ex);
    size = (size + 255) & ~255ull;
    Chunk *c = r->nchunks ? &r->chunks[r->nchunks - 1] : NULL;
    if (!c || c->used + size > c->size) {
        if (r->nchunks == r->cchunks) r->chunks = grow(r->chunks, &r->cchunks, sizeof *r->chunks);
        c = &r->chunks[r->nchunks];
        uint64_t n = size > CHUNK_BYTES ? size : CHUNK_BYTES;
        int reused = 0;
        if (n == CHUNK_BYTES) {
            plat_mutex_lock(&spare_lock);
            if (nspare_chunk) *c = spare_chunk[--nspare_chunk], reused = 1;
            plat_mutex_unlock(&spare_lock);
        }
        if (reused) {
            c->used = 0;
            r->nchunks++;
            *buf = c->buf;
            *off = 0;
            c->used = size;
            return c->map;
        }
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n,
                                 .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        // HOST_COHERENT: the CPU fills these chunks and never flushes them. Intel's integrated GPUs (ANV, Meteor Lake)
        // offer host-cached memory only without coherence, which VMA picks for random host access: the GPU then read
        // what an earlier submission left in the chunk (an UpdateUniformBuffer payload of another draw: a lighting
        // shader's step count of 0x3f800000 instead of 128, and a loop of a billion steps).
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                      .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
        // Out of memory: the recording waits for memory (rt_oom_wait) with what it holds (the texture lock at most,
        // which vk_oom_release takes before cmd_lock); new chunks are made only when the spare ones ran out.
        ChunkTry ct = {.bi = &bi, .ai = &ai, .c = c};
        ct.r = rt_oom_forced(RT_OOM_CHUNK) ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                                           : vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &c->buf, &c->alloc, &ct.info);
        if (ct.r == VK_ERROR_OUT_OF_HOST_MEMORY || ct.r == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            vk_oom_release();
            rt_oom_wait("vk: transient buffer", n, chunk_try, &ct);
        }
        if (ct.r != VK_SUCCESS) rt_fatal("vk: transient buffer of %llu bytes failed (%d)", (unsigned long long)n, (int)ct.r);
        c->map = ct.info.pMappedData, c->size = n, c->used = 0;
        r->nchunks++;
    }
    *buf = c->buf;
    *off = c->used;
    uint8_t *p = c->map + c->used;
    c->used += size;
    return p;
}

static int ub_bind(VkExec *ex, uint64_t addr, uint64_t n, uint64_t align, VkDescriptorBufferInfo *out);
static void ub_written(VkExec *ex, uint64_t addr, uint64_t n);

static void note_written(VkExec *ex, uint64_t addr, uint64_t size) {
    CbRes *r = cb_res(ex);
    for (int i = 0; i < r->nwritten; i++)
        if (r->written[i].addr == addr && r->written[i].size == size) return;
    if (r->nwritten == r->cwritten) r->written = grow(r->written, &r->cwritten, sizeof *r->written);
    r->written[r->nwritten++] = (Range){addr, size};
    vk_mem_gpu_pending(addr, size);
}

// ---- stand-ins for what a binding names but the game did not bind (zeros, a 1x1 image, the default sampler)

enum { DV_1D, DV_1D_ARRAY, DV_2D, DV_2D_ARRAY, DV_3D, DV_CUBE, DV_CUBE_ARRAY, DV_2D_MS, DV_2D_MS_ARRAY, DV_COUNT };

static struct {
    int ready;
    VkBuffer zbuf;
    VmaAllocation zalloc;
    VkBufferView zview;
    VkSampler samp, samp_shadow;
    struct { VkImage image; VmaAllocation alloc; VkImageView view, sview; } img[DV_COUNT];
} Z;

static const struct { VkImageType t; VkImageViewType vt; uint32_t layers; VkSampleCountFlagBits s; VkImageCreateFlags f; } dinfo[DV_COUNT] = {
    {VK_IMAGE_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1, VK_SAMPLE_COUNT_1_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 6, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, VK_SAMPLE_COUNT_4_BIT, 0},
    {VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, VK_SAMPLE_COUNT_4_BIT, 0}};

// A 1x1 stand-in image of view type dinfo[i] in `fmt`, moved to GENERAL by the upload batch (which reaches the queue before
// the submission that uses it). *sview is the view a storage descriptor takes (a cube becomes a 2D array).
static void make_dummy(int i, VkFormat fmt, VkImage *image, VmaAllocation *alloc, VkImageView *view, VkImageView *sview) {
    VkCtx *c = vk_ctx();
    int ms = dinfo[i].s != VK_SAMPLE_COUNT_1_BIT;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = dinfo[i].f, .imageType = dinfo[i].t,
                            .format = fmt, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = dinfo[i].layers,
                            .samples = dinfo[i].s, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | (ms ? 0 : VK_IMAGE_USAGE_STORAGE_BIT),
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ia = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ia, image, alloc, NULL) != VK_SUCCESS) rt_fatal("vk: stand-in image %d failed", i);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *image,
                                .viewType = dinfo[i].vt, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, dinfo[i].layers}};
    if (vkCreateImageView(c->device, &vi, NULL, view) != VK_SUCCESS) rt_fatal("vk: stand-in view failed");
    *sview = *view;
    if (!ms && (dinfo[i].vt == VK_IMAGE_VIEW_TYPE_CUBE || dinfo[i].vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)) {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (vkCreateImageView(c->device, &vi, NULL, sview) != VK_SUCCESS) rt_fatal("vk: stand-in view failed");
    }
    VkImageMemoryBarrier2 bar = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                 .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                 .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                 .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .image = *image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, dinfo[i].layers}};
    VkCommandBuffer cb = vk_upload_begin();
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &bar};
    vkCmdPipelineBarrier2(cb, &di);
    vk_upload_end();
}

// Storage image stand-ins in the format a shader declares (GfxShaderRes.format: Ryujinx TextureFormat order); Vulkan wants
// the view format to match the declaration. Created on first use.
static const VkFormat tex_format[40] = {
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT,
    VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT,
    VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM,
    VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM,
    VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT,
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
    VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT,
    VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT,
    VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_B10G11R11_UFLOAT_PACK32};
static struct { int used, di; VkFormat fmt; VkImage image; VmaAllocation alloc; VkImageView view, sview; } SD[48];

static PlatMutex dummy_lock = PLAT_MUTEX_INIT;   // the contexts make stand-ins on first use

static VkImageView storage_dummy(int di, int code) {
    VkFormat fmt = code > 0 && code < 40 ? tex_format[code] : VK_FORMAT_R8G8B8A8_UNORM;
    if (fmt == VK_FORMAT_R8G8B8A8_UNORM || dinfo[di].s != VK_SAMPLE_COUNT_1_BIT) return Z.img[di].sview;
    VkImageView v = Z.img[di].sview;
    plat_mutex_lock(&dummy_lock);
    for (int i = 0; i < 48; i++) {
        if (!SD[i].used) {
            SD[i].used = 1, SD[i].di = di, SD[i].fmt = fmt;
            make_dummy(di, fmt, &SD[i].image, &SD[i].alloc, &SD[i].view, &SD[i].sview);
            v = SD[i].sview;
            break;
        }
        if (SD[i].di == di && SD[i].fmt == fmt) {
            v = SD[i].sview;
            break;
        }
    }
    plat_mutex_unlock(&dummy_lock);
    return v;
}

static void dummies_make(void);

static void dummies_init(void) {
    if (__atomic_load_n(&Z.ready, __ATOMIC_ACQUIRE)) return;
    plat_mutex_lock(&dummy_lock);
    if (!Z.ready) dummies_make();
    plat_mutex_unlock(&dummy_lock);
}

static void dummies_make(void) {
    VkCtx *c = vk_ctx();
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = ZERO_BYTES,
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};   // the memset below is not flushed
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &Z.zbuf, &Z.zalloc, &info) != VK_SUCCESS) rt_fatal("vk: zero buffer failed");
    memset(info.pMappedData, 0, ZERO_BYTES);
    VkBufferViewCreateInfo bv = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = Z.zbuf,
                                 .format = VK_FORMAT_R32G32B32A32_SFLOAT, .range = ZERO_BYTES};
    if (vkCreateBufferView(c->device, &bv, NULL, &Z.zview) != VK_SUCCESS) rt_fatal("vk: zero buffer view failed");
    samp_par sp;
    memset(&sp, 0, sizeof sp);
    sp.lod_max = 1000.0f;
    Z.samp = vk_sampler_for(&sp);
    sp.cmp_mode = 1, sp.cmp_func = NVN_DEPTH_FUNC_ALWAYS;
    Z.samp_shadow = vk_sampler_for(&sp);

    for (int i = 0; i < DV_COUNT; i++)
        make_dummy(i, VK_FORMAT_R8G8B8A8_UNORM, &Z.img[i].image, &Z.img[i].alloc, &Z.img[i].view, &Z.img[i].sview);
    __atomic_store_n(&Z.ready, 1, __ATOMIC_RELEASE);
}

static void twins_release(VkCtx *c);

static void rs_forget(VkExec *ex) {   // the pipeline infos the lists are keyed by go with the device
    if (!ex->draw) return;
    nvn_hm_clear(&ex->draw->rs_lists, free);
    ex->draw->rs_last = NULL, ex->draw->rs_last_l = NULL;
    memset(ex->draw->PC, 0, sizeof ex->draw->PC), ex->draw->PM.valid = 0;
}

// Releases the stand-ins and the depth twins. The device must be idle; the game path leaves them to process exit like
// the pipelines.
void vk_exec_draw_shutdown(void) {
    if (!Z.ready) return;
    VkCtx *c = vk_ctx();
    twins_release(c);
    for (int i = 0; i < DV_COUNT; i++) {
        if (Z.img[i].sview != Z.img[i].view) vkDestroyImageView(c->device, Z.img[i].sview, NULL);
        vkDestroyImageView(c->device, Z.img[i].view, NULL);
        vmaDestroyImage(c->vma, Z.img[i].image, Z.img[i].alloc);
    }
    for (int i = 0; i < 48 && SD[i].used; i++) {
        if (SD[i].sview != SD[i].view) vkDestroyImageView(c->device, SD[i].sview, NULL);
        vkDestroyImageView(c->device, SD[i].view, NULL);
        vmaDestroyImage(c->vma, SD[i].image, SD[i].alloc);
    }
    memset(SD, 0, sizeof SD);
    vk_exec_contexts(rs_forget);
    for (int i = 0; i < nspare_pool; i++) vkDestroyDescriptorPool(c->device, spare_pool[i], NULL);
    for (int i = 0; i < nspare_chunk; i++) vmaDestroyBuffer(c->vma, spare_chunk[i].buf, spare_chunk[i].alloc);
    nspare_pool = nspare_chunk = 0;
    vk_usnap_shutdown();
    vkDestroyBufferView(c->device, Z.zview, NULL);
    vmaDestroyBuffer(c->vma, Z.zbuf, Z.zalloc);
    memset(&Z, 0, sizeof Z);
}

// ---- resource types

static VkImageViewType res_view_type(uint16_t t) {
    int arr = t & 0x100;
    switch (t & 0xff) {
    case 1: return arr ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
    case 4: return VK_IMAGE_VIEW_TYPE_3D;
    case 5: return arr ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
    default: return arr ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}

static int dummy_index(VkImageViewType vt, int ms) {
    switch (vt) {
    case VK_IMAGE_VIEW_TYPE_1D: return DV_1D;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return DV_1D_ARRAY;
    case VK_IMAGE_VIEW_TYPE_3D: return DV_3D;
    case VK_IMAGE_VIEW_TYPE_CUBE: return DV_CUBE;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return DV_CUBE_ARRAY;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return ms ? DV_2D_MS_ARRAY : DV_2D_ARRAY;
    default: return ms ? DV_2D_MS : DV_2D;
    }
}

// GFX_TEX_* of a bound texture slot (the translator's tex_type / img_type answers).
static int gfx_type_of(const VkTexImage *img, const view_t *v, int hv) {
    if (hv && v->set_target) return vk_pipeline_tex_type(v->target);
    int ms = img->samples > 1;
    switch (img->view_type) {
    case VK_IMAGE_VIEW_TYPE_1D: return GFX_TEX_1D;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return GFX_TEX_1D_ARRAY;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY: return ms ? GFX_TEX_2D_MS_ARRAY : GFX_TEX_2D_ARRAY;
    case VK_IMAGE_VIEW_TYPE_3D: return GFX_TEX_3D;
    case VK_IMAGE_VIEW_TYPE_CUBE: return GFX_TEX_CUBE;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return GFX_TEX_CUBE_ARRAY;
    default: return ms ? GFX_TEX_2D_MS : GFX_TEX_2D;
    }
}

static VkComponentSwizzle swz_of(uint8_t s) {
    switch (s) {
    case NVN_TEXTURE_SWIZZLE_ZERO: return VK_COMPONENT_SWIZZLE_ZERO;
    case NVN_TEXTURE_SWIZZLE_ONE: return VK_COMPONENT_SWIZZLE_ONE;
    case NVN_TEXTURE_SWIZZLE_R: return VK_COMPONENT_SWIZZLE_R;
    case NVN_TEXTURE_SWIZZLE_G: return VK_COMPONENT_SWIZZLE_G;
    case NVN_TEXTURE_SWIZZLE_B: return VK_COMPONENT_SWIZZLE_B;
    default: return VK_COMPONENT_SWIZZLE_A;
    }
}

// A view in STENCIL mode samples the stencil plane (TextureViewSetDepthStencilMode); otherwise depth wins. The
// translator declares every sampled image float and the shader takes the raw bits, as for integer formats (nxnative
// nvn_mtl.m).
VkImageAspectFlags vk_exec_view_aspect(const VkTexImage *img, const view_t *v, int hv) {
    if (hv && v->set_dsmode && v->dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL && (img->aspect & VK_IMAGE_ASPECT_STENCIL_BIT))
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    return img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT     ? VK_IMAGE_ASPECT_DEPTH_BIT
           : img->aspect & VK_IMAGE_ASPECT_STENCIL_BIT ? VK_IMAGE_ASPECT_STENCIL_BIT
                                                       : VK_IMAGE_ASPECT_COLOR_BIT;
}

// Image views of registered textures, kept for their image's lifetime: made per command buffer they were created again
// every frame (vkCreateImageView took about 8% of the fight replay on the 7900X). Used under the texture lock, as
// vk_exec_image_gone is (destroy_tex), which hands the views of a destroyed image to the deferred release with it.
typedef struct ViewNode { struct ViewNode *next; ViewKey k; VkImageView view; } ViewNode;
static nvn_hmap view_of;                 // ViewKey -> VkImageView
static nvn_hmap views_by_image;          // VkImage -> its first ViewNode

// An image view of a registered texture for a shader resource of type `rtype` (GfxShaderRes.type); VK_NULL_HANDLE when
// the image cannot serve it (the caller binds a stand-in). Texture lock held.
static VkImageView get_view(VkExec *ex, const VkTexImage *img, const view_t *v, int hv, uint16_t rtype, int storage) {
    VkImageViewType vt = res_view_type(rtype);
    int ms = (rtype & 0x200) != 0;
    if ((img->samples > 1) != ms || (storage && ms)) return VK_NULL_HANDLE;
    if (storage && !(img->usage & VK_IMAGE_USAGE_STORAGE_BIT)) return VK_NULL_HANDLE;
    int cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    int ok = 0;
    switch (vt) {
    case VK_IMAGE_VIEW_TYPE_1D:
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: ok = img->type == VK_IMAGE_TYPE_1D; break;
    case VK_IMAGE_VIEW_TYPE_3D: ok = img->type == VK_IMAGE_TYPE_3D; break;
    case VK_IMAGE_VIEW_TYPE_CUBE:
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
        ok = img->type == VK_IMAGE_TYPE_2D && (img->view_type == VK_IMAGE_VIEW_TYPE_CUBE || img->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY);
        break;
    default: ok = img->type == VK_IMAGE_TYPE_2D; break;
    }
    if (!ok) return VK_NULL_HANDLE;
    uint32_t l0 = hv && v->set_levels ? v->base_level : 0;
    if (l0 >= img->levels) return VK_NULL_HANDLE;
    uint32_t nl = hv && v->set_levels && v->nlevels ? v->nlevels : img->levels - l0;
    if (l0 + nl > img->levels) nl = img->levels - l0;
    if (storage) nl = 1;
    uint32_t y0 = 0, ny = 1;
    if (vt != VK_IMAGE_VIEW_TYPE_3D) {
        y0 = hv && v->set_layers ? v->min_layer : 0;
        if (y0 >= img->layers) return VK_NULL_HANDLE;
        uint32_t rest = hv && v->set_layers && v->nlayers ? v->nlayers : img->layers - y0;
        if (y0 + rest > img->layers) rest = img->layers - y0;
        if (vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_2D) ny = 1;
        else if (vt == VK_IMAGE_VIEW_TYPE_CUBE) ny = 6;
        else if (vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) ny = rest / 6 * 6;
        else ny = rest;
        if (!ny || y0 + ny > img->layers) return VK_NULL_HANDLE;
    }
    if (storage && cube) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    uint32_t aspect = vk_exec_view_aspect(img, v, hv);
    uint8_t sw[4] = {NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_A};
    if (!storage) {   // MNG nvn_mtl.m tex_swizzle_sel: view selectors replace the texture's, then the format remap
        memcpy(sw, hv && v->set_swizzle ? v->swz : img->swz, 4);
        if (img->alpha_one || img->chan[0])
            for (int i = 0; i < 4; i++) {
                if (img->alpha_one && sw[i] == NVN_TEXTURE_SWIZZLE_A) sw[i] = NVN_TEXTURE_SWIZZLE_ONE;
                else if (img->chan[0] && sw[i] >= NVN_TEXTURE_SWIZZLE_R && sw[i] <= NVN_TEXTURE_SWIZZLE_A)
                    sw[i] = img->chan[sw[i] - NVN_TEXTURE_SWIZZLE_R];
            }
    }
    uint32_t swz = (uint32_t)sw[0] | (uint32_t)sw[1] << 8 | (uint32_t)sw[2] << 16 | (uint32_t)sw[3] << 24;
    ViewKey k = {img->image, img->format, vt, aspect, l0, nl, y0, ny, swz};
    if (!view_of.klen) nvn_hm_init(&view_of, sizeof(ViewKey)), nvn_hm_init(&views_by_image, sizeof(VkImage));
    VkImageView found = (VkImageView)nvn_hm_get(&view_of, &k);
    if (found) return found;
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img->image, .viewType = vt,
                                .format = img->format,
                                .components = {swz_of(sw[0]), swz_of(sw[1]), swz_of(sw[2]), swz_of(sw[3])},
                                .subresourceRange = {aspect, l0, nl, y0, ny}};
    VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                      .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    if (img->block_views) ci.pNext = &use;   // the image's COLOR_ATTACHMENT usage is its block views' (vk_texture.c)
    VkImageView view;
    if (vkCreateImageView(vk_ctx()->device, &ci, NULL, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    if (img->gone) {   // dropped since the lookup: its views were purged and a new one would outlive the image
        vk_defer_view(view);
        return view;
    }
    ViewNode *n = malloc(sizeof *n);
    if (!n) rt_fatal("vk: out of memory");
    *n = (ViewNode){nvn_hm_get(&views_by_image, &img->image), k, view};
    nvn_hm_put(&views_by_image, &img->image, n);
    nvn_hm_put(&view_of, &k, (void *)view);
    return view;
}

// An attachment view of a registered texture, kept with its sampled views for the image's lifetime: made per command
// buffer, every render target's views were created again each frame (vkCreateImageView under vk_exec_pass_begin, 2%
// of the fight replay on the 7900X). Attachment keys carry a swizzle word no sampled key has (those hold four NVN
// selectors, each below 8). The texture lock guards the tables, as for get_view; vk_exec_pass_begin may already hold it.
VkImageView vk_exec_target_view(const VkTexImage *img, VkFormat format, VkImageViewType type, uint32_t aspect,
                                uint32_t level, uint32_t base, uint32_t layers, int blocks) {
    ViewKey k = {img->image, format, type, aspect, level, 1, base, layers, blocks ? 0xfffffffeu : 0xffffffffu};
    vk_texture_lock();
    if (!view_of.klen) nvn_hm_init(&view_of, sizeof(ViewKey)), nvn_hm_init(&views_by_image, sizeof(VkImage));
    VkImageView view = (VkImageView)nvn_hm_get(&view_of, &k);
    if (!view) {
        VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img->image,
                                    .viewType = type, .format = format, .subresourceRange = {aspect, level, 1, base, layers}};
        VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                          .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
        if (blocks) ci.pNext = &use;   // not the BC image's sampling and transfer usages
        if (vkCreateImageView(vk_ctx()->device, &ci, NULL, &view) != VK_SUCCESS)
            rt_fatal("vk: render target view (format %d) could not be created", (int)format);
        if (img->gone) {   // dropped since resolve_target (get_view)
            vk_defer_view(view);
        } else {
            ViewNode *n = malloc(sizeof *n);
            if (!n) rt_fatal("vk: out of memory");
            *n = (ViewNode){nvn_hm_get(&views_by_image, &img->image), k, view};
            nvn_hm_put(&views_by_image, &img->image, n);
            nvn_hm_put(&view_of, &k, (void *)view);
        }
    }
    vk_texture_unlock();
    return view;
}

void vk_exec_image_gone(VkImage image) {
    vk_exec_bc_gone(image);
    vk_hdr_image_gone(image);
    if (!views_by_image.klen) return;
    for (ViewNode *n = nvn_hm_del(&views_by_image, &image), *next; n; n = next) {
        next = n->next;
        nvn_hm_del(&view_of, &n->k);
        vk_defer_view(n->view);
        free(n);
    }
}

// ---- program to shaders


static int find_stage_scan(const prog_t *p, int nvn_stage, const uint8_t **ctl, const uint8_t **code, uint32_t *len) {
    for (uint32_t i = 0; i < p->nstage && i < 6; i++) {
        const uint8_t *c = p->sd[i].control;
        if (!c || !p->sd[i].data) continue;
        uint32_t st;
        memcpy(&st, c + 0x714, 4);
        if ((int)st != nvn_stage) continue;
        memcpy(len, c + 0x704, 4);
        *ctl = c, *code = (const uint8_t *)(uintptr_t)p->sd[i].data;
        return 1;
    }
    return 0;
}


static int find_stage(VkExecDraw *D, const prog_t *p, int nvn_stage, const uint8_t **ctl, const uint8_t **code, uint32_t *len) {
    if (!p->serial || (unsigned)nvn_stage >= 6) return find_stage_scan(p, nvn_stage, ctl, code, len);
    typeof(D->stagem[0]) *m = &D->stagem[p->serial * 0x9E3779B97F4A7C15ull >> 55];
    if (m->serial != p->serial) {
        m->serial = p->serial;
        memset(m->idx, -1, sizeof m->idx);
        for (uint32_t i = 0; i < p->nstage && i < 6; i++) {
            const uint8_t *c = p->sd[i].control;
            if (!c || !p->sd[i].data) continue;
            uint32_t st;
            memcpy(&st, c + 0x714, 4);
            if (st < 6 && m->idx[st] < 0) m->idx[st] = (int8_t)i, memcpy(&m->len[st], c + 0x704, 4);
        }
    }
    int i = m->idx[nvn_stage];
    if (i < 0) return 0;
    *ctl = p->sd[i].control, *code = (const uint8_t *)(uintptr_t)p->sd[i].data, *len = m->len[nvn_stage];
    return 1;
}


// vk_texture_slot_token of the slot a texture or image handle names; an empty handle always resolves to nothing.
static uint64_t token_of(uint64_t h) { return h ? vk_texture_slot_token((int)(h & 0xfffff)) : 1; }

// A chunk asks for this handle at every op: what it resolves to depends on the op (vk_texture_slot_moves).
static int slot_moves(const VkExec *ex, uint64_t h) { return ex->chunked && h && vk_texture_slot_moves((int)(h & 0xfffff)); }

// Whether what handle h resolved to under the global generation *egen and its slot token etok (both read before that
// lookup) still holds at global generation gen: nothing at all changed since, or not its slot (the token, read only when
// the global generation moved, about every 16 draws). A token hit moves *egen to gen.
static int slot_still(uint32_t *egen, uint64_t etok, uint64_t h, uint32_t gen) {
    if (*egen == gen) return 1;
    uint64_t t = token_of(h);
    if (!t || t != etok) return 0;
    *egen = gen;
    return 1;
}
static int shader_memo_on(void);


static uint32_t slotm_idx(uint64_t serial, int st) { return (uint32_t)((serial * 8 + (uint64_t)st) * 0x9E3779B97F4A7C15ull >> 52); }

static uint32_t slot_mask_of(VkExecDraw *D, uint64_t serial, int st) {
    const typeof(D->slotm[0]) *e = &D->slotm[slotm_idx(serial, st)];
    return serial && e->serial == serial && e->st == st ? e->m : ~0u;
}

static void slot_mask_learn(VkExecDraw *D, uint64_t serial, int st, const GfxShader *s) {
    if (!serial || slot_mask_of(D, serial, st) != ~0u) return;
    D->slotm[slotm_idx(serial, st)] = (typeof(D->slotm[0])){serial, st, gfx_shader_slot_mask(s)};
}

// The shader of each (program, stage) a context translated, for the state scan of a chunked replay
// (vk_exec_draw_writes): which storage buffers a stage writes is a property of its code, the same in every translation.
// s NULL: the program has no such stage, or it failed to translate. Shared by the contexts and read by the scan without
// a lock: a writer takes wt_lock and clears the key while it changes the entry, a reader checks the key before and
// after reading the shader. A program not found holds the chunks after its draw (vk_exec.c scan_writes), so the table
// is large and set-associative: a field frame in 1.2.1 draws with programs whose serials reach some 25,000 within a
// minute, and the 4096 direct-mapped entries this replaced missed in nearly every chunk, which then waited for the one
// before it. A full set gives up its oldest program (the lowest serial).
#define WT_WAYS 8
#define WT_SETS (1 << 14)
static struct { uint64_t key; const GfxShader *s; } WT[WT_SETS][WT_WAYS];
static PlatMutex wt_lock = PLAT_MUTEX_INIT;

static uint64_t wt_key(uint64_t serial, int st) { return serial << 4 | (uint64_t)st << 1 | 1; }

static typeof(WT[0][0]) *wt_set(uint64_t k) { return WT[(k * 0x9E3779B97F4A7C15ull) >> (64 - 14)]; }

static int wt_way(const typeof(WT[0][0]) *set, uint64_t k) {
    for (int w = 0; w < WT_WAYS; w++)
        if (__atomic_load_n(&set[w].key, __ATOMIC_ACQUIRE) == k) return w;
    return -1;
}

static void wt_learn(uint64_t serial, int st, const GfxShader *s) {
    uint64_t k = wt_key(serial, st);
    typeof(WT[0][0]) *set = wt_set(k);
    if (!serial || wt_way(set, k) >= 0) return;
    plat_mutex_lock(&wt_lock);
    if (wt_way(set, k) < 0) {
        int w = 0;
        for (int i = 1; i < WT_WAYS; i++)
            if (set[i].key < set[w].key) w = i;   // an empty way (key 0) first, else the oldest
        __atomic_store_n(&set[w].key, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&set[w].s, s, __ATOMIC_RELEASE);
        __atomic_store_n(&set[w].key, k, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&wt_lock);
}

static int wt_find(uint64_t serial, int st, const GfxShader **s) {
    uint64_t k = wt_key(serial, st);
    const typeof(WT[0][0]) *set = wt_set(k);
    int w = serial ? wt_way(set, k) : -1;
    if (w < 0) return 0;
    *s = __atomic_load_n(&set[w].s, __ATOMIC_ACQUIRE);
    return __atomic_load_n(&set[w].key, __ATOMIC_ACQUIRE) == k;
}

static uint8_t type_of_handle(uint64_t h) {   // texture lock held
    view_t v;
    int hv;
    const VkTexImage *img = h ? vk_texture_by_id_locked((int)(h & 0xfffff), &v, &hv) : NULL;
    return img ? (uint8_t)gfx_type_of(img, &v, hv) : 0;
}

// The answers of stage st for the slots in mask; the types of other slots stay zero.
static void answers_for(VkExec *ex, int st, const uint8_t *ctl, uint32_t mask, GfxShaderAnswers *a) {
    VkExecDraw *D = ex->draw;
    memset(a, 0, sizeof *a);
    if (st == NVN_STAGE_VS) vk_pipeline_answers(&ex->pipe, a, NULL);
    else if (st == NVN_STAGE_FS) vk_pipeline_answers(&ex->pipe, NULL, a);
    else
        for (int k = 0; k < 3; k++) {
            uint32_t l;
            memcpy(&l, ctl + 0x748 + 4 * k, 4);
            a->local[k] = (uint16_t)(l ? l : 1);
        }
    typeof(D->AC[0]) *c = &D->AC[st];
    // The generation and tokens are read before the lookups (one that creates an entry changes them) and unlocked: a
    // registration racing with them is ordered after this draw, and a slot stored under older ones is only looked up
    // again.
    uint32_t gen = vk_texture_slots_gen();
    int locked = 0, check = shader_memo_on() == 2;
    for (uint32_t m = mask; m; m &= m - 1) {
        int slot = __builtin_ctz(m);
        uint64_t h = ex->tex[st][slot], hi = ex->img[st][slot];
        int tmiss = c->tex[slot] != h || !c->ttok[slot] || slot_moves(ex, h) || !slot_still(&c->tgen[slot], c->ttok[slot], h, gen);
        int imiss = c->img[slot] != hi || !c->itok[slot] || slot_moves(ex, hi) || !slot_still(&c->igen[slot], c->itok[slot], hi, gen);
        uint64_t tt = tmiss ? token_of(h) : 0, it = imiss ? token_of(hi) : 0;
        if ((tmiss || imiss || check) && !locked) vk_texture_lock(), locked = 1, D->BS.ac_miss++;
        if (tmiss) c->tt[slot] = type_of_handle(h), c->tex[slot] = h, c->ttok[slot] = tt, c->tgen[slot] = gen;
        if (imiss) c->it[slot] = type_of_handle(hi), c->img[slot] = hi, c->itok[slot] = it, c->igen[slot] = gen;
        if (check && (type_of_handle(h) != c->tt[slot] || type_of_handle(hi) != c->it[slot]))   // TOTK_SHADER_MEMO=check
            rt_fatal("vk_exec: kept texture types of stage %d slot %d differ from a lookup", st, slot);
        a->tex_type[slot] = c->tt[slot], a->img_type[slot] = c->it[slot];
    }
    if (locked) vk_texture_unlock();
}

// *present: the program has this stage. Returns NULL when it has not or the translation failed. pending (graphics
// stages, vk_pipeline_async_on): a shader not translated yet is queued and NULL returned with *pending = 1. await: a
// shader queued earlier is waited for (gfx_shader_await) instead.
static _Thread_local int sf_mvprev;   // shader_for asks for the vertex shader's last-frame variant (per-object motion)
// Chunks of one submission note their scene draws for DLSS's motion one at a time: the list, its memos and the
// last-frame block tables (vk_mvprev.c, vk_mvsurvey.c) are shared; vk_mvobj_execute reads the list once the chunks
// before DLSS's evaluation are recorded (vk_exec.c run_job).
static PlatMutex mo_lock = PLAT_MUTEX_INIT;

static const GfxShader *shader_for(VkExec *ex, int st, int *present, int *pending, int await) {
    VkExecDraw *D = ex->draw;
    const uint8_t *ctl, *code;
    uint32_t len;
    *present = 0;
    if (pending) *pending = 0;
    if (!ex->prog) return NULL;
    if (!find_stage(ex->draw, ex->prog, st, &ctl, &code, &len)) {
        wt_learn(ex->prog->serial, st, NULL);
        return NULL;
    }
    *present = 1;
    GfxShaderAnswers a;
    uint64_t serial = ex->prog->serial;
    answers_for(ex, st, ctl, slot_mask_of(D, serial, st), &a);
    a.mvprev = (uint8_t)(sf_mvprev && st == NVN_STAGE_VS);
    ShaderEnt *set = NULL;
    uint64_t h = 0;
    if (serial) {
        h = (serial * 0x9E3779B97F4A7C15ull ^ (uint64_t)st) ^ nvn_hm_hash(&a, sizeof a);   // a is zero-padded
        set = D->shader_tab[(h ^ h >> 29) & (SHADER_SETS - 1)];
        for (int w = 0; w < SHADER_WAYS && set[w].used; w++) {
            ShaderEnt *e = &set[w];
            if (e->serial != serial || e->stage != st || (vk_memx_mode == 1 && e->h != h) || MX_NE(&e->a, &a, sizeof a)) continue;
            if (w) {   // to the front
                ShaderEnt hit = *e;
                memmove(&set[1], &set[0], w * sizeof *set);
                set[0] = hit;
            }
            wt_learn(serial, st, set[0].s);   // again after WT gave the entry up
            return set[0].s;
        }
    }
    int gs = st == NVN_STAGE_VS ? GFX_STAGE_VS : st == NVN_STAGE_FS ? GFX_STAGE_FS : GFX_STAGE_CS;
    const GfxShader *s = await ? gfx_shader_await(gs, ctl, code, len, &a)
                       : pending && (vk_pipeline_async_on() || a.mvprev) ? gfx_shader_get_async(gs, ctl, code, len, &a, pending)
                                                           : gfx_shader_get(gs, ctl, code, len, NULL, &a);
    if (pending && *pending) return NULL;
    if (s) slot_mask_learn(D, serial, st, s);
    wt_learn(serial, st, s);
    if (!s) warn_once(W_SHADER, "a stage %d shader of program %llu failed to translate: draws with it are skipped", st,
                      (unsigned long long)serial);
    else if (set) {   // in front, the least recently used way drops out
        memmove(&set[1], &set[0], (SHADER_WAYS - 1) * sizeof *set);
        set[0] = (ShaderEnt){.serial = serial, .h = h, .stage = st, .used = 1, .a = a, .s = s};
    }
    return s;
}

// ---- descriptors

static int nvn_stage_of(int gfx_stage) {
    return gfx_stage == GFX_STAGE_VS ? NVN_STAGE_VS : gfx_stage == GFX_STAGE_FS ? NVN_STAGE_FS : NVN_STAGE_CS;
}

static VkDescriptorBufferInfo zero_info(uint64_t max) {
    return (VkDescriptorBufferInfo){Z.zbuf, 0, max < ZERO_BYTES ? max : ZERO_BYTES};
}

// vk_mem_count, also kept per (class, use) while a graphics build runs (RS).
static void count_use(VkExecDraw *D, int cls, int use, uint64_t n) {
    vk_mem_count(cls, use, n);
    if (D->BI.cur) {
        BiMeta *m = D->BI.cur;
        if (m->n++) m->vol = 1;   // a second count: not kept (one is all a descriptor build makes)
        m->cls = (int16_t)cls, m->use = use, m->bytes = n;
    }
    if (!D->RS.rec) return;
    for (int i = 0; i < D->RS.ncnt; i++)
        if (D->RS.cnt[i].cls == cls && D->RS.cnt[i].use == use) {
            D->RS.cnt[i].bytes += n;
            return;
        }
    if (D->RS.ncnt < (int)(sizeof D->RS.cnt / sizeof D->RS.cnt[0])) D->RS.cnt[D->RS.ncnt++] = (RsCount){cls, use, n};
    else D->RS.cnt_over = 1;
}

// A buffer descriptor for [addr, addr+size) of guest memory: its backing (a pool's VRAM mirror when it has one) when
// aligned, a copy in transient memory when the backing is host memory with an unaligned offset. 0 when neither works.
// written: the shader stores into it (a mirror's range goes back to guest memory at the end of the submission).
static int bind_range(VkExec *ex, uint64_t addr, uint64_t size, uint64_t align, uint64_t max, int use, int written,
                      VkDescriptorBufferInfo *out) {
    VkExecDraw *D = ex->draw;
    VkMemSpan sp;
    if (!addr || !vk_mem_resolve(addr, 1, &sp)) return 0;
    uint64_t n = size && size < sp.avail ? size : sp.avail;
    if (n > max) n = max;
    if (!n) return 0;
    if (!written && (use == VK_MUSE_UBO || use == VK_MUSE_SSBO) && ub_bind(ex, addr, n, align, out)) {
        count_use(ex->draw, VK_MCLS_SCRATCH, use, n);
        return 1;
    }
    if (sp.offset % align == 0) {
        VK_MEM_USE(sp);
        if (written) ub_written(ex, addr, n);
        *out = (VkDescriptorBufferInfo){sp.buffer, sp.offset, n};
        if (written)
            vk_mem_gpu_wrote_shader(&sp, n), vk_usnap_drop_at(addr, n, ex->chunked ? ex->drop_n : 0),
                vk_exec_ub_foreign(ex, addr, n);
        count_use(ex->draw, sp.cls, use, n);
        VK_MEM_BOUND(sp);
        return 1;
    }
    if (sp.alloc) return 0;
    VkBuffer b;
    VkDeviceSize o;
    D->RS.volatile_ = 1;
    if (D->BI.cur) D->BI.cur->vol = 1;
    memcpy(scratch(ex, n, &b, &o), (const void *)(uintptr_t)addr, n);
    vk_mem_count(sp.cls, VK_MUSE_CPU_READ, n);
    vk_mem_count(VK_MCLS_SCRATCH, use, n);
    *out = (VkDescriptorBufferInfo){b, o, n};
    return 1;
}

static void c0_table(uint8_t *at, const p_bind_buf b[16]) {
    for (int r = 0; r < 16; r++) {
        uint64_t a = b[r].addr;
        uint32_t n = (uint32_t)b[r].size;
        memcpy(at + 16 * r, &a, 8);
        memcpy(at + 16 * r + 8, &n, 4);
    }
}

// The driver constant bank c0 as GLSLC-compiled shaders read it. Storage-buffer tables {u64 address, u32 size} every
// 16 bytes at 0x110 (vertex), 0x510 (fragment), 0x310 (compute): Ryujinx's GlobalToStorage fallback loads each
// candidate base from here and compares the global pointer against it. Compute: dispatch size at 0x200, the uniform
// buffer table at 0x210 + 16 r. Graphics: the largest point size (Maxwell's limit, GLSLC clamps gl_PointSize to it) at
// 0x610, the base instance at 0x644. Other stages: zeros. Unbound slots read zero, since the replay keeps them zero.
// Layout: nxnative dm/gfx/nvn/nvn_mtl.m (MIT).
void vk_c0_fill(uint8_t bank[VK_C0_BYTES], int stage, const p_bind_buf ssbo[16], const p_bind_buf ubo[16],
                const uint32_t dispatch[3], uint32_t base_instance) {
    memset(bank, 0, VK_C0_BYTES);
    int base = stage == NVN_STAGE_VS ? 0x110 : stage == NVN_STAGE_FS ? 0x510 : stage == NVN_STAGE_CS ? 0x310 : -1;
    if (base < 0) return;
    c0_table(bank + base, ssbo);
    if (stage == NVN_STAGE_CS) {
        memcpy(bank + 0x200, dispatch, 12);
        c0_table(bank + 0x210, ubo);
        return;
    }
    const float max_point = 2047.0f;
    memcpy(bank + 0x610, &max_point, 4);
    memcpy(bank + 0x644, &base_instance, 4);
}

static int c0_on(void) {   // TOTK_FIX_C0=0: c0 reads zeros instead of the driver constants
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("TOTK_FIX_C0");
    }
    return on;
}

// c0 from replay scratch; a bank equal to the stage's previous one reuses its range, so it costs no copy.
static VkDescriptorBufferInfo c0_info(VkExec *ex, int st) {
    VkExecDraw *D = ex->draw;
    int k = st == NVN_STAGE_VS ? 0 : st == NVN_STAGE_FS ? 1 : 2;
    // vk_c0_fill reads the storage slots, and the uniform slots and dispatch size for compute or the base instance for
    // graphics: equal inputs give the same bank, so neither the fill nor the 2 KiB compare is needed.
    typeof(D->B.c0_in[0]) in;
    memset(&in, 0, sizeof in);
    memcpy(in.ssbo, ex->ssbo[st], sizeof in.ssbo);
    if (st == NVN_STAGE_CS) memcpy(in.ubo, ex->ubo[st], sizeof in.ubo), memcpy(in.dim, D->c0_dim, sizeof in.dim);
    else in.binst = D->c0_binst;
    if (D->B.c0_valid[k] && MX_EQ(&in, &D->B.c0_in[k], sizeof in))
        return (VkDescriptorBufferInfo){D->B.c0_buf[k], D->B.c0_off[k], VK_C0_BYTES};
    D->B.c0_in[k] = in;
    uint8_t tmp[VK_C0_BYTES];
    vk_c0_fill(tmp, st, ex->ssbo[st], ex->ubo[st], D->c0_dim, D->c0_binst);
    if (!D->B.c0_valid[k] || MX_NE(tmp, D->B.c0[k], VK_C0_BYTES)) {
        MX_CPY(scratch(ex, VK_C0_BYTES, &D->B.c0_buf[k], &D->B.c0_off[k]), tmp, VK_C0_BYTES);
        MX_CPY(D->B.c0[k], tmp, VK_C0_BYTES);
        D->B.c0_valid[k] = 1;
    }
    return (VkDescriptorBufferInfo){D->B.c0_buf[k], D->B.c0_off[k], VK_C0_BYTES};
}

// The buffer range limits, read once: the device's properties are a cold cache line on every binding of every draw.
static struct { uint64_t ubo_max, ubo_align, sbo_max, sbo_align; } LIM;

static void limits_init(void) {
    const VkPhysicalDeviceLimits *l = &vk_ctx()->props.limits;
    LIM.ubo_align = l->minUniformBufferOffsetAlignment, LIM.sbo_max = l->maxStorageBufferRange;
    LIM.sbo_align = l->minStorageBufferOffsetAlignment;
    __atomic_store_n(&LIM.ubo_max, l->maxUniformBufferRange, __ATOMIC_RELEASE);
}

// Per-object motion (section at the end): while it records a draw again, these replace the uniform bindings of the
// vertex stage's slots 18-20 (last frame's camera, bones and place) and the fragment stage's slot 3 (its parameters).
// Per thread: the other chunks' draws go on meanwhile and keep their own bindings.
static _Thread_local int mo_ovr_on;
static _Thread_local VkDescriptorBufferInfo mo_ovr[2][32];   // [vertex, fragment][slot]; buffer 0: not replaced

static VkDescriptorBufferInfo ubo_info(VkExec *ex, int st, int slot) {
    if (mo_ovr_on && (st == NVN_STAGE_VS || st == NVN_STAGE_FS) && slot >= 0 && slot < 32 && mo_ovr[st][slot].buffer)
        return mo_ovr[st][slot];
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    uint64_t max = LIM.ubo_max, align = LIM.ubo_align;
    VkDescriptorBufferInfo bi;
    if (slot == 0 && c0_on()) return c0_info(ex, st);
    if (slot == 1 && ex->prog) {   // compiler constants stored after the shader code
        const uint8_t *ctl, *code;
        uint32_t len, c1;
        if (find_stage(ex->draw, ex->prog, st, &ctl, &code, &len)) {
            memcpy(&c1, ctl + 0x700, 4);
            if (c1 < len && bind_range(ex, (uint64_t)(uintptr_t)code + c1, len - c1, align, max, VK_MUSE_UBO, 0, &bi))
                return bi;
        }
    } else if (slot >= 3 && slot - 3 < 18) {
        const p_bind_buf *b = &ex->ubo[st][slot - 3];
        if (b->size && ub_bind(ex, b->addr, b->size < max ? b->size : max, align, &bi)) {   // updated in this pass
            count_use(ex->draw, VK_MCLS_SCRATCH, VK_MUSE_UBO, bi.range);
            return bi;
        }
        if (b->size && vk_usnap_find(b->addr, b->size < max ? b->size : max, ex->chunked ? ex->drop_n : UINT32_MAX, align,
                                     &bi)) {   // as of the submit call
            count_use(ex->draw, VK_MCLS_SCRATCH, VK_MUSE_UBO, bi.range);
            return bi;
        }
        if (bind_range(ex, b->addr, b->size, align, max, VK_MUSE_UBO, 0, &bi)) return bi;
    }
    return zero_info(max);
}

// The NVN slot of a storage binding. Ryujinx numbers a shader's storage buffers in the order it meets them
// (GfxShaderRes.slot); the NVN slot is the entry of the driver constant buffer c0 that holds the buffer's address
// (cbuf_slot 0, handle = word offset): 16-byte entries at byte 0x110 (vertex), 0x510 (fragment) and 0x310 (compute).
// A compute address at c0 0x210..0x30f is uniform block (offset - 0x210) / 16 read as storage: *ubo is set. -1: the
// address is not in c0 (the shader loads it from a user uniform block: ubo_ptr) or the stage has no known table.
// Mapping: MacNativeGames games/totk/tools/totk_xlate/Service.cs:206-222 @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou).
static int ssbo_slot(const GfxShaderRes *r, int st, int *ubo) {
    *ubo = 0;
    if (r->cbuf_slot != 0) return -1;
    int off = r->handle * 4, base = st == NVN_STAGE_VS ? 0x110 : st == NVN_STAGE_FS ? 0x510 : st == NVN_STAGE_CS ? 0x310 : -1;
    if (st == NVN_STAGE_CS && off >= 0x210 && off < 0x310 && (off - 0x210) % 16 == 0) {
        *ubo = 1;
        return (off - 0x210) / 16;
    }
    return base >= 0 && off >= base && off < base + 0x100 && (off - base) % 16 == 0 ? (off - base) / 16 : -1;
}

// 1 when the shader loads the storage buffer's address from a user uniform block (cbuf_slot 3.., NVN uniform slot
// cbuf_slot - 3, byte offset handle * 4). The Depths terrain reads its vertices through one, about 500 draws a frame;
// with a zero buffer they collapse and the ground is missing. MacNativeGames resolves these the same way (NVN_RB_SBUFU,
// games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m @ b75be8cf).
static int ubo_ptr_res(const GfxShaderRes *r) {
    return r->cbuf_slot >= 3 && r->cbuf_slot - 3 < 18;
}

// The inline uniform updates of the submission being replayed, newest last (a ring of UPD_N). op_update_ub writes them
// into the buffer's GPU copy; guest memory gets them only from the write-back when the submission ends. The terrain
// rewrites its pointer block this way before every draw, all at one address, so ubo_ptr reads the latest update that
// covers the pointer, else memory. Payloads stay in the recorded command buffer for the whole replay.
static void upd_note(VkExec *ex, const p_update_ub *u) {
    VkExecDraw *D = ex->draw;
    if (ex->chunked) {   // the scan listed it
        ex->upd_n++;
        return;
    }
    if (D->UPD.fence != ex->fence) D->UPD.fence = ex->fence, D->UPD.n = 0;
    D->UPD.u[D->UPD.n++ % UPD_N] = (VkExecUpd){u->dst, u->dst + u->size, u->data};
}

// 1 when an update of this submission supplied [a, a+n).
static int upd_read(const VkExec *ex, uint64_t a, void *out, size_t n) {
    VkExecDraw *D = ex->draw;
    const VkExecUpd *u = ex->upd;   // a chunk: the scan's list of the submission, of which upd_n precede this op
    uint32_t cnt = ex->upd_n, mask = UINT32_MAX;
    if (!ex->chunked) {
        if (D->UPD.fence != ex->fence) return 0;
        u = D->UPD.u, cnt = D->UPD.n, mask = UPD_N - 1;
    }
    for (uint32_t i = cnt, end = cnt > UPD_N ? cnt - UPD_N : 0; i-- > end;) {
        const VkExecUpd *e = &u[i & mask];
        if (a >= e->hi || a + n <= e->lo) continue;
        if (a < e->lo || a + n > e->hi) return 0;   // partly covered: memory
        MX_CPY(out, e->data + (a - e->lo), n);
        return 1;
    }
    return 0;
}

// Applies the updates of this submission that touch [a, a+n) onto out, oldest first (ub_bind's merged copies).
static void upd_apply(const VkExec *ex, uint64_t a, uint8_t *out, size_t n) {
    const UpdRing *U = &ex->draw->UPD;
    if (U->fence != ex->fence) return;
    for (uint32_t i = U->n > UPD_N ? U->n - UPD_N : 0; i < U->n; i++) {
        const VkExecUpd *e = &U->u[i % UPD_N];
        uint64_t lo = e->lo > a ? e->lo : a, hi = e->hi < a + n ? e->hi : a + n;
        if (lo < hi) MX_CPY(out + (lo - a), e->data + (lo - e->lo), hi - lo);
    }
}

// That address as the bound block holds it at this point of the replay; 0 when the block is unbound or too short. The
// block has no size field.
static uint64_t ubo_ptr(const VkExec *ex, int st, const GfxShaderRes *r) {
    const p_bind_buf *u = &ex->ubo[st][r->cbuf_slot - 3];
    uint64_t off = (uint64_t)r->handle * 4, a = 0;
    VkMemSpan sp;
    if (!u->addr || u->size < off + 8 || upd_read(ex, u->addr + off, &a, 8)) return a;
    if (vk_mem_resolve(u->addr + off, 8, &sp)) memcpy(&a, (const void *)(uintptr_t)(u->addr + off), 8);
    return a;
}

// The binding a storage buffer resource names at this point of the replay, NULL when it names none. *ubo: a compute
// shader's uniform binding; up holds the binding of an address read from a uniform block (size 0: up to the end of
// the backing).
static const p_bind_buf *ssbo_bound(const VkExec *ex, int st, const GfxShaderRes *r, p_bind_buf *up, int *ubo) {
    int slot = ssbo_slot(r, st, ubo);
    if (slot >= 0 && slot < 16) return *ubo ? &ex->ubo[st][slot] : &ex->ssbo[st][slot];
    if (!ubo_ptr_res(r)) return NULL;
    up->addr = ubo_ptr(ex, st, r);
    return up;
}

// vk_caps.c: shaders translated for sbuf_align address the buffer from its address rounded down to it.
static uint64_t sbuf_lead(uint64_t addr) { return vk_caps.sbuf_align ? addr & (vk_caps.sbuf_align - 1) : 0; }

static VkDescriptorBufferInfo ssbo_info(VkExec *ex, int st, const GfxShaderRes *r, int written) {
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    uint64_t max = LIM.sbo_max, align = LIM.sbo_align;
    VkDescriptorBufferInfo bi;
    int ubo;
    p_bind_buf up = {0};
    const p_bind_buf *b = ssbo_bound(ex, st, r, &up, &ubo);
    if (b) {
        uint64_t lead = sbuf_lead(b->addr);
        if (bind_range(ex, b->addr - lead, b->size ? b->size + lead : 0, align, max, ubo ? VK_MUSE_UBO : VK_MUSE_SSBO,
                       written, &bi)) {
            if (written && bi.buffer != VK_NULL_HANDLE) note_written(ex, b->addr - lead, bi.range);
            return bi;
        }
        if (b->addr) warn_once(W_SSBO_ALIGN, "storage buffer %#llx is not backed or not aligned: a zero buffer is bound",
                               (unsigned long long)b->addr);
    } else {
        warn_once(W_SSBO_SLOT, "a storage buffer whose address is at c%d+%#x (stage %d) is not replayed: a zero buffer is bound",
                  r->cbuf_slot, r->handle * 4, st);
    }
    return zero_info(max);
}

int vk_exec_draw_writes(const VkExec *s, int dispatch, uint64_t (*out)[2], int max) {
    if (!s->prog) return 0;
    static const int gfx[] = {NVN_STAGE_VS, NVN_STAGE_FS}, cs[] = {NVN_STAGE_CS};
    const int *st = dispatch ? cs : gfx, nst = dispatch ? 1 : 2;
    const GfxShader *sh[2];
    for (int i = 0; i < nst; i++)
        if (!wt_find(s->prog->serial, st[i], &sh[i])) return -1;
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    int n = 0;
    for (int i = 0; i < nst; i++) {
        const GfxShaderRes *r;
        int nr = sh[i] ? gfx_shader_resources(sh[i], GFX_RES_SSBO, &r) : 0;
        for (int j = 0; j < nr; j++) {
            int ubo;
            p_bind_buf up = {0};
            const p_bind_buf *b = (r[j].flags & 1) ? ssbo_bound(s, st[i], &r[j], &up, &ubo) : NULL;
            VkMemSpan sp;
            if (!b || !b->addr) continue;
            uint64_t lead = sbuf_lead(b->addr), addr = b->addr - lead, size = b->size ? b->size + lead : 0;
            if (!vk_mem_resolve(addr, 1, &sp)) continue;
            uint64_t len = size && size < sp.avail ? size : sp.avail;   // as bind_range binds it
            if (len > LIM.sbo_max) len = LIM.sbo_max;
            if (!len) continue;
            if (n == max) return -1;
            out[n][0] = addr, out[n][1] = len, n++;
        }
    }
    return n;
}

// ---- feedback loops (maxwell.md 1.9, vk.h VkCtx.feedback)
// TotK's G-buffer and terrain shaders sample targets of the pass that renders them, at their own pixel, after earlier
// draws of the pass wrote them (albedo, normals, R32F linear depth). Maxwell reads see those writes; in Vulkan the read
// is undefined without a barrier, and on the RX 9070 it showed as dark screen-aligned blocks on the island ground
// and hid Tulin's sage spirit. Before a draw whose fragment shader samples an attachment of
// the open pass that a draw or clear of the pass wrote since the pass began or the last barrier:
// - VK_FEEDBACK_LAYOUT: the image is learned (VkTexImage.fb_learned; it has the feedback usage since its creation), the
//   pass restarts once with it in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT, and from then on its passes attach it in that
//   layout, its descriptors carry that layout, the draws that sample it use a pipeline with the feedback flag
//   (VkPipeState.feedback) and a by-region barrier precedes such a draw.
// - VK_FEEDBACK_RESTART: the pass ends and restarts behind the full barrier (vk_exec_pass_begin).
static int in_fbl(const VkExec *ex, VkImage img) {
    for (int i = 0; i < ex->nfbl; i++)
        if (ex->fbl_img[i] == img) return 1;
    return 0;
}


static SubRange sub_of(const VkTexImage *img, const view_t *v, int hv) {
    SubRange r = {img->image, 0, img->levels, 0, img->layers};
    if (hv && v->set_levels) r.l0 = v->base_level, r.l1 = v->base_level + (v->nlevels ? v->nlevels : img->levels);
    if (hv && v->set_layers) r.a0 = v->min_layer, r.a1 = v->min_layer + (v->nlayers ? v->nlayers : img->layers);
    return r;
}

// Attachment slots of the open pass (bit 8 depth) whose subresource r covers.
static uint32_t attached_as(const VkExec *ex, const SubRange *r) {
    uint32_t m = 0;
    if (!r->image) return 0;
    for (uint32_t i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if ((i < 8 && i >= ex->pass_ncolor) || t->image != r->image) continue;
        if (t->level < r->l0 || t->level >= r->l1) continue;
        if (!t->is3d && (t->base_layer + t->layers <= r->a0 || t->base_layer >= r->a1)) continue;
        m |= 1u << i;
    }
    return m;
}

// TOTK_TEX_MEMO (tex_info, feedback_scan): 0 off, 1 on, 2 (=check) computes every hit again and stops on a difference.
static int td_mode(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_value("TOTK_TEX_MEMO");
        on = !strcmp(e, "check") ? 2 : strcmp(e, "0") != 0;
    }
    return on;
}

// The subresource a texture handle samples, per handle, kept while its slot resolves the same (slot_still): a draw's
// texture bindings change the whole-scan memo below, but the handles they bind were mostly looked up before, in this
// pass or an earlier one. Direct-mapped.
static SubRange sampled_sub(uint64_t h) {   // texture lock held
    view_t v;
    int hv;
    const VkTexImage *img = vk_texture_by_id_locked((int)(h & 0xfffff), &v, &hv);
    return img ? sub_of(img, &v, hv) : (SubRange){0};
}

// Attachments of the open pass the fragment shader fs samples; kept while the shader, the texture bindings and the
// pass stay the same.
static const RsList *rs_list(const VkExec *ex, const VkPipeInfo *info);

static uint32_t feedback_scan(VkExec *ex, const GfxShader *fs) {
    VkExecDraw *D = ex->draw;
    if (ex->fb_fs == fs && ex->fb_serial == ex->tex_serial && ex->fb_pass == ex->pass_serial) return ex->fb_sampled;
    // TOTK_BIND_INC: the memo holds the last prepared draw's scan (BL.fb_ok) of this shader in this pass, and since then
    // no op changed a fragment texture slot the shader reads (the pipeline's BiMask.ftex) and no slot was registered:
    // a texture bind elsewhere (tex_serial) no longer forces the scan.
    int bim = bind_inc_mode(), keep = 0;
    if (bim && D->B.bi_valid && D->BL.fb_ok && !ex->chunked && ex->fb_fs == fs && D->BL.fs == fs &&
        ex->fb_pass == ex->pass_serial && vk_texture_slots_gen() == D->BL.slots_gen) {
        const BiMask *bm = &rs_list(ex, D->BL.info)->m;
        keep = !(ex->dirt.tex[NVN_STAGE_FS][0] & bm->ftex[0]) && !(ex->dirt.tex[NVN_STAGE_FS][1] & bm->ftex[1]);
        if (keep && bim == 1) {
            ex->fb_serial = ex->tex_serial;
            return ex->fb_sampled;
        }
    }
    uint32_t kept = ex->fb_sampled;
    const GfxShaderRes *res;
    int n = gfx_shader_resources(fs, GFX_RES_TEX, &res);
    uint32_t m = 0, gen = vk_texture_slots_gen();   // before any lookup
    int memo = td_mode(), locked = 0;
    for (int r = 0; r < n; r++)
        for (int k = 0; k < (res[r].count ? res[r].count : 1); k++) {
            int slot = res[r].slot + k;
            uint64_t h = slot >= 0 && slot < 128 ? ex->tex[NVN_STAGE_FS][slot] : 0;
            if (!h) continue;
            typeof(D->FM[0]) *f = &D->FM[(h * 0x9E3779B97F4A7C15ull) >> 51];
            if (memo && f->h == h && !slot_moves(ex, h) && slot_still(&f->gen, f->tok, h, gen)) {
                if (memo == 2) {
                    if (!locked) vk_texture_lock(), locked = 1;
                    SubRange c = sampled_sub(h);
                    if (memcmp(&c, &f->r, sizeof c))
                        rt_fatal("vk_exec: feedback memo of handle %#llx differs from a lookup", (unsigned long long)h);
                }
                m |= attached_as(ex, &f->r);
                continue;
            }
            uint64_t tok = token_of(h);   // before the lookup
            if (!locked) vk_texture_lock(), locked = 1;
            SubRange sr = sampled_sub(h);
            m |= attached_as(ex, &sr);
            if (tok) *f = (typeof(*f)){.h = h, .tok = tok, .gen = gen, .r = sr};
            else f->h = 0;
        }
    if (locked) vk_texture_unlock();
    if (keep) {
        D->BC.checked++;
        if (m != kept) bi_mismatch(ex, BIM_FB, "sampled attachments %#x, kept %#x", m, kept);
    }
    ex->fb_fs = fs, ex->fb_serial = ex->tex_serial, ex->fb_pass = ex->pass_serial, ex->fb_sampled = m;
    return m;
}

// Before the pipeline lookup of a draw with fragment shader fs: learns and restarts as the mode says. Returns the
// sampled attachments in the feedback layout (the pipeline's feedback flag and feedback_barrier).
static uint32_t feedback_prepare(VkExec *ex, const GfxShader *fs) {
    int mode = vk_ctx()->feedback;
    if (mode == VK_FEEDBACK_OFF || !fs || !ex->in_pass) return 0;
    uint32_t m = feedback_scan(ex, fs);
    uint32_t d = m & ex->fb_dirty;
    if (d) ex->fr.fb_draws++;
    if (d & ~(mode == VK_FEEDBACK_LAYOUT ? ex->fbl_mask : 0)) {
        if (mode == VK_FEEDBACK_LAYOUT)
            for (uint32_t i = 0; i < 9; i++) {
                const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
                if ((d >> i & 1) && t->img && t->img->fb_usage) ((VkTexImage *)t->img)->fb_learned = 1;
            }
        vk_exec_pass_end(ex);
        vk_exec_pass_begin(ex);   // learned targets now in the feedback layout; the full barrier cleared fb_dirty
        ex->fr.fb_restarts++;
    }
    return mode == VK_FEEDBACK_LAYOUT ? m & ex->fbl_mask : 0;
}

// By-region barrier inside the pass: earlier attachment writes become visible to this draw's fragment shader reads.
static void feedback_barrier(VkExec *ex) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
                                           VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                           .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                           .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
                           .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded++;
    ex->fb_dirty = 0;
    ex->fr.fb_barriers++;
}

// Marks the attachments the draw with state s writes: colour targets with a nonzero effective write mask, depth with
// depth writes or the stencil test on.
static void feedback_written(VkExec *ex, const VkPipeState *s) {
    if (vk_ctx()->feedback == VK_FEEDBACK_OFF) return;
    if (ex->depth.image && !(ex->fb_dirty & 0x100) && ((s->ds.dt && s->ds.dw) || s->ds.st)) ex->fb_dirty |= 0x100;
    for (uint32_t i = 0; i < ex->pass_ncolor; i++)
        if (ex->color[i].image && !(ex->fb_dirty >> i & 1) && vk_pipeline_target_wmask(s, (int)i)) ex->fb_dirty |= 1u << i;
}

static void tex_info_full(VkExec *ex, const GfxShaderRes *r, uint64_t h, int storage, int separate,
                          VkDescriptorImageInfo *out, VkImage *image);

// Sampled-texture descriptors per (handle, resource type, separate), kept while nothing they derive from changed: the
// slot's resolution and texture entry (slot_still) and the samplers (vk_sampler_gen); the layout is set again on every
// hit from the images in the feedback layout (in_fbl of the image kept with it). The first binding of a slot in a submission (vk_texture_epoch) still makes the slot lookup
// for its storage check; a re-upload writes the same image, so the descriptor stays. Draws bind the same textures
// thousands of times a frame (15,000 lookups a frame in the monsters fight); a hit skips the slot lookup, the view key
// with its hash probe and the sampler lookup. Storage images, which note their binding per submission, are
// not kept. Direct-mapped; the texture lock only for the lookups (tex_lock). TOTK_TEX_MEMO=0 turns it off; =check computes every hit again and stops
// on a difference.
// The texture lock, taken on the first lookup that needs it (*locked): a memo hit needs none, and the contexts of a
// chunked replay take it from several threads.
static void tex_lock(int *locked) {
    if (!*locked) vk_texture_lock(), *locked = 1;
}

static void tex_info_lookup(VkExec *ex, const GfxShaderRes *r, uint64_t h, int storage, int separate,
                            VkDescriptorImageInfo *out, int *locked) {
    VkExecDraw *D = ex->draw;
    int td_on = td_mode();
    VkImage image;
    if (!h || storage || !td_on || slot_moves(ex, h)) return tex_lock(locked), tex_info_full(ex, r, h, storage, separate, out, &image);
    uint32_t gen = vk_texture_slots_gen(), ep = vk_texture_epoch(), sg = vk_sampler_gen();   // before any lookup
    typeof(D->TD[0]) *m = &D->TD[(h * 0x9E3779B97F4A7C15ull ^ (uint64_t)r->type << 7 ^ (uint64_t)separate) >> 51];
    if (m->valid && m->h == h && m->type == r->type && m->separate == separate && m->sgen == sg &&
        slot_still(&m->gen, m->tok, h, gen)) {
        if (m->epoch != ep) {   // the slot's first binding in this submission: only its storage check is due
            if (!vk_texture_slot_checked((int)(h & 0xfffff))) {   // another context may have made it
                tex_lock(locked);
                vk_texture_by_id_locked((int)(h & 0xfffff), NULL, NULL);
            }
            m->epoch = ep;
        }
        *out = m->out;
        out->imageLayout = m->image && ex->nfbl && in_fbl(ex, m->image) ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
                                                                          : VK_IMAGE_LAYOUT_GENERAL;
        if (td_on == 2) {
            VkDescriptorImageInfo full;
            tex_lock(locked);
            tex_info_full(ex, r, h, storage, separate, &full, &image);
            if (full.sampler != out->sampler || full.imageView != out->imageView || full.imageLayout != out->imageLayout)
                rt_fatal("vk_exec: texture descriptor memo of handle %#llx type %#x differs from a lookup",
                         (unsigned long long)h, r->type);
        }
        return;
    }
    uint64_t tok = token_of(h);   // before the lookup
    tex_lock(locked);
    tex_info_full(ex, r, h, storage, separate, out, &image);
    *m = (typeof(*m)){.h = h, .tok = tok, .gen = gen, .epoch = ep, .sgen = sg, .type = r->type,
                      .separate = (uint8_t)separate, .valid = 1, .image = image, .out = *out};
}

static void tex_info_full(VkExec *ex, const GfxShaderRes *r, uint64_t h, int storage, int separate,
                          VkDescriptorImageInfo *out, VkImage *image) {
    int ms = (r->type & 0x200) != 0;
    *image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler samp = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    if (h) {
        view_t v;
        int hv;
        const VkTexImage *img = vk_texture_by_id_locked((int)(h & 0xfffff), &v, &hv);
        if (img) view = get_view(ex, img, &v, hv, r->type, storage), *image = img->image;
        if (img && storage) vk_texture_storage_bound_locked(img);
        if (img && !storage && ex->nfbl && in_fbl(ex, img->image)) layout = VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT;
        if (!storage && !separate) {
            // A depth texture sampled without a compare reads one texel on the Switch, whatever the sampler's filter:
            // blending neighbouring depths puts reconstructed positions in mid-air at object edges (shadow masks).
            int sid = (int)(h >> 20 & 0xfff);
            int point = img && (img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && !(r->type & 0x400) &&
                        vk_exec_view_aspect(img, &v, hv) != VK_IMAGE_ASPECT_STENCIL_BIT;
            samp = point ? vk_sampler_point_by_id_locked(sid) : vk_sampler_by_id_locked(sid);
        }
    }
    if (!view) {
        int di = dummy_index(res_view_type(r->type), ms);
        view = storage ? storage_dummy(di, r->format) : Z.img[di].view;
    }
    if (!samp) samp = r->type & 0x400 ? Z.samp_shadow : Z.samp;
    *out = (VkDescriptorImageInfo){separate || storage ? VK_NULL_HANDLE : samp, view, layout};
}

// ---- depth twins: colour images under a compare sampler in a compute shader
// TotK's compute passes sample an R32_SFLOAT texture through a shadow sampler. Maxwell compares any single-channel
// float or unorm texel; Vulkan only formats with SAMPLED_IMAGE_DEPTH_COMPARISON (VUID-vkCmdDispatch-None-06479), which
// AMD's drivers do not give R32_SFLOAT, so the result there is undefined. Such an image gets a depth twin of the same
// size and bits (R32_SFLOAT -> D32_SFLOAT, R16_UNORM -> D16_UNORM), copied through a buffer right before each dispatch
// that samples it (a dispatch is outside any render pass), and the dispatch samples the twin. Draws keep the colour
// image: a copy cannot go inside their pass. Design: Savage Wind Recomp runtime/nvn/vulkan/exec.c depth_twin_update
// (https://github.com/Revivedx/SavageWindRecomp, MIT); the code is ours. TOTK_DEPTH_TWIN=0: off.
#define TWINS 8
typedef struct { VkImage src, img; VmaAllocation mem; VkImageView view[2]; uint32_t w, h, layers; VkFormat fmt; } Twin;
static PlatMutex twin_lock = PLAT_MUTEX_INIT;   // the table and the copy buffer: contexts recording chunks share them
static Twin twins[TWINS];
static unsigned twin_next;
static VkBuffer twin_buf;
static VmaAllocation twin_buf_mem;
static VkDeviceSize twin_buf_cap;

static void twin_free(void *arg) {   // completion thread, after the last submission that may sample it
    Twin *t = arg;
    VkCtx *c = vk_ctx();
    for (int k = 0; k < 2; k++)
        if (t->view[k]) vkDestroyImageView(c->device, t->view[k], NULL);
    vmaDestroyImage(c->vma, t->img, t->mem);
    free(t);
}

typedef struct { VkBuffer buf; VmaAllocation mem; } TwinBuf;
static void twin_buf_free(void *arg) {   // completion thread, after the last submission that may copy through it
    TwinBuf *b = arg;
    vmaDestroyBuffer(vk_ctx()->vma, b->buf, b->mem);
    free(b);
}

static VkFormat twin_format(const VkTexImage *img) {
    static int on = -1;
    if (on < 0) on = rt_knob("DEPTH_TWIN", "1");
    if (!on || (img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || img->samples > 1 || img->depth > 1) return VK_FORMAT_UNDEFINED;
    VkFormat df = img->format == VK_FORMAT_R32_SFLOAT ? VK_FORMAT_D32_SFLOAT
                : img->format == VK_FORMAT_R16_UNORM  ? VK_FORMAT_D16_UNORM : VK_FORMAT_UNDEFINED;
    if (!df) return df;
    static int compares[2] = {-1, -1};   // per source format: the driver compares it as it is
    int i = df == VK_FORMAT_D16_UNORM;
    if (compares[i] < 0) {
        VkFormatProperties3 p3 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
        VkFormatProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &p3};
        vkGetPhysicalDeviceFormatProperties2(vk_ctx()->phys, img->format, &p2);
        compares[i] = (p3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT) != 0;
        rt_log("vk_exec: %s under a compare sampler: %s", i ? "R16_UNORM" : "R32_SFLOAT",
               compares[i] ? "compared by the driver" : "sampled through a depth twin in compute passes");
    }
    return compares[i] ? VK_FORMAT_UNDEFINED : df;
}

static void twins_release(VkCtx *c) {
    for (int i = 0; i < TWINS; i++) {
        for (int k = 0; k < 2; k++)
            if (twins[i].view[k]) vkDestroyImageView(c->device, twins[i].view[k], NULL);
        if (twins[i].img) vmaDestroyImage(c->vma, twins[i].img, twins[i].mem);
    }
    memset(twins, 0, sizeof twins);
    if (twin_buf) vmaDestroyBuffer(c->vma, twin_buf, twin_buf_mem);
    twin_buf = VK_NULL_HANDLE, twin_buf_mem = NULL, twin_buf_cap = 0;
}

// The twin of img holding img's current texels, the copies recorded into ex->cb; NULL when img needs none or it cannot
// be made.
static Twin *twin_update(VkExec *ex, const VkTexImage *img) {
    VkFormat df = twin_format(img);
    if (!df) return NULL;
    VkCtx *c = vk_ctx();
    uint32_t bpp = df == VK_FORMAT_D32_SFLOAT ? 4 : 2, layers = img->layers ? img->layers : 1;
    VkDeviceSize bytes = (VkDeviceSize)img->width * img->height * layers * bpp;
    if (bytes > (64u << 20)) return NULL;
    if (bytes > twin_buf_cap) {   // one copy buffer, grown; the old one goes after the submission being recorded
        VkDeviceSize want = twin_buf_cap ? twin_buf_cap * 2 : 1u << 16;
        while (want < bytes) want *= 2;
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = want,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkBuffer nb;
        VmaAllocation nm;
        if (vmaCreateBuffer(c->vma, &bi, &ai, &nb, &nm, NULL) != VK_SUCCESS) return NULL;
        if (twin_buf) {
            TwinBuf *old = malloc(sizeof *old);
            if (!old) rt_fatal("vk: out of memory (depth twin)");
            *old = (TwinBuf){twin_buf, twin_buf_mem};
            vk_completion_add(vk_sync_issued() + 1, twin_buf_free, old);
        }
        twin_buf = nb, twin_buf_mem = nm, twin_buf_cap = want;
    }
    Twin *t = NULL;
    for (int i = 0; i < TWINS && !t; i++)
        if (twins[i].img && twins[i].src == img->image && twins[i].w == img->width && twins[i].h == img->height &&
            twins[i].layers == layers && twins[i].fmt == df)
            t = &twins[i];
    if (!t) {
        t = &twins[twin_next++ % TWINS];
        if (t->img) {   // the submission being recorded may still sample the replaced twin
            Twin *old = malloc(sizeof *old);
            if (!old) rt_fatal("vk: out of memory (depth twin)");
            *old = *t;
            vk_completion_add(vk_sync_issued() + 1, twin_free, old);
        }
        *t = (Twin){.src = img->image, .w = img->width, .h = img->height, .layers = layers, .fmt = df};
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = df,
                                .extent = {img->width, img->height, 1}, .mipLevels = 1, .arrayLayers = layers,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ci, &ai, &t->img, &t->mem, NULL) != VK_SUCCESS) {
            *t = (Twin){0};
            return NULL;
        }
        for (int k = 0; k < 2; k++) {
            VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img,
                                        .viewType = k ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, .format = df,
                                        .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, k ? layers : 1}};
            if (vkCreateImageView(c->device, &vi, NULL, &t->view[k]) != VK_SUCCESS) t->view[k] = VK_NULL_HANDLE;
        }
        static int nlog;
        if (nlog++ < 8)
            rt_log("vk_exec: depth twin %ux%ux%u %s for a compute pass's compare sampler", img->width, img->height,
                   layers, df == VK_FORMAT_D32_SFLOAT ? "D32_SFLOAT" : "D16_UNORM");
    }
    // The source's earlier writes and the twin's earlier readers before the copies; the twin's old texels are dropped.
    VkMemoryBarrier2 m0 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = t->img, .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers}};
    VkDependencyInfo d0 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m0,
                           .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(ex->cb, &d0);
    VkBufferImageCopy src = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers},
                             .imageExtent = {img->width, img->height, 1}};
    vkCmdCopyImageToBuffer(ex->cb, img->image, VK_IMAGE_LAYOUT_GENERAL, twin_buf, 1, &src);
    VkMemoryBarrier2 m1 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo d1 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m1};
    vkCmdPipelineBarrier2(ex->cb, &d1);
    VkBufferImageCopy dst = {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, layers},
                             .imageExtent = {img->width, img->height, 1}};
    vkCmdCopyBufferToImage(ex->cb, twin_buf, t->img, VK_IMAGE_LAYOUT_GENERAL, 1, &dst);
    // The twin's texels before the dispatch; the source's next writers and the buffer's next copy after these copies.
    VkMemoryBarrier2 m2 = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d2 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m2};
    vkCmdPipelineBarrier2(ex->cb, &d2);
    ex->recorded += 5;
    return t;
}

int vk_exec_draw_samples(VkExec *ex, const GfxShader *fs, const VkTexImage *img) {
    const GfxShaderRes *res;
    int n = fs && img ? gfx_shader_resources(fs, GFX_RES_TEX, &res) : 0, hit = 0;
    if (!n) return 0;
    vk_texture_lock();
    for (int i = 0; i < n && !hit; i++) {
        if (res[i].separate || res[i].slot < 0) continue;
        for (int k = 0; k < (res[i].count ? res[i].count : 1) && !hit; k++) {
            int slot = res[i].slot + k;
            uint64_t h = slot < 128 ? ex->tex[NVN_STAGE_FS][slot] : 0;
            hit = h && vk_texture_by_id_locked((int)(h & 0xfffff), NULL, NULL) == img;
        }
    }
    vk_texture_unlock();
    return hit;
}

// The image descriptor of slot h; in a dispatch, a compare-sampled colour image the driver cannot compare is swapped
// for its depth twin; in a pass larger than it, the scene's light target, once DLSS scaled it this frame, for DLSS's
// output (vk_dlaa.c).
static void tex_info(VkExec *ex, const GfxShaderRes *r, uint64_t h, int storage, int separate, VkDescriptorImageInfo *out,
                     int *locked) {
    VkExecDraw *D = ex->draw;
    tex_info_lookup(ex, r, h, storage, separate, out, locked);
    if (h && !storage && vk_dlaa_swap_img && ex->in_pass && ex->pass_w > vk_dlaa_swap_img->width + 1) {
        tex_lock(locked);
        if (vk_texture_by_id_locked((int)(h & 0xfffff), NULL, NULL) == vk_dlaa_swap_img)
            out->imageView = vk_dlaa_swap_view, out->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    if (!D->twin_dispatch || storage || separate || !h || !(r->type & 0x400)) return;
    VkImageViewType vt = res_view_type(r->type);
    if (vt != VK_IMAGE_VIEW_TYPE_2D && vt != VK_IMAGE_VIEW_TYPE_2D_ARRAY) return;
    tex_lock(locked);
    const VkTexImage *img = vk_texture_by_id_locked((int)(h & 0xfffff), NULL, NULL);
    plat_mutex_lock(&twin_lock);
    Twin *t = img ? twin_update(ex, img) : NULL;
    VkImageView v = t ? t->view[vt == VK_IMAGE_VIEW_TYPE_2D_ARRAY] : VK_NULL_HANDLE;
    plat_mutex_unlock(&twin_lock);
    if (v) out->imageView = v, out->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
}

static VkDescriptorSet alloc_set(VkExec *ex, VkDescriptorSetLayout layout) {
    CbRes *r = cb_res(ex);
    VkCtx *c = vk_ctx();
    for (int attempt = 0; attempt < 2; attempt++) {
        if (r->npools) {
            VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                              .descriptorPool = r->pools[r->npools - 1], .descriptorSetCount = 1,
                                              .pSetLayouts = &layout};
            VkDescriptorSet set;
            VkResult res = vkAllocateDescriptorSets(c->device, &ai, &set);
            if (res == VK_SUCCESS) return set;
            if (res != VK_ERROR_OUT_OF_POOL_MEMORY && res != VK_ERROR_FRAGMENTED_POOL) rt_fatal("vk: descriptor set allocation failed (%d)", (int)res);
        }
        static const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 256},
            {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 256}};
        VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 256,
                                         .poolSizeCount = sizeof sizes / sizeof sizes[0], .pPoolSizes = sizes};
        if (r->npools == r->cpools) r->pools = grow(r->pools, &r->cpools, sizeof *r->pools);
        VkDescriptorPool pool = VK_NULL_HANDLE;
        plat_mutex_lock(&spare_lock);
        if (nspare_pool) pool = spare_pool[--nspare_pool];
        plat_mutex_unlock(&spare_lock);
        if (!pool && vkCreateDescriptorPool(c->device, &pi, NULL, &pool) != VK_SUCCESS) rt_fatal("vk: descriptor pool failed");
        r->pools[r->npools++] = pool;
    }
    rt_fatal("vk: descriptor set allocation failed twice");
    return VK_NULL_HANDLE;
}

// Sets come from the submission's pools in batches of one layout that grow 1, 2, 4, 8, 16 while the layout repeats,
// and are handed out one by one: a vkAllocateDescriptorSets per set costs the driver a lock and a free-list walk. A
// batch belongs to its submission's pools (fence) and dies with their reset; sets it leaves unused stay in the pool.
static VkDescriptorSet alloc_set_batched(VkExec *ex, VkDescriptorSetLayout layout) {
    VkExecDraw *D = ex->draw;
    CbRes *r = cb_res(ex);
    ASetCache *c = &D->aset[(uint32_t)(((uintptr_t)layout >> 4) * 2654435761u) >> 28];
    if (c->layout == layout && c->fence == ex->fence && c->n > 0) return c->set[--c->n];
    if (c->layout != layout) c->layout = layout, c->next = 1;
    else if (c->next < ASET_BATCH) c->next *= 2;
    c->n = 0;
    int want = c->next;
    if (want > 1 && r->npools) {
        VkDescriptorSetLayout ls[ASET_BATCH];
        VkDescriptorSet got[ASET_BATCH];
        for (int i = 0; i < want; i++) ls[i] = layout;
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = r->pools[r->npools - 1], .descriptorSetCount = (uint32_t)want,
                                          .pSetLayouts = ls};
        if (vkAllocateDescriptorSets(vk_ctx()->device, &ai, got) == VK_SUCCESS) {
            for (int i = 1; i < want; i++) c->set[i - 1] = got[i];
            c->n = want - 1, c->fence = ex->fence;
            return got[0];
        }   // the pool is nearly full: one set, from a new pool when needed
    }
    return alloc_set(ex, layout);
}

static void support_in(const VkExec *ex, float in[3]) {   // what support_fill reads: viewport size, point size
    float k = ex->pass_scale > 1 ? (float)ex->pass_scale : 1.0f;
    in[0] = ex->in_pass && ex->has_vp ? ex->vp[0][2] * k : (float)ex->pass_w;
    in[1] = ex->in_pass && ex->has_vp ? ex->vp[0][3] * k : (float)ex->pass_h;
    in[2] = ex->point_size > 0 ? ex->point_size : 1.0f;
}

// TOTK_PCSS (percent, 0 = off): the pcss field of the support buffer, which translator patch 0010 reads in every
// explicit-LOD compare on a 2D array texture (the sun cascades). x: penumbra width in UV per unit of light-space depth
// between receiver and blocker (0 keeps the game's single compare), y and z: the narrowest and widest disc radius,
// w: the blocker search radius, all in UV of the cascade map. Radii are in UV, so they hold at any TOTK_SHADOW_SCALE.
// TOTK_PCSS_K overrides the depth-to-penumbra factor at 100 percent, for tuning.
static const float *pcss_params(void) {
    static float p[4] = {-1, 0, 0, 0};
    if (p[0] < 0) {
        int pct = cfg_int("TOTK_PCSS");
        pct = pct < 0 ? 0 : pct > 200 ? 200 : pct;
        const char *k = cfg_get("TOTK_PCSS_K");
        float k100 = k && *k ? (float)atof(k) : 0.5f;
        p[0] = k100 * (float)pct / 100.0f;
        p[1] = 0.35f / 960.0f, p[2] = 5.0f / 960.0f, p[3] = 4.0f / 960.0f;
        if (pct) rt_log("vk: soft shadows (PCSS) at %d%% (depth factor %g)", pct, p[0]);
    }
    return p;
}

// The shadow_spread field of the support buffer: the factor translator patch 0011 applies to the lighting shaders' sun
// shadow filter offset (element 56 .xy of their lighting block), 1 / TOTK_SHADOW_SCALE. The game spaces its four
// compares for its own shadow map texels; with maps TOTK_SHADOW_SCALE times larger, the same offset in UV spans that
// many more texels and the near cascade shows four separate copies of each edge. Dividing by the scale keeps the game's
// spacing in texels. The offset is in UV of the shadow map, so the output resolution plays no part. The scale is read
// once, as vk_texture.c shadow_scale reads it: the maps keep the size they were made with.
static const float *shadow_spread_params(void) {
    static float p[4] = {0, 0, 0, 0};
    if (p[0] == 0) {
        int scale = cfg_int("TOTK_SHADOW_SCALE");
        scale = scale < 1 ? 1 : scale > 4 ? 4 : scale;
        p[0] = 1.0f / (float)scale;
    }
    return p;
}

static void support_fill(uint8_t *b, float w, float h, float point_size) {
    memset(b, 0, SUPPORT_BYTES);
    float inv[4] = {w > 0 ? 2.0f / w : 0, h > 0 ? 2.0f / h : 0, 0, 0}, size[4] = {w, h, 0, 0};
    memcpy(b + 144, inv, 16);
    memcpy(b + 160, size, 16);
    const float one = 1.0f;
    memcpy(b + 1392, &point_size, 4);   // point_size (translator patch 0008): vertex shaders that write none output it
    memcpy(b + 1408, pcss_params(), 16);   // pcss (translator patch 0010): soft sun shadows in the cascade compares
    memcpy(b + 1424, shadow_spread_params(), 16);    // shadow_spread (translator patch 0011): sun shadow filter width
    for (int i = 0; i < 73; i++) memcpy(b + 192 + 16 * i, &one, 4);   // render scale 1: the host renders at guest size
    if (vk_hdr_wanted()) {   // read by the HDR tonemap only (shaders/hdr_tonemap.frag), which declares it past the translator's
        float hdr[4];
        vk_hdr_support(hdr);
        memcpy(b + 1440, hdr, 16);
    }
}

// TOTK_BIND_STATS=1: bindings and descriptors per draw of each stage (Task 2.2's choice of the pushed stage), and how
// often a draw repeats the previous one, logged every 20000 draws: pushes recorded and skipped, allocated sets written
// and reused, stage texture-type recomputations (answers_for), draws with the previous draw's program, bound state
// and textures (shaders and pipeline would be the same) and those that also have its buffers and viewport (all
// descriptors would be the same). BS is declared beside DS.
static int bind_stats_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_get("TOTK_BIND_STATS");
        on = e && *e && strcmp(e, "0");
    }
    return on;
}

static void bind_stats(VkExecDraw *D, const VkPipeInfo *info) {
    if (!bind_stats_on() || info->bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        int fs = info->n_sets > 1 ? b->set : !(b->stages & VK_SHADER_STAGE_VERTEX_BIT);
        D->BS.bind[fs]++, D->BS.desc[fs] += b->count ? b->count : 1;
    }
    if (++D->BS.draws % 20000 == 0) {
        double d = (double)D->BS.draws;
        rt_log("vk_exec: per draw over %llu draws: vertex %.2f bindings %.2f descriptors, fragment %.2f bindings %.2f descriptors",
               (unsigned long long)D->BS.draws, D->BS.bind[0] / d, D->BS.desc[0] / d, D->BS.bind[1] / d, D->BS.desc[1] / d);
        rt_log("vk_exec: per draw: pushes %.3f skipped %.3f, sets written %.3f reused %.3f, texture types %.3f; "
               "same program and state as the draw before %.3f, also same buffers %.3f; descriptor builds skipped %.3f",
               D->BS.push / d, D->BS.push_skip / d, D->BS.set_new / d, D->BS.set_reuse / d, D->BS.ac_miss / d, D->BS.same_prog / d,
               D->BS.same_all / d, D->BS.bind_skip / d);
    }
}

// The bound state a draw reads, compared with the previous draw's (TOTK_BIND_STATS only).
static void repeat_stats(const VkExec *ex, const VkPipeInfo *info) {
    VkExecDraw *D = ex->draw;
    static struct {
        const prog_t *prog;
        VkPipeState pipe;
        uint64_t tex[2][128], img[2][32];
        const VkPipeInfo *info;
        p_bind_buf ubo[2][18], ssbo[2][16];
        float vp[6], point_size;
    } last, cur;
    if (!bind_stats_on()) return;
    memset(&cur, 0, sizeof cur);
    cur.prog = ex->prog, cur.pipe = ex->pipe, cur.info = info;
    const int st[2] = {NVN_STAGE_VS, NVN_STAGE_FS};
    for (int k = 0; k < 2; k++) {
        memcpy(cur.tex[k], ex->tex[st[k]], sizeof cur.tex[k]), memcpy(cur.img[k], ex->img[st[k]], sizeof cur.img[k]);
        memcpy(cur.ubo[k], ex->ubo[st[k]], sizeof cur.ubo[k]), memcpy(cur.ssbo[k], ex->ssbo[st[k]], sizeof cur.ssbo[k]);
    }
    memcpy(cur.vp, ex->vp[0], sizeof cur.vp), cur.point_size = ex->point_size;
    size_t prog_part = offsetof(typeof(cur), info);
    if (!memcmp(&cur, &last, prog_part)) {
        D->BS.same_prog++;
        if (!memcmp((const char *)&cur + prog_part, (const char *)&last + prog_part, sizeof cur - prog_part)) D->BS.same_all++;
    }
    last = cur;
}

static int push_skip_on(void) {   // TOTK_PUSH_SKIP=0: every graphics draw pushes its set again
    static int on = -1;
    if (on < 0) {
        on = cfg_bool("TOTK_PUSH_SKIP");
    }
    return on;
}

// A graphics draw whose descriptor inputs equal the previous graphics draw's in this command buffer skips building its
// sets: every write would be the same, the push would be skipped and the allocated set reused without a bind (about a
// quarter of field and monsters2 draws, TOTK_BIND_STATS). The inputs, compared and stored in place, are what
// bind_resources and its helpers read beyond the pipeline info's fixed bindings and Z's stand-ins:
// - the info (bindings, layouts), the program and its serial (the compiler-constant range of UBO slot 1);
// - vk_texture_slots_gen, vk_texture_epoch (a slot's image, view and once-per-submit storage check), vk_sampler_gen;
// - in a chunk, the pass: a slot can resolve otherwise from one op to the next (vk_texture_slot_moves), at a SET_RT or a copy;
// - vk_mem_gen (buffer and offset of a span, mirror creation and per-submission sync);
// - the images in the feedback layout (the sampled layout);
// - per binding: the support inputs, c0's storage table and base instance, the bound uniform and storage ranges, the
//   texture and image handles (RsList; compared only when a bind op ran since the last draw, bind_gen);
// - storage buffer addresses read from user uniform blocks (RsList.p, ubo_ptr): compared on every draw, since the
//   block's contents change without a bind op.
// The side effects of a build repeat as no-ops in the same command buffer and submission: c0 and support buffers kept
// in B, note_written's ranges, vk_mem_gpu_wrote's write-backs, the per-submission mirror sync and dirty census note,
// the views cached per submission. The census counts are added again. A build that copied guest bytes into transient
// memory (an unaligned host range) is never skipped after: the copy may differ. B.res_valid says the sets bound hold
// the last build; vk_exec_draw_new_cb's memset clears it, and every build clears it until it ends.
// TOTK_BIND_SKIP=0 builds every draw; =check builds every draw and stops when a draw with equal inputs gets other
// writes, census counts or descriptor commands.
static int bind_skip_on(void) {   // TOTK_BIND_SKIP: 1 skip (default), 2 check, 0 off
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_value("TOTK_BIND_SKIP");
        on = !strcmp(e, "check") ? 2 : strcmp(e, "0") != 0;
        if (on != 1) rt_log("vk_exec: descriptor build skip %s", on ? "check" : "off");
    }
    return on;
}

static void rs_add(RsRange *r, uint32_t *n, const VkExec *ex, const void *at, size_t len) {
    uint32_t off = (uint32_t)((const uint8_t *)at - (const uint8_t *)ex);
    if (*n && r[*n - 1].off + r[*n - 1].len == off) r[*n - 1].len += (uint32_t)len;
    else r[(*n)++] = (RsRange){off, (uint32_t)len};
}

static void bi_mask(const VkPipeInfo *info, BiMask *m) {
    memset(m, 0, sizeof *m);
    m->gfx = info->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        const GfxShaderRes *r = b->res;
        uint32_t cnt = b->count ? b->count : 1;
        int st = nvn_stage_of(b->stage);
        if (st != NVN_STAGE_VS && st != NVN_STAGE_FS && b->kind != VK_PIPE_SUPPORT) {
            m->gfx = 0;
            continue;
        }
        m->ndesc = (uint16_t)(m->ndesc + cnt);
        if (b->kind == GFX_RES_UBO) {
            for (uint32_t k = 0; k < cnt; k++) {
                int slot = r->slot + (int)k;
                if (slot == 0) m->c0[st] = 1;
                else if (slot == 1) m->c1[st] = 1;
                else if (slot >= 3 && slot - 3 < 18) m->ubo[st] |= 1u << (slot - 3);
            }
        } else if (b->kind == GFX_RES_SSBO) {
            int ubo, slot = ssbo_slot(r, st, &ubo);
            if (r->flags & 1) m->wr = 1;
            if ((r->flags & 1) || slot < 0 || slot >= 16 || ubo) m->always = (uint16_t)(m->always + cnt);
            else m->ssbo[st] |= 1u << slot;
        } else if ((b->kind == GFX_RES_TEX || b->kind == GFX_RES_IMG) && b->type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER &&
                   b->type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            int img = b->kind == GFX_RES_IMG;
            for (uint32_t k = 0; k < cnt; k++) {
                int slot = r->slot + (int)k;
                if (img && slot >= 0 && slot < 32) m->img[st] |= 1u << slot;
                else if (!img && slot >= 0 && slot < 128) m->tex[st][slot >> 6] |= 1ull << (slot & 63);
            }
        }
        if (b->kind == GFX_RES_TEX && st == NVN_STAGE_FS)
            for (uint32_t k = 0; k < cnt; k++) {
                int slot = r->slot + (int)k;
                if (slot >= 0 && slot < 128) m->ftex[slot >> 6] |= 1ull << (slot & 63);
            }
    }
}

enum { BI_TEX = 1, BI_BUF = 2 };   // BiTest.dep: a kept binding stays valid while G_tex / G_buf stay

// The rules of bi_plan per binding, made once per pipeline info:
// - support: always (its inputs are compared inside, 12 bytes);
// - uniform slot 0 (c0): a storage slot of the stage or the base instance changed; slot 1 (the compiler constants):
//   the program or its serial changed, or G_buf moved; slots 3..20: the slot changed or G_buf moved (snapshots,
//   overlays, pools and mirrors); other slots bind the zero buffer;
// - storage: as a uniform slot; an address held in a uniform block (ubo_ptr): always, the block changes without an op;
// - textures and images: a handle changed or G_tex moved (slot resolution, samplers, the storage check once per
//   submission, the feedback layout, DLSS's swap); buffer textures bind the zero view.
// dep: the generations the binding's build reads, which a build of an earlier binding in the same draw may move.
static BiTest *bi_tests(const VkPipeInfo *info) {
    BiTest *t = calloc(info->n_bindings ? info->n_bindings : 1, sizeof *t);
    if (!t) rt_fatal("vk: out of memory");
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        const GfxShaderRes *r = b->res;
        uint32_t cnt = b->count ? b->count : 1;
        int st = nvn_stage_of(b->stage);
        BiTest *x = &t[i];
        x->st = (uint8_t)(st >= 0 && st < 6 ? st : 0);
        if (b->kind == VK_PIPE_SUPPORT) {
            x->f = BT_ALWAYS;
        } else if (b->kind == GFX_RES_UBO) {
            x->dep = BI_BUF;
            for (uint32_t k = 0; k < cnt; k++) {
                int slot = r->slot + (int)k;
                if (slot == 0) x->f |= BT_C0;
                else if (slot == 1) x->f |= BT_C1 | BT_BUF;
                else if (slot >= 3 && slot - 3 < 18) x->f |= BT_BUF, x->ubo |= 1u << (slot - 3);
            }
        } else if (b->kind == GFX_RES_SSBO) {
            x->dep = BI_BUF;
            int ubo, slot = ssbo_slot(r, st, &ubo);
            if (ubo || slot < 0 || slot >= 16) x->f = BT_ALWAYS;
            else x->f = BT_BUF, x->ssbo = 1u << slot;
        } else if (b->kind == GFX_RES_TEX || b->kind == GFX_RES_IMG) {
            x->dep = BI_TEX;
            if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || b->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) continue;
            int img = b->kind == GFX_RES_IMG;
            x->f = BT_TEX;
            for (uint32_t k = 0; k < cnt; k++) {
                int slot = r->slot + (int)k;
                if (img && slot >= 0 && slot < 32) x->img |= 1u << slot;
                else if (!img && slot >= 0 && slot < 128) x->tex[slot >> 6] |= 1ull << (slot & 63);
            }
        }
    }
    return t;
}

static const RsList *rs_list(const VkExec *ex, const VkPipeInfo *info) {
    VkExecDraw *D = ex->draw;
    if (info == D->rs_last) return D->rs_last_l;
    if (!D->rs_lists.klen) nvn_hm_init(&D->rs_lists, sizeof info);
    RsList *l = nvn_hm_get(&D->rs_lists, &info);
    if (!l) {
        RsRange *tmp = ex->draw->rs_tmp;
        RsPtr ptr[RS_PTRS];
        uint32_t n = 0, nd = 0, np = 0;
        int sup = 0, c0 = 0, ok = 1;
        for (uint32_t i = 0; i < info->n_bindings && ok; i++) {
            const VkPipeBinding *b = &info->bindings[i];
            const GfxShaderRes *r = b->res;
            uint32_t cnt = b->count ? b->count : 1;
            if ((nd += cnt) > MAX_DESC) { ok = 0; break; }   // the build stops on it
            int st = nvn_stage_of(b->stage);
            if (b->kind == VK_PIPE_SUPPORT) {
                sup = 1;
            } else if (b->kind == GFX_RES_UBO) {
                for (uint32_t k = 0; k < cnt; k++) {
                    int slot = r->slot + (int)k;
                    if (slot == 0 && c0_on()) c0 = 1, rs_add(tmp, &n, ex, ex->ssbo[st], sizeof ex->ssbo[st]);
                    else if (slot >= 3 && slot - 3 < 18) rs_add(tmp, &n, ex, &ex->ubo[st][slot - 3], sizeof(p_bind_buf));
                }   // slot 1: the program (bind_sig); others: the zero buffer
            } else if (b->kind == GFX_RES_SSBO) {   // every descriptor of the binding is the same range
                int ubo, slot = ssbo_slot(r, st, &ubo);
                if (slot >= 0 && slot < 16) {
                    rs_add(tmp, &n, ex, ubo ? &ex->ubo[st][slot] : &ex->ssbo[st][slot], sizeof(p_bind_buf));
                } else if (ubo_ptr_res(r)) {
                    if (np == RS_PTRS) { ok = 0; break; }
                    rs_add(tmp, &n, ex, &ex->ubo[st][r->cbuf_slot - 3], sizeof(p_bind_buf));
                    ptr[np++] = (RsPtr){st, r};
                }
            } else if ((b->kind == GFX_RES_TEX || b->kind == GFX_RES_IMG) && b->type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER &&
                       b->type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                int img = b->kind == GFX_RES_IMG, cap = img ? 32 : 128;
                for (uint32_t k = 0; k < cnt; k++) {
                    int slot = r->slot + (int)k;
                    if (slot >= 0 && slot < cap) rs_add(tmp, &n, ex, img ? &ex->img[st][slot] : &ex->tex[st][slot], 8);
                }
            }
        }
        uint64_t bytes = 0;
        for (uint32_t i = 0; i < n; i++) bytes += tmp[i].len;
        BiMask m;
        bi_mask(info, &m);
        if (bytes > RS_BYTES) ok = 0;
        l = malloc(sizeof *l + sizeof *tmp * n);
        if (!l) rt_fatal("vk: out of memory");
        l->n = n, l->bytes = (uint32_t)bytes, l->sup = (uint8_t)sup, l->c0 = (uint8_t)c0, l->ok = (uint8_t)ok;
        l->np = (uint8_t)np;
        l->m = m;
        l->t = bi_tests(info);
        memcpy(l->p, ptr, sizeof *ptr * np);
        memcpy(l->r, tmp, sizeof *tmp * n);
        nvn_hm_put(&D->rs_lists, &info, l);
    }
    D->rs_last = info, D->rs_last_l = l;
    return l;
}

// The signature of a graphics draw's descriptor inputs into RS; 1 when it equals the stored one. The bound-slot bytes
// are compared only when a bind op ran since they were stored or the pipeline changed: otherwise they are the same.
// Check mode compares them anyway and stops when the bind op generation said they could not differ but they do.
static int bind_sig(const VkExec *ex, const VkPipeInfo *info) {
    VkExecDraw *D = ex->draw;
    const RsList *l = rs_list(ex, info);
    if (!l->ok) return D->RS.hn = 0, D->RS.info = NULL, 0;
    uint64_t *h = D->RS.h, diff = 0;
    uint32_t n = 0;
#define PUT(v) do { uint64_t v_ = (uint64_t)(v); diff |= h[n] ^ v_; h[n++] = v_; } while (0)
    PUT((uintptr_t)info);
    PUT((uintptr_t)ex->prog);
    PUT(ex->prog->serial);
    PUT((uint64_t)vk_texture_slots_gen() << 32 | vk_texture_epoch());
    PUT((uint64_t)vk_sampler_gen() << 32 | (uint32_t)ex->nfbl);
    PUT(ex->chunked ? ex->pass_serial : 0);
    PUT(vk_mem_gen() + vk_usnap_gen() + ex->drop_n);   // all only grow in a submission; drop_n: copies a chunk passed
    for (int i = 0; i < ex->nfbl && i < 9; i++) PUT((uintptr_t)ex->fbl_img[i]);
    if (l->sup) {
        float in[3];
        support_in(ex, in);
        uint64_t a;
        uint32_t ps;
        memcpy(&a, in, 8), memcpy(&ps, &in[2], 4);
        PUT(a);
        PUT(ps);
    }
    if (l->c0) PUT(D->c0_binst);
    for (int i = 0; i < l->np; i++) PUT(ubo_ptr(ex, l->p[i].st, l->p[i].res));
    // The uniform overlays the draw's uniform and storage bindings read (op_update_ub): a draw of an updated block binds
    // other bytes after each update, every other draw is left to skip
    if (OV.n) {
        uint64_t ov = 0;
        const size_t lo = offsetof(VkExec, ubo), hi = offsetof(VkExec, ssbo) + sizeof ex->ssbo;
        for (uint32_t i = 0; i < l->n; i++) {
            const RsRange *r = &l->r[i];
            if (r->off + r->len <= lo || r->off >= hi) continue;
            for (uint32_t o = 0; o + sizeof(p_bind_buf) <= r->len; o += sizeof(p_bind_buf)) {
                size_t at = r->off + o;
                if (at < lo || at + sizeof(p_bind_buf) > hi) continue;
                if ((at < offsetof(VkExec, ssbo) ? at - lo : at - offsetof(VkExec, ssbo)) % sizeof(p_bind_buf)) continue;
                const p_bind_buf *b = (const p_bind_buf *)((const uint8_t *)ex + at);
                if (b->addr) ov = ov * 1099511628211ull ^ ov_at(ex, b->addr, b->size ? b->size : ov_rest(b->addr));
            }
        }
        for (int i = 0; i < l->np; i++) {   // storage read through an address in a uniform block: no size either
            uint64_t a = ubo_ptr(ex, l->p[i].st, l->p[i].res);
            if (a) ov = ov * 1099511628211ull ^ ov_at(ex, a, ov_rest(a));
        }
        PUT(ov);
    } else {
        PUT(0);
    }
#undef PUT
    diff |= n != D->RS.hn;
    D->RS.hn = n;
    int fast = D->RS.info == info && D->RS.gen == D->bind_gen;
    if (fast && bind_skip_on() != 2) return !diff;
    const uint8_t *e = (const uint8_t *)ex;
    uint8_t *d = D->RS.body;
    int bd = 0;
    for (uint32_t i = 0; i < l->n; i++) {
        const RsRange *r = &l->r[i];
        if (!bd && memcmp(d, e + r->off, r->len)) bd = 1;
        if (bd) memcpy(d, e + r->off, r->len);
        d += r->len;
    }
    if (fast && bd) rt_fatal("vk_exec: descriptor build skip check: bound slots changed without a bind op");
    D->RS.info = info, D->RS.gen = D->bind_gen;
    return !diff && !bd;
}

// Writes allocated set s from its packed descriptors d: through the layout's update template, or with the write
// structs w[i0..i1) when it has none.
static void write_set(const VkPipeInfo *info, uint32_t s, VkDescriptorSet set, const VkDescSlot *d, VkWriteDescriptorSet *w,
                      uint32_t i0, uint32_t i1) {
    if (i1 == i0) return;
    if (info->tpl[s]) {
        vk_pipeline_update_tpl_fn()(vk_ctx()->device, set, info->tpl[s], d);
        return;
    }
    for (uint32_t k = i0; k < i1; k++) w[k].dstSet = set;
    vkUpdateDescriptorSets(vk_ctx()->device, i1 - i0, w + i0, 0, NULL);
}

// Writes the pipeline's sets from the bound state: set 0, and set 1 with independent layouts. Each is pushed into the
// command buffer, or allocated, written and bound; a graphics set whose contents equal the last one allocated for that
// set number in this command buffer is reused, and bound again only when another layout or a push came between. A
// graphics push equal to the last one at its set number, with no other layout or bind between, is not recorded.
// The descriptors go into one packed array d, a VkDescSlot per descriptor in binding order, which a set's update
// template reads directly (VkPipeInfo.tpl); write structs are filled only when a set has no template.
static int bind_inc_mode(void);
static void bi_mismatch(VkExec *ex, int kind, const char *fmt, ...);

static void bi_gt(const VkExec *ex, uint64_t g[4]);
static void bi_gb(const VkExec *ex, uint64_t g[4]);

// TOTK_BIND_INC: whether a graphics draw with the last prepared draw's pipeline may keep descriptors of its last build,
// and the BiTest flags that hold for every binding of this draw (*fl), from the marks since that draw and the
// generations G_tex and G_buf read right before this build (gt, gb) against those read before its build. The tests
// (bi_tests, per binding) are evaluated in bind_resources' loop. NULL when nothing can be kept: no last draw in this
// command buffer, another pipeline, a chunk, a storage binding the shaders write (its build restarts passes and notes
// writes). A descriptor whose last build copied guest bytes or counted more than once (BiMeta, BiDesc.vol) is always
// built again.
static const BiTest *bi_plan(VkExec *ex, const VkPipeInfo *info, const uint64_t gt[4], const uint64_t gb[4], int *fl) {
    VkExecDraw *D = ex->draw;
    const BiLast *L = &D->BL;
    if (!D->B.bi_valid || ex->chunked || L->info != info || D->BI.info != info) return NULL;
    const RsList *l = rs_list(ex, info);
    if (!l->m.gfx || l->m.wr) return NULL;
    int f = BT_ALWAYS;
    if (memcmp(gt, L->gt, 4 * sizeof *gt)) f |= BT_TEX;
    if (memcmp(gb, L->gb, 4 * sizeof *gb)) f |= BT_BUF;
    if (ex->dirt.prog || ex->prog != L->prog || ex->prog->serial != L->serial) f |= BT_C1;
    if (D->c0_binst != L->binst) f |= BT_C0;
    *fl = f;
    return l->t;
}

static inline int bi_again(const BiTest *t, const VkExecDirty *dt, int fl, uint8_t vol) {
    int st = t->st;
    return vol | ((t->f & fl) != 0) | ((t->f & BT_C0) && dt->ssbo[st]) | ((t->ubo & dt->ubo[st]) != 0) |
           ((t->ssbo & dt->ssbo[st]) != 0) | ((t->img & dt->img[st]) != 0) |
           (((t->tex[0] & dt->tex[st][0]) | (t->tex[1] & dt->tex[st][1])) != 0);
}

static void bind_resources(VkExec *ex, const VkPipeInfo *info, const uint64_t *bgt, const uint64_t *bgb) {
    VkExecDraw *D = ex->draw;
    VkWriteDescriptorSet *w = ex->draw->w;
    VkBufferView *bv = ex->draw->bv;
    if (info->n_bindings > MAX_BINDINGS) rt_fatal("vk: %u descriptor bindings exceed the replay's %d", info->n_bindings, MAX_BINDINGS);
    bind_stats(ex->draw, info);
    int gfx = info->bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS;
    // TOTK_BIND_INC: a graphics build writes BI's array and per-descriptor census (bgt: a prepared draw's), on builds
    // only the bindings bi_plan names, check builds everything and compares what on would have kept.
    int bim = gfx && bgt ? bind_inc_mode() : 0;
    int fl = 0;
    const BiTest *bt = bim ? bi_plan(ex, info, bgt, bgb, &fl) : NULL;
    int inc = bt != NULL;
    VkDescSlot *d = bim ? D->BI.d : ex->draw->d;
    BiMeta *meta = bim ? D->BI.meta : NULL;
    int skip = gfx && bim != 1 ? bind_skip_on() : 0, same = 0, was_valid = D->B.res_valid;
    if (skip) {
        same = bind_sig(ex, info);
        if (same && was_valid && skip == 1) {
            for (int i = 0; i < D->RS.ncnt; i++) vk_mem_count(D->RS.cnt[i].cls, D->RS.cnt[i].use, D->RS.cnt[i].bytes);
            D->BS.bind_skip++;
            ex->recorded++;
            return;
        }
        D->B.res_valid = 0;   // until the build below ends
        if (skip == 2) memcpy(D->RS.pcnt, D->RS.cnt, sizeof D->RS.cnt), D->RS.pncnt = D->RS.ncnt;
        D->RS.rec = 1, D->RS.ncnt = 0, D->RS.cnt_over = 0, D->RS.volatile_ = 0, D->RS.cmds = 0;
    }
    uint32_t n_sets = info->n_sets ? info->n_sets : 1;
    uint32_t lo[2] = {0, 0}, hi[2] = {0, 0}, d0[2] = {0, 0};   // bindings [lo, hi) and first descriptor per set
    int need_w = 0;                                             // a set without a template: fill the write structs
    for (uint32_t s = 0, i0 = 0; s < n_sets; s++) {
        uint32_t i1 = i0;
        while (i1 < info->n_bindings && (n_sets == 1 || info->bindings[i1].set == s)) i1++;
        lo[s] = i0, hi[s] = i1, i0 = i1;
        if (i1 > lo[s] && !info->tpl[s]) need_w = 1;
    }
    if (inc && need_w) inc = 0;   // a set without a template reads the write structs, which only a full build fills
    int changed[2] = {!inc, !inc};   // inc: a descriptor of the set differs from the last build's
    if (bim == 2 && inc) {   // check: the descriptors before the build, to compare those on would have kept
        uint32_t n = 0;
        for (uint32_t i = 0; i < info->n_bindings; i++) n += info->bindings[i].count ? info->bindings[i].count : 1;
        memcpy(D->BI.old, d, n * sizeof *d), memcpy(D->BI.mold, meta, n * sizeof *meta);
    }
    uint32_t nd = 0;
    // inc: the generations a build in this draw moved (BI_TEX, BI_BUF); per group, a binding was built since it was
    // last read. A kept binding also depends on the generations its build reads: a lookup or resolve of a binding built
    // again in this draw may move them (an entry made, a mirror created), and the bindings after it are then built too.
    int midmv = 0, built = 0;
    const VkExecDirty *dt = &ex->dirt;
    for (uint32_t i = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        const GfxShaderRes *r = b->res;
        uint32_t cnt = b->count ? b->count : 1;
        if (nd + cnt > MAX_DESC) rt_fatal("vk: %u descriptors exceed the replay's %d", nd + cnt, MAX_DESC);
        if (n_sets == 2 && i == lo[1]) d0[1] = nd;
        VkDescSlot *o = &d[nd];
        int st = nvn_stage_of(b->stage), kind = 0;   // kind: 0 buffer, 1 image, 2 texel buffer view
        uint32_t set_of = n_sets == 2 && i >= lo[1] ? 1 : 0;
        int keepb = 0;
        if (inc && !bi_again(&bt[i], dt, fl, D->BI.vol[i])) {
            int dep = bt[i].dep, rd = dep & built & ~midmv;
            if (rd) {
                uint64_t g[4];
                if (rd & BI_TEX) bi_gt(ex, g), midmv |= memcmp(g, bgt, sizeof g) ? BI_TEX : 0;
                else bi_gb(ex, g), midmv |= memcmp(g, bgb, sizeof g) ? BI_BUF : 0;
                built &= ~rd;
            }
            keepb = !(dep & midmv);
        }
        if (!keepb) built = BI_TEX | BI_BUF;
        if (bim == 1 && keepb) {   // kept: what it counted is counted again
            for (uint32_t k = 0; k < cnt; k++)
                if (meta[nd + k].n) vk_mem_count(meta[nd + k].cls, meta[nd + k].use, meta[nd + k].bytes);
            nd += cnt;
            continue;
        }
        VkDescSlot prev[4];   // on: the descriptors before the build, to tell whether the set changed
        int cmp = inc && bim == 1 && cnt <= 4 && !changed[set_of];
        if (cmp) memcpy(prev, o, cnt * sizeof *o);
        else if (inc && bim == 1) changed[set_of] = 1;
        if (meta) memset(&meta[nd], 0, cnt * sizeof *meta);
#define BI_CUR(k) (D->BI.cur = meta ? &meta[nd + (k)] : NULL)
        if (b->kind == VK_PIPE_SUPPORT) {
            float in[3];
            support_in(ex, in);
            if (!D->B.sup_valid || MX_NE(in, D->B.sup_in, sizeof in)) {   // support_fill reads only these
                uint8_t tmp[SUPPORT_BYTES];
                support_fill(tmp, in[0], in[1], in[2]);
                MX_CPY(D->B.sup_in, in, sizeof in);
                if (!D->B.sup_valid || MX_NE(tmp, D->B.support, SUPPORT_BYTES)) {   // a new buffer only when the contents changed
                    VkBuffer sb;
                    VkDeviceSize so;
                    MX_CPY(scratch(ex, SUPPORT_BYTES, &sb, &so), tmp, SUPPORT_BYTES);
                    MX_CPY(D->B.support, tmp, SUPPORT_BYTES);
                    D->B.sup_buf = sb, D->B.sup_off = so, D->B.sup_valid = 1;
                }
            }
            o->buffer = (VkDescriptorBufferInfo){D->B.sup_buf, D->B.sup_off, SUPPORT_BYTES};
        } else if (b->kind == GFX_RES_UBO) {
            for (uint32_t k = 0; k < cnt; k++) BI_CUR(k), o[k].buffer = ubo_info(ex, st, r->slot + (int)k);
        } else if (b->kind == GFX_RES_SSBO) {
            for (uint32_t k = 0; k < cnt; k++) BI_CUR(k), o[k].buffer = ssbo_info(ex, st, r, r->flags & 1);
        } else if (b->kind == GFX_RES_TEX || b->kind == GFX_RES_IMG) {
            int img = b->kind == GFX_RES_IMG;
            if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || b->type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                kind = 2;
                for (uint32_t k = 0; k < cnt; k++) {   // buffer textures are not replayed yet
                    o[k].w[0] = 0, o[k].w[1] = 0, o[k].w[2] = 0;
                    o[k].view = Z.zview;
                    if (need_w) bv[nd + k] = Z.zview;
                }
            } else {
                kind = 1;
                int locked = 0;   // tex_info takes the texture lock for the lookups that need it
                for (uint32_t k = 0; k < cnt; k++) {
                    int slot = r->slot + (int)k;
                    uint64_t h = 0;
                    if (img) h = slot >= 0 && slot < 32 ? ex->img[st][slot] : 0;
                    else h = slot >= 0 && slot < 128 ? ex->tex[st][slot] : 0;
                    BI_CUR(k);
                    tex_info(ex, r, h, img, r->separate, &o[k].image, &locked);
                    o[k].w[2] = (uint32_t)o[k].image.imageLayout;   // zero the tail padding
                }
                if (locked) vk_texture_unlock();
            }
        } else {
            rt_fatal("vk: binding %u has an unknown kind %d", (unsigned)b->binding, (int)b->kind);
        }
        D->BI.cur = NULL;
#undef BI_CUR
        if (meta) {
            uint8_t v = 0;
            for (uint32_t k = 0; k < cnt; k++) v |= meta[nd + k].vol;
            D->BI.vol[i] = v;
        }
        if (cmp && memcmp(prev, o, cnt * sizeof *o)) changed[set_of] = 1;
        if (bim == 2 && keepb) {   // check: on would have kept these
            D->BC.checked++;
            for (uint32_t k = 0; k < cnt; k++) {
                const BiMeta *mo = &D->BI.mold[nd + k], *mn = &meta[nd + k];
                if (!memcmp(&D->BI.old[nd + k], &o[k], sizeof *o) && mo->n == mn->n &&
                    (!mn->n || (mo->cls == mn->cls && mo->use == mn->use && mo->bytes == mn->bytes)))
                    continue;
                bi_mismatch(ex, BIM_DESC, "binding %u (stage %d, kind %d, slot %d + %u, set %u): kept %016llx %016llx %016llx, "
                            "built %016llx %016llx %016llx; census %d/%d/%llu x%u, now %d/%d/%llu x%u", (unsigned)b->binding,
                            st, (int)b->kind, r ? r->slot : -1, k, set_of, (unsigned long long)D->BI.old[nd + k].w[0],
                            (unsigned long long)D->BI.old[nd + k].w[1], (unsigned long long)D->BI.old[nd + k].w[2],
                            (unsigned long long)o[k].w[0], (unsigned long long)o[k].w[1], (unsigned long long)o[k].w[2],
                            mo->cls, mo->use, (unsigned long long)mo->bytes, mo->n, mn->cls, mn->use,
                            (unsigned long long)mn->bytes, mn->n);
                break;
            }
        }
        if (need_w) {
            w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstBinding = b->binding, .descriptorCount = cnt, .descriptorType = b->type};
            if (kind == 0) w[i].pBufferInfo = &o->buffer;   // a VkDescSlot is as large as either info struct
            else if (kind == 1) w[i].pImageInfo = &o->image;
            else w[i].pTexelBufferView = &bv[nd];
        }
        nd += cnt;
    }
    // Under the validation layer, a new graphics layout binds set 1 before set 0 is pushed or bound. The layer (SDK
    // 1.4.363, PipelineLayoutCompatDef::operator==) takes any two INDEPENDENT_SETS layouts as compatible, so
    // vkCmdPushDescriptorSetKHR keeps the push set it made for an earlier layout and drops writes to bindings that
    // layout lacks (false VUID-vkCmdDrawIndexed-None-08114, then an access violation inside the layer). A bind at set 1
    // disturbs set 0 by its canonical id, which drops the layer's push set: the push after it gets a new one.
    int set1_first = 0;
    if (gfx && info->layout != D->B.ds_layout) {   // another layout: sets bound before may be disturbed
        D->B.ds_layout = info->layout;
        D->B.ds_bound[0] = D->B.ds_bound[1] = VK_NULL_HANDLE;
        D->B.ds_pushed[0] = D->B.ds_pushed[1] = 0;
        set1_first = n_sets == 2 && vk_ctx()->validation;
    }
    for (uint32_t k = 0; k < n_sets; k++) {
        uint32_t s = set1_first ? 1 - k : k, i0 = lo[s], i1 = hi[s];
        const VkDescSlot *ds = &d[d0[s]];
        uint32_t dbytes = (uint32_t)(((s + 1 < n_sets ? d0[s + 1] : nd) - d0[s]) * sizeof *d);
        if (s && i1 == i0) {               // no fragment stage: set 1 is empty and unused
            if (set1_first) {              // bound only to drop the layer's push set (comment above)
                VkDescriptorSet set = alloc_set(ex, info->set_layout1);
                vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, 1, 1, &set, 0, NULL);
                D->B.ds_bound[1] = set, D->B.ds_pushed[1] = 0;
                D->RS.cmds++;
            }
            continue;
        }
        if (s ? info->push1 : info->push) {
            uint32_t n = gfx && push_skip_on() ? dbytes : 0;
            if (inc && bim == 1 && !changed[s] && n && D->B.ds_pushed[s] && D->DS.pn[s] == n) {   // psig holds ds
                D->BS.push_skip++;
                continue;
            }
            if (n && D->B.ds_pushed[s] && D->DS.pn[s] == n && MX_EQ(D->DS.psig[s], ds, n)) {
                D->BS.push_skip++;
                continue;
            }
            if (info->tpl[s]) rec_push_tpl(vk_pipeline_push_tpl_fn(), ex->cb, info->tpl[s], info->layout, s, ds, dbytes);
            else rec_push(vk_pipeline_push_fn(), ex->cb, info->bind_point, info->layout, s, i1 - i0, w + i0);
            D->BS.push++;
            D->RS.cmds++;
            if (gfx) {
                D->B.ds_bound[s] = VK_NULL_HANDLE, D->B.ds_pushed[s] = n != 0;
                if (n) D->DS.pn[s] = n, MX_CPY(D->DS.psig[s], ds, n);
            }
        } else if (!gfx || s > 1) {
            VkDescriptorSet set = alloc_set(ex, s ? info->set_layout1 : info->set_layout);
            write_set(info, s, set, ds, w, i0, i1);
            vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, s, 1, &set, 0, NULL);
        } else {
            VkDescriptorSetLayout sl = s ? info->set_layout1 : info->set_layout;
            VkDescriptorSet set = D->B.ds_last[s];
            int kept = inc && bim == 1 && !changed[s] && set && D->B.ds_last_layout[s] == sl && D->DS.n[s] == dbytes;
            if (!kept && (!set || D->B.ds_last_layout[s] != sl || D->DS.n[s] != dbytes || MX_NE(D->DS.sig[s], ds, dbytes))) {
                set = bim == 1 && !ex->chunked ? alloc_set_batched(ex, sl) : alloc_set(ex, sl);
                write_set(info, s, set, ds, w, i0, i1);
                D->B.ds_last[s] = set, D->B.ds_last_layout[s] = sl, D->DS.n[s] = dbytes;
                MX_CPY(D->DS.sig[s], ds, dbytes);
                D->BS.set_new++;
                D->RS.cmds++;
            } else {
                D->BS.set_reuse++;
            }
            if (D->B.ds_bound[s] != set) {
                vkCmdBindDescriptorSets(ex->cb, info->bind_point, info->layout, s, 1, &set, 0, NULL);
                D->B.ds_bound[s] = set, D->B.ds_pushed[s] = 0;
                D->RS.cmds++;
            }
        }
    }
    if (skip) {
        D->RS.rec = 0;
        // An equal next draw records nothing when every push set holds its push (TOTK_PUSH_SKIP on) and nothing made
        // the build unrepeatable; allocated sets are always bound to the one written last.
        int valid = !D->RS.volatile_ && !D->RS.cnt_over && D->RS.hn;
        for (uint32_t s = 0; s < n_sets; s++)
            if (hi[s] > lo[s] && (s ? info->push1 : info->push) && !D->B.ds_pushed[s]) valid = 0;
        if (skip == 2) {   // with the pipeline info equal (bind_sig), the packed descriptors determine every write
            uint32_t n = (uint32_t)(nd * sizeof *d);
            int wr = n != D->RS.ckn || memcmp(D->RS.ck, d, n);
            int cn = D->RS.ncnt != D->RS.pncnt || memcmp(D->RS.cnt, D->RS.pcnt, sizeof D->RS.cnt[0] * (size_t)D->RS.ncnt);
            if (same && was_valid && (wr || cn || D->RS.cmds))
                rt_fatal("vk_exec: descriptor build skip check: a draw of program %llu with the inputs of the previous one "
                         "gets %s", (unsigned long long)ex->prog->serial,
                         D->RS.cmds ? "descriptor commands" : wr ? "other writes" : "other census counts");
            if (same && was_valid) D->BS.bind_skip++;   // the draws a skip would have spared, compared instead
            D->RS.ckn = n;
            memcpy(D->RS.ck, d, n);
        }
        D->B.res_valid = valid;
    }
    if (bim) D->BI.info = info;
    ex->recorded++;
}

// ---- draws

static void bind_pipeline(VkExec *ex, VkPipelineBindPoint bp, VkPipeline p) {
    VkExecDraw *D = ex->draw;
    int k = bp == VK_PIPELINE_BIND_POINT_COMPUTE;
    if (D->B.pipe[k] == p) return;
    vkCmdBindPipeline(ex->cb, bp, p);
    ex->recorded++;
    D->B.pipe[k] = p;
    if (!k) D->B.st_valid = 0;
}

static VkViewport draw_viewport(VkExec *ex) {
    float x = 0, y = 0, w = (float)ex->pass_w, h = (float)ex->pass_h;
    if (ex->has_vp) {
        float k = ex->pass_scale > 1 ? (float)ex->pass_scale : 1.0f;
        x = ex->vp[0][0] * k, y = ex->vp[0][1] * k, w = ex->vp[0][2] * k, h = ex->vp[0][3] * k;
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    VkViewport v = {x, y + h, w, -h, ex->depth_range[0], ex->depth_range[1]};   // the y flip: negative height
    vk_dlaa_viewport(ex, &v);   // the scene's draws move by the DLAA jitter
    return v;
}

static void record_fixed(VkExec *ex) {
    VkCommandBuffer cb = ex->cb;
    if (ex->dirty & VK_EXEC_DIRTY_VIEWPORT) {
        VkViewport v = draw_viewport(ex);
        vkCmdSetViewportWithCount(cb, 1, &v);
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_SCISSOR) {
        VkRect2D r = vk_exec_scissor(ex);
        vkCmdSetScissorWithCount(cb, 1, &r);
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_DEPTH_BIAS) {
        vkCmdSetDepthBias(cb, ex->poly_offset.units, ex->poly_offset.clamp, ex->poly_offset.factor);
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_BLEND_CONST) {
        vkCmdSetBlendConstants(cb, ex->blend_color);
        ex->recorded++;
    }
    if (ex->dirty & VK_EXEC_DIRTY_STENCIL) {
        for (int f = 0; f < 2; f++) {
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            vkCmdSetStencilCompareMask(cb, face, ex->st_vmask[f]);
            vkCmdSetStencilWriteMask(cb, face, ex->st_wmask[f]);
            vkCmdSetStencilReference(cb, face, ex->st_ref[f]);
            ex->recorded += 3;
        }
    }
    ex->dirty = 0;
}

// TOTK_VB_USED=0: every bound stream is resolved and bound, also those the draw's vertex input does not read.
static int vb_used_on(void) {
    static int on = -1;
    if (on < 0) on = cfg_bool("TOTK_VB_USED");
    return on;
}

static void bind_vertex_buffers(VkExec *ex) {
    VkExecDraw *D = ex->draw;
    int dyn = vk_pipeline_vertex_input_dynamic();
    // Only the streams the draw's vertex input reads (D->B.vmask, from vk_pipeline_set_dynamic): the game keeps about ten
    // bound and a draw reads a few. The streams it does not read keep what is bound. Giving them the zero buffer made
    // nearly every draw rebind all 17 slots, since draws in a row read different streams; the NVIDIA driver's cost
    // for those binds took more than the resolves saved (issue #974: RTX 5060 Laptop, 16 % fps back with vb_used=0).
    // A buffer left bound was bound earlier in this command buffer, so the deferred frees keep it alive until the
    // submission is done.
    uint32_t used = vb_used_on() && D->B.st_valid ? D->B.vmask : VK_PIPE_ALL_BINDINGS;
    uint32_t chg = 0;
    // TOTK_BIND_INC: the last prepared draw resolved and bound these streams, and since then no op changed one of their
    // ranges, the pools and mirrors stayed (vk_mem_gen) and, with static vertex input, the strides (pipe_gen): every
    // stream would resolve to what is bound, so the loop is skipped.
    int bim = bind_inc_mode(), keep = 0;
    if (bim && D->B.bi_valid && D->B.vb_valid && !ex->chunked && used == D->BL.vused && !(ex->dirt.vbuf & used) &&
        D->B.vgen == vk_mem_gen() && (dyn || ex->pipe_gen == D->BL.pipe_gen)) {
        if (bim == 1) return;
        keep = 1;
    }
    if (!D->B.vb_valid) {   // the command buffer's first bind sets all 17 slots, the zero buffer where nothing resolves
        for (int b = 0; b < 17; b++)
            D->B.vb[b] = Z.zbuf, D->B.vo[b] = 0, D->B.vsz[b] = ZERO_BYTES, D->B.vstr[b] = b < 16 ? ex->pipe.vstream[b].stride : 0,
            D->B.vcls[b] = 0, D->B.vpool[b] = NULL;
        D->B.vres = 0, chg = VK_PIPE_ALL_BINDINGS;
    }
    // A stream whose range is the one its bound buffer was resolved from, with no pool or mirror change since
    // (vk_mem_gen, read before any resolve), resolves to that buffer again: most draws change one stream or none. A
    // change of vk_mem_gen voids the resolutions of every slot, read by this draw or not.
    uint64_t gen = vk_mem_gen();
    if (D->B.vgen != gen) D->B.vres = 0;
    D->B.vgen = gen;
    for (int b = 0; b < 17; b++) {
        if (!(used >> b & 1)) continue;
        VkBuffer nb = Z.zbuf;
        VkDeviceSize no = 0, nsz = ZERO_BYTES, nstr = b < 16 ? ex->pipe.vstream[b].stride : 0;
        int ncls = 0;
        void *npool = NULL;
        if (b != VK_PIPE_ZERO_BINDING) {
            const NVNbufferRange *r = &ex->vbuf[b];
            if ((D->B.vres >> b & 1) && D->B.vin[b].address == r->address && D->B.vin[b].size == r->size) {
                nb = D->B.vb[b], no = D->B.vo[b], nsz = D->B.vsz[b], ncls = D->B.vcls[b], npool = D->B.vpool[b];
            } else {
                VkMemSpan sp;
                if (r->address && vk_mem_resolve(r->address, 1, &sp)) {
                    VK_MEM_USE(sp);
                    nb = sp.buffer, no = sp.offset, nsz = r->size && r->size < sp.avail ? r->size : sp.avail;
                    ncls = sp.cls, npool = sp.pool;
                }
                D->B.vin[b] = *r, D->B.vres |= 1u << b;
            }
        }
        if (nb != D->B.vb[b] || no != D->B.vo[b] || nsz != D->B.vsz[b] || (!dyn && nstr != D->B.vstr[b])) chg |= 1u << b;
        D->B.vb[b] = nb, D->B.vo[b] = no, D->B.vsz[b] = nsz, D->B.vstr[b] = nstr, D->B.vcls[b] = ncls, D->B.vpool[b] = npool;
    }
    if (keep) {
        D->BC.checked++;
        if (chg) bi_mismatch(ex, BIM_VB, "streams %#x would be bound again (used %#x)", chg, used);
    }
    if (!chg) return;
    // One bind over the changed slots; the slots between them get what they hold again.
    int lo = __builtin_ctz(chg), hi = 31 - __builtin_clz(chg);
    vkCmdBindVertexBuffers2(ex->cb, (uint32_t)lo, (uint32_t)(hi - lo + 1), &D->B.vb[lo], &D->B.vo[lo], &D->B.vsz[lo],
                            dyn ? NULL : &D->B.vstr[lo]);
    ex->recorded++;
    for (int b = lo; b <= hi; b++) {
        if (D->B.vcls[b]) vk_mem_count(D->B.vcls[b], VK_MUSE_VB, D->B.vsz[b]);
        if ((rt_trace & RT_TRACE_PERF) && D->B.vpool[b]) vk_mem_bound_note(D->B.vpool[b]);
    }
    D->B.vb_valid = 1;
}

// TOTK_PIPE_SKIP: which draws are skipped, rather than waited for, while their shaders or pipeline build.
// strict (0): only draws whose pass targets were all rendered in the previous presented frame; a pass with a target
//   that was not counts as one-shot and waits.
// any (1): also draws in a pass with at least one target rendered in the previous frame (a per-frame depth
//   buffer or g-buffer with a colour target the frame before did not use: half-resolution effect buffers in fights).
//   Passes with none (volumes, LUTs, shadow layers seen for the first time) still wait, and so do passes whose only
//   recent target is the depth buffer while a colour target is rendered for the first time (item icon captures).
// fullscreen (2, default): as any, and full-screen draws into such passes are skipped instead of waiting from their
//   budget. A full-screen pass whose stage libraries are cold in the driver cache took 86 ms on average (up to 194 ms)
//   in a fight on an RX 9070: three frames in a row each spent the 50 ms budget, and some of the draws were skipped
//   after all. Skipping costs the pass for the frames until the pipeline is ready.
static int pipe_skip_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = cfg_value("TOTK_PIPE_SKIP");
        mode = !strcmp(e, "strict") || !strcmp(e, "0") ? 0 : !strcmp(e, "fullscreen") || !strcmp(e, "2") ? 2 : 1;
        static const char *const what[3] = {"only draws into per-frame passes (TOTK_PIPE_SKIP=strict)",
                                            "draws into passes with a target the previous frame rendered",
                                            "those and full-screen draws (TOTK_PIPE_SKIP=fullscreen)"};
        rt_log("vk_exec: draws skipped while their pipeline builds: %s", what[mode]);
    }
    return mode;
}

// The previous draw's shaders, kept while its program, bound pipeline state, the vertex and fragment stages' first 32
// texture and image handles and every slot's resolution (vk_texture_slots_gen, read unlocked: a registration racing
// with it is ordered after this draw, as answers_for's own check would order it) stay the same: answers_for reads
// nothing else, so shader_for would return the same shaders. About 65% of field draws repeat them
// (TOTK_BIND_STATS). TOTK_SHADER_MEMO=0 turns it off; =check looks the shaders up on every hit too, and every slot's
// texture type in answers_for, and stops on a difference.
// tex_serial (bumped by every texture and image bind) equal to the key's means the handles were not rebound since
// they were compared, so the 1 KiB compare is skipped; a rebind to the same handles still matches by the compare.

static void memo_key(const VkExec *ex, uint32_t gen, MemoKey *k) {
    k->prog = ex->prog, k->serial = ex->prog->serial, k->gen = gen, k->tex_serial = ex->tex_serial;
    k->pipe_gen = ex->pipe_gen;
    memcpy(k->tex[0], ex->tex[NVN_STAGE_VS], sizeof k->tex[0]), memcpy(k->tex[1], ex->tex[NVN_STAGE_FS], sizeof k->tex[1]);
    memcpy(k->img[0], ex->img[NVN_STAGE_VS], sizeof k->img[0]), memcpy(k->img[1], ex->img[NVN_STAGE_FS], sizeof k->img[1]);
    MX_CPY(&k->pipe, &ex->pipe, sizeof k->pipe);
}

static int bind_inc_mode(void);
static void bi_mismatch(VkExec *ex, int kind, const char *fmt, ...);

// TOTK_BIND_INC: pipe equals the key's while pipe_gen does (no write changed it since the copy), so the 360-byte
// compare is skipped; check compares anyway and counts a difference the generation missed.
static int memo_match(VkExec *ex, uint32_t gen, MemoKey *k) {
    if (k->prog != ex->prog || k->serial != ex->prog->serial || k->gen != gen) return 0;
    int bim = bind_inc_mode(), same_gen = k->pipe_gen == ex->pipe_gen;
    if (!(bim == 1 && same_gen && !ex->chunked) && MX_NE(&k->pipe, &ex->pipe, sizeof k->pipe)) {
        if (bim == 2 && same_gen && !ex->chunked) bi_mismatch(ex, BIM_PIPE, "shader memo: bound state changed without its generation");
        return 0;
    }
    k->pipe_gen = ex->pipe_gen;
    if (k->tex_serial == ex->tex_serial) return 1;
    if (MX_NE(k->tex[0], ex->tex[NVN_STAGE_VS], sizeof k->tex[0]) || MX_NE(k->tex[1], ex->tex[NVN_STAGE_FS], sizeof k->tex[1]) ||
        MX_NE(k->img[0], ex->img[NVN_STAGE_VS], sizeof k->img[0]) || MX_NE(k->img[1], ex->img[NVN_STAGE_FS], sizeof k->img[1]))
        return 0;
    k->tex_serial = ex->tex_serial;
    return 1;
}

static int shader_memo_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cfg_value("TOTK_SHADER_MEMO");
        on = !strcmp(e, "check") ? 2 : strcmp(e, "0") != 0;
    }
    return on;
}

// ---- inline compares and copies (memx.h, TOTK_MEMX)
int vk_memx_mode = -1;

void vk_memx_init(void) {
    if (vk_memx_mode >= 0) return;
    const char *e = cfg_value("TOTK_MEMX");
    int m = !strcmp(e, "check") ? 2 : strcmp(e, "0") && strcmp(e, "off") ? 1 : 0;
    rt_log("vk: inline compares and copies (TOTK_MEMX) %s", m == 2 ? "check" : m ? "on" : "off");
    __atomic_store_n(&vk_memx_mode, m, __ATOMIC_RELAXED);
}

void vk_memx_bad(const char *file, int line, size_t n) {
    static int logged;
    static long total;
    long t = __atomic_add_fetch(&total, 1, __ATOMIC_RELAXED);
    if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 20 || t % 100000 == 0)
        rt_log("vk: TOTK_MEMX check: the inline compare of %zu bytes at %s:%d differs from memcmp (%ld so far)", n, file,
               line, t);
}

static int pipe_memo_on(void) {
    static int on = -1;
    if (on < 0) on = cfg_bool("TOTK_PIPE_MEMO");
    return on;
}

// ---- incremental draw preparation (TOTK_BIND_INC)
// A draw is prepared from what changed since the context's last prepared draw in the same command buffer: the bind
// ops mark the slots whose bytes they changed (VkExec.dirt, vk_exec_bind), every write that changes the pipeline state
// bumps VkExec.pipe_gen, and the global inputs are read as two groups of generations, G_tex (bi_gt: texture slots,
// samplers, the submission's storage epoch, the images in the feedback layout, DLSS's swap) and G_buf (bi_gb: pool
// index and mirrors, uniform snapshots, uniform overlays, the submission). A step whose inputs are all unchanged keeps
// the last draw's result. A new command buffer or a replaced bound state (vk_exec_draw_new_cb, copy_state) sets every
// mark, the CbState memset drops BL, and a chunk of a chunked replay never keeps anything (its slots resolve per op).
// off: as before. on: the kept results. check: prepares every draw as before, takes every decision of on beside it,
// compares what on would have kept with what the full path gave, logs each kind of difference once with its marks and
// counts them; it also counts the decisions (bi_census) and logs them every 300 frames.
static int bind_inc_mode(void) {   // 0 off, 1 on, 2 check
    static int mode = -1;
    if (mode < 0) {
        const char *e = cfg_value("TOTK_BIND_INC");
        int m = !strcmp(e, "off") || !strcmp(e, "0") ? 0 : !strcmp(e, "check") ? 2 : 1;
        rt_log("vk_exec: incremental draw preparation (TOTK_BIND_INC) %s", m == 2 ? "check" : m ? "on" : "off");
        mode = m;
    }
    return mode;
}

static void bi_gt(const VkExec *ex, uint64_t g[4]) {
    g[0] = (uint64_t)vk_texture_slots_gen() << 32 | vk_sampler_gen();
    g[1] = (uint64_t)vk_texture_epoch() << 32 | ex->fbl_serial;
    g[2] = (uintptr_t)vk_dlaa_swap_img;
    int swap = vk_dlaa_swap_img && ex->in_pass && ex->pass_w > vk_dlaa_swap_img->width + 1;   // tex_info's swap test
    g[3] = swap ? (uint64_t)(uintptr_t)vk_dlaa_swap_view | 1 : 0;
}

static void bi_gb(const VkExec *ex, uint64_t g[4]) {
    g[0] = vk_mem_gen(), g[1] = vk_usnap_gen(), g[2] = OV.gen, g[3] = ex->fence;
}

// check mode: a decision of on would have kept a result that differs from the full path's. Counted; the first of each
// kind is logged with the marks, generations and counters it was taken under.
static void bi_mismatch(VkExec *ex, int kind, const char *fmt, ...) {
    static const char *const name[BIM_N] = {"pipeline state", "recorded dynamic state", "shaders", "feedback scan",
                                            "vertex buffers", "descriptor"};
    static volatile int32_t seen[BIM_N];
    VkExecDraw *D = ex->draw;
    D->BC.mism++, D->BC.mism_total++, D->BC.mism_frame++;
    if (__atomic_exchange_n(&seen[kind], 1, __ATOMIC_RELAXED)) return;
    char msg[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    const VkExecDirty *d = &ex->dirt;
    const BiLast *L = &D->BL;
    uint64_t gt[4], gb[4];
    bi_gt(ex, gt), bi_gb(ex, gb);
    rt_log("bind_inc check: kept %s differs from the full path: %s; program %llu (last %llu), marks prog %u ubo %#x/%#x "
           "ssbo %#x/%#x tex %016llx:%016llx/%016llx:%016llx img %#x/%#x vbuf %#x, pipe_gen %llu (last %llu); G_tex "
           "%llx %llx %llx %llx (last %llx %llx %llx %llx), G_buf %llx %llx %llx %llx (last %llx %llx %llx %llx); "
           "pass %u (last %u), draw %llu of the census window, bi_valid %d",
           name[kind], msg, ex->prog ? (unsigned long long)ex->prog->serial : 0ull, (unsigned long long)L->serial,
           d->prog, d->ubo[0], d->ubo[1], d->ssbo[0], d->ssbo[1], (unsigned long long)d->tex[0][1],
           (unsigned long long)d->tex[0][0], (unsigned long long)d->tex[1][1], (unsigned long long)d->tex[1][0], d->img[0],
           d->img[1], d->vbuf, (unsigned long long)ex->pipe_gen, (unsigned long long)L->pipe_gen,
           (unsigned long long)gt[0], (unsigned long long)gt[1], (unsigned long long)gt[2], (unsigned long long)gt[3],
           (unsigned long long)L->gt[0], (unsigned long long)L->gt[1], (unsigned long long)L->gt[2],
           (unsigned long long)L->gt[3], (unsigned long long)gb[0], (unsigned long long)gb[1], (unsigned long long)gb[2],
           (unsigned long long)gb[3], (unsigned long long)L->gb[0], (unsigned long long)L->gb[1],
           (unsigned long long)L->gb[2], (unsigned long long)L->gb[3], ex->pass_serial, L->pass_serial,
           (unsigned long long)D->BC.draws, D->B.bi_valid);
}

static double bi_pct(uint64_t n, uint64_t of) { return of ? 100.0 * (double)n / (double)of : 0; }

static void bi_log(BiCensus *c) {
    double fr = c->frames ? (double)c->frames : 1, d = c->draws ? (double)c->draws : 1;
    rt_log("bind_inc: %u frames, %.0f draws a frame: nothing changed %.1f%%, uniforms only %.1f%% (%.1f%% of draws with a "
           "buffer generation moved), other slots %.1f%%, another pipeline or nothing kept %.1f%%; descriptors %.2f a draw, "
           "to rebuild per draw of each class %.2f %.2f %.2f %.2f, always %.2f",
           c->frames, d / fr, bi_pct(c->cls[0], c->draws), bi_pct(c->cls[1], c->draws), bi_pct(c->b_gen, c->draws),
           bi_pct(c->cls[2], c->draws), bi_pct(c->cls[3], c->draws), c->desc / d,
           c->cls[0] ? c->rec[0] / (double)c->cls[0] : 0, c->cls[1] ? c->rec[1] / (double)c->cls[1] : 0,
           c->cls[2] ? c->rec[2] / (double)c->cls[2] : 0, c->cls[3] ? c->rec[3] / (double)c->cls[3] : 0, c->always / d);
    rt_log("bind_inc: moved since the draw before (%% of draws): slots %.1f, samplers %.1f, epoch %.1f, feedback layout "
           "%.1f, dlss swap %.1f, pools %.1f, uniform snapshots %.1f, overlays %.1f, submission %.1f, pass %.1f; changed "
           "slots read per draw: uniform %.2f, storage %.2f, texture %.2f, image %.2f, vertex %.2f",
           bi_pct(c->mv[0], c->draws), bi_pct(c->mv[1], c->draws), bi_pct(c->mv[2], c->draws), bi_pct(c->mv[3], c->draws),
           bi_pct(c->mv[4], c->draws), bi_pct(c->mv[5], c->draws), bi_pct(c->mv[6], c->draws), bi_pct(c->mv[7], c->draws),
           bi_pct(c->mv[8], c->draws), bi_pct(c->mv[9], c->draws), c->slots[0] / d, c->slots[1] / d, c->slots[2] / d,
           c->slots[3] / d, c->slots[4] / d);
    rt_log("bind_inc: kept (%% of draws): shaders %.1f (memo today %.1f), pipeline state %.1f, vertex buffers %.1f, "
           "feedback scan %.1f of %.1f (today %.1f); mismatches %.2f a frame, %llu in all, %.0f kept results checked a frame",
           bi_pct(c->sh_keep, c->draws), bi_pct(c->sh_memo, c->draws), bi_pct(c->pg_hit, c->draws),
           bi_pct(c->vb_skip, c->draws), bi_pct(c->fb_keep, c->draws), bi_pct(c->fb_n, c->draws),
           bi_pct(c->fb_memo, c->draws), c->mism / fr, (unsigned long long)c->mism_total, c->checked / fr);
    uint64_t total = c->mism_total;
    uint32_t lines = c->frame_lines;
    memset(c, 0, sizeof *c);
    c->mism_total = total, c->frame_lines = lines;
}

static int popc64(uint64_t v) { return __builtin_popcountll(v); }

// Counts what the draw about to be bound (state s, pipeline info) could keep from the last prepared draw, before
// bind_resources; gt and gb were read right before it, slots_gen before shader_for. sm_hit: today's shader memo hit.
static void bi_census(VkExec *ex, const VkPipeInfo *info, const VkPipeState *s, const uint64_t gt[4],
                      const uint64_t gb[4], uint32_t slots_gen, int sm_hit) {
    VkExecDraw *D = ex->draw;
    BiCensus *c = &D->BC;
    const BiLast *L = &D->BL;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f != c->frame) {
        if (c->mism_frame && c->frame_lines < 200)
            c->frame_lines++, rt_log("bind_inc check: frame %llu: %u mismatches", (unsigned long long)c->frame, c->mism_frame);
        c->mism_frame = 0;
        if (c->frames >= 300) bi_log(c);
        c->frame = f, c->frames++;
    }
    c->draws++;
    c->sh_memo += sm_hit;
    const RsList *l = rs_list(ex, info);
    const BiMask *m = &l->m;
    const VkExecDirty *dt = &ex->dirt;
    if (!D->B.bi_valid || ex->chunked) {
        c->cls[3]++, c->desc += m->ndesc, c->rec[3] += m->ndesc, c->always += m->always;
        return;
    }
    int mv[BI_MV] = {(uint32_t)(gt[0] >> 32) != (uint32_t)(L->gt[0] >> 32), (uint32_t)gt[0] != (uint32_t)L->gt[0],
                     (uint32_t)(gt[1] >> 32) != (uint32_t)(L->gt[1] >> 32), (uint32_t)gt[1] != (uint32_t)L->gt[1],
                     gt[2] != L->gt[2] || gt[3] != L->gt[3], gb[0] != L->gb[0], gb[1] != L->gb[1], gb[2] != L->gb[2],
                     gb[3] != L->gb[3], ex->pass_serial != L->pass_serial};
    for (int i = 0; i < BI_MV; i++) c->mv[i] += (uint64_t)mv[i];
    int tmv = mv[0] || mv[1] || mv[2] || mv[3] || mv[4], bmv = mv[5] || mv[6] || mv[7] || mv[8];
    int prog = dt->prog || ex->prog != L->prog || ex->prog->serial != L->serial;
    int binst = D->c0_binst != L->binst;
    uint32_t du = 0, ds = 0, di = 0, c0d = 0;
    int ndt = 0;
    for (int k = 0; k < 2; k++) {
        du += (uint32_t)__builtin_popcount(dt->ubo[k] & m->ubo[k]);
        ds += (uint32_t)__builtin_popcount(dt->ssbo[k] & m->ssbo[k]);
        di += (uint32_t)__builtin_popcount(dt->img[k] & m->img[k]);
        ndt += popc64(dt->tex[k][0] & m->tex[k][0]) + popc64(dt->tex[k][1] & m->tex[k][1]);
        c0d += m->c0[k] && (dt->ssbo[k] || binst);
    }
    c->slots[0] += du, c->slots[1] += ds, c->slots[2] += (uint64_t)ndt, c->slots[3] += di;
    c->slots[4] += (uint64_t)__builtin_popcount(dt->vbuf & D->B.vmask);
    int same = L->info == info && m->gfx;
    int cls = !same ? 3 : !prog && !tmv && !bmv && !du && !ds && !di && !ndt && !c0d ? 0
                        : !prog && !tmv && !ds && !di && !ndt && !c0d ? 1 : 2;
    c->cls[cls]++;
    if (cls == 1 && bmv) c->b_gen++;
    c->desc += m->ndesc;
    c->always += m->always;
    uint32_t rec = m->ndesc;
    if (same) {
        int nubo = 0, nssbo = 0, ntex = 0, nimg = 0;
        for (int k = 0; k < 2; k++)
            nubo += __builtin_popcount(m->ubo[k]), nssbo += __builtin_popcount(m->ssbo[k]),
                nimg += __builtin_popcount(m->img[k]), ntex += popc64(m->tex[k][0]) + popc64(m->tex[k][1]);
        rec = m->always + c0d + (uint32_t)(prog || bmv) * (uint32_t)(m->c1[0] + m->c1[1]) +
              (bmv ? (uint32_t)(nubo + nssbo) : du + ds) + (tmv ? (uint32_t)(ntex + nimg) : (uint32_t)ndt + di);
    }
    c->rec[cls] += rec;
    // Step candidates: shaders (the masked slots of both stages), pipeline state (the generation and the per-draw
    // fields), vertex buffers (the streams the state reads), feedback scan (the fragment stage's textures).
    if (!dt->prog && ex->prog == L->prog && ex->prog->serial == L->serial && ex->pipe_gen == L->pipe_gen &&
        slots_gen == L->slots_gen) {
        int keep = 1;
        for (int k = 0; k < 2 && keep; k++) {
            uint32_t sm = slot_mask_of(D, ex->prog->serial, k);
            keep = !((uint32_t)dt->tex[k][0] & sm) && !(dt->img[k] & sm);
        }
        c->sh_keep += (uint64_t)keep;
    }
    uint8_t sx[4] = {s->prim, s->prim_restart, s->feedback, s->depth_minus_w};
    c->pg_hit += ex->pipe_gen == L->pipe_gen && s->vs == L->vs && s->fs == L->fs && !memcmp(sx, L->sx, 4);
    c->vb_skip += !(dt->vbuf & D->B.vmask) && D->B.vmask == L->vmask && vk_mem_gen() == L->vgen &&
                  ex->pipe_gen == L->pipe_gen && D->B.vb_valid;
    if (s->fs && ex->in_pass) {
        c->fb_n++;
        int fsame = s->fs == L->fs && ex->pass_serial == L->pass_serial;
        c->fb_keep += fsame && !(dt->tex[1][0] & m->tex[1][0]) && !(dt->tex[1][1] & m->tex[1][1]) && slots_gen == L->slots_gen;
        c->fb_memo += fsame && ex->tex_serial == L->tex_serial;
    }
}

// A graphics draw was prepared: it becomes the one the next draw compares with, and the marks start again.
static void bi_done(VkExec *ex, const VkPipeInfo *info, const VkPipeState *s, const uint64_t gt[4], const uint64_t gb[4],
                    uint32_t slots_gen) {
    VkExecDraw *D = ex->draw;
    BiLast *L = &D->BL;
    L->info = info, L->prog = ex->prog, L->serial = ex->prog->serial, L->pipe_gen = ex->pipe_gen;
    memcpy(L->gt, gt, sizeof L->gt), memcpy(L->gb, gb, sizeof L->gb);
    L->binst = D->c0_binst, L->slots_gen = slots_gen, L->pass_serial = ex->pass_serial, L->tex_serial = ex->tex_serial;
    L->vs = s->vs, L->fs = s->fs;
    L->sx[0] = s->prim, L->sx[1] = s->prim_restart, L->sx[2] = s->feedback, L->sx[3] = s->depth_minus_w;
    L->vmask = D->B.vmask, L->vgen = D->B.vgen;
    L->vused = vb_used_on() && D->B.st_valid ? D->B.vmask : VK_PIPE_ALL_BINDINGS;
    L->sm_ok = (uint8_t)(D->SM.valid && D->SM.vs == s->vs && D->SM.fs == s->fs && D->SM.k.prog == ex->prog);
    L->fb_ok = (uint8_t)(s->fs && ex->fb_fs == s->fs && ex->fb_serial == ex->tex_serial && ex->fb_pass == ex->pass_serial);
    memset(&ex->dirt, 0, sizeof ex->dirt);
    D->B.bi_valid = 1;
}

// Pass, shaders, pipeline, fixed state and resources for a draw. 0: the draw is skipped.
static int prepare_draw(VkExec *ex, int prim, int32_t count, int restart) {
    VkExecDraw *D = ex->draw;
    if (ex->render_disabled) return 0;
    cb_res(ex);
    dummies_init();
    if (ex->pass_noatt) {   // no attachment: the pass covers this draw's viewport or scissor, reopened when it needs more
        uint32_t w, h;
        vk_exec_noatt_dims(ex, &w, &h);
        if (ex->in_pass && (w > ex->pass_w || h > ex->pass_h)) vk_exec_pass_end(ex);
        if (!ex->in_pass) ex->rt_resolved = 0;
    }
    if (!ex->in_pass) ex->dirty |= VK_EXEC_DIRTY_VIEWPORT | VK_EXEC_DIRTY_SCISSOR;
    vk_exec_pass_begin(ex);
    VkRect2D sc = vk_exec_scissor(ex);
    if (!sc.extent.width || !sc.extent.height) return 0;
    if (!ex->prog) return warn_once(W_PROGRAM, "a draw without a bound program is skipped"), 0;
    int has_vs, has_fs, vs_wait = 0, fs_wait = 0, memo = shader_memo_on();
    const GfxShader *vs, *fs;
    // Before shader_for, whose lookups may bump it. In a chunk per pass too, as in bind_sig.
    uint32_t gen = memo ? vk_texture_slots_gen() + (ex->chunked ? ex->pass_serial : 0) : 0;
    int bim = bind_inc_mode(), sm_hit = 0;
    uint64_t bgt[4], bgb[4];   // TOTK_BIND_INC: G_tex and G_buf right before the descriptors are built
    uint32_t bsg = bim ? vk_texture_slots_gen() : 0;   // the shaders' slot generation, before shader_for
    // TOTK_BIND_INC: the shader memo holds the last prepared draw's shaders (BL.sm_ok), and since then no op changed the
    // program, its serial, the bound state, the slot generation, or a texture or image slot of either stage whose type
    // the program's translation reads (slot_mask_of; ~0u while unknown): shader_for would answer the same.
    int sh_keep = 0;
    if (bim && memo && D->SM.valid && D->B.bi_valid && D->BL.sm_ok && !ex->chunked && !ex->dirt.prog &&
        ex->prog == D->BL.prog && ex->prog->serial == D->BL.serial && ex->pipe_gen == D->BL.pipe_gen &&
        bsg == D->BL.slots_gen) {
        uint32_t m0 = slot_mask_of(D, ex->prog->serial, NVN_STAGE_VS), m1 = slot_mask_of(D, ex->prog->serial, NVN_STAGE_FS);
        sh_keep = !((uint32_t)ex->dirt.tex[NVN_STAGE_VS][0] & m0) && !(ex->dirt.img[NVN_STAGE_VS] & m0) &&
                  !((uint32_t)ex->dirt.tex[NVN_STAGE_FS][0] & m1) && !(ex->dirt.img[NVN_STAGE_FS] & m1);
    }
    if (sh_keep && bim == 1) {
        vs = D->SM.vs, fs = D->SM.fs, has_vs = 1, has_fs = D->SM.has_fs;
        sm_hit = 1;
    } else if (memo && D->SM.valid && memo_match(ex, gen, &D->SM.k)) {
        vs = D->SM.vs, fs = D->SM.fs, has_vs = 1, has_fs = D->SM.has_fs;
        sm_hit = 1;
        if (memo == 2) {
            int hv, hf, wv, wf;
            const GfxShader *v2 = shader_for(ex, NVN_STAGE_VS, &hv, &wv, 0), *f2 = shader_for(ex, NVN_STAGE_FS, &hf, &wf, 0);
            if (v2 != vs || f2 != fs || hf != has_fs || wv || wf)
                rt_fatal("vk_exec: shader memo of program %llu differs from the lookup", (unsigned long long)ex->prog->serial);
        }
    } else {
        D->SM.valid = 0;
        vs = shader_for(ex, NVN_STAGE_VS, &has_vs, &vs_wait, 0);
        fs = shader_for(ex, NVN_STAGE_FS, &has_fs, &fs_wait, 0);
        if (memo && vs && !vs_wait && !fs_wait && (!has_fs || fs))
            memo_key(ex, gen, &D->SM.k), D->SM.vs = vs, D->SM.fs = fs, D->SM.has_fs = has_fs, D->SM.valid = 1;
    }
    if (sh_keep && bim == 2) {
        D->BC.checked++;
        if (vs != D->BL.vs || fs != D->BL.fs || vs_wait || fs_wait)
            bi_mismatch(ex, BIM_SHADER, "vs %p fs %p (kept %p %p), pending %d %d", (const void *)vs, (const void *)fs,
                        (const void *)D->BL.vs, (const void *)D->BL.fs, vs_wait, fs_wait);
    }
    if (!has_vs) return warn_once(W_NO_VS, "a draw with a program that has no vertex stage is skipped"), 0;
    // A draw into a target the previous frame did not render (or into none) waits for a pending shader or pipeline:
    // one-shot draws (volumes and LUTs generated once, later only sampled) would leave their target empty for the
    // session. A draw into per-frame targets is skipped until they are ready and comes back next frame.
    int oneshot = -1, skip = pipe_skip_mode();   // -1: not looked up
    if (vs_wait || fs_wait) {
        if (!(oneshot = !vk_exec_targets_recent(ex, skip >= 1))) return perf_count(PERF_CNT_SHADER_SKIP), 0;   // skipped until ready
        perf_count(PERF_CNT_ONESHOT_WAIT);
        if (vs_wait) vs = shader_for(ex, NVN_STAGE_VS, &has_vs, NULL, 1);
        if (fs_wait) fs = shader_for(ex, NVN_STAGE_FS, &has_fs, NULL, 1);
    }
    if (!vs || (has_fs && !fs)) return 0;
    if (vk_dlaa_skip_draw(ex, fs)) return 0;   // the game's FXAA off, or its scaling replaced by DLSS (vk_dlaa.c)
    uint32_t fbl = feedback_prepare(ex, fs);
    uint8_t fb = (uint8_t)((fbl & 0xff ? 1 : 0) | (fbl & 0x100 ? 2 : 0)), dmw = (uint8_t)(nvn_depth_minus_w() != 0);
    // TOTK_BIND_INC: the pipeline memo's state is this draw's when pipe was not written since the memo's state was built
    // from it and the fields a draw sets are the same (y_flip is always 1): its pipeline is taken without building the
    // state and comparing it with the memo's and the recorded one (3 x 360 bytes). check builds and compares as before.
    int pg = pipe_memo_on() && !ex->chunked ? bim : 0;
    int pg_hit = pg && D->PM.valid && D->PM.gen == ex->pipe_gen && D->PM.s.vs == vs && D->PM.s.fs == fs &&
                 D->PM.s.feedback == fb && D->PM.s.prim == (uint8_t)prim && D->PM.s.depth_minus_w == dmw &&
                 D->PM.s.prim_restart == (uint8_t)(restart != 0);
    VkPipeState s;   // built from pipe unless the generation hit
    const VkPipeState *sp = &s;
    uint64_t pgen = ex->pipe_gen;
    if (pg_hit && pg == 1) {
        sp = &D->PM.s;
    } else {
        MX_CPY(&s, &ex->pipe, sizeof s);
        s.vs = vs, s.fs = fs;
        s.feedback = fb;
        s.prim = (uint8_t)prim;
        s.depth_minus_w = dmw;
        s.y_flip = 1;
        s.prim_restart = (uint8_t)(restart != 0);
        if (pg_hit) {   // check
            D->BC.checked++;
            if (memcmp(&s, &D->PM.s, sizeof s)) bi_mismatch(ex, BIM_PIPE, "pipeline memo: state differs from the memo its generation matched");
        }
    }
    const VkPipeInfo *info = NULL;
    int pending;
    // A draw of at most 6 vertices over half the render area or more (a full-screen triangle or quad, a tile) waits for
    // its pipeline: skipping it can leave a black tile. The waits share 50 ms per presented frame
    // (TOTK_PIPE_SKIP=fullscreen skips them instead). A one-shot draw waits without that budget. Everything else is
    // skipped while its pipeline is built.
    uint32_t wait_ms = 0;
    uint64_t t0 = 0;
    if (oneshot > 0) {
        wait_ms = ONESHOT_WAIT_MS;
    } else if (skip < 2 && count <= 6 && (uint64_t)sc.extent.width * sc.extent.height * 2 >= (uint64_t)ex->pass_w * ex->pass_h) {
        uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        if (f != D->wait_frame) D->wait_frame = f, D->wait_left_ns = 50000000;
        wait_ms = (uint32_t)(D->wait_left_ns / 1000000);
        if (wait_ms) t0 = kern_now_ns();
    }
    // The previous draw's pipeline while its state repeats (PM, TOTK_PIPE_MEMO=0 turns it off): the cache key is made
    // from the state alone, and a pipeline once found only changes from its fast link to its optimised build, read
    // from the info the cache entry keeps.
    VkPipeline p;
    const char *why = NULL;
    uint64_t w0 = 0;
    PipeCache *pc = NULL;
    int pg_take = pg_hit && pg == 1;   // the memo's pipeline without building or comparing the state
    if (!pg_take && pipe_memo_on() && !(D->PM.valid && MX_EQ(&D->PM.s, &s, sizeof s)))
        pc = &D->PC[nvn_hm_hash(&s, sizeof s) & (PC_N - 1)];
    if (pg_take) {
        info = D->PM.info, pending = 0;
        p = __atomic_load_n(&info->pipeline, __ATOMIC_ACQUIRE);
    } else if (pipe_memo_on() && (!pc || (pc->info && MX_EQ(&pc->s, &s, sizeof s)))) {
        info = pc ? pc->info : D->PM.info, pending = 0;
        p = __atomic_load_n(&info->pipeline, __ATOMIC_ACQUIRE);
        if (pc) D->PM.valid = 1, MX_CPY(&D->PM.s, &s, sizeof s), D->PM.info = info, D->B.st_pm = 0;
        D->PM.gen = pgen;   // s equals PM.s: built from pipe as it is now
    } else {
        w0 = t0 ? t0 : wait_ms ? kern_now_ns() : 0;   // the clock is read only for draws that may wait
        p = vk_pipeline_graphics_async(&s, &info, &pending, wait_ms);
        if (t0) {
            uint64_t spent = kern_now_ns() - t0;
            D->wait_left_ns = spent < D->wait_left_ns ? D->wait_left_ns - spent : 0;
        }
        why = oneshot > 0 ? "one-shot, shader" : wait_ms ? "full-screen" : NULL;
        if (pending && oneshot < 0 && !vk_exec_targets_recent(ex, skip >= 1)) {
            perf_count(PERF_CNT_ONESHOT_WAIT);
            why = "one-shot";
            if (!w0) w0 = kern_now_ns();
            p = vk_pipeline_graphics_async(&s, &info, &pending, ONESHOT_WAIT_MS);
        }
        D->PM.valid = !pending && p && info, D->B.st_pm = 0;
        if (D->PM.valid) MX_CPY(&D->PM.s, &s, sizeof s), D->PM.info = info, D->PM.gen = pgen;
        if (D->PM.valid && pc) MX_CPY(&pc->s, &s, sizeof s), pc->info = info;
    }
    uint64_t waited = w0 ? kern_now_ns() - w0 : 0;
    static int wait_logs;
    if (why && waited >= 2000000 && wait_logs < 200) {
        char tg[512];
        wait_logs++;
        vk_exec_targets_describe(ex, tg, sizeof tg);
        rt_log("vk_exec: frame %llu: a draw waited %.1f ms for its pipeline (%s%s): %u vertices, pass %ux%u, %s",
               (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1, waited / 1e6, why,
               pending ? ", skipped after all" : "", count, ex->pass_w, ex->pass_h, tg);
    }
    if (pending) return 0;   // a background worker is building it: the draw is skipped until it is ready
    if (!p || !info) return warn_once(W_PIPELINE, "a graphics pipeline could not be created: draws with it are skipped"), 0;
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    // On a generation hit the state recorded last is the memo's when st_pm says so (both were the last draw's).
    int st_same = D->B.st_valid && (pg_take && D->B.st_pm ? 1 : MX_EQ(&D->B.st, sp, sizeof *sp));
    if (pg_hit && pg == 2 && D->B.st_valid && D->B.st_pm && !st_same)
        bi_mismatch(ex, BIM_DYN, "the recorded dynamic state differs from the pipeline memo's");
    if (!st_same) {
        // TOTK_DYN_DELTA: only the dynamic states whose values changed since the last call in this command buffer. A
        // pipeline bind clears st_valid but not dyn_valid: every pipeline here declares the same dynamic states, which
        // the bind leaves as they were (the viewport, scissor and stencil values of record_fixed rely on that too).
        // dyn_valid is per context and cleared with D->B for every new command buffer (vk_exec_draw_new_cb).
        if (DD.mode < 0) DD.mode = rt_knob("DYN_DELTA", "1");
        uint32_t cmds = 1;
        if (RT_KNOB_ON(DD.mode)) {
            D->B.vmask = vk_pipeline_set_dynamic_delta(ex->cb, sp, D->B.dyn_valid, &cmds);
            D->B.dyn_valid = 1;
        } else {
            D->B.vmask = vk_pipeline_set_dynamic(ex->cb, sp);
            D->B.dyn_valid = 0;   // the delta's record is stale from here on
        }
        __atomic_fetch_add(&DD.calls, 1, __ATOMIC_RELAXED), __atomic_fetch_add(&DD.cmds, cmds, __ATOMIC_RELAXED);
        if (!cmds) __atomic_fetch_add(&DD.empty, 1, __ATOMIC_RELAXED);
        ex->recorded += cmds != 0;
        MX_CPY(&D->B.st, sp, sizeof *sp), D->B.st_valid = 1;
        if (ex->mark && ex->fr.draws == ex->mark_draws0) ex->mark->first[0] = vs, ex->mark->first[1] = fs;
    }
    D->B.st_pm = D->PM.valid;   // st is this draw's state, which PM holds while valid
    vk_dlaa_draw(ex);
    int mo = ex->chunked && vk_dlaa_available();
    if (mo) plat_mutex_lock(&mo_lock);
    vk_mvsurvey_draw(ex);
    vk_mvprev_draw(ex);
    if (mo) plat_mutex_unlock(&mo_lock);
    record_fixed(ex);
    repeat_stats(ex, info);
    if (bim) bi_gt(ex, bgt), bi_gb(ex, bgb);
    if (bim == 2) bi_census(ex, info, sp, bgt, bgb, bsg, sm_hit);   // on counts nothing: it is what gets timed
    bind_resources(ex, info, bim ? bgt : NULL, bgb);
    if (fbl & ex->fb_dirty) feedback_barrier(ex);
    feedback_written(ex, sp);
    bind_vertex_buffers(ex);
    if (vk_dump_armed) vk_dump_draw(ex, sp, p, info);
    if (vk_hdr_track) vk_hdr_draw(ex, info);
    vk_exec_oq_draw(ex);
    if (bim) bi_done(ex, info, sp, bgt, bgb, bsg);
    return 1;
}

// ---- per-object motion
// The camera pass (vk_dlaa.c mv_pass) gives everything the camera's motion, so a character that moves with the camera,
// or on its own, gets the wrong one and DLSS throws its history away there. Each solid scene draw whose position reads
// an object's own uniform blocks (bones c5, place c7, c6; mvobj_draw_ says which are left out and why) is therefore
// drawn a second time (listed during the replay, MQ, and recorded by mv_pass) with its vertex shader's last-frame
// variant (translator patch 0015: the position again from last frame's blocks, bound at slot bank + 15, and last
// frame's camera, written to a spare output) and the fragment shader of vk_mvobj_fs.h, which writes last frame's window
// position minus this frame's. The draw keeps everything else of the original: vertex and index buffers, the other
// uniform blocks, the jittered viewport and the scene depth, tested with the game's own test and its equal case
// (LEQUAL, GEQUAL) without writes, so only the visible surface writes its vector. Last frame's blocks:
// vk_mvprev_find and mo_prev_block; the camera: vk_dlaa_prev_camera. A draw whose variant or pipeline is still
// building, or whose last-frame blocks are not in the snapshot, keeps the camera's motion. TOTK_DLAA_OBJMV=0 turns it
// off (vk_mvprev.c).
#include "vk_mvobj_fs.h"
static const char MO_FS_RES[] = "{\"key\":\"mvobj\",\"stage\":\"ps\",\"notes\":[],\"cbufs\":[{\"binding\":1,\"slot\":3}],"
                                "\"sbufs\":[],\"textures\":[],\"images\":[]}";
static struct {
    const GfxShader *fs[16];     // per input Location
    int failed;
    uint64_t cand, still, draws, nodata, building, ran, dropped;
    uint64_t ns_check, ns_rec, n_rec, ns_emit, ns_still;
    uint64_t ns_part[3], n_part[3], misses, cyc[3], ncyc, big;   // checks: filter, data and stillness (memo misses), variant and pipeline   // time spent: checks and lookups of every candidate; recording the draws again   // cand: draws whose vertex shader reads c5 or c7
    uint64_t why[5];             // nodata by cause: camera, bones, place, bones copy, place copy
    uint64_t no_out, storage, no_fs;   // variant without the extra output, with storage resources; no fs
    uint64_t other_cam;          // listed draws left out: their camera block is not the scene camera
    uint64_t lk[14][5];          // last-frame lookups per binding index: none, self, side, nearer, refused (another object)
    uint64_t e_draws, e_pipe, e_dyn, e_push, e_pushw, e_sets, e_vb, e_setw, e_setreuse;   // emitted commands (summary)
} MO;

// The motion draws of the frame, kept as a list while the scene replays and recorded into the replay's own command
// buffer by vk_mvobj_execute, through the recorder thread (vk_rec.h) like every other draw: recording them straight
// into a secondary command buffer made the driver's work for each one on the replay thread. What a draw shares with
// the one before it (pipeline, dynamic state, viewport, buffers, descriptors) is recorded once; a push descriptor set
// is pushed again only where a binding changed (pushes update the set in place while its layout stays bound).
typedef struct {
    VkPipeline pipe;
    const VkPipeInfo *info;
    uint32_t st, desc, vb;       // indices into MQ.st, MQ.desc (the draw's packed descriptors) and MQ.vb
    VkDescriptorSet set[2];      // allocated sets; VK_NULL_HANDLE for a pushed one
    VkViewport v;
    VkRect2D sc;
    float bias[3];
    VkBuffer ib;
    VkDeviceSize ib_off;
    VkIndexType it;
    uint32_t count, instances, base_instance;
    int32_t base_vertex;
    uint64_t cam;                // the draw's camera block (c4), this frame's
} MoDraw;
typedef struct { VkBuffer b[17]; VkDeviceSize o[17], sz[17], str[17]; } MoVb;
static struct {
    uint64_t frame, fence;       // the frame and submission the list belongs to
    VkFormat depth_fmt;
    MoDraw *d;
    VkPipeState *st;
    VkDescSlot *desc;
    MoVb *vb;
    int n, cap, nst, cst, nvb, cvb;
    uint32_t ndesc, cdesc;
    NVNbufferRange vin[17];      // the vertex ranges MQ.vb[nvb - 1] was resolved from, under vk_mem_gen vgen
    uint64_t vgen;
    float sup_in[3];             // the support buffer last filled for the list and its inputs
    VkDescriptorBufferInfo sup;
    float par_in[4];             // the fragment shader's parameters last written and their inputs
    VkDescriptorBufferInfo par;
    VkDescriptorSetLayout aset_layout[2];   // the last allocated set per set number, with its descriptors
    VkDescriptorSet aset[2];
    uint32_t aset_n[2];
    VkDescSlot aset_d[2][MAX_DESC];
} MQ;

static void *mo_grow(void *p, int *cap, int need, size_t elem) {
    if (need <= *cap) return p;
    int c = *cap ? *cap : 256;
    while (c < need) c *= 2;
    if (!(p = realloc(p, elem * (size_t)c))) rt_fatal("vk_exec: out of memory");
    *cap = c;
    return p;
}

// The position bank masks of the draws considered (gfx_shader_mvprev_banks), with counts, for the summary.
static struct { uint32_t mask; uint64_t n; int ubo, ssbo, tex; } MM[32];   // ubo, ssbo, tex: the first vertex shader's
static void mo_mask_note(uint32_t banks, const GfxShader *vs) {
    for (int i = 0; i < 32; i++)
        if (MM[i].mask == banks || !MM[i].n) {
            if (!MM[i].n) {
                const GfxShaderRes *r;
                MM[i].ubo = gfx_shader_resources(vs, GFX_RES_UBO, &r), MM[i].ssbo = gfx_shader_resources(vs, GFX_RES_SSBO, &r);
                MM[i].tex = gfx_shader_resources(vs, GFX_RES_TEX, &r);
            }
            MM[i].mask = banks, MM[i].n++;
            return;
        }
}

static void mo_drop(void) {
    if (MQ.n) MO.dropped++;
    MQ.n = MQ.nst = MQ.nvb = 0, MQ.ndesc = 0, MQ.frame = 0;
}

static uint64_t mo_gen = 1;   // the list's snapshot (mo_prev_cached), bumped by mo_begin
static struct { uint32_t mask, chg, n; } MF[16];   // this list's draws per position banks and changed banks
static char fm_last[400];                          // the last executed list's, formatted (vk_mvobj_frame_masks)

// Starts this frame's list (the old one, never executed, is dropped).
static void mo_begin(VkExec *ex, uint64_t frame) {
    mo_drop();
    mo_gen++;
    memset(MF, 0, sizeof MF);
    MQ.frame = frame, MQ.fence = ex->fence, MQ.depth_fmt = ex->depth.format;
    memset(MQ.vin, 0, sizeof MQ.vin);
    MQ.vgen = 0, MQ.sup.buffer = MQ.par.buffer = VK_NULL_HANDLE;
    memset(MQ.aset, 0, sizeof MQ.aset);
}

static const GfxShader *mo_fs(int loc) {
    if (loc < 0 || loc >= 16) return NULL;
    if (!MO.fs[loc]) {
        uint32_t w[sizeof mvobj_fs_spv / 4];
        memcpy(w, mvobj_fs_spv, sizeof w);
        w[MVOBJ_FS_LOCATION_WORD] = (uint32_t)loc;
        MO.fs[loc] = gfx_shader_own(w, sizeof w / 4, MO_FS_RES);
        if (!MO.fs[loc]) MO.failed = 1, rt_log("vk_exec: the per-object motion fragment shader is malformed: off");
    }
    return MO.fs[loc];
}

// The write of binding b from its packed descriptors.
static VkWriteDescriptorSet mo_write(const VkPipeBinding *b, const VkDescSlot *d) {
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b->binding,
                              .descriptorCount = b->count ? b->count : 1, .descriptorType = b->type};
    if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || b->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
        b->type == VK_DESCRIPTOR_TYPE_SAMPLER)
        w.pImageInfo = &d->image;
    else
        w.pBufferInfo = &d->buffer;   // VkDescSlot is 24 bytes, as VkDescriptorBufferInfo: arrays line up
    return w;
}

// The descriptors of a motion draw, packed into MQ.desc at q->desc without the binding caches of the command buffer
// being replayed (B, DS, RS), which then stay true for its next draw: uniform buffers (through ubo_info, so with
// mo_ovr), the support buffer and sampled textures. An allocated set is written here (or the last one reused when it
// would hold the same); a pushed set is pushed by mo_emit. 0 for a pipeline with storage buffers, images or texel
// buffers (not drawn again).
static int mo_desc(VkExec *ex, const VkPipeInfo *info, MoDraw *q) {
    if (info->n_bindings > MAX_BINDINGS) return 0;
    uint32_t total = 0;
    for (uint32_t i = 0; i < info->n_bindings; i++) total += info->bindings[i].count ? info->bindings[i].count : 1;
    if (total > MAX_DESC) return 0;
    int cdesc = (int)MQ.cdesc;
    MQ.desc = mo_grow(MQ.desc, &cdesc, (int)(MQ.ndesc + total), sizeof *MQ.desc);
    MQ.cdesc = (uint32_t)cdesc;
    VkDescSlot *d = MQ.desc + MQ.ndesc;
    memset(d, 0, total * sizeof *d);   // unused bytes zero: equal descriptors compare equal
    for (uint32_t i = 0, nd = 0; i < info->n_bindings; i++) {
        const VkPipeBinding *b = &info->bindings[i];
        const GfxShaderRes *r = b->res;
        uint32_t cnt = b->count ? b->count : 1;
        VkDescSlot *o = &d[nd];
        int st = nvn_stage_of(b->stage);
        if (b->kind == VK_PIPE_SUPPORT) {
            float in[3];
            support_in(ex, in);
            if (!MQ.sup.buffer || memcmp(in, MQ.sup_in, sizeof in)) {
                VkBuffer sb;
                VkDeviceSize so;
                support_fill(scratch(ex, SUPPORT_BYTES, &sb, &so), in[0], in[1], in[2]);
                MQ.sup = (VkDescriptorBufferInfo){sb, so, SUPPORT_BYTES};
                memcpy(MQ.sup_in, in, sizeof in);
            }
            o->buffer = MQ.sup;
        } else if (b->kind == GFX_RES_UBO) {
            for (uint32_t k = 0; k < cnt; k++) o[k].buffer = ubo_info(ex, st, r->slot + (int)k);
        } else if (b->kind == GFX_RES_SSBO && !(r->flags & 1)) {   // read only: instance data of foliage and such
            for (uint32_t k = 0; k < cnt; k++) o[k].buffer = ssbo_info(ex, st, r, 0);
        } else if (b->kind == GFX_RES_TEX && b->type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
            int locked = 0;   // tex_info takes the texture lock for the lookups that need it
            for (uint32_t k = 0; k < cnt; k++) {
                int slot = r->slot + (int)k;
                tex_info(ex, r, slot >= 0 && slot < 128 ? ex->tex[st][slot] : 0, 0, r->separate, &o[k].image, &locked);
            }
            if (locked) vk_texture_unlock();
        } else {
            return 0;
        }
        nd += cnt;
    }
    uint32_t n_sets = info->n_sets ? info->n_sets : 1;
    q->set[0] = q->set[1] = VK_NULL_HANDLE;
    for (uint32_t s = 0, i0 = 0, k0 = 0; s < n_sets && s < 2; s++) {
        uint32_t i1 = i0, k1 = k0;
        while (i1 < info->n_bindings && (n_sets == 1 || info->bindings[i1].set == s))
            k1 += info->bindings[i1].count ? info->bindings[i1].count : 1, i1++;
        if (i1 > i0 && !(s ? info->push1 : info->push)) {
            VkDescriptorSetLayout sl = s ? info->set_layout1 : info->set_layout;
            if (MQ.aset[s] && MQ.aset_layout[s] == sl && MQ.aset_n[s] == k1 - k0 &&
                !memcmp(MQ.aset_d[s], d + k0, (k1 - k0) * sizeof *d)) {
                q->set[s] = MQ.aset[s];
                MO.e_setreuse++;
            } else {
                static VkWriteDescriptorSet w[MAX_BINDINGS];
                VkDescriptorSet set = alloc_set(ex, sl);
                for (uint32_t i = i0, k = k0; i < i1; k += info->bindings[i].count ? info->bindings[i].count : 1, i++)
                    w[i - i0] = mo_write(&info->bindings[i], d + k), w[i - i0].dstSet = set;
                vkUpdateDescriptorSets(vk_ctx()->device, i1 - i0, w, 0, NULL);
                q->set[s] = MQ.aset[s] = set, MQ.aset_layout[s] = sl, MQ.aset_n[s] = k1 - k0;
                memcpy(MQ.aset_d[s], d + k0, (k1 - k0) * sizeof *d);
                MO.e_setw++;
            }
        }
        i0 = i1, k0 = k1;
    }
    q->desc = MQ.ndesc;
    MQ.ndesc += total;
    return 1;
}

// The draw's vertex buffers as bind_vertex_buffers resolves them, into MQ.vb; the previous entry again while the
// ranges and vk_mem_gen are the same (most motion draws in a row are one model's parts).
static uint32_t mo_vertex_buffers(VkExec *ex) {
    uint64_t gen = vk_mem_gen();
    if (MQ.nvb && MQ.vgen == gen && !memcmp(MQ.vin, ex->vbuf, sizeof MQ.vin)) {
        const MoVb *l = &MQ.vb[MQ.nvb - 1];
        int same = 1;
        if (!vk_pipeline_vertex_input_dynamic())
            for (int b = 0; b < 16 && same; b++) same = l->str[b] == ex->pipe.vstream[b].stride;
        if (same) return (uint32_t)MQ.nvb - 1;
    }
    MQ.vb = mo_grow(MQ.vb, &MQ.cvb, MQ.nvb + 1, sizeof *MQ.vb);
    MoVb *v = &MQ.vb[MQ.nvb];
    for (int b = 0; b < 17; b++) {
        v->b[b] = Z.zbuf, v->o[b] = 0, v->sz[b] = ZERO_BYTES, v->str[b] = b < 16 ? ex->pipe.vstream[b].stride : 0;
        if (b == VK_PIPE_ZERO_BINDING) continue;
        const NVNbufferRange *r = &ex->vbuf[b];
        VkMemSpan sp;
        if (!r->address || !vk_mem_resolve(r->address, 1, &sp)) continue;
        VK_MEM_USE(sp);
        v->b[b] = sp.buffer, v->o[b] = sp.offset, v->sz[b] = r->size && r->size < sp.avail ? r->size : sp.avail;
    }
    memcpy(MQ.vin, ex->vbuf, sizeof MQ.vin);
    MQ.vgen = gen;
    return (uint32_t)MQ.nvb++;
}

// Records the list into cb (the replay's command buffer, inside the motion pass's rendering), skipping what the draw
// before already set.
static void mo_emit(VkCommandBuffer cb) {
    int dyn_vi = vk_pipeline_vertex_input_dynamic();
    VkPipeline pipe = VK_NULL_HANDLE;
    uint32_t st = UINT32_MAX, vb = UINT32_MAX;
    VkPipelineLayout lay[2] = {0};
    const VkPipeInfo *pinfo[2] = {0};
    int pdesc[2] = {-1, -1};      // the pushed set's descriptors (MQ.desc index) under lay/pinfo, -1 none
    VkDescriptorSet bound[2] = {0};
    const MoDraw *p = NULL;
    for (int n = 0; n < MQ.n; p = &MQ.d[n++]) {
        const MoDraw *q = &MQ.d[n];
        const VkPipeInfo *info = q->info;
        if (q->pipe != pipe) vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, q->pipe), pipe = q->pipe, MO.e_pipe++;
        if (q->st != st) vk_pipeline_set_dynamic(cb, &MQ.st[q->st]), st = q->st, MO.e_dyn++;
        if (!p || memcmp(&q->v, &p->v, sizeof q->v)) vkCmdSetViewportWithCount(cb, 1, &q->v);
        if (!p || memcmp(&q->sc, &p->sc, sizeof q->sc)) vkCmdSetScissorWithCount(cb, 1, &q->sc);
        if (!p || memcmp(q->bias, p->bias, sizeof q->bias)) vkCmdSetDepthBias(cb, q->bias[0], q->bias[1], q->bias[2]);
        uint32_t n_sets = info->n_sets ? info->n_sets : 1;
        const VkDescSlot *d = MQ.desc + q->desc;
        // Under the validation layer, a new layout binds set 1 before set 0 is pushed, as the replay's draws do
        // (bind_descriptors, the comment at set1_first): the layer (SDK 1.4.363) keeps the push set it made for an
        // earlier INDEPENDENT_SETS layout and drops writes to bindings that layout lacks, then reports the motion draw's
        // last-frame banks as missing (VUID-vkCmdDrawIndexed-None-08114, "vp_c20") and faults inside the layer. The bind
        // at set 1 disturbs set 0, so the push below gets a new push set.
        if (n_sets == 2 && lay[0] != info->layout && info->push && !info->push1 && q->set[1] && vk_ctx()->validation) {
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, 1, 1, &q->set[1], 0, NULL);
            bound[1] = q->set[1], lay[1] = info->layout, pinfo[1] = info, pdesc[1] = -1, MO.e_sets++;
        }
        for (uint32_t s = 0, i0 = 0, k0 = 0; s < n_sets && s < 2; s++) {
            uint32_t i1 = i0, k1 = k0;
            while (i1 < info->n_bindings && (n_sets == 1 || info->bindings[i1].set == s))
                k1 += info->bindings[i1].count ? info->bindings[i1].count : 1, i1++;
            if (i1 > i0) {
                if (s ? info->push1 : info->push) {
                    // the bindings that differ from the last push of this layout and pipeline (all after another)
                    int same = lay[s] == info->layout && pinfo[s] == info && pdesc[s] >= 0;
                    const VkDescSlot *pd = same ? MQ.desc + pdesc[s] : NULL;
                    VkWriteDescriptorSet w[MAX_BINDINGS];
                    uint32_t nw = 0;
                    for (uint32_t i = i0, k = k0; i < i1; i++) {
                        uint32_t cnt = info->bindings[i].count ? info->bindings[i].count : 1;
                        if (!same || memcmp(d + k, pd + k, cnt * sizeof *d)) w[nw++] = mo_write(&info->bindings[i], d + k);
                        k += cnt;
                    }
                    if (nw) rec_push(vk_pipeline_push_fn(), cb, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, s, nw, w);
                    MO.e_push += nw != 0, MO.e_pushw += nw;
                    lay[s] = info->layout, pinfo[s] = info, pdesc[s] = (int)q->desc, bound[s] = VK_NULL_HANDLE;
                } else if (q->set[s] != bound[s] || lay[s] != info->layout) {
                    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, info->layout, s, 1, &q->set[s], 0, NULL);
                    bound[s] = q->set[s], lay[s] = info->layout, pinfo[s] = info, pdesc[s] = -1, MO.e_sets++;
                }
            }
            i0 = i1, k0 = k1;
        }
        if (q->vb != vb) {
            const MoVb *v = &MQ.vb[q->vb];
            vkCmdBindVertexBuffers2(cb, 0, 17, v->b, v->o, v->sz, dyn_vi ? NULL : v->str);
            vb = q->vb, MO.e_vb++;
        }
        if (!p || q->ib != p->ib || q->ib_off != p->ib_off || q->it != p->it) vkCmdBindIndexBuffer(cb, q->ib, q->ib_off, q->it);
        vkCmdDrawIndexed(cb, q->count, q->instances, 0, q->base_vertex, q->base_instance);
        MO.e_draws++;
    }
}

static void mvobj_draw_(VkExec *ex, const p_draw_idx *d, VkBuffer ib, VkDeviceSize ib_off, uint64_t t0);
static void mvobj_draw(VkExec *ex, const p_draw_idx *d, VkBuffer ib, VkDeviceSize ib_off) {
    VkExecDraw *D = ex->draw;
    if (MO.failed || !vk_mvprev_on() || !D->B.st_valid) return;
    static int timed = -1;
    if (timed < 0) timed = rt_knob("MVOBJ_TIMES", "0");   // TOTK_MVOBJ_TIMES=1: the summary's times (the clock reads cost)
    if (ex->chunked) plat_mutex_lock(&mo_lock);
    uint64_t t0 = timed ? kern_now_ns() : 0;
    mvobj_draw_(ex, d, ib, ib_off, t0);
    if (timed) MO.ns_check += kern_now_ns() - t0;
    if (ex->chunked) plat_mutex_unlock(&mo_lock);
}

// TOTK_MVOBJ_BANKS=1 (test): for scene draws, the words of each other uniform bank (c3, c6, c8-c16) that changed
// since the same shader's draw last frame, logged at frame 3000: where the time that moves wind and water lives
static void mvobj_banks(VkExec *ex) {
    VkExecDraw *D = ex->draw;
    static int on = -1;
    if (on < 0) on = rt_knob("MVOBJ_BANKS", "0");
    if (!on || !D->B.st_valid) return;
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (frame < 2999 || frame > 3000) return;
    static struct { const GfxShader *vs; int bank; uint64_t frame; float w[64]; int n; } tab[4096];
    static int logged;
    const GfxShaderRes *ur;
    int nu = gfx_shader_resources(D->B.st.vs, GFX_RES_UBO, &ur);
    for (int i = 0; i < nu; i++) {
        int bank = ur[i].slot;
        if (bank < 3 || bank == 4 || bank == 5 || bank == 7 || bank > 16) continue;
        const p_bind_buf *u = &ex->ubo[NVN_STAGE_VS][bank - 3];
        uint32_t n = u->size < 256 ? (uint32_t)u->size / 4 : 64;
        const float *now = n ? (const float *)vk_usnap_bytes(u->addr, n * 4) : NULL;
        if (!now) continue;
        uint32_t k = (uint32_t)(((uintptr_t)D->B.st.vs ^ (uint64_t)bank * 0x9E3779B97F4A7C15ull) * 0xff51afd7ed558ccdull >> 52);
        if (frame == 2999) {
            tab[k].vs = D->B.st.vs, tab[k].bank = bank, tab[k].frame = frame, tab[k].n = (int)n;
            memcpy(tab[k].w, now, n * 4);
        } else if (tab[k].vs == D->B.st.vs && tab[k].bank == bank && tab[k].frame == 2999 && logged < 80) {
            char line[700];
            int o = 0, nd = 0;
            for (int j = 0; j < tab[k].n && j < (int)n && o < 600; j++)
                if (memcmp(&tab[k].w[j], &now[j], 4)) nd++, o += snprintf(line + o, sizeof line - o, " [%d] %g>%g", j, tab[k].w[j], now[j]);
            if (nd) logged++, rt_log("mvbanks: vs %p c%d size %#llx: %d words%s", (const void *)D->B.st.vs, bank,
                                     (unsigned long long)u->size, nd, line);
            tab[k].frame = 0;
        }
    }
}

// 1: old is cur a little changed or the same (every float within 10 %, at least 1.0 and at most 4.0); *changed: not
// the same. The cap: TotK's world positions are in the thousands, and 10 % of them let another object's block a few
// units away pass as this one moved; nothing moves 4 units in a frame.
static int mo_similar(const uint8_t *cur, const uint8_t *old, uint64_t n, int *changed) {
    *changed = 0;
    if (!memcmp(cur, old, n)) return 1;
    // word by word as integers; float arithmetic only on two normal floats (the blocks hold integers too, and
    // denormal arithmetic took this loop 1.9 million cycles a block)
    const uint32_t *x = (const uint32_t *)cur, *y = (const uint32_t *)old;
    for (uint64_t w = 0; w < n / 4; w++) {
        if (x[w] == y[w]) continue;
        uint32_t ex = x[w] >> 23 & 0xff, ey = y[w] >> 23 & 0xff;
        if (!ex || ex == 0xff || !ey || ey == 0xff) continue;   // not both normal floats: an integer or a tiny value
        float fx, fy;
        memcpy(&fx, &x[w], 4), memcpy(&fy, &y[w], 4);
        float dd = fabsf(fx - fy), tol = 0.1f * fabsf(fy);
        if (dd > (tol < 1.0f ? 1.0f : tol > 4.0f ? 4.0f : tol)) return 0;
    }
    *changed = 1;
    return 1;
}

// Last frame's copy of a block, accepted when it is this frame's block a little changed (mo_similar,
// which see): a block of another object the address rule found would make motion up. 1 and *out: found (the block itself
// when it is unchanged, *changed 0); 0: none, the draw keeps this frame's block there.
static int mo_prev_block(int index, const p_bind_buf *u, VkDescriptorBufferInfo *out, int *changed) {
    uint64_t max = LIM.ubo_max, n = u->size < max ? u->size : max, pa;
    int how;
    *changed = 0;
    // c9 and c11 keep this frame's block. With the copies the address rule finds for c11 and last frame's c9, the
    // c4 c7 c9 c11 draws (walls, cliffs, arches) got several pixels of motion under a still camera (TOTK_MV_CHECK,
    // those draws alone: 34 % of the picture moving; with c11 kept, 0.8 %). With c11 kept, their c9 still changes every
    // frame while nothing moves (the check's per-draw line: about 120 such draws a frame changed in c9 alone), the
    // object scale the picture shows is 0.6-0.8 of theirs, and with Link stopped against a cliff 40-77 % of the picture
    // got 1-3 px of motion that flipped direction, three times the warp error of the camera's vectors. With c9 kept too,
    // the turning run is as good (object warp error 0.46 of the camera's). Why their copies are wrong is not known.
    // TOTK_MVPREV_SKIP=<hex bank mask> (test) replaces the default a00 (bit n: cn).
    static long skip = -2;
    if (skip == -2) {
        const char *e = cfg_get("TOTK_MVPREV_SKIP");
        skip = e && *e ? strtol(e, NULL, 16) : 1L << 9 | 1L << 11;
    }
    if (skip >> (index + 3) & 1) return 0;
    uint64_t c0 = __builtin_ia32_rdtsc();
    int found = vk_mvprev_find(index, u->addr, u->size, &pa, &how);
    uint64_t c1 = __builtin_ia32_rdtsc();
    MO.cyc[0] += c1 - c0, MO.ncyc++, MO.big += n;
    if (index >= 0 && index < 14) MO.lk[index][found ? how : MVPREV_NONE]++;
    if (!found || !vk_usnap_find(pa, n, UINT32_MAX, LIM.ubo_align, out)) return 0;
    if (how == MVPREV_SELF) return 1;   // bound at the same address last frame: not written again
    const uint8_t *map = vk_usnap_map(), *cur = vk_usnap_bytes(u->addr, n);
    uint64_t c2 = __builtin_ia32_rdtsc();
    MO.cyc[1] += c2 - c1;
    if (!cur) return 0;
    const uint8_t *old = map + out->offset;
    static long trust = -2;   // TOTK_MVPREV_TRUST=<hex bank mask> (test): the copy found is taken without the check
    if (trust == -2) {
        const char *e = cfg_get("TOTK_MVPREV_TRUST");
        trust = e && *e ? strtol(e, NULL, 16) : 0;
    }
    if (trust >> (index + 3) & 1) {
        *changed = memcmp(cur, old, n) != 0;
        return 1;
    }
    int r = mo_similar(cur, old, n, changed);
    MO.cyc[2] += __builtin_ia32_rdtsc() - c2;
    if (!r && index >= 0 && index < 14) MO.lk[index][4]++;
    return r;
}

// mo_prev_block once per block and list: many draws share their blocks (the scene's c14, a model's c7 and c9 across
// its parts), and the lookup with its comparison was most of the checks' time.
static int mo_prev_cached(int index, const p_bind_buf *u, VkDescriptorBufferInfo *out, int *changed) {
    static struct { uint64_t gen, addr, size; int index, r, changed; VkDescriptorBufferInfo out; } bc[4096];
    uint32_t i = (uint32_t)((u->addr ^ (uint64_t)index << 56 ^ u->size << 40) * 0x9E3779B97F4A7C15ull >> 52);
    if (bc[i].gen == mo_gen && bc[i].addr == u->addr && bc[i].size == u->size && bc[i].index == index) {
        *out = bc[i].out, *changed = bc[i].changed;
        return bc[i].r;
    }
    int r = mo_prev_block(index, u, out, changed);
    bc[i].gen = mo_gen, bc[i].addr = u->addr, bc[i].size = u->size, bc[i].index = index;
    bc[i].r = r, bc[i].changed = *changed, bc[i].out = *out;
    return r;
}

static void mvobj_draw_(VkExec *ex, const p_draw_idx *d, VkBuffer ib, VkDeviceSize ib_off, uint64_t t0) {
    VkExecDraw *D = ex->draw;
    mvobj_banks(ex);
    const VkExecTarget *z = &ex->depth;
    if (!z->image || z->width < DLAA_SCENE_MIN_W || z->format != VK_FORMAT_D32_SFLOAT || D->B.st.samples > 1 || D->B.st.raster_discard)
        return;
    // solid geometry only, which tests and writes the scene depth: effects, particles and screen quads write none, and
    // drawn again they would put their motion on the surfaces behind them (DLSS takes those from the surface)
    if (!D->B.st.ds.dt || !D->B.st.ds.dw) return;
    // a vertex shader that reads a user bank besides the camera's (c4) may move on its own
    const GfxShaderRes *ur;
    int nu = gfx_shader_resources(D->B.st.vs, GFX_RES_UBO, &ur), reads = 0;
    for (int i = 0; i < nu; i++) reads |= ur[i].slot >= 3 && ur[i].slot <= 16 && ur[i].slot != 4;
    if (!reads) return;
    MO.cand++;
    uint64_t tp = t0 ? kern_now_ns() : 0;
    if (t0) MO.ns_part[0] += tp - t0, MO.n_part[0]++;
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (MQ.frame != frame || MQ.fence != ex->fence || MQ.depth_fmt != z->format) mo_begin(ex, frame);
    if (!__atomic_load_n(&LIM.ubo_max, __ATOMIC_ACQUIRE)) limits_init();
    // the variant: the translation of the same code and answers, kept per shader once found; it names the banks the
    // position depends on (0: none, the shader is skipped from then on)
    static struct { const GfxShader *of, *vs; uint32_t banks; } vmemo[1024];
    uint32_t vi = (uint32_t)((uintptr_t)D->B.st.vs * 0x9E3779B97F4A7C15ull >> 54);
    if (vmemo[vi].of != D->B.st.vs) {
        int has, pending = 0;
        sf_mvprev = 1;
        const GfxShader *v = shader_for(ex, NVN_STAGE_VS, &has, &pending, 0);
        sf_mvprev = 0;
        if (!v || pending) {
            MO.building++;
            return;
        }
        GfxShaderIo io[32];
        uint32_t extra = gfx_shader_outputs(v, io) & ~gfx_shader_outputs(D->B.st.vs, io);
        uint32_t banks = extra ? gfx_shader_mvprev_banks(v) & 0x1fff8u : 0;
        vmemo[vi].of = D->B.st.vs, vmemo[vi].vs = v, vmemo[vi].banks = banks;
        vk_mvprev_want((banks & ~(1u << 4)) >> 3);   // binding index = bank - 3
    }
    const GfxShader *vs = vmemo[vi].vs;
    uint32_t banks = vmemo[vi].banks;
    if (!banks || !(banks & ~(1u << 4))) {
        MO.no_out++;
        return;
    }
    // a position without the scene camera (c4) has a view of its own (c8 c9 c10 [c13]: its c8 is at a new address every
    // frame, last frame's is never found): drawn again it would get this frame's view and no camera motion
    // (TOTK_MV_CHECK while turning: 6.2 px end-point error against the camera vectors' 1.7)
    static int noc4 = -1;
    if (noc4 < 0) noc4 = rt_knob("MVOBJ_NOC4", "0");   // TOTK_MVOBJ_NOC4=1 (test): drawn again all the same
    if (!(banks >> 4 & 1) && !noc4) {
        MO.no_out++;
        return;
    }
    // only positions that read an object's own blocks: bones (c5), place (c7) or c6. The rest (terrain and grass: c14
    // c15 c16, with or without c9) measured no better than the camera's motion (TOTK_MV_CHECK, turning, those draws
    // alone: object warp error 0.112 against the camera's 0.113, the object part 0.4 of what the picture shows) and
    // were half the draws checked and listed. TOTK_MVOBJ_ALL=1 (test) takes them again.
    static int all = -1;
    if (all < 0) all = rt_knob("MVOBJ_ALL", "0");
    if (!all && !(banks & (1u << 5 | 1u << 6 | 1u << 7))) {
        MO.no_out++;
        return;
    }
    // TOTK_MVOBJ_ONLY=<hex bank mask> (test): only draws whose position reads no bank outside the mask (bit n: cn)
    static long only = -2;
    if (only == -2) {
        const char *e = cfg_get("TOTK_MVOBJ_ONLY");
        only = e && *e ? strtol(e, NULL, 16) : -1;
    }
    if (only >= 0 && banks & ~(uint32_t)only & ~(1u << 4)) return;
    static long exact = -2;   // TOTK_MVOBJ_EXACT=<hex bank mask> (test): only draws with exactly these position banks
    if (exact == -2) {
        const char *e = cfg_get("TOTK_MVOBJ_EXACT");
        exact = e && *e ? strtol(e, NULL, 16) : -1;
    }
    if (exact >= 0 && banks != (uint32_t)exact) return;
    mo_mask_note(banks, D->B.st.vs);
    // last frame's camera and blocks; held still when none of the position's blocks changed (most of the scenery)
    VkDescriptorBufferInfo cam, prevb[17];
    float jit[2];
    if (!vk_dlaa_prev_camera(&cam, jit)) {
        MO.nodata++, MO.why[0]++;
        return;
    }
    static struct { uint64_t frame, key; int moving; VkDescriptorBufferInfo b[17]; uint32_t have; } memo[8192];
    uint64_t key = banks;
    for (int b = 3; b <= 16; b++)
        if (banks >> b & 1 && b != 4) key = (key ^ ex->ubo[NVN_STAGE_VS][b - 3].addr) * 0x100000001b3ull;
    uint32_t mi = (uint32_t)(key * 0xff51afd7ed558ccdull >> 51);
    uint32_t have = 0;
    if (memo[mi].frame == frame && memo[mi].key == key) {
        if (!memo[mi].moving) {
            if (t0) MO.ns_still += kern_now_ns() - tp;
            return (void)MO.still++;
        }
        memcpy(prevb, memo[mi].b, sizeof prevb), have = memo[mi].have;
    } else {
        MO.misses++;
        int moving = 0;
        for (int b = 3; b <= 16; b++) {
            if (!(banks >> b & 1) || b == 4) continue;
            const p_bind_buf *u = &ex->ubo[NVN_STAGE_VS][b - 3];
            int changed;
            if (u->addr && u->size && mo_prev_cached(b - 3, u, &prevb[b], &changed))
                have |= 1u << b, moving |= changed << b;   // the banks whose block changed
        }
        memo[mi].frame = frame, memo[mi].key = key, memo[mi].moving = moving;
        memcpy(memo[mi].b, prevb, sizeof prevb), memo[mi].have = have;
        if (!moving) {
            if (t0) MO.ns_still += kern_now_ns() - tp;
            return (void)MO.still++;
        }
    }
    if (t0) MO.ns_part[1] += kern_now_ns() - tp, MO.n_part[1]++, tp = kern_now_ns();
    GfxShaderIo io[32];
    uint32_t extra = gfx_shader_outputs(vs, io) & ~gfx_shader_outputs(D->B.st.vs, io);
    const GfxShaderRes *r;
    if (gfx_shader_resources(vs, GFX_RES_IMG, &r)) {
        MO.storage++;
        return;
    }
    const GfxShader *fs = mo_fs(31 - __builtin_clz(extra));
    if (!fs) {
        MO.no_fs++;
        return;
    }
    VkPipeState ms = D->B.st;
    ms.vs = vs, ms.fs = fs, ms.feedback = 0;
    memset(ms.color_fmt, 0, sizeof ms.color_fmt);
    ms.color_fmt[0] = NVN_FORMAT_RG16F;
    // blending off (blend_en below); the factors stay the game's, valid NVN values
    ms.color.blend_en = 0, ms.color.logic_op = 0;
    ms.chmask.mask = 0x3;   // r and g of target 0
    ms.ms.a2c = 0;
    // the game's own test with its equal case (LESS -> LEQUAL, GREATER -> GEQUAL): the surface the draw left in the
    // scene depth passes, anything nearer hides it (TOTK_MVOBJ_EQUAL=1: EQUAL, for comparison)
    static int eq = -1;
    if (eq < 0) eq = rt_knob("MVOBJ_EQUAL", "0");
    int df = D->B.st.ds.df;
    df = eq ? NVN_DEPTH_FUNC_EQUAL : df == NVN_DEPTH_FUNC_LESS ? NVN_DEPTH_FUNC_LEQUAL
       : df == NVN_DEPTH_FUNC_GREATER ? NVN_DEPTH_FUNC_GEQUAL : df == NVN_DEPTH_FUNC_LEQUAL || df == NVN_DEPTH_FUNC_GEQUAL ? df
       : NVN_DEPTH_FUNC_EQUAL;
    ms.ds.dt = 1, ms.ds.dw = 0, ms.ds.df = (uint64_t)df, ms.ds.st = 0;
    // the pipeline per state, kept: the lookup hashes and locks the whole state
    static struct { VkPipeState st; VkPipeline p; const VkPipeInfo *info; int used; } pmemo[64];
    static int plast = -1;   // the entry of the last draw: a model's parts in a row share the state, no hash then
    uint32_t pi;
    if (plast >= 0 && pmemo[plast].used && !memcmp(&pmemo[plast].st, &ms, sizeof ms)) {
        pi = (uint32_t)plast;
    } else {
        uint64_t ph = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < sizeof ms; i += 8) {
            uint64_t w8;
            memcpy(&w8, (const uint8_t *)&ms + i, sizeof ms - i < 8 ? sizeof ms - i : 8);
            ph = (ph ^ w8) * 0x100000001b3ull;
        }
        pi = (uint32_t)(ph >> 58);
    }
    const VkPipeInfo *info = NULL;
    VkPipeline pipe;
    int pending = 0;
    if (pmemo[pi].used && !memcmp(&pmemo[pi].st, &ms, sizeof ms)) {
        info = pmemo[pi].info;
        pipe = __atomic_load_n(&info->pipeline, __ATOMIC_ACQUIRE);   // its optimised build once swapped in
        if (!pipe) return;
    } else {
        pipe = vk_pipeline_graphics_async(&ms, &info, &pending, 0);
        if (pending || !pipe || !info) {
            MO.building += pending != 0;
            return;
        }
        pmemo[pi].st = ms, pmemo[pi].p = pipe, pmemo[pi].info = info, pmemo[pi].used = 1;
    }
    plast = (int)pi;
    uint64_t t1 = kern_now_ns();
    if (t0) MO.ns_part[2] += t1 - tp, MO.n_part[2]++;
    if (t0) MO.ns_check -= t1 - t0;   // counted as recording below
    // into the frame's list, past the primary's binding caches (mo_desc); mo_emit records it
    MQ.d = mo_grow(MQ.d, &MQ.cap, MQ.n + 1, sizeof *MQ.d);
    MoDraw *q = &MQ.d[MQ.n];
    float par_in[4] = {(float)z->width, (float)z->height, jit[0], jit[1]};
    if (!MQ.par.buffer || memcmp(par_in, MQ.par_in, sizeof par_in)) {
        VkBuffer pbuf;
        VkDeviceSize poff;
        memcpy(scratch(ex, 16, &pbuf, &poff), par_in, sizeof par_in);
        MQ.par = (VkDescriptorBufferInfo){pbuf, poff, 16};
        memcpy(MQ.par_in, par_in, sizeof par_in);
    }
    memset(mo_ovr, 0, sizeof mo_ovr);
    for (int bk = 3; bk <= 16; bk++)   // last frame's block, or this frame's where none was found
        mo_ovr[NVN_STAGE_VS][bk + 15] = bk == 4 ? cam : have >> bk & 1 ? prevb[bk] : ubo_info(ex, NVN_STAGE_VS, bk);
    mo_ovr[NVN_STAGE_FS][3] = MQ.par;
    mo_ovr_on = 1;
    int bound = mo_desc(ex, info, q);
    mo_ovr_on = 0;
    if (!bound) {
        MO.storage++;
        return;
    }
    q->pipe = pipe, q->info = info;
    if (!MQ.nst || memcmp(&MQ.st[MQ.nst - 1], &ms, sizeof ms)) {
        MQ.st = mo_grow(MQ.st, &MQ.cst, MQ.nst + 1, sizeof *MQ.st);
        MQ.st[MQ.nst++] = ms;
    }
    q->st = (uint32_t)MQ.nst - 1;
    q->v = draw_viewport(ex), q->sc = vk_exec_scissor(ex);
    q->bias[0] = ex->poly_offset.units, q->bias[1] = ex->poly_offset.clamp, q->bias[2] = ex->poly_offset.factor;
    q->vb = mo_vertex_buffers(ex);
    q->ib = ib, q->ib_off = ib_off;
    q->it = d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    q->count = (uint32_t)d->count, q->instances = d->instances > 0 ? (uint32_t)d->instances : 1;
    q->base_vertex = d->base_vertex, q->base_instance = (uint32_t)d->base_instance;
    q->cam = ex->ubo[NVN_STAGE_VS][1].addr;
    MQ.n++;
    MO.draws++;
    for (int i = 0; i < 16; i++)   // the frame's draws per position banks and changed banks (vk_mvobj_frame_masks)
        if (!MF[i].n || (MF[i].mask == banks && MF[i].chg == (uint32_t)memo[mi].moving)) {
            MF[i].mask = banks, MF[i].chg = (uint32_t)memo[mi].moving, MF[i].n++;
            break;
        }
    if (t0) MO.ns_rec += kern_now_ns() - t1, MO.n_rec++;
}

int vk_mvobj_execute(VkExec *ex, uint64_t cam, VkImageView mv, VkImageView depth, VkFormat depth_fmt, uint32_t w,
                     uint32_t h) {
    static uint64_t frames;
    if (++frames % 60 == 0)
        rt_log("vk_exec: per-object motion: %.2f ms a frame checking, %.2f ms listing (%.1f us a draw), %.2f ms recording",
               MO.ns_check / 1e6 / (frames ? frames : 1), MO.ns_rec / 1e6 / (frames ? frames : 1),
               MO.n_rec ? MO.ns_rec / 1e3 / MO.n_rec : 0.0, MO.ns_emit / 1e6 / (frames ? frames : 1));
    if (frames % 600 == 0) {
        char l[1400];
        int o = 0;
        for (int i = 0; i < 32 && MM[i].n && o < 1300; i++)
            o += snprintf(l + o, sizeof l - o, " %#x:%llu (u%d s%d t%d)", MM[i].mask, (unsigned long long)MM[i].n, MM[i].ubo,
                          MM[i].ssbo, MM[i].tex);
        if (o) rt_log("vk_exec: per-object motion position banks (bit n: cn) and draws:%s", l);
        o = 0;
        for (int i = 0; i < 14 && o < 1300; i++)
            if (MO.lk[i][0] + MO.lk[i][1] + MO.lk[i][2] + MO.lk[i][3])
                o += snprintf(l + o, sizeof l - o, " c%d %llu/%llu/%llu/%llu/%llu", i + 3, (unsigned long long)MO.lk[i][0],
                              (unsigned long long)MO.lk[i][1], (unsigned long long)MO.lk[i][2],
                              (unsigned long long)MO.lk[i][3], (unsigned long long)MO.lk[i][4]);
        if (o) rt_log("vk_exec: per-object motion last-frame lookups (none/self/side/nearer/refused):%s", l);
    }
    if (frames % 60 == 0 && MO.ran)
        rt_log("vk_exec: per-object motion recorded a frame: %.0f draws, %.0f pipeline binds, %.0f dynamic states, %.0f "
               "pushes of %.1f bindings, %.0f set binds, %.0f vertex buffer binds; sets written %.0f, reused %.0f",
               (double)MO.e_draws / MO.ran, (double)MO.e_pipe / MO.ran, (double)MO.e_dyn / MO.ran, (double)MO.e_push / MO.ran,
               MO.e_push ? (double)MO.e_pushw / MO.e_push : 0.0, (double)MO.e_sets / MO.ran, (double)MO.e_vb / MO.ran,
               (double)MO.e_setw / MO.ran, (double)MO.e_setreuse / MO.ran);
    if (frames % 60 == 0)
        rt_log("vk_exec: per-object motion checks: filter %.2f ms (%.0f a frame), data %.2f ms (%.0f), variant and "
               "pipeline %.2f ms (%.0f); %.0f objects looked up; still draws %.2f ms", MO.ns_part[0] / 1e6 / frames,
               (double)MO.n_part[0] / frames,
               MO.ns_part[1] / 1e6 / frames, (double)MO.n_part[1] / frames, MO.ns_part[2] / 1e6 / frames,
               (double)MO.n_part[2] / frames, (double)MO.misses / frames, MO.ns_still / 1e6 / frames);
    if (frames % 60 == 0 && MO.ncyc)
        rt_log("vk_exec: per-object motion lookups: %llu cycles finding last frame's, %llu the copies, %llu comparing, "
               "a lookup; %llu bytes a lookup", (unsigned long long)(MO.cyc[0] / MO.ncyc),
               (unsigned long long)(MO.cyc[1] / MO.ncyc), (unsigned long long)(MO.cyc[2] / MO.ncyc),
               (unsigned long long)(MO.big / MO.ncyc));
    if (frames % 60 == 0)
        rt_log("vk_exec: per-object motion: %llu draws read place or bones, %llu of them still, %llu drawn again, %llu without last frame's data, %llu "
               "waiting for a shader or pipeline; %llu frames ran, %llu dropped (no data: camera %llu, bones %llu, place %llu, "
               "bones copy %llu, place copy %llu; uniform alignment %llu; variant without its output %llu, with storage %llu, "
               "no fragment shader %llu; another camera %llu)",
               (unsigned long long)MO.cand, (unsigned long long)MO.still, (unsigned long long)MO.draws, (unsigned long long)MO.nodata,
               (unsigned long long)MO.building,
               (unsigned long long)MO.ran, (unsigned long long)MO.dropped, (unsigned long long)MO.why[0],
               (unsigned long long)MO.why[1], (unsigned long long)MO.why[2], (unsigned long long)MO.why[3],
               (unsigned long long)MO.why[4], (unsigned long long)LIM.ubo_align, (unsigned long long)MO.no_out,
               (unsigned long long)MO.storage, (unsigned long long)MO.no_fs, (unsigned long long)MO.other_cam);
    if (!MQ.n) return 0;
    static int exec = -1;
    if (exec < 0) exec = rt_knob("MVOBJ_EXEC", "1");   // TOTK_MVOBJ_EXEC=0: listed, never recorded (test)
    if (!exec) {
        mo_drop();
        return 0;
    }
    uint64_t frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (MQ.frame != frame || MQ.fence != ex->fence || MQ.depth_fmt != depth_fmt) {
        mo_drop();
        return 0;
    }
    int fo = 0;
    for (int i = 0; i < 16 && MF[i].n && fo < (int)sizeof fm_last - 40; i++)
        fo += snprintf(fm_last + fo, sizeof fm_last - fo, " %#x/%#x:%u", MF[i].mask, MF[i].chg, MF[i].n);
    if (!fo) fm_last[0] = 0;
    // only draws with the scene camera block (vk_dlaa.c mv_pass's): last frame's camera they get is that block's, and
    // a draw of another view would move by the difference between the two views
    int kept = 0;
    for (int i = 0; i < MQ.n; i++)
        if (MQ.d[i].cam == cam) MQ.d[kept++] = MQ.d[i];
        else MO.other_cam++;
    MQ.n = kept;
    if (!MQ.n) {
        mo_drop();
        return 0;
    }
    uint64_t t0 = kern_now_ns();
    VkCommandBuffer cb = ex->cb;
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = mv,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingAttachmentInfo da = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = depth,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca, .pDepthAttachment = &da};
    vkCmdBeginRendering(cb, &ri);   // through the recorder (vk_rec.h), in order with the draws and the end
    mo_emit(cb);
    vkCmdEndRendering(cb);
    // the draws left their pipeline, descriptors, buffers and dynamic state bound: the next game draw sets everything
    vk_exec_draw_new_cb(ex);
    ex->dirty = VK_EXEC_DIRTY_ALL;
    ex->recorded += MQ.n;
    MQ.n = MQ.nst = MQ.nvb = 0, MQ.ndesc = 0, MQ.frame = 0;
    MO.ran++;
    MO.ns_emit += kern_now_ns() - t0;
    return 1;
}

static void bind_index(VkExec *ex, const void *data, uint64_t bytes, VkIndexType type) {
    VkBuffer b;
    VkDeviceSize o;
    MX_CPY(scratch(ex, bytes, &b, &o), data, bytes);
    vkCmdBindIndexBuffer(ex->cb, b, o, type);
    ex->recorded++;
}

static void op_draw(VkExec *ex, const p_draw *d) {
    VkExecDraw *D = ex->draw;
    D->c0_binst = (uint32_t)d->base_instance;
    if (d->count <= 0 || !prepare_draw(ex, d->prim, d->count, 0)) return;
    ex->fr.draws++;
    uint32_t inst = d->instances > 0 ? (uint32_t)d->instances : 1, first = (uint32_t)d->first, n = (uint32_t)d->count;
    if (d->prim == NVN_DRAW_PRIMITIVE_QUADS && n >= 4) {   // quads become two triangles each
        uint32_t q = n / 4;
        uint32_t *ix = malloc(sizeof *ix * 6 * q);
        if (!ix) rt_fatal("vk: out of memory");
        for (uint32_t i = 0; i < q; i++) {
            uint32_t v = first + 4 * i, k[6] = {0, 1, 2, 0, 2, 3};
            for (int j = 0; j < 6; j++) ix[6 * i + j] = v + k[j];
        }
        bind_index(ex, ix, sizeof *ix * 6 * q, VK_INDEX_TYPE_UINT32);
        free(ix);
        vkCmdDrawIndexed(ex->cb, 6 * q, inst, 0, 0, (uint32_t)d->base_instance);
    } else if (d->prim == NVN_DRAW_PRIMITIVE_LINE_LOOP && n >= 2) {   // a strip that returns to its first vertex
        uint32_t *ix = malloc(sizeof *ix * (n + 1));
        if (!ix) rt_fatal("vk: out of memory");
        for (uint32_t i = 0; i < n; i++) ix[i] = first + i;
        ix[n] = first;
        bind_index(ex, ix, sizeof *ix * (n + 1), VK_INDEX_TYPE_UINT32);
        free(ix);
        vkCmdDrawIndexed(ex->cb, n + 1, inst, 0, 0, (uint32_t)d->base_instance);
    } else {
        vkCmdDraw(ex->cb, n, inst, first, (uint32_t)d->base_instance);
    }
    ex->recorded++;
}

// Primitive restart of an indexed draw. NVN restarts strips wherever an index equals the SetPrimitiveRestart value;
// Vulkan restarts only at the all-ones value of the bound index type, and only strips and fans without
// primitiveTopologyListRestart. Byte indices are widened to 16 bits, so their restart value becomes 0xffff
// (op_draw_idx). Without restart the 0xffff entries of TOTK's terrain and cliff strips fetch vertex 65535 + base and
// the triangles around them drop out.
static int restart_for(const VkExec *ex, int prim, int itype) {
    if (!ex->prim_restart.enable) return 0;
    switch (vk_pipeline_topology(prim)) {
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY: break;
    default: return 0;   // lists: a restart index only drops the unfinished primitive
    }
    static const uint32_t all_ones[3] = {0xffu, 0xffffu, 0xffffffffu};
    if (ex->prim_restart.index == all_ones[itype]) return 1;
    warn_once(W_RESTART, "primitive restart index %#x with %d-byte indices is not replayed: strips drawn without restart",
              ex->prim_restart.index, 1 << itype);
    return 0;
}

static void op_draw_idx(VkExec *ex, const p_draw_idx *d) {
    VkExecDraw *D = ex->draw;
    D->c0_binst = (uint32_t)d->base_instance;
    static const uint32_t isize[3] = {1, 2, 4};
    if (d->itype < 0 || d->itype > 2) return warn_once(W_INDEX, "index type %d is unknown: draw skipped", d->itype);
    int restart = restart_for(ex, d->prim, d->itype);
    if (d->count <= 0 || !prepare_draw(ex, d->prim, d->count, restart)) return;
    ex->fr.draws++;
    uint64_t bytes = (uint64_t)d->count * isize[d->itype];
    VkMemSpan sp;
    if (!vk_mem_resolve(d->ibuf, bytes, &sp)) return warn_once(W_INDEX, "index buffer %#llx is not backed: draws with it are skipped", (unsigned long long)d->ibuf);
    if (d->itype == NVN_INDEX_TYPE_UNSIGNED_BYTE) {   // no uint8 index type here: widen (host-readable backing only)
        if (sp.alloc) return warn_once(W_INDEX, "byte indices in device-local memory: draw skipped");
        vk_mem_count(sp.cls, VK_MUSE_CPU_READ, bytes);
        vk_mem_count(VK_MCLS_SCRATCH, VK_MUSE_IB, 2 * bytes);
        uint16_t *ix = malloc(sizeof *ix * (size_t)d->count);
        if (!ix) rt_fatal("vk: out of memory");
        for (int i = 0; i < d->count; i++) {
            uint8_t v = ((const uint8_t *)(uintptr_t)d->ibuf)[i];
            ix[i] = restart && v == 0xff ? 0xffff : v;
        }
        bind_index(ex, ix, sizeof *ix * (size_t)d->count, VK_INDEX_TYPE_UINT16);
        free(ix);
    } else if (sp.offset % isize[d->itype]) {
        return warn_once(W_INDEX, "index buffer %#llx is not aligned to its index size: draw skipped", (unsigned long long)d->ibuf);
    } else {
        VK_MEM_USE(sp);
        vkCmdBindIndexBuffer(ex->cb, sp.buffer, sp.offset, d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        ex->recorded++;
        vk_mem_count(sp.cls, VK_MUSE_IB, bytes);
        VK_MEM_BOUND(sp);
    }
    vkCmdDrawIndexed(ex->cb, (uint32_t)d->count, d->instances > 0 ? (uint32_t)d->instances : 1, 0, d->base_vertex,
                     (uint32_t)d->base_instance);
    ex->recorded++;
    if (d->itype != NVN_INDEX_TYPE_UNSIGNED_BYTE) mvobj_draw(ex, d, sp.buffer, sp.offset);
}

// DrawElementsIndirect: the arguments are read by the GPU from the pool buffer (they are often written by an earlier
// dispatch), as nxnative nvn_mtl.m does. The base instance stays out of c0 (0x644): c0 cannot be patched by a transfer
// inside a pass; gl_BaseInstance of the Vulkan draw is the argument's. Byte indices need the count to be widened: skipped.
static void op_draw_idx_ind(VkExec *ex, const p_draw_idx_ind *d) {
    VkExecDraw *D = ex->draw;
    D->c0_binst = 0;
    if (d->itype != NVN_INDEX_TYPE_UNSIGNED_SHORT && d->itype != NVN_INDEX_TYPE_UNSIGNED_INT)
        return warn_once(W_INDEX, "indirect draw with index type %d: skipped", d->itype);
    VkMemSpan ib, ip;
    if (!vk_mem_resolve(d->ind, sizeof(VkDrawIndexedIndirectCommand), &ip) || ip.offset % 4)
        return warn_once(W_INDIRECT, "indirect draw arguments at %#llx are not backed or not aligned: draws skipped", (unsigned long long)d->ind);
    if (!vk_mem_resolve(d->ibuf, 1, &ib) || ib.offset % (d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? 2 : 4))
        return warn_once(W_INDEX, "index buffer %#llx is not backed or not aligned: indirect draws skipped", (unsigned long long)d->ibuf);
    if (!prepare_draw(ex, d->prim, 0x7fffffff, restart_for(ex, d->prim, d->itype))) return;
    ex->fr.draws++;
    VK_MEM_USE(ip);
    VK_MEM_USE(ib);
    vkCmdBindIndexBuffer(ex->cb, ib.buffer, ib.offset, d->itype == NVN_INDEX_TYPE_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexedIndirect(ex->cb, ip.buffer, ip.offset, 1, sizeof(VkDrawIndexedIndirectCommand));
    ex->recorded += 2;
    vk_mem_count(ip.cls, VK_MUSE_IND, sizeof(VkDrawIndexedIndirectCommand));
    VK_MEM_BOUND(ip);
    VK_MEM_BOUND(ib);
}

// ---- dispatch and uniform buffer updates

static void op_dispatch(VkExec *ex, const p_dispatch *d) {
    VkExecDraw *D = ex->draw;
    if (d->x <= 0 || d->y <= 0 || d->z <= 0) return;
    if (!ex->prog) return warn_once(W_PROGRAM, "a dispatch without a bound program is skipped");
    vk_contact_flush(ex);
    cb_res(ex);
    dummies_init();
    int present;
    const GfxShader *cs = shader_for(ex, NVN_STAGE_CS, &present, NULL, 0);   // synchronous: a skipped dispatch changes results
    if (!cs) return;
    const VkPipeInfo *info = NULL;
    VkPipeline p = vk_pipeline_compute(cs, &info);
    if (!p || !info) return warn_once(W_PIPELINE, "a compute pipeline could not be created: dispatches with it are skipped");
    vk_exec_barrier(ex);   // ends the pass; earlier writes are visible to the shader
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    D->c0_dim[0] = (uint32_t)d->x, D->c0_dim[1] = (uint32_t)d->y, D->c0_dim[2] = (uint32_t)d->z;
    D->twin_dispatch = 1;
    bind_resources(ex, info, NULL, NULL);
    D->twin_dispatch = 0;
    VkPassMark *m = dispatch_mark(ex, cs, (uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z);
    vkCmdDispatch(ex->cb, (uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z);
    vk_pass_mark_end(ex->cb, m);
    ex->recorded++;
    if (vk_dump_armed) vk_dump_dispatch(ex, cs, p, info, d);
}

// DispatchComputeIndirect: vkCmdDispatchIndirect from the pool buffer. The shader reads the group counts from c0 0x200,
// so they are copied there on the GPU, into a c0 range no other dispatch uses, before the dispatch.
static void op_dispatch_ind(VkExec *ex, const p_dispatch_ind *d) {
    VkExecDraw *D = ex->draw;
    if (!ex->prog) return warn_once(W_PROGRAM, "a dispatch without a bound program is skipped");
    VkMemSpan ip;
    if (!vk_mem_resolve(d->ind, sizeof(VkDispatchIndirectCommand), &ip) || ip.offset % 4)
        return warn_once(W_INDIRECT, "indirect dispatch arguments at %#llx are not backed or not aligned: skipped", (unsigned long long)d->ind);
    cb_res(ex);
    dummies_init();
    int present;
    const GfxShader *cs = shader_for(ex, NVN_STAGE_CS, &present, NULL, 0);
    if (!cs) return;
    const VkPipeInfo *info = NULL;
    VkPipeline p = vk_pipeline_compute(cs, &info);
    if (!p || !info) return warn_once(W_PIPELINE, "a compute pipeline could not be created: dispatches with it are skipped");
    vk_exec_barrier(ex);
    VK_MEM_USE(ip);
    bind_pipeline(ex, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    D->c0_dim[0] = D->c0_dim[1] = D->c0_dim[2] = 0;
    D->B.c0_valid[2] = 0;   // a fresh c0 range: the copy below must not change one an earlier dispatch reads
    D->twin_dispatch = 1;
    bind_resources(ex, info, NULL, NULL);
    D->twin_dispatch = 0;
    if (D->B.c0_valid[2]) {   // the shader binds c0
        VkBufferCopy cp = {ip.offset, D->B.c0_off[2] + 0x200, sizeof(VkDispatchIndirectCommand)};
        vkCmdCopyBuffer(ex->cb, ip.buffer, D->B.c0_buf[2], 1, &cp);
        ex->recorded++;
        vk_exec_barrier(ex);
        D->B.c0_valid[2] = 0;   // the range no longer holds B.c0[2]
    }
    VkPassMark *m = dispatch_mark(ex, cs, 0, 0, 0);
    vkCmdDispatchIndirect(ex->cb, ip.buffer, ip.offset);
    vk_pass_mark_end(ex->cb, m);
    ex->recorded++;
    vk_mem_count(ip.cls, VK_MUSE_IND, sizeof(VkDispatchIndirectCommand));
    VK_MEM_BOUND(ip);
    if (vk_dump_armed) vk_dump_dispatch(ex, cs, p, info, &(p_dispatch){0, 0, 0, d->caller});
}

// ---- UpdateUniformBuffer inside a pass (TOTK_UB_OVERLAY)
// nvnCommandBufferUpdateUniformBuffer writes a few hundred bytes of constants between draws: the draws recorded after
// it read the new bytes, those before it the old ones. Replayed as a buffer copy at that point it needs a barrier
// against the earlier draws, and a barrier ends the open pass; the terrain and the sky islands' meshes update before
// most of their draws, about 200 updates inside passes in a field submission, each a pass end, a full barrier and a
// pass begin. Here an update inside a pass, and with TOTK_UB_OVERLAY_OUT one between passes, goes into the scratch
// buffer as an overlay: the later bindings of a range it covers bind the scratch bytes (ub_bind, from ubo_info and
// bind_range), and the copies into the real buffers wait for the next full barrier, where the pass has ended anyway
// (vk_exec_ub_flush from vk_exec.c barrier_to: a pass end, a dispatch, a transfer, the end of the submission), behind
// one barrier of their own. Each draw reads the bytes it would have read with the copy in between.
// - A GPU write of another kind (a copy, a fill, a shader's storage write) is ordered with the copies only by the
//   command buffer. vk_exec_ub_foreign notes it and retires the overlays it touches; an update over such bytes takes
//   the old path. A shader that writes a range with copies pending gets them first (ub_written: the pass restarts).
// - An update that takes the old path (outside a pass, a full list) retires the overlays it covers (ov_retire).
// - A binding an overlay covers only in part, or at an offset the binding's alignment refuses, gets a merged scratch
//   copy: the snapshot's bytes (vk_usnap.c) or guest memory, with every update of the submission so far (UPD) on top.
// - The descriptor build skip (bind_sig) hashes the overlay each uniform and storage binding of the draw reads
//   (ov_at), so a draw of an updated block binds the newest bytes and the other draws still skip. The lists are per submission and keep one entry per range (ov_add), and a binding outside their span is
//   answered in one comparison; when one is full, updates take the old path.
// - A chunked replay (vk_exec.c) takes the old path for every update: OV is one list in replay order, which the chunks
//   recording on several threads at once neither share nor could fill in order. Its fence never matches OV.fence, so
//   ub_bind, ub_written, ov_at and vk_exec_ub_flush leave its chunks alone.

static int ov_on(void) {
    if (OV.mode < 0) OV.mode = rt_knob("UB_OVERLAY", "1");
    return RT_KNOB_ON(OV.mode);
}

// TOTK_UB_OVERLAY_OUT: updates between passes become overlays too. Copied at once, each needs a full barrier of its
// own, and the copy it records makes the next update or pass begin need another: a run of k updates before a pass cost
// k + 1 barriers. Every GPU reader of the real buffer after them comes behind barrier_to, which records the copies
// first (a pass begin, a dispatch, a transfer, the end of the submission), so they cost one barrier for the run.
static int ov_out_on(void) {
    static int mode = -1;
    if (mode < 0) mode = rt_knob("UB_OVERLAY_OUT", "1");
    return RT_KNOB_ON(mode);
}

static void ov_sub(const VkExec *ex) {   // the submission being replayed
    if (OV.fence == ex->fence) return;
    if (OV.np)   // barrier_to records them at the latest at the submission's closing barrier
        rt_log("vk: %u overlaid uniform updates of submission %llu were never copied into their buffers", OV.np,
               (unsigned long long)OV.fence);
    OV.fence = ex->fence, OV.n = OV.np = OV.nf = 0, OV.f_over = 0, OV.lo = UINT64_MAX, OV.hi = 0;
    OV.gen++;
}

static void ov_span(void) {
    OV.lo = UINT64_MAX, OV.hi = 0;
    for (uint32_t i = 0; i < OV.n; i++) {
        if (OV.o[i].lo < OV.lo) OV.lo = OV.o[i].lo;
        if (OV.o[i].hi > OV.hi) OV.hi = OV.o[i].hi;
    }
}

// A new overlay of [lo, hi), and its copy. An older overlay or pending copy of exactly that range is dropped: the new
// one covers it, and the terrain rewrites one block before each draw, so the lists stay as short as the blocks.
static void ov_add(uint64_t lo, uint64_t hi, VkBuffer b, VkDeviceSize o, const VkMemSpan *sp) {
    for (uint32_t i = 0; i < OV.n; i++)
        if (OV.o[i].lo == lo && OV.o[i].hi == hi) {
            memmove(&OV.o[i], &OV.o[i + 1], sizeof OV.o[0] * (OV.n - i - 1));
            OV.n--;
            break;
        }
    for (uint32_t i = 0; i < OV.np; i++)
        if (OV.p[i].dst == lo && OV.p[i].size == hi - lo) {
            memmove(&OV.p[i], &OV.p[i + 1], sizeof OV.p[0] * (OV.np - i - 1));
            OV.np--;
            break;
        }
    OV.o[OV.n++] = (typeof(OV.o[0])){lo, hi, ++OV.serial, b, o};
    OV.gen++;
    OV.p[OV.np++] = (typeof(OV.p[0])){lo, b, o, *sp, hi - lo};
    if (lo < OV.lo) OV.lo = lo;
    if (hi > OV.hi) OV.hi = hi;
}

static int ov_foreign_hits(uint64_t lo, uint64_t hi) {
    if (OV.f_over) return 1;
    for (uint32_t i = 0; i < OV.nf; i++)
        if (lo < OV.f[i].hi && hi > OV.f[i].lo) return 1;
    return 0;
}

// The overlays over [lo, hi) no longer hold the newest bytes: the real buffer does. Their copies were recorded already
// (the write came after a barrier, or ub_written brought them forward), so they only stop serving bindings.
static void ov_retire(uint64_t lo, uint64_t hi) {
    if (!OV.n || lo >= OV.hi || hi <= OV.lo) return;
    uint32_t k = 0;
    for (uint32_t i = 0; i < OV.n; i++)
        if (!(lo < OV.o[i].hi && hi > OV.o[i].lo)) OV.o[k++] = OV.o[i];
    if (k == OV.n) return;
    OV.n = k;
    OV.gen++;
    ov_span();
}

// A GPU write of another kind into [addr, addr+n) in this submission (vk_exec_xfer.c note_gpu_write, storage bindings).
void vk_exec_ub_foreign(VkExec *ex, uint64_t addr, uint64_t n) {
    if (OV.mode <= 0 || !n || ex->chunked) return;
    ov_sub(ex);
    if (OV.nf == OV_FOREIGN) OV.f_over = 1;
    else OV.f[OV.nf].lo = addr, OV.f[OV.nf].hi = addr + n, OV.nf++;
    ov_retire(addr, addr + n);
}

// Records the copies of the updates overlaid so far, behind a barrier against the draws that read the old bytes.
void vk_exec_ub_flush(VkExec *ex) {
    if (!OV.np || OV.fence != ex->fence) return;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded++;
    // Copies without a barrier between them may run in any order, and ov_add only drops an older copy of exactly the
    // same range: a copy that overlaps one recorded since the last barrier waits for it, so the newer bytes land last.
    VkMemoryBarrier2 ww = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkDependencyInfo wdi = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &ww};
    uint32_t since = 0;   // the first copy recorded after the last barrier
    for (uint32_t i = 0; i < OV.np; i++) {
        for (uint32_t k = since; k < i; k++)
            if (OV.p[i].dst < OV.p[k].dst + OV.p[k].size && OV.p[k].dst < OV.p[i].dst + OV.p[i].size) {
                vkCmdPipelineBarrier2(ex->cb, &wdi);
                ex->recorded++;
                OV.c_order++;
                since = i;
                break;
            }
        VkBufferCopy cp = {OV.p[i].soff, OV.p[i].sp.offset, OV.p[i].size};
        vkCmdCopyBuffer(ex->cb, OV.p[i].src, OV.p[i].sp.buffer, 1, &cp);
        ex->recorded++;
    }
    OV.np = 0;
    OV.gen++;
    // The full barrier barrier_to records next makes the copied bytes visible: later bindings read the real buffer,
    // as after an update copied at once, and need no overlay lookups or merged copies (ov_at answers 0 for them, so
    // the descriptor build skip rebuilds the draws that bound an overlay)
    OV.n = 0;
    ov_span();
    OV.c_flush++;
}

// TOTK_TRACE=perf, every 600 submissions: per submission, the updates and how many stayed in their pass.
void vk_exec_ub_perf(void) {
    static unsigned subs, dsubs;
    if (DD.mode >= 0 && ++dsubs % 600 == 0 && DD.calls) {
        rt_log("perf: dynamic state per submission: %.1f calls, %.1f commands (a full set counts 1), %.1f calls with "
               "nothing changed%s", DD.calls / 600.0, DD.cmds / 600.0, DD.empty / 600.0,
               DD.mode == 2 ? " (TOTK_DYN_DELTA=ab)" : DD.mode ? "" : " (TOTK_DYN_DELTA=0)");
        DD.calls = DD.cmds = DD.empty = 0;
    }
    if (OV.mode <= 0 || ++subs % 600) return;
    const double k = 1 / 600.0;
    rt_log("perf: uniform updates per submission %.1f: %.1f overlaid (%.1f of them between passes; copied in %.2f "
           "batches), %.1f copied in a pass, %.1f outside one; bindings from an overlay %.1f (%.1f merged, %.1f over "
           "other GPU writes), %.2f batches brought forward for a storage write, %.2f barriers between overlapping copies",
           OV.c_update * k, OV.c_overlaid * k, OV.c_overlaid_out * k, OV.c_flush * k, OV.c_inpass * k, OV.c_outside * k,
           OV.c_bind * k, OV.c_merged * k, OV.c_mixed * k, OV.c_early * k, OV.c_order * k);
    OV.c_update = OV.c_overlaid = OV.c_overlaid_out = OV.c_flush = OV.c_bind = OV.c_merged = OV.c_mixed = 0;
    OV.c_inpass = OV.c_outside = OV.c_early = OV.c_order = 0;
}

// The scratch bytes of a uniform or storage binding [addr, addr+n) an overlay of this submission touches: 1 with *out
// set, 0 when none does.
static int ub_bind(VkExec *ex, uint64_t addr, uint64_t n, uint64_t align, VkDescriptorBufferInfo *out) {
    if (OV.mode <= 0 || OV.fence != ex->fence || !OV.n || addr >= OV.hi || addr + n <= OV.lo) return 0;
    uint64_t hi = addr + n;
    int newest = -1;
    for (int i = (int)OV.n; i-- > 0;)
        if (addr < OV.o[i].hi && hi > OV.o[i].lo) { newest = i; break; }
    if (newest < 0) return 0;
    OV.c_bind++;
    if (OV.o[newest].lo <= addr && OV.o[newest].hi >= hi) {
        VkDeviceSize off = OV.o[newest].off + (addr - OV.o[newest].lo);
        if (off % align == 0) {
            *out = (VkDescriptorBufferInfo){OV.o[newest].buf, off, n};
            return 1;
        }
    }
    // Merged: the bytes as of the submit call (the snapshot, else guest memory), then this submission's updates in
    // order. A transient copy, so the descriptor build skip leaves the draw after it alone (RS.volatile_). Bytes a copy
    // or a storage write of this submission produced are not in either source: counted.
    VkBuffer b;
    VkDeviceSize o;
    uint8_t *dst = scratch(ex, n, &b, &o);
    ex->draw->RS.volatile_ = 1;
    if (ex->draw->BI.cur) ex->draw->BI.cur->vol = 1;
    const uint8_t *base = vk_usnap_bytes(addr, n);
    VkMemSpan sp;
    if (base) MX_CPY(dst, base, n);
    else if (vk_mem_resolve(addr, n, &sp)) MX_CPY(dst, (const void *)(uintptr_t)addr, n);
    else memset(dst, 0, n);
    if (ov_foreign_hits(addr, hi)) OV.c_mixed++;
    upd_apply(ex, addr, dst, n);
    *out = (VkDescriptorBufferInfo){b, o, n};
    OV.c_merged++;
    return 1;
}

// Before a binding the shader writes: overlaid updates whose copies are still pending go to the buffer first, behind a
// barrier, and the pass restarts, so the shader's writes land after them as they did when every update was copied at
// once.
static void ub_written(VkExec *ex, uint64_t addr, uint64_t n) {
    if (OV.mode <= 0 || OV.fence != ex->fence || !OV.np) return;
    for (uint32_t i = 0; i < OV.np; i++)
        if (addr < OV.p[i].dst + OV.p[i].size && OV.p[i].dst < addr + n) {
            int reopen = ex->in_pass;
            vk_exec_barrier(ex);   // records the pending copies (vk_exec_ub_flush) before its barrier
            if (reopen) vk_exec_pass_begin(ex);
            OV.c_early++;
            return;
        }
}

static void op_update_ub(VkExec *ex, const p_update_ub *u) {
    if (!u->size) return;
    VkMemSpan sp;
    if (!vk_mem_resolve(u->dst, u->size, &sp)) return warn_once(W_UB, "UpdateUniformBuffer to %#llx is not backed: dropped", (unsigned long long)u->dst);
    cb_res(ex);
    VkBuffer b;
    VkDeviceSize o;
    if (!ex->chunked && ov_on()) {
        ov_sub(ex);
        OV.c_update++;
        int here = ex->in_pass || ov_out_on();
        if (here && OV.n < OV_N && OV.np < OV_N && !ov_foreign_hits(u->dst, u->dst + u->size)) {
            uint8_t *d = scratch(ex, u->size, &b, &o);
            MX_CPY(d, u->data, u->size);
            VK_MEM_PICK(sp);
            ov_add(u->dst, u->dst + u->size, b, o, &sp);
            vk_mem_gpu_wrote(&sp, u->size);
            vk_usnap_drop(u->dst, u->size);
            vk_mem_count(sp.cls, VK_MUSE_GPU_WRITE, u->size);
            note_written(ex, u->dst, u->size);
            upd_note(ex, u);
            OV.c_overlaid++;
            if (!ex->in_pass) OV.c_overlaid_out++;
            return;
        }
        if (ex->in_pass) OV.c_inpass++;
        else OV.c_outside++;
        ov_retire(u->dst, u->dst + u->size);
    }
    vk_exec_barrier(ex);   // earlier draws finished with the old contents; overlaid updates' copies go first
    MX_CPY(scratch(ex, u->size, &b, &o), u->data, u->size);
    VK_MEM_PICK(sp);
    VkBufferCopy cp = {o, sp.offset, u->size};
    vkCmdCopyBuffer(ex->cb, b, sp.buffer, 1, &cp);
    vk_mem_gpu_wrote(&sp, u->size);
    if (!ex->chunked) vk_usnap_drop(u->dst, u->size);   // a chunk: the scan dropped it in its place
    ex->recorded++;
    vk_mem_count(sp.cls, VK_MUSE_GPU_WRITE, u->size);
    note_written(ex, u->dst, u->size);
    upd_note(ex, u);
}

// ---- bind ops

// Writes the handles of b into dst; returns the slots whose handle changed (bit i: slot b->first + i).
static uint32_t bind_handles(uint64_t *dst, int cap, const p_bind_handles *b) {
    if (b->stage < 0 || b->stage >= 6 || b->first < 0 || b->n < 0 || b->n > 32 || b->first + b->n > cap) {
        warn_once(W_BIND, "a bind of %d handles at %d of stage %d is out of range: ignored", b->n, b->first, b->stage);
        return 0;
    }
    uint32_t chg = 0;
    for (int i = 0; i < b->n; i++)
        if (dst[b->first + i] != b->h[i]) dst[b->first + i] = b->h[i], chg |= 1u << i;
    return chg;
}

// Writes n bytes at dst; 1 when they differ from what they replace.
static int put_changed(void *dst, const void *src, size_t n) {
    if (!memcmp(dst, src, n)) return 0;
    memcpy(dst, src, n);
    return 1;
}

// A write of n bytes of ex->pipe; one that changes them bumps ex->pipe_gen, so a state object bound again with the same
// values leaves the draws after it on the generation's fast path.
static void pipe_put(VkExec *ex, void *dst, const void *src, size_t n) {
    if (put_changed(dst, src, n)) ex->pipe_gen++;
}

// The changed handle slots chg of a bind at first into a 128-bit dirty mask.
static void dirty_slots(uint64_t m[2], int first, uint32_t chg) {
    if (!chg) return;
    uint64_t c = (uint64_t)chg;
    if (first < 64) m[0] |= c << first, m[1] |= first ? c >> (64 - first) : 0;
    else m[1] |= c << (first - 64);
}

// The bind ops: writes into ex alone, so the state scan (vk_exec.c) applies them too. 1 when the op wrote ex->prog,
// ubo, ssbo, tex or img (VkExecDraw.bind_gen). Each op marks in ex->dirt the slots whose bytes it changed and bumps
// ex->pipe_gen when it changed pipe (TOTK_BIND_INC); a rebind of the same values marks nothing.
int vk_exec_bind(VkExec *ex, uint32_t op, const void *payload) {
    int gen = 0;
    switch (op) {
    case OP_BIND_PROGRAM: {
        const p_bind_program *b = payload;
        if (ex->prog != b->prog || ex->prog_stages != b->stages) ex->dirt.prog = 1;
        ex->prog = b->prog, ex->prog_stages = b->stages;
        gen = 1;
        break;
    }
    case OP_BIND_STATE: {
        const p_bind_state *b = payload;
        blend_st bs;
        switch (b->kind) {
        case 0:
            memcpy(&bs, b->bytes, sizeof bs);
            if (bs.target < 8) pipe_put(ex, &ex->pipe.blend[bs.target], &bs, sizeof bs);
            break;
        case 1: pipe_put(ex, &ex->pipe.color, b->bytes, sizeof ex->pipe.color); break;
        case 2: pipe_put(ex, &ex->pipe.chmask, b->bytes, sizeof ex->pipe.chmask); break;
        case 3: pipe_put(ex, &ex->pipe.ms, b->bytes, sizeof ex->pipe.ms); break;
        case 4: pipe_put(ex, &ex->pipe.poly, b->bytes, sizeof ex->pipe.poly); break;
        case 5: pipe_put(ex, &ex->pipe.ds, b->bytes, sizeof ex->pipe.ds); break;
        case 6: memcpy(&ex->point_size, b->bytes, sizeof ex->point_size); break;   // a support input: compared per draw
        }
        break;
    }
    case OP_BIND_VATTR: {
        const p_bind_vattr *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        uint32_t v[16] = {0};   // MNG: the new list replaces the old one
        MX_CPY(&v[b->first], b->st, sizeof(uint32_t) * (size_t)b->n);
        pipe_put(ex, ex->pipe.vattr, v, sizeof v);
        break;
    }
    case OP_BIND_VSTREAM: {
        const p_bind_vstream *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        vs_st v[16];
        memset(v, 0, sizeof v);
        MX_CPY(&v[b->first], b->st, sizeof(vs_st) * (size_t)b->n);
        pipe_put(ex, ex->pipe.vstream, v, sizeof v);
        break;
    }
    case OP_BIND_VBUF: {
        const p_bind_vbuf *b = payload;
        if (b->first < 0 || b->n < 0 || b->first + b->n > 16) break;
        for (int i = 0; i < b->n; i++)
            if (put_changed(&ex->vbuf[b->first + i], &b->r[i], sizeof(NVNbufferRange))) ex->dirt.vbuf |= 1u << (b->first + i);
        break;
    }
    case OP_BIND_UBO: {
        const p_bind_buf *b = payload;
        if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 18 &&
            put_changed(&ex->ubo[b->stage][b->index], b, sizeof *b))
            ex->dirt.ubo[b->stage] |= 1u << b->index;
        gen = 1;
        break;
    }
    case OP_BIND_SSBO: {
        const p_bind_buf *b = payload;
        if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16 &&
            put_changed(&ex->ssbo[b->stage][b->index], b, sizeof *b))
            ex->dirt.ssbo[b->stage] |= 1u << b->index;
        gen = 1;
        break;
    }
    case OP_BIND_TEX: {
        const p_bind_handles *b = payload;
        uint32_t chg = bind_handles(b->stage >= 0 && b->stage < 6 ? ex->tex[b->stage] : NULL, 128, b);
        if (chg) dirty_slots(ex->dirt.tex[b->stage], b->first, chg);
        ex->tex_serial++;
        gen = 1;
        break;
    }
    case OP_BIND_SAMP: {   // not read by the bind path: a texture handle carries its sampler id (h >> 20)
        const p_bind_handles *b = payload;
        bind_handles(b->stage >= 0 && b->stage < 6 ? ex->samp[b->stage] : NULL, 128, b);
        break;
    }
    case OP_BIND_IMG: {
        const p_bind_handles *b = payload;
        uint32_t chg = bind_handles(b->stage >= 0 && b->stage < 6 ? ex->img[b->stage] : NULL, 32, b);
        if (chg) ex->dirt.img[b->stage] |= chg << b->first;
        ex->tex_serial++;   // the shader memo's handle check (feedback_scan reads only textures: a rescan at most)
        gen = 1;
        break;
    }
    default: rt_fatal("vk: op %s is not a bind op", nvn_op_name(op));
    }
    return gen;
}

void vk_exec_op_draw(VkExec *ex, uint32_t op, const void *payload) {
    VkExecDraw *D = ex->draw ? ex->draw : draw_new(ex);
    switch (op) {
    case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
    case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG:
        if (vk_exec_bind(ex, op, payload)) D->bind_gen++;
        break;
    case OP_UPDATE_UB: op_update_ub(ex, payload); break;
    case OP_DRAW: op_draw(ex, payload); break;
    case OP_DRAW_IDX: op_draw_idx(ex, payload); break;
    case OP_DISPATCH: op_dispatch(ex, payload); break;
    case OP_DRAW_IDX_IND: op_draw_idx_ind(ex, payload); break;
    case OP_DISPATCH_IND: op_dispatch_ind(ex, payload); break;
    default: rt_fatal("vk: op %s is not a draw op", nvn_op_name(op));
    }
}

// The last executed list's draws as " <position banks>/<changed banks>:<draws>" (bit n: cn), for TOTK_MV_CHECK's lines.
const char *vk_mvobj_frame_masks(void) { return fm_last; }
