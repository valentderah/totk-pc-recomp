// NVN entry points beyond the core implementation that only report stored object state, attach debugging metadata,
// convert timestamps or take hardware hints. None of them changes rendering
// results. Ported from MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn/totk_nvn_ext.c @ b75be8cf (MIT,
// Copyright (c) 2026 jhaizhou; origin d2r-mobile); mach timing and Metal wording dropped. Its *Deferred binds and
// memory-callback switches live with the command buffer in nvn_api_cmd.c.
#include <stdint.h>
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_tex.h"

// The CPU is about to read what the GPU wrote into pool storage: the back end writes its host-side copies (the Vulkan
// back end's images) back first. TotK invalidates its 260x260 R16 grass maps and 1x1 pick targets this way. A
// CPU-invisible pool has no storage to read.
void nvn_pool_invalidate(const pool_t *p, uint64_t off, uint64_t size) {
    if (!p->mem || nvn_gpu_only((uintptr_t)p->mem) || off >= p->size || !size) return;
    if (size > p->size - off) size = p->size - off;
    gfx_backend()->guest_read((uintptr_t)p->mem + off, size);
}

__attribute__((weak)) void nvn_pool_flushed(const pool_t *p, uint64_t off, uint64_t size) { (void)p, (void)off, (void)size; }

// ---------- debug labels, markers, groups, checkpoints: meaningful only to NVN debugging tools
// void nvn<Object>SetDebugLabel(NVN<object> *o, const char *label)
NVN_API(nvnDeviceSetDebugLabel) { (void)c; }
NVN_API(nvnQueueSetDebugLabel) { (void)c; }
NVN_API(nvnWindowSetDebugLabel) { (void)c; }
NVN_API(nvnProgramSetDebugLabel) { (void)c; }
NVN_API(nvnMemoryPoolSetDebugLabel) { (void)c; }
NVN_API(nvnTexturePoolSetDebugLabel) { (void)c; }
NVN_API(nvnSamplerPoolSetDebugLabel) { (void)c; }
NVN_API(nvnBufferSetDebugLabel) { (void)c; }
NVN_API(nvnTextureSetDebugLabel) { (void)c; }
NVN_API(nvnSamplerSetDebugLabel) { (void)c; }
NVN_API(nvnCommandBufferSetDebugLabel) { (void)c; }
NVN_API(nvnSyncSetDebugLabel) { (void)c; }
// void nvnCommandBufferPushDebugGroup{Static,Dynamic,}(NVNcommandBuffer *cmd, uint32_t domain_id, const char *text),
// PopDebugGroup(cmd), PopDebugGroupId(cmd, uint32_t id), InsertDebugMarker{Static,Dynamic}(cmd, uint32_t id,
// const char *text), InsertDebugMarker(cmd, const char *text), InsertCheckpoint{Static,Dynamic}(cmd, uint32_t id,
// const char *text)
NVN_API(nvnCommandBufferPushDebugGroupStatic) { (void)c; }
NVN_API(nvnCommandBufferPushDebugGroupDynamic) { (void)c; }
NVN_API(nvnCommandBufferPushDebugGroup) { (void)c; }
NVN_API(nvnCommandBufferPopDebugGroup) { (void)c; }
NVN_API(nvnCommandBufferPopDebugGroupId) { (void)c; }
NVN_API(nvnCommandBufferInsertDebugMarkerStatic) { (void)c; }
NVN_API(nvnCommandBufferInsertDebugMarkerDynamic) { (void)c; }
NVN_API(nvnCommandBufferInsertDebugMarker) { (void)c; }
NVN_API(nvnCommandBufferInsertCheckpointStatic) { (void)c; }
NVN_API(nvnCommandBufferInsertCheckpointDynamic) { (void)c; }
// NVNboolean nvnDeviceIsExternalDebuggerAttached(const NVNdevice *d)
NVN_API(nvnDeviceIsExternalDebuggerAttached) { ret_x(c, 0); }
// void nvnDeviceSetAutomaticCheckpointsMask(NVNdevice *d, uint32_t mask) (NVN 55, no prototype in nvn.h)
NVN_API(nvnDeviceSetAutomaticCheckpointsMask) { (void)c; }

// ---------- timestamps: counters are written in Tegra 19.2 MHz ticks, the base of GetSystemTick (rt_ticks)
static uint64_t ticks_to_ns(uint64_t t) { return t / 12 * 625 + t % 12 * 625 / 12; }
// uint64_t nvnDeviceGetCurrentTimestampInNanoseconds(const NVNdevice *d)
NVN_API(nvnDeviceGetCurrentTimestampInNanoseconds) { ret_x(c, ticks_to_ns(rt_ticks())); }
// uint64_t nvnDeviceGetTimestampInNanoseconds(const NVNdevice *d, const NVNcounterData *counter)
NVN_API(nvnDeviceGetTimestampInNanoseconds) { ret_x(c, ticks_to_ns(((const NVNcounterData *)arg_p(c, 1))->timestamp)); }

// ---------- memory pools and buffers
// size_t nvnMemoryPoolGetSize(const NVNmemoryPool *p)
NVN_API(nvnMemoryPoolGetSize) { ret_x(c, ((const pool_t *)arg_p(c, 0))->size); }
// void nvnMemoryPoolInvalidateMappedRange(const NVNmemoryPool *p, ptrdiff_t offset, size_t size)
NVN_API(nvnMemoryPoolInvalidateMappedRange) {
    int64_t off = (int64_t)arg_x(c, 1);
    if (off >= 0) nvn_pool_invalidate(arg_p(c, 0), (uint64_t)off, arg_x(c, 2));
}
// void nvnBufferInvalidateMappedRange(const NVNbuffer *b, ptrdiff_t offset, size_t size): buffers are pool storage,
// which the GPU writes directly
NVN_API(nvnBufferInvalidateMappedRange) { (void)c; }

// ---------- texture builder and texture queries
static const tex_builder_t *tb_of(const Cpu *c) { return arg_p(c, 0); }
static const tex_t *tex_of(const Cpu *c) { return arg_p(c, 0); }
// void ...GetSwizzle(const T *o, NVNtextureSwizzle *r, *g, *b, *a)
static void get_swizzle(const Cpu *c, const nvn_texdesc *d) {
    *(int32_t *)arg_p(c, 1) = d->swizzle_r;
    *(int32_t *)arg_p(c, 2) = d->swizzle_g;
    *(int32_t *)arg_p(c, 3) = d->swizzle_b;
    *(int32_t *)arg_p(c, 4) = d->swizzle_a;
}
NVN_API(nvnTextureBuilderGetDevice) { ret_x(c, (uintptr_t)tb_of(c)->dev); }
NVN_API(nvnTextureBuilderGetTarget) { ret_x(c, (uint32_t)tb_of(c)->d.target); }
NVN_API(nvnTextureBuilderGetWidth) { ret_x(c, tb_of(c)->d.width); }
NVN_API(nvnTextureBuilderGetHeight) { ret_x(c, tb_of(c)->d.height); }
NVN_API(nvnTextureBuilderGetDepth) { ret_x(c, tb_of(c)->d.depth); }
NVN_API(nvnTextureBuilderGetLevels) { ret_x(c, tb_of(c)->d.levels); }
NVN_API(nvnTextureBuilderGetFormat) { ret_x(c, (uint32_t)tb_of(c)->d.format); }
NVN_API(nvnTextureBuilderGetSamples) { ret_x(c, (uint32_t)tb_of(c)->d.samples); }
NVN_API(nvnTextureBuilderGetSwizzle) { get_swizzle(c, &tb_of(c)->d); }
NVN_API(nvnTextureBuilderGetDepthStencilMode) { ret_x(c, NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH); }
NVN_API(nvnTextureBuilderGetStride) { ret_x(c, tb_of(c)->d.stride); }
NVN_API(nvnTextureBuilderGetMemoryPool) { ret_x(c, (uintptr_t)tb_of(c)->pool); }
NVN_API(nvnTextureBuilderGetMemoryOffset) { ret_x(c, (uint64_t)tb_of(c)->off); }
NVN_API(nvnTextureGetTarget) { ret_x(c, (uint32_t)tex_of(c)->d.target); }
NVN_API(nvnTextureGetLevels) { ret_x(c, tex_of(c)->d.levels); }
NVN_API(nvnTextureGetFormat) { ret_x(c, (uint32_t)tex_of(c)->d.format); }
NVN_API(nvnTextureGetSamples) { ret_x(c, (uint32_t)tex_of(c)->d.samples); }
NVN_API(nvnTextureGetSwizzle) { get_swizzle(c, &tex_of(c)->d); }
NVN_API(nvnTextureGetDepthStencilMode) { ret_x(c, NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH); }
NVN_API(nvnTextureGetStride) { ret_x(c, tex_of(c)->d.stride); }
NVN_API(nvnTextureGetTextureAddress) { ret_x(c, (uintptr_t)tex_of(c)->mem); }
NVN_API(nvnTextureGetMemoryPool) { ret_x(c, (uintptr_t)tex_of(c)->pool); }
NVN_API(nvnTextureGetMemoryOffset) { ret_x(c, (uint64_t)tex_of(c)->off); }
NVN_API(nvnTextureGetDebugID) { ret_x(c, tex_of(c)->serial); }
// ptrdiff_t nvnTextureBuilderGetViewOffset(const NVNtextureBuilder *b, const NVNtextureView *view) (NVN 55)
NVN_API(nvnTextureBuilderGetViewOffset) {
    const tex_builder_t *o = tb_of(c);
    const view_t *w = arg_p(c, 1);
    if (!w) { ret_x(c, 0); return; }
    nvn_texlayout L;
    nvn_tex_layout(&o->d, &L);
    uint64_t off = w->set_levels ? nvn_tex_level_offset(&o->d, w->base_level) : 0;
    if (w->set_layers) off += (uint64_t)w->min_layer * L.layer_size;
    ret_x(c, off);
}
// size_t nvnTextureBuilderGetZCullStorageSize / nvnTextureGetZCullStorageSize: the depth-cull cache needs no storage
NVN_API(nvnTextureBuilderGetZCullStorageSize) { ret_x(c, 0); }
NVN_API(nvnTextureGetZCullStorageSize) { ret_x(c, 0); }

// ---------- window queries
static const win_builder_t *wb_of(const Cpu *c) { return arg_p(c, 0); }
static const win_t *win_of(const Cpu *c) { return arg_p(c, 0); }
NVN_API(nvnWindowBuilderGetDevice) { ret_x(c, (uintptr_t)wb_of(c)->dev); }
NVN_API(nvnWindowBuilderGetNativeWindow) { ret_x(c, (uintptr_t)wb_of(c)->native); }
NVN_API(nvnWindowBuilderGetPresentInterval) { ret_x(c, (uint32_t)wb_of(c)->interval); }
NVN_API(nvnWindowBuilderGetNumTextures) { ret_x(c, (uint32_t)wb_of(c)->ntex); }
// NVNtexture *nvnWindowBuilderGetTexture(const NVNwindowBuilder *b, int i)
NVN_API(nvnWindowBuilderGetTexture) {
    const win_builder_t *b = wb_of(c);
    int i = (int)arg_x(c, 1);
    ret_x(c, i >= 0 && i < b->ntex && i < 4 ? (uintptr_t)b->tex[i] : 0);
}
NVN_API(nvnWindowGetNativeWindow) { ret_x(c, (uintptr_t)win_of(c)->native); }
NVN_API(nvnWindowGetPresentInterval) { ret_x(c, (uint32_t)win_of(c)->interval); }
NVN_API(nvnWindowGetNumTextures) { ret_x(c, (uint32_t)win_of(c)->ntex); }
// void nvnWindowGetCrop(const NVNwindow *w, NVNrectangle *rect): the default crop is the whole image
NVN_API(nvnWindowGetCrop) {
    const win_t *o = win_of(c);
    NVNrectangle *r = arg_p(c, 1);
    if (!o->crop[2] && !o->crop[3] && o->ntex) {
        const tex_t *t = (const tex_t *)o->tex[0];
        *r = (NVNrectangle){0, 0, (int)t->d.width, (int)t->d.height};
        return;
    }
    *r = (NVNrectangle){o->crop[0], o->crop[1], o->crop[2], o->crop[3]};
}

// ---------- texture views
static view_t *view_of(const Cpu *c, int i) { return arg_p(c, i); }
// void nvnTextureViewSetSwizzle(NVNtextureView *v, NVNtextureSwizzle r, g, b, a)
NVN_API(nvnTextureViewSetSwizzle) {
    view_t *v = view_of(c, 0);
    for (int i = 0; i < 4; i++) {
        unsigned s = (unsigned)arg_x(c, 1 + i);
        if (s > 5) rt_fatal("nvn: invalid texture view swizzle %u", s);
        v->swz[i] = (uint8_t)s;
    }
    v->set_swizzle = 1;
}
// void nvnTextureViewSetDepthStencilMode(NVNtextureView *v, NVNtextureDepthStencilMode mode): STENCIL makes the replay
// sample a depth-stencil texture through a stencil-aspect view (vk_exec_draw.c get_view; nxnative hle_nvn.c)
NVN_API(nvnTextureViewSetDepthStencilMode) {
    uint32_t m = (uint32_t)arg_x(c, 1);
    if (m > NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL) rt_fatal("nvn: invalid texture view depth-stencil mode %u", m);
    view_t *v = view_of(c, 0);
    v->set_dsmode = 1;
    v->dsmode = (uint8_t)m;
}
// NVNboolean nvnTextureViewGetLevels(const NVNtextureView *v, int *base, int *n)
NVN_API(nvnTextureViewGetLevels) {
    const view_t *v = view_of(c, 0);
    *(int32_t *)arg_p(c, 1) = v->base_level;
    *(int32_t *)arg_p(c, 2) = v->nlevels;
    ret_x(c, v->set_levels);
}
// NVNboolean nvnTextureViewGetFormat(const NVNtextureView *v, NVNformat *format)
NVN_API(nvnTextureViewGetFormat) {
    const view_t *v = view_of(c, 0);
    *(int32_t *)arg_p(c, 1) = v->format;
    ret_x(c, v->set_format);
}
// NVNboolean nvnTextureViewGetTarget(const NVNtextureView *v, NVNtextureTarget *target)
NVN_API(nvnTextureViewGetTarget) {
    const view_t *v = view_of(c, 0);
    *(int32_t *)arg_p(c, 1) = v->target;
    ret_x(c, v->set_target);
}
// NVNboolean nvnTextureViewGetSwizzle(const NVNtextureView *v, NVNtextureSwizzle *r, *g, *b, *a)
NVN_API(nvnTextureViewGetSwizzle) {
    const view_t *v = view_of(c, 0);
    for (int i = 0; i < 4; i++) *(int32_t *)arg_p(c, 1 + i) = v->swz[i];
    ret_x(c, v->set_swizzle);
}
// NVNboolean nvnTextureViewGetDepthStencilMode(const NVNtextureView *v, NVNtextureDepthStencilMode *mode)
NVN_API(nvnTextureViewGetDepthStencilMode) {
    const view_t *v = view_of(c, 0);
    *(int32_t *)arg_p(c, 1) = v->dsmode;
    ret_x(c, v->set_dsmode);
}
// NVNboolean nvnTextureViewCompare(const NVNtextureView *a, const NVNtextureView *b)
NVN_API(nvnTextureViewCompare) {
    const view_t *x = view_of(c, 0), *y = view_of(c, 1);
    int same = x->set_levels == y->set_levels && x->set_layers == y->set_layers && x->set_format == y->set_format &&
               x->set_target == y->set_target && x->set_swizzle == y->set_swizzle && x->set_dsmode == y->set_dsmode;
    if (same && x->set_levels) same = x->base_level == y->base_level && x->nlevels == y->nlevels;
    if (same && x->set_layers) same = x->min_layer == y->min_layer && x->nlayers == y->nlayers;
    if (same && x->set_format) same = x->format == y->format;
    if (same && x->set_target) same = x->target == y->target;
    if (same && x->set_swizzle) same = !memcmp(x->swz, y->swz, 4);
    if (same && x->set_dsmode) same = x->dsmode == y->dsmode;
    ret_x(c, same);
}

// ---------- sampler queries
static const samp_par *sbp_of(const Cpu *c) { return &((const samp_builder_t *)arg_p(c, 0))->p; }
static const samp_par *sp_of(const Cpu *c) { return &((const samp_t *)arg_p(c, 0))->p; }
// void nvnSamplerBuilderSetBorderColor{i,ui}(NVNsamplerBuilder *b, const int / uint32_t *rgba)
static void border_from_int(const Cpu *c, int is_signed) {
    samp_par *p = &((samp_builder_t *)arg_p(c, 0))->p;
    const int32_t *v = arg_p(c, 1);
    for (int i = 0; i < 4; i++) p->border[i] = is_signed ? (float)v[i] : (float)(uint32_t)v[i];
}
NVN_API(nvnSamplerBuilderSetBorderColori) { border_from_int(c, 1); }
NVN_API(nvnSamplerBuilderSetBorderColorui) { border_from_int(c, 0); }
NVN_API(nvnSamplerBuilderGetDevice) { ret_x(c, (uintptr_t)((const samp_builder_t *)arg_p(c, 0))->dev); }
// Shared shapes: (o, float *lo, float *hi), (o, NVNcompareMode *, NVNcompareFunc *), (o, float / int / uint32_t *rgba)
static void lod_clamp(const Cpu *c, const samp_par *p) {
    *(float *)arg_p(c, 1) = p->lod_min;
    *(float *)arg_p(c, 2) = p->lod_max;
}
static void compare(const Cpu *c, const samp_par *p) {
    *(int32_t *)arg_p(c, 1) = p->cmp_mode;
    *(int32_t *)arg_p(c, 2) = p->cmp_func;
}
static void border_f(const Cpu *c, const samp_par *p) { memcpy(arg_p(c, 1), p->border, 16); }
static void border_i(const Cpu *c, const samp_par *p) {
    for (int i = 0; i < 4; i++) ((int32_t *)arg_p(c, 1))[i] = (int32_t)p->border[i];
}
static void border_ui(const Cpu *c, const samp_par *p) {
    for (int i = 0; i < 4; i++) ((uint32_t *)arg_p(c, 1))[i] = (uint32_t)p->border[i];
}
NVN_API(nvnSamplerBuilderGetLodClamp) { lod_clamp(c, sbp_of(c)); }
NVN_API(nvnSamplerBuilderGetLodBias) { ret_s(c, sbp_of(c)->lod_bias); }
NVN_API(nvnSamplerBuilderGetCompare) { compare(c, sbp_of(c)); }
NVN_API(nvnSamplerBuilderGetBorderColor) { border_f(c, sbp_of(c)); }
NVN_API(nvnSamplerBuilderGetBorderColori) { border_i(c, sbp_of(c)); }
NVN_API(nvnSamplerBuilderGetBorderColorui) { border_ui(c, sbp_of(c)); }
NVN_API(nvnSamplerBuilderGetMaxAnisotropy) { ret_s(c, sbp_of(c)->aniso); }
NVN_API(nvnSamplerBuilderGetLodSnap) { ret_s(c, sbp_of(c)->lod_snap); }
// void nvnSamplerGetMinMagFilter(const NVNsampler *s, NVNminFilter *min, NVNmagFilter *mag)
NVN_API(nvnSamplerGetMinMagFilter) {
    const samp_par *p = sp_of(c);
    *(int32_t *)arg_p(c, 1) = p->minf;
    *(int32_t *)arg_p(c, 2) = p->magf;
}
// void nvnSamplerGetWrapMode(const NVNsampler *s, NVNwrapMode *s, *t, *r)
NVN_API(nvnSamplerGetWrapMode) {
    const samp_par *p = sp_of(c);
    *(int32_t *)arg_p(c, 1) = p->ws;
    *(int32_t *)arg_p(c, 2) = p->wt;
    *(int32_t *)arg_p(c, 3) = p->wr;
}
NVN_API(nvnSamplerGetLodClamp) { lod_clamp(c, sp_of(c)); }
NVN_API(nvnSamplerGetLodBias) { ret_s(c, sp_of(c)->lod_bias); }
NVN_API(nvnSamplerGetCompare) { compare(c, sp_of(c)); }
NVN_API(nvnSamplerGetBorderColor) { border_f(c, sp_of(c)); }
NVN_API(nvnSamplerGetBorderColori) { border_i(c, sp_of(c)); }
NVN_API(nvnSamplerGetBorderColorui) { border_ui(c, sp_of(c)); }
NVN_API(nvnSamplerGetMaxAnisotropy) { ret_s(c, sp_of(c)->aniso); }
// NVNboolean nvnSamplerCompare(const NVNsampler *a, const NVNsampler *b)
NVN_API(nvnSamplerCompare) { ret_x(c, !memcmp(sp_of(c), &((const samp_t *)arg_p(c, 1))->p, sizeof(samp_par))); }
// uint64_t nvnSamplerGetDebugID(const NVNsampler *s)
NVN_API(nvnSamplerGetDebugID) { ret_x(c, arg_x(c, 0)); }

// ---------- fixed-function state objects: setters without a host equivalent store the value (the replay reports any
// non-default use); getters return what was set
#define ST(T) ((T *)arg_p(c, 0))
// void nvnColorStateSetLogicOp(NVNcolorState *s, NVNlogicOp op) / SetAlphaTest(NVNcolorState *s, NVNalphaFunc f)
NVN_API(nvnColorStateSetLogicOp) {
    unsigned op = (unsigned)arg_x(c, 1);
    if (op > 15) rt_fatal("nvn: logic op %u", op);
    ST(color_st)->logic_op = op;
}
NVN_API(nvnColorStateSetAlphaTest) {
    unsigned f = (unsigned)arg_x(c, 1);
    if (f > 15) rt_fatal("nvn: alpha func %u", f);
    ST(color_st)->alpha_func = f;
}
// NVNboolean nvnColorStateGetBlendEnable(const NVNcolorState *s, int index)
NVN_API(nvnColorStateGetBlendEnable) {
    int i = (int)arg_x(c, 1);
    ret_x(c, i >= 0 && i < 8 && (ST(color_st)->blend_en >> i & 1));
}
NVN_API(nvnColorStateGetLogicOp) { ret_x(c, ST(color_st)->logic_op); }
NVN_API(nvnColorStateGetAlphaTest) { ret_x(c, ST(color_st)->alpha_func); }
// void nvnChannelMaskStateGetChannelMask(const NVNchannelMaskState *s, int index, NVNboolean *r, *g, *b, *a)
NVN_API(nvnChannelMaskStateGetChannelMask) {
    int i = (int)arg_x(c, 1);
    uint32_t m = i >= 0 && i < 8 ? ST(chmask_st)->mask >> (4 * i) : 0;
    for (int k = 0; k < 4; k++) *(NVNboolean *)arg_p(c, 2 + k) = (NVNboolean)(m >> k & 1);
}
NVN_API(nvnBlendStateGetBlendTarget) { ret_x(c, ST(blend_st)->target); }
// void nvnBlendStateGetBlendFunc(const NVNblendState *s, NVNblendFunc *src_rgb, *dst_rgb, *src_a, *dst_a)
NVN_API(nvnBlendStateGetBlendFunc) {
    const blend_st *b = ST(blend_st);
    *(int32_t *)arg_p(c, 1) = b->src_rgb;
    *(int32_t *)arg_p(c, 2) = b->dst_rgb;
    *(int32_t *)arg_p(c, 3) = b->src_a;
    *(int32_t *)arg_p(c, 4) = b->dst_a;
}
// void nvnBlendStateGetBlendEquation(const NVNblendState *s, NVNblendEquation *rgb, NVNblendEquation *alpha)
NVN_API(nvnBlendStateGetBlendEquation) {
    *(int32_t *)arg_p(c, 1) = ST(blend_st)->eq_rgb;
    *(int32_t *)arg_p(c, 2) = ST(blend_st)->eq_a;
}
// void nvnBlendStateSetAdvancedMode(NVNblendState *s, NVNblendAdvancedMode mode): only "none" is implemented; overlap,
// premultiplied source and normalized destination only matter with an advanced mode
NVN_API(nvnBlendStateSetAdvancedMode) {
    unsigned m = (unsigned)arg_x(c, 1);
    if (m) rt_fatal("nvn: advanced blend mode %u is not implemented", m);
}
NVN_API(nvnBlendStateSetAdvancedOverlap) { (void)c; }
NVN_API(nvnBlendStateSetAdvancedPremultipliedSrc) { (void)c; }
NVN_API(nvnBlendStateSetAdvancedNormalizedDst) { (void)c; }
NVN_API(nvnMultisampleStateGetMultisampleEnable) { ret_x(c, ST(ms_st)->enable); }
NVN_API(nvnMultisampleStateGetSamples) { ret_x(c, ST(ms_st)->samples); }
NVN_API(nvnMultisampleStateGetAlphaToCoverageEnable) { ret_x(c, ST(ms_st)->a2c); }
NVN_API(nvnMultisampleStateGetRasterSamples) { ret_x(c, ST(ms_st)->raster_samples); }
// void nvnMultisampleStateSetRasterSamples(NVNmultisampleState *s, int n)
NVN_API(nvnMultisampleStateSetRasterSamples) {
    int n = (int)arg_x(c, 1);
    if (n < 0 || n > 16) rt_fatal("nvn: raster samples %d", n);
    ST(ms_st)->raster_samples = (uint8_t)n;
}
// void nvnMultisampleStateSetAlphaToCoverageDither(NVNmultisampleState *s, NVNboolean enable)
NVN_API(nvnMultisampleStateSetAlphaToCoverageDither) { ST(ms_st)->a2c_dither = nvn_arg_bool(c, 1) != 0; }
NVN_API(nvnPolygonStateGetCullFace) { ret_x(c, ST(poly_st)->cull); }
NVN_API(nvnPolygonStateGetFrontFace) { ret_x(c, ST(poly_st)->front); }
NVN_API(nvnPolygonStateGetPolygonMode) { ret_x(c, ST(poly_st)->mode); }
NVN_API(nvnPolygonStateGetPolygonOffsetEnables) { ret_x(c, ST(poly_st)->offset_en); }
NVN_API(nvnDepthStencilStateGetDepthTestEnable) { ret_x(c, ST(ds_st)->dt); }
NVN_API(nvnDepthStencilStateGetDepthWriteEnable) { ret_x(c, ST(ds_st)->dw); }
NVN_API(nvnDepthStencilStateGetDepthFunc) { ret_x(c, ST(ds_st)->df); }
NVN_API(nvnDepthStencilStateGetStencilTestEnable) { ret_x(c, ST(ds_st)->st); }
// NVNstencilFunc nvnDepthStencilStateGetStencilFunc(const NVNdepthStencilState *s, NVNface face)
NVN_API(nvnDepthStencilStateGetStencilFunc) {
    const ds_st *d = ST(ds_st);
    ret_x(c, (uint32_t)arg_x(c, 1) == NVN_FACE_BACK ? d->sf_b : d->sf_f);
}
// void nvnDepthStencilStateGetStencilOp(const NVNdepthStencilState *s, NVNface face, NVNstencilOp *fail,
// NVNstencilOp *depth_fail, NVNstencilOp *depth_pass)
NVN_API(nvnDepthStencilStateGetStencilOp) {
    const ds_st *d = ST(ds_st);
    int back = (uint32_t)arg_x(c, 1) == NVN_FACE_BACK;
    *(int32_t *)arg_p(c, 2) = (int32_t)(back ? d->fail_b : d->fail_f);
    *(int32_t *)arg_p(c, 3) = (int32_t)(back ? d->zfail_b : d->zfail_f);
    *(int32_t *)arg_p(c, 4) = (int32_t)(back ? d->zpass_b : d->zpass_f);
}
// void nvnVertexAttribStateGetFormat(const NVNvertexAttribState *s, NVNformat *format, ptrdiff_t *offset)
NVN_API(nvnVertexAttribStateGetFormat) {
    *(int32_t *)arg_p(c, 1) = ST(va_st)->fmt;
    *(int64_t *)arg_p(c, 2) = ST(va_st)->offset;
}
NVN_API(nvnVertexAttribStateGetStreamIndex) { ret_x(c, ST(va_st)->stream); }
NVN_API(nvnVertexStreamStateGetStride) { ret_x(c, ST(vs_st)->stride); }
NVN_API(nvnVertexStreamStateGetDivisor) { ret_x(c, ST(vs_st)->divisor); }
#undef ST

// ---------- rasterisation state without a host equivalent: lines are 1 pixel wide and point size comes from the vertex
// shader. Non-default values are reported once; the drawing proceeds without them.
#define ONCE(...) do { static int once_; if (!once_++) rt_log("nvn: " __VA_ARGS__); } while (0)
// void nvnCommandBufferSetLineWidth / SetPointSize(NVNcommandBuffer *cmd, float value)
// SetLineWidth is recorded in nvn_api_cmd.c (OP_BIND_STATE kind 7).
// SetPointSize is recorded in nvn_api_cmd.c (OP_BIND_STATE kind 6): the replay feeds it to the vertex shaders through the
// support buffer (translator patch 0008).
// void nvnCommandBufferSetConservativeRasterEnable(NVNcommandBuffer *cmd, NVNboolean enable)
NVN_API(nvnCommandBufferSetConservativeRasterEnable) {
    if (nvn_arg_bool(c, 1)) ONCE("conservative rasterisation requested: drawn normally");
}
// void nvnCommandBufferSetConservativeRasterDilate(NVNcommandBuffer *cmd, float dilate)
NVN_API(nvnCommandBufferSetConservativeRasterDilate) { (void)c; }
// void nvnCommandBufferSetSampleMask(NVNcommandBuffer *cmd, int mask)
NVN_API(nvnCommandBufferSetSampleMask) {
    int m = (int)arg_x(c, 1);
    if ((m & 0xffff) != 0xffff) ONCE("SetSampleMask %#x: fixed-function sample mask not applied", m);
}
// void nvnCommandBufferSetAlphaRef(NVNcommandBuffer *cmd, float ref): used only with the alpha test, which the replay
// reports
NVN_API(nvnCommandBufferSetAlphaRef) { (void)c; }
// void nvnCommandBufferSetDepthBounds(NVNcommandBuffer *cmd, NVNboolean enable, float min, float max): disabling is
// exact; an enabled range is drawn without the test and reported
NVN_API(nvnCommandBufferSetDepthBounds) {
    float lo = arg_s(c, 0), hi = arg_s(c, 1);
    if (nvn_arg_bool(c, 1) && (lo > 0.0f || hi < 1.0f))
        ONCE("SetDepthBounds [%g, %g]: depth-bounds test not applied (fragments outside the range are kept)", lo, hi);
}
// void nvnCommandBufferSetProvokingVertexMode(NVNcommandBuffer *cmd, int mode) (NVN 55): the NVN default is the last
// vertex (GL convention); only the default is accepted
NVN_API(nvnCommandBufferSetProvokingVertexMode) {
    int mode = (int)arg_x(c, 1);
    if (mode != 0) ONCE("SetProvokingVertexMode %d: provoking-vertex conversion not implemented", mode);
}
#undef ONCE

// ---------- Tegra tiled-cache, ZCull and rasterisation hints
NVN_API(nvnCommandBufferSetTiledCacheAction) { (void)c; }          // tiled-cache flush / enable hint
NVN_API(nvnCommandBufferSetTiledCacheTileSize) { (void)c; }
NVN_API(nvnCommandBufferSetSubpixelPrecisionBias) { (void)c; }
NVN_API(nvnCommandBufferDiscardColor) { (void)c; }                 // contents become undefined: keeping them is valid
NVN_API(nvnCommandBufferDiscardDepthStencil) { (void)c; }
NVN_API(nvnCommandBufferSaveZCullData) { (void)c; }                // no depth-cull cache: nothing to save or restore
NVN_API(nvnCommandBufferRestoreZCullData) { (void)c; }
NVN_API(nvnCommandBufferOverrideSubtileSize) { (void)c; }          // NVN 55 hints, no prototype in nvn.h
NVN_API(nvnCommandBufferSetZCullZF32CompressionEnable) { (void)c; }
// nvnProgramGetSubtileSize (NVN 55): its signature is not identified
NVN_API(nvnProgramGetSubtileSize) { (void)c; rt_fatal("nvn: nvnProgramGetSubtileSize: signature not identified"); }
