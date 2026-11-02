// Ported from MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn/nvn_obj.h and the two types of nvn_tex.h @ b75be8cf (MIT, Copyright (c) 2026 jhaizhou; origin d2r-mobile); Metal entry points dropped, layouts unchanged.
// NVN host-side object layout and command record format: shared by hle_nvn.c (interface layer, writes) and nvn_mtl.m (Metal replay layer, reads).
// Objects are stored in place in the NVN object memory allocated by the game, with size not exceeding the reserved size in nvn.h (FITS static assertion).
// State objects (Blend / Color / ... 4–0x18 bytes) are small structs the game copies by value; their layout is packed in place with bit-fields.
//
// In this port the interface layer is runtime/nvn_api_*.c and nvn_ext.c, and the replay layers are the back ends of
// gfx.h (GfxBackend).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "nvn.h"

#define MAGIC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FITS(T, H) _Static_assert(sizeof(T) <= sizeof(H), #T " exceeds the reserved size of " #H)

// ---------- Texture description and storage layout (nvn_tex.h; the functions over them live in nvn_tex.c) ----------
typedef struct {
    int target, format, flags, samples;   // NVNtextureTarget / NVNformat / NVNtextureFlags / sample count (0 or 1 = single-sampled)
    uint32_t width, height, depth, levels, stride;
    signed int pkg_block_h:8; unsigned int swizzle_r:3,swizzle_g:3,swizzle_b:3,swizzle_a:3,swizzle_pad:12;                      // Byte 0 of the packaged texture layout parameters + 1 (low nibble log2 block height, high nibble log2 block depth); 0 = use the automatic algorithm
} nvn_texdesc;

typedef struct {
    uint64_t size, layer_size;            // Total storage bytes, bytes per layer
    uint32_t align, layers, bpb;          // Alignment, layer count, bytes per block
    unsigned tileH, tileD;                // Block height / block depth (log2 of the GOB count)
} nvn_texlayout;

typedef struct { uint32_t flags; } dev_builder_t;                       FITS(dev_builder_t, NVNdeviceBuilder);
typedef struct { uint32_t magic, flags, origin, depth_mode; void *dbg_cb, *dbg_data; } dev_t_;  FITS(dev_t_, NVNdevice);

typedef struct { NVNdevice *dev; uint32_t flags; uint64_t cmd_sz, comp_sz, ctl_sz, flush; void *mem; uint64_t mem_sz; } queue_builder_t;
FITS(queue_builder_t, NVNqueueBuilder);
typedef struct { uint32_t magic; NVNdevice *dev; uint64_t submits; } queue_t;                 FITS(queue_t, NVNqueue);

typedef struct { NVNdevice *dev; void *mem; uint64_t size; uint32_t flags; } pool_builder_t;  FITS(pool_builder_t, NVNmemoryPoolBuilder);
// game_mem/host/span: TOTK host-owned VM storage for physical/virtual pools (totk_vpool.c); host is NULL for game-backed pools.
typedef struct { uint32_t magic, flags; uint8_t *mem; uint64_t size; uint8_t *game_mem, *host; uint64_t span; } pool_t; FITS(pool_t, NVNmemoryPool);

typedef struct { const pool_t *pool; int64_t off; int32_t n; } dpool_t;                       // Texture / sampler descriptor pool
FITS(dpool_t, NVNtexturePool); FITS(dpool_t, NVNsamplerPool);

typedef struct {
    NVNdevice *dev; nvn_texdesc d; pool_t *pool; int64_t off;
    uint8_t has_layout, layout[8];
    const void *pkg_data;                // TOTK: SetPackagedTextureData source, copied into storage at Initialize
} tex_builder_t;                                                                             FITS(tex_builder_t, NVNtextureBuilder);
typedef struct {
    uint32_t magic; nvn_texdesc d; uint8_t has_layout, layout[8];
    uint16_t pool_flags;                 // the pool's flags at Initialize (in the padding): a game may finalize and free
                                         // the pool object while the back end still looks the texture up, and the
                                         // driver keeps no pointer to it either
    nvn_texlayout L; uint8_t *mem; pool_t *pool; int64_t off;
    uint64_t serial;                     // Initialization serial: the back end uses it to find its texture in the host table (the game may copy the object by value, so object pointers cannot be stored)
} tex_t;                                                                                     FITS(tex_t, NVNtexture);
// TOTK: the game reserves 16 bytes for NVNtextureView on its stack (main+0x823398 SetDefaults at sp+0x30, its own
// data at sp+0x40), so the view is packed into 16 bytes. swz = NVN view swizzle selectors. dsmode:
// NVNtextureDepthStencilMode (nxnative nvn_obj.h:42 packs it the same way).
typedef struct {
    uint8_t set_levels : 1, set_layers : 1, set_format : 1, set_target : 1, set_swizzle : 1, set_dsmode : 1, dsmode : 2;
    uint8_t base_level, nlevels, target; uint16_t min_layer, nlayers; uint16_t format; uint8_t swz[4]; uint8_t pad1[2];
} view_t;
_Static_assert(sizeof(view_t) == 16, "NVNtextureView is 16 bytes in TOTK's NVN");
FITS(view_t, NVNtextureView);

// Sampler parameters (one copy shared by builder and object)
typedef struct { uint8_t minf, magf, ws, wt, wr, cmp_mode, cmp_func, reduction; float lod_min, lod_max, lod_bias, aniso, border[4], lod_snap; } samp_par;   // TOTK: lod_snap stored for queries (Metal has no equivalent)   // Enum values are all < 256
typedef struct { NVNdevice *dev; samp_par p; } samp_builder_t;                                FITS(samp_builder_t, NVNsamplerBuilder);
typedef struct { uint32_t magic; samp_par p; } samp_t;                                        FITS(samp_t, NVNsampler);
_Static_assert(sizeof(samp_par) == 44, "TOTK sampler parameters");

typedef struct { uint32_t magic, nstage; NVNshaderData sd[6]; uint64_t serial; } prog_t;     FITS(prog_t, NVNprogram);   // serial: table key for the back end's resolved result

typedef struct { uint32_t magic, fenced; uint64_t fence; } sync_t;                            FITS(sync_t, NVNsync);
typedef struct { const pool_t *pool; int64_t off; } ev_builder_t;                             FITS(ev_builder_t, NVNeventBuilder);
typedef struct { uint32_t magic; volatile uint32_t *val; } ev_t;                              FITS(ev_t, NVNevent);

typedef struct { NVNdevice *dev; void *native; int32_t ntex, interval, active; NVNtexture *tex[4]; } win_builder_t;
FITS(win_builder_t, NVNwindowBuilder);
// avail[i]: time at which swapchain image i can be acquired again (ns, monotonic clock) = its display time + one present period; pace: target time of this window's present pacer
typedef struct { uint32_t magic; void *native; int32_t ntex, interval, next, active, crop[4]; NVNtexture *tex[4]; uint64_t presents, avail[4], pace, ticket[4]; } win_t;   // ticket[i]: ticket of the most recent present of image i
FITS(win_t, NVNwindow);

// Command records live in host memory; finished copies are owned by guest control memory.
typedef struct {
    uint32_t magic, recording;
    uint8_t *cur;
    uint64_t guest_flags;                // +0x10: NVN 55 inline code writes the dirty flag here
    uint8_t *end;
    // TOTK: NVN 55 headers inline fast paths that read the command write pointer at +0x20 and its end at +0x28
    // (main+0x70644c) and, when 0x30 bytes fit, emit raw GPU methods themselves. ~0 / 0 never fit, so every
    // call takes the API path (the *Deferred entry points).
    uint64_t inline_cur, inline_end;
    PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb; void *cb_data;
    NVNdevice *dev;
    uint8_t *seg;                        // TOTK: start of the host recording buffer (records never go into game memory)
    void *ctl; size_t ctl_size;          // TOTK: control memory given by the game; recording copies live as long as it
    size_t game_size;                    // TOTK: command memory the game added (accounting only; not used for storage)
    uintptr_t cmd_base;                  // TOTK: pool storage address + offset of the command memory the game added
    size_t cmd_used, ctl_used, rec_start;   // rec_start: cmd_used when the current recording began
    size_t hw_used, last_rec, hw_start;            // TOTK: command memory the real NVN driver would have consumed (what GetCommandMemoryUsed reports; the game sizes resource heaps from it), size of the last record           // TOTK: usage reported to the game since the memory was added (main+0x711e4c submits only non-empty buffers)
} cmd_t;                                                                                     FITS(cmd_t, NVNcommandBuffer);
_Static_assert(__builtin_offsetof(cmd_t, guest_flags) == 0x10, "NVN inline dirty flag");
_Static_assert(__builtin_offsetof(cmd_t, inline_cur) == 0x20 && __builtin_offsetof(cmd_t, inline_end) == 0x28, "NVN inline fast-path fields");

// ---------- State objects (defaults follow the NVN / GL defaults) ----------
typedef struct { uint32_t blend_en : 8, logic_op : 4, alpha_func : 4; } color_st;            FITS(color_st, NVNcolorState);
typedef struct { uint32_t mask; } chmask_st;            /* 4 bits per target: r=1 g=2 b=4 a=8, bit set = write */ FITS(chmask_st, NVNchannelMaskState);
typedef struct { uint64_t target : 3, src_rgb : 7, dst_rgb : 7, src_a : 7, dst_a : 7, eq_rgb : 3, eq_a : 3; } blend_st;
FITS(blend_st, NVNblendState);
typedef struct { uint8_t enable, samples, a2c, a2c_dither, raster_samples, cov_mod, cov2color_en, cov2color_out, sl_en, sl_grid; } ms_st;
FITS(ms_st, NVNmultisampleState);
typedef struct { uint32_t cull : 2, front : 1, mode : 2, offset_en : 3; } poly_st;           FITS(poly_st, NVNpolygonState);
typedef struct {
    uint64_t dt : 1, dw : 1, df : 4, st : 1, sf_f : 4, sf_b : 4,
             fail_f : 4, zfail_f : 4, zpass_f : 4, fail_b : 4, zfail_b : 4, zpass_b : 4;
} ds_st;                                                                                     FITS(ds_st, NVNdepthStencilState);
typedef struct { uint32_t fmt : 8, offset : 19, stream : 5; } va_st;                         FITS(va_st, NVNvertexAttribState);
typedef struct { uint32_t stride, divisor; } vs_st;                                          FITS(vs_st, NVNvertexStreamState);

// ---------- Command records: {op, size (including header, aligned to 8)} + payload ----------
typedef struct { uint32_t op, size; } op_hdr;
enum {
    OP_END, OP_JUMP, OP_BEGIN, OP_CALL,
    OP_SIGNAL_EVENT, OP_REPORT_COUNTER, OP_COPY_B2B, OP_UPDATE_UB,                 // CPU-visible side effects
    OP_SET_RT, OP_CLEAR_COLOR, OP_CLEAR_DS, OP_VIEWPORTS, OP_SCISSOR, OP_DEPTH_RANGE,
    OP_BIND_PROGRAM, OP_BIND_STATE, OP_BIND_VATTR, OP_BIND_VSTREAM, OP_BIND_VBUF,
    OP_BIND_UBO, OP_BIND_SSBO, OP_BIND_TEX, OP_BIND_SAMP,
    OP_STENCIL, OP_POLY_OFFSET, OP_DRAW, OP_DRAW_IDX, OP_DISPATCH, OP_BARRIER,
    OP_SET_TEX_POOL, OP_SET_SAMP_POOL, OP_COPY_B2T, OP_COPY_STRIDE, OP_BLEND_COLOR, OP_COPY_T2B, OP_BIND_IMG, OP_COPY_T2T, OP_RESET_COUNTER, OP_CLEAR_TEX, OP_DEPTH_CLAMP, OP_RENDER_ENABLE, OP_RASTER_DISCARD, OP_RENDER_COND, OP_PRIM_RESTART, OP_DRAW_IDX_IND, OP_DISPATCH_IND, OP_CLEAR_BUF, OP_COUNT_
};
// Payload (immediately follows op_hdr)
typedef struct { int32_t n; const tex_t *color[8]; view_t cview[8]; uint8_t has_cview[8]; const tex_t *depth; view_t dview; uint8_t has_dview; } p_set_rt;
typedef struct { int32_t index, mask; float rgba[4]; } p_clear_color;
typedef struct { float depth; int32_t depth_mask, stencil, stencil_mask; } p_clear_ds;
typedef struct { int32_t first, n; float v[16][6]; } p_viewports;          // x, y, w, h, near, far (near and far are overridden by DEPTH_RANGE)
typedef struct { int32_t x, y, w, h; } p_scissor;
typedef struct { int32_t value; } p_flag;   // TOTK: single boolean render state (OP_DEPTH_CLAMP)
typedef struct { int32_t mode; int32_t pad; uint64_t addr; } p_render_cond;   // TOTK: SetRenderEnableConditional: mode 0 = render if the two NVNcounterData.counter values at addr / addr+16 are equal, 1 = if not equal
typedef struct { int32_t enable; uint32_t index; } p_prim_restart;   // SetPrimitiveRestart: indexed strips restart at index
typedef struct { float n, f; } p_depth_range;
typedef struct { const prog_t *prog; int32_t stages; } p_bind_program;
typedef struct { int32_t kind; uint8_t bytes[0x18]; } p_bind_state;      // kind: 0 blend 1 color 2 chmask 3 ms 4 poly 5 ds 6 point size (float)
typedef struct { int32_t first, n; uint32_t st[16]; } p_bind_vattr;
typedef struct { int32_t first, n; vs_st st[16]; } p_bind_vstream;
typedef struct { int32_t first, n; NVNbufferRange r[16]; } p_bind_vbuf;
typedef struct { int32_t stage, index; uint64_t addr, size; } p_bind_buf;  // UBO / SSBO
// src: NVN 55 *Deferred binds keep the game's handle array pointer (read at submit into h, see hle_nvn.c resolve_deferred); 0 = immediate
typedef struct { int32_t stage, first, n, pad_; uint64_t src; uint64_t h[32]; } p_bind_handles; // Separate texture / sampler handles
typedef struct { int32_t face, value_mask, mask, ref, which; } p_stencil;  // which: 0 value mask 1 write mask 2 reference value
typedef struct { float factor, units, clamp; } p_poly_offset;
typedef struct { int32_t prim, first, count, base_instance, instances; } p_draw;   // TOTK: instances 0 = one instance (DrawArrays)
typedef struct { int32_t prim, itype, count, base_vertex, base_instance, instances; uint64_t ibuf; } p_draw_idx;
typedef struct { int32_t x, y, z; uint64_t caller; } p_dispatch;   // caller: return address of the game call site (to locate the dispatcher)
typedef struct { const dpool_t *pool; } p_set_pool;
typedef struct { uint64_t src; const tex_t *tex; view_t view; uint8_t has_view; NVNcopyRegion r; int32_t flags; } p_copy_b2t;
typedef struct { const tex_t *src, *dst; view_t sv, dv; uint8_t has_sv, has_dv; NVNcopyRegion sr, dr; int32_t flags; } p_copy_t2t;   // Reused for T2B: src stores the destination buffer address
typedef struct { const tex_t *tex; view_t view; uint8_t has_view, is_uint; NVNcopyRegion r; int32_t mask; union { float f[4]; uint32_t u[4]; } c; } p_clear_tex;
typedef struct { int64_t stride; } p_copy_stride;
typedef struct { float rgba[4]; } p_blend_color;
