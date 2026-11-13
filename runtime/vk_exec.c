// The op-stream replay core of the Vulkan back end: walks the command handles of
// one submission into its command buffer, keeps render targets and fixed-function state, opens and closes dynamic
// rendering passes and records clears and barriers. Draws, binds and dispatches go to vk_exec_op_draw
// (vk_exec_draw.c); copies, counters, events and conditional rendering to vk_exec_op_xfer (vk_exec_xfer.c). Images
// stay in VK_IMAGE_LAYOUT_GENERAL (vk_texture.c); full memory barriers order passes, clears and whatever those two
// record between them.
//
// The op loop (END / BEGIN / JUMP / CALL, which ops are pure state, the state defaults, render targets kept until they
// change, clears skipped while rendering is disabled) follows MacNativeGames
// games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m:10588-10700, 7409-7460, 5449-5457
// (https://github.com/ly05010419/MacNativeGames @ b75be8cfa704df3701e86130c8ced268e79d6b11, MIT, Copyright (c) 2026
// jhaizhou). Pass begin and end (GENERAL-layout attachments, LOAD/STORE, render area from the smallest attachment, a
// full barrier before each pass), the scissor clip and attachment clears follow LAS Recomp
// runtime/nvn/vulkan/exec.c:855-1149, 3553-3778 (https://github.com/peiikai/LASRecomp @
// da35b83a9611d046a9a681aae668d7085954d37f, MIT, Copyright (c) 2026 Peiikai).
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_tex.h"
#include "platform/platform.h"
#include "tex.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_exec.h"

#define WEAK __attribute__((weak))
#define CALL_DEPTH 8

typedef struct {
    VkImage image;
    VkFormat format;
    VkImageViewType type;
    VkImageAspectFlags aspect;
    uint32_t level, base, layers;
    VkImageView view;
} ViewEnt;

static VkExec X;
static int x_ready;
static _Thread_local VkExec *x_cur;   // the context this thread records with (vk_exec_inline_begin), NULL outside

// ---- TOTK_TRACE=gfx frame counters

VkExecFrame vk_exec_frame;
static struct { const uint8_t *mem; uint32_t w, h, fmt, n; } fw[64];   // textures written this frame
static int nfw;
static PlatMutex fw_lock = PLAT_MUTEX_INIT;   // the recording threads of a chunked replay

uint64_t vk_exec_recording_fence(void) { return x_cur ? x_cur->fence : 0; }
uint32_t vk_exec_replay_pos(void) { return x_cur && x_cur->chunked ? x_cur->pos : UINT32_MAX; }
static _Thread_local int chunk_k = -1;   // the chunk this thread records (chunk_record), -1 none


// A context's counters go into vk_exec_frame (the submitting thread, after the recording).
static void frame_add(VkExec *ex) {
    uint64_t *d = (uint64_t *)&vk_exec_frame, *s = (uint64_t *)&ex->fr;
    for (size_t i = 0; i < sizeof ex->fr / sizeof *s; i++) d[i] += s[i];
    memset(&ex->fr, 0, sizeof ex->fr);
}

void vk_exec_frame_written(const tex_t *t) {
    if (!(rt_trace & RT_TRACE_GFX) || !t) return;
    plat_mutex_lock(&fw_lock);
    int i = 0;
    while (i < nfw && fw[i].mem != t->mem) i++;
    if (i < nfw) fw[i].n++;
    else if (nfw < 64) {
        fw[nfw].mem = t->mem, fw[nfw].w = t->d.width, fw[nfw].h = t->d.height, fw[nfw].fmt = (uint32_t)t->d.format;
        fw[nfw++].n = 1;
    }
    plat_mutex_unlock(&fw_lock);
}

void vk_exec_frame_log(const tex_t *p, uint64_t frame) {
    if (!(rt_trace & RT_TRACE_GFX)) return;
    VkExecFrame *f = &vk_exec_frame;
    int hit = 0;
    char list[512] = "";
    size_t o = 0;
    for (int i = 0; i < nfw; i++) {
        if (p && fw[i].mem == p->mem) hit = 1;
        if (o < sizeof list - 48)
            o += (size_t)snprintf(list + o, sizeof list - o, " %p:%ux%u/%#x*%u", (const void *)fw[i].mem, fw[i].w,
                                  fw[i].h, fw[i].fmt, fw[i].n);
    }
    rt_log("frame %llu: submits %llu passes %llu draws %llu/%llu clears %llu copies %llu dispatches %llu; present %p "
           "%ux%u/%#x written %d; feedback draws %llu barriers %llu restarts %llu layouts %llu; targets%s",
           (unsigned long long)frame, (unsigned long long)f->submits,
           (unsigned long long)f->passes, (unsigned long long)f->draws, (unsigned long long)f->draws_seen,
           (unsigned long long)f->clears, (unsigned long long)f->copies, (unsigned long long)f->dispatches,
           p ? (const void *)p->mem : NULL, p ? p->d.width : 0, p ? p->d.height : 0, p ? (uint32_t)p->d.format : 0,
           hit, (unsigned long long)f->fb_draws, (unsigned long long)f->fb_barriers, (unsigned long long)f->fb_restarts,
           (unsigned long long)f->fb_layouts, list);
    memset(f, 0, sizeof *f);
    nfw = 0;
}

WEAK void vk_exec_ub_flush(VkExec *ex) { (void)ex; }
WEAK void vk_exec_ub_foreign(VkExec *ex, uint64_t addr, uint64_t n) { (void)ex, (void)addr, (void)n; }
WEAK void vk_exec_ub_perf(void) {}

WEAK void vk_exec_op_draw(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex, (void)payload;
    rt_fatal("vk: op %s is not replayed yet", nvn_op_name(op));
}

WEAK void vk_exec_op_xfer(VkExec *ex, uint32_t op, const void *payload) {
    (void)ex, (void)payload;
    rt_fatal("vk: op %s is not replayed yet", nvn_op_name(op));
}

enum { W_UNRENDERABLE, W_LEVEL, W_LAYER, W_VIEW_FORMAT, W_BLOCK_VIEW, W_BLOCK_LAYERS, W_CLEAR_TARGET, W_CLEAR_MASK, W_STENCIL_MASK, W_COUNT };

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

// NVN defaults of the replay state (nvn_mtl.m).
static void state_reset(VkExec *ex) {
    memset(ex, 0, sizeof *ex);
    ex->st_vmask[0] = ex->st_vmask[1] = ex->st_wmask[0] = ex->st_wmask[1] = 0xff;
    ex->depth_range[1] = 1.0f;
    vk_pipe_state_defaults(&ex->pipe);
    ex->barrier_at = -1;
    ex->oq_open = -1;
}

// ---- render targets

static uint32_t level_extent(uint32_t v, uint32_t level) {
    v >>= level;
    return v ? v : 1;
}

// A view format other than the texture's is used when both map directly and have the same texel size (the image is
// created MUTABLE_FORMAT for colour formats). blocks: t is a BC texture with block_views, and the view an uncompressed
// format whose texel is one of its blocks.
static int view_format(const tex_t *t, const view_t *v, int blocks, VkFormat *out) {
    TexVkFormat a, b;
    int bw, bh, vbw, vbh;
    if (!tex_vk_format(t->d.format, &a) || a.vk != a.ideal || !tex_vk_format(v->format, &b) || b.vk != b.ideal ||
        b.depth || b.stencil)
        return 0;
    int bpb = nvn_fmt_bpb(t->d.format, &bw, &bh), vbpb = nvn_fmt_bpb(v->format, &vbw, &vbh);
    if (!bpb || bpb != vbpb || (!blocks && (bw != 1 || bh != 1)) || vbw != 1 || vbh != 1) return 0;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys, b.vk, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return 0;
    *out = b.vk;
    return 1;
}

// ---- 3D BC render targets
// A 3D BC texture has no block views (vk_texture.c create_tex, TOTK_BLOCK_VIEWS_3D): on RDNA2 cards such an image
// made BLOCK_TEXEL_VIEW_COMPATIBLE and 2D_ARRAY_COMPATIBLE took GPU writes past the end of its memory (issue #5). Draws
// into it through an uncompressed view of its blocks (agl's BC4 encoder fills 32x32x32 and 64x64x64 volumes and their
// levels at the logo) go to a stand-in: a 2D array of the blocks of one level in the view's format, one layer per
// depth plane. It starts with the level's content, so a pass that writes part of a plane keeps the rest, and each pass
// end copies the planes it drew into the level (a copy between a compressed and a size-compatible uncompressed format).
typedef struct { VkImage bc, img; VmaAllocation alloc; VkFormat fmt; uint32_t level, w, h, d; } BcStandIn;
#define BC_STANDINS 64   // one per level drawn: 6 volumes of up to 7 levels at the logo
static BcStandIn bcs[BC_STANDINS];
static int nbcs;
static PlatMutex bcs_lock = PLAT_MUTEX_INIT;   // the replay adds and reads; destroy_tex (any thread) removes

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, uint32_t layers) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = from, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
}

static void memory_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                           VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// The stand-in of level level of a 3D BC image for draws through view format fmt, made on first use; 0 when none can
// be made (logged). Replay thread.
static int bc_standin(const VkTexImage *img, uint32_t level, VkFormat fmt, BcStandIn *out) {
    static int warned;
    if (img->gone) return 0;   // dropped since the lookup: a stand-in kept by its handle would outlive it
    plat_mutex_lock(&bcs_lock);
    int full = nbcs == BC_STANDINS;
    for (int i = 0; i < nbcs; i++)
        if (bcs[i].bc == img->image && bcs[i].level == level && bcs[i].fmt == fmt) {
            *out = bcs[i];
            plat_mutex_unlock(&bcs_lock);
            return 1;
        }
    plat_mutex_unlock(&bcs_lock);
    if (full) {
        if (!warned++) rt_log("vk: more than %d 3D BC render targets: the others are left out", BC_STANDINS);
        return 0;
    }
    uint32_t lw = level_extent(img->width, level), lh = level_extent(img->height, level);
    BcStandIn s = {img->image, VK_NULL_HANDLE, NULL, fmt, level, (lw + 3) / 4, (lh + 3) / 4,
                   level_extent(img->depth, level)};
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {s.w, s.h, 1}, .mipLevels = 1, .arrayLayers = s.d, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(vk_ctx()->vma, &ci, &ai, &s.img, &s.alloc, NULL) != VK_SUCCESS) {
        if (!warned++) rt_log("vk: stand-in for a 3D BC render target could not be created: left out");
        return 0;
    }
    // In the upload batch, which goes to the queue before this part: GENERAL, then the texture's blocks.
    VkCommandBuffer cb = vk_upload_begin();
    image_barrier(cb, s.img, VK_IMAGE_LAYOUT_UNDEFINED, s.d);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, s.d},
                      .extent = {lw, lh, s.d}};   // texels of the compressed source
    vkCmdCopyImage(cb, img->image, VK_IMAGE_LAYOUT_GENERAL, s.img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    memory_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                   VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    vk_upload_end();
    plat_mutex_lock(&bcs_lock);
    bcs[nbcs++] = s;
    plat_mutex_unlock(&bcs_lock);
    rt_log("vk: 3D BC render target %ux%ux%u level %u drawn through a %ux%u x %u stand-in (format %d)", img->width,
           img->height, img->depth, level, s.w, s.h, s.d, (int)fmt);
    *out = s;
    return 1;
}

// vk_exec_draw.c vk_exec_image_gone: the stand-in of a BC image that goes, freed once the GPU is done with it.
void vk_exec_bc_gone(VkImage image) {
    plat_mutex_lock(&bcs_lock);
    for (int i = 0; i < nbcs; i++)
        if (bcs[i].bc == image) {
            vk_defer_image(bcs[i].img, bcs[i].alloc);
            bcs[i--] = bcs[--nbcs];
        }
    plat_mutex_unlock(&bcs_lock);
}

// At a pass end: the planes the pass drew into a stand-in go to its texture.
static void bc_copy_back(VkExec *ex, const VkExecTarget *t) {
    plat_mutex_lock(&bcs_lock);
    for (int i = 0; i < nbcs; i++) {
        if (bcs[i].img != t->image) continue;
        memory_barrier(ex->cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t->base_layer, t->layers},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, bcs[i].level, 0, 1},
                          .dstOffset = {0, 0, (int32_t)t->base_layer},
                          .extent = {bcs[i].w, bcs[i].h, t->layers}};   // texels of the uncompressed source
        vkCmdCopyImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, bcs[i].bc, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        memory_barrier(ex->cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        ex->recorded++;
        break;
    }
    plat_mutex_unlock(&bcs_lock);
}

// A draw into a 3D BC image without block views through an uncompressed view of its blocks: the target is the
// image's stand-in (bc_standin). 1 when o is filled (target or left out), 0 for any other target.
static int bc_target(const tex_t *t, const view_t *v, int has_v, const VkTexImage *img, VkExecTarget *o, int peek) {
    int bw, bh;
    if (img->type != VK_IMAGE_TYPE_3D || img->block_views || !nvn_fmt_bpb(t->d.format, &bw, &bh) || bw != 4 || bh != 4)
        return 0;
    if (peek) return 1;   // the scan makes no stand-in: left out
    VkFormat fmt;
    BcStandIn s;
    uint32_t level = has_v && v->set_levels ? v->base_level : 0;
    if (level >= img->levels) {
        warn_once(W_LEVEL, "render target level %u of a %u-level texture: left out", level, img->levels);
        return 1;
    }
    if (!has_v || !v->set_format || !view_format(t, v, 1, &fmt) || !bc_standin(img, level, fmt, &s)) {
        warn_once(W_BLOCK_VIEW, "render target format %#x (%ux%u) through view format %#x: no block view, left out",
                  t->d.format, t->d.width, t->d.height, has_v && v->set_format ? v->format : 0);
        return 1;
    }
    uint32_t base = 0, layers = s.d;
    if (v->set_layers) {
        base = v->min_layer;
        layers = v->nlayers ? v->nlayers : (base < s.d ? s.d - base : 0);
    }
    if (base >= s.d || !layers) return 1;
    if (base + layers > s.d) layers = s.d - base;
    o->image = s.img;   // not img->image: view_of makes this command buffer's view of the stand-in
    o->img = img;
    o->format = fmt;
    o->view_type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    o->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    o->level = 0, o->base_layer = base, o->layers = layers;
    o->width = s.w, o->height = s.h, o->samples = 1;
    o->nvn_format = v->format;
    return 1;
}

enum { HOLD_TEX, HOLD_DLSS, HOLD_WRITES, HOLD_N };
static void scan_hold(int why);

// scan: the state scan's resolution, which creates nothing and leaves out a texture without an image yet.
static void resolve_target(const tex_t *t, const view_t *v, int has_v, int depth_slot, VkExecTarget *o, const VkExec *scan) {
    memset(o, 0, sizeof *o);
    o->tex = t;
    if (!t) return;
    const VkTexImage *img = scan ? vk_texture_revive(t, scan->pos) : vk_texture_get(t);
    if (scan && !img) {
        if (t->mem) scan_hold(HOLD_TEX);
        return;
    }
    if (img && !depth_slot && bc_target(t, v, has_v, img, o, scan != NULL)) return;   // a 3D BC image: its stand-in
    VkImageUsageFlags need = depth_slot ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    int is3d = img && img->type == VK_IMAGE_TYPE_3D && !depth_slot;   // slices drawn through 2D views (2D_ARRAY_COMPATIBLE)
    if (!img || !(img->usage & need) || (img->type != VK_IMAGE_TYPE_2D && !is3d)) {
        warn_once(W_UNRENDERABLE, "render target format %#x target %d (%ux%u) is not renderable here: left out",
                  t->d.format, t->d.target, t->d.width, t->d.height);
        return;
    }
    uint32_t level = has_v && v->set_levels ? v->base_level : 0;
    if (level >= img->levels) {
        warn_once(W_LEVEL, "render target level %u of a %u-level texture: left out", level, img->levels);
        return;
    }
    // A 3D level's depth planes take the place of layers (Vulkan views them as the array layers of a 2D array view).
    uint32_t nlay = is3d ? level_extent(img->depth, level) : img->layers;
    uint32_t base = 0, layers = nlay;
    if (has_v && v->set_layers) {
        base = v->min_layer;
        layers = v->nlayers ? v->nlayers : (base < nlay ? nlay - base : 0);
    }
    if (base >= nlay || !layers) {
        warn_once(W_LAYER, "render target layer %u of a %u-layer texture: left out", base, nlay);
        return;
    }
    if (base + layers > nlay) layers = nlay - base;
    VkFormat fmt = img->format;
    int32_t nvn_fmt = t->d.format;
    int blocks = !depth_slot && img->block_views;
    if (blocks) {   // a BC texture takes draws only through an uncompressed view of its blocks
        if (!has_v || !v->set_format || !view_format(t, v, 1, &fmt)) {
            warn_once(W_BLOCK_VIEW, "render target format %#x (%ux%u) through view format %#x: no block view, left out",
                      t->d.format, t->d.width, t->d.height, has_v && v->set_format ? v->format : 0);
            return;
        }
        nvn_fmt = v->format;
        if (layers > 1) {   // an uncompressed view of a BC image has one layer (no maintenance6 here)
            warn_once(W_BLOCK_LAYERS, "render target format %#x: block view of %u layers, first layer only", t->d.format,
                      layers);
            layers = 1;
        }
    } else if (!depth_slot && has_v && v->set_format && v->format != t->d.format) {
        if (view_format(t, v, 0, &fmt)) nvn_fmt = v->format;
        else warn_once(W_VIEW_FORMAT, "render target view format %#x of a format %#x texture: texture format used",
                       v->format, t->d.format);
    }
    o->image = img->image;
    o->img = img;
    o->scale = img->scale ? img->scale : 1;
    o->format = fmt;
    o->view_type = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    o->aspect = depth_slot ? img->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                           : VK_IMAGE_ASPECT_COLOR_BIT;
    o->level = level, o->base_layer = base, o->layers = layers;
    o->is3d = (uint8_t)is3d;
    o->blocks = (uint8_t)blocks;
    o->width = level_extent(img->width, level), o->height = level_extent(img->height, level);
    if (blocks) o->width = (o->width + 3) / 4, o->height = (o->height + 3) / 4;   // the view's extent: 4x4 blocks
    o->samples = img->samples ? img->samples : 1;
    o->nvn_format = nvn_fmt;
    if (!o->aspect) o->image = VK_NULL_HANDLE;
}

/* The size of a pass without attachments: the far corner of viewport 0, else of the scissor, 1..16384, as
   MacNativeGames and nxnative size it. TotK draws a fragment shader that only writes a storage buffer into a 1x1
   viewport; a pass sized by the viewport's width alone puts an offset viewport outside the scissor, and the cave rock
   loses its ambient light. */
void vk_exec_noatt_dims(const VkExec *ex, uint32_t *w, uint32_t *h) {
    double x1 = 0, y1 = 0;
    if (ex->has_vp) {
        x1 = ceil(fmax(ex->vp[0][0], ex->vp[0][0] + ex->vp[0][2]));
        y1 = ceil(fmax(ex->vp[0][1], ex->vp[0][1] + ex->vp[0][3]));
    }
    if ((x1 < 1 || y1 < 1) && ex->has_scissor) x1 = (double)ex->scissor.x + ex->scissor.w, y1 = (double)ex->scissor.y + ex->scissor.h;
    *w = x1 < 1 ? 1 : x1 > 16384 ? 16384 : (uint32_t)x1;
    *h = y1 < 1 ? 1 : y1 > 16384 ? 16384 : (uint32_t)y1;
}

// Resolves ex->rt to this command buffer's images and derives the pass parameters and the pipeline's target formats.
static void resolve_rt_as(VkExec *ex, int peek) {
    if (ex->rt_resolved) return;
    ex->rt_resolved = 1;
    const p_set_rt *r = &ex->rt;
    int n = r->n < 0 ? 0 : r->n > 8 ? 8 : r->n;
    uint32_t w = UINT32_MAX, h = UINT32_MAX, layers = UINT32_MAX, samples = 0, scale = 1;
    for (int i = 0; i < 8; i++) {
        VkExecTarget *t = &ex->color[i];
        resolve_target(i < n ? r->color[i] : NULL, &r->cview[i], i < n && r->has_cview[i], 0, t, peek ? ex : NULL);
        ex->pass_color[i] = t->image ? t->format : VK_FORMAT_UNDEFINED;
        ex->pipe.color_fmt[i] = t->image ? t->nvn_format : 0;
        if (!t->image) continue;
        if (t->width < w) w = t->width;
        if (t->height < h) h = t->height;
        if (t->layers < layers) layers = t->layers;
        if (!samples) samples = t->samples;
        if (t->scale > scale) scale = t->scale;
    }
    VkExecTarget *d = &ex->depth;
    resolve_target(r->depth, &r->dview, r->has_dview, 1, d, peek ? ex : NULL);
    ex->pass_depth = d->image && (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? d->format : VK_FORMAT_UNDEFINED;
    ex->pass_stencil = d->image && (d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? d->format : VK_FORMAT_UNDEFINED;
    ex->pipe.depth_fmt = d->image ? d->nvn_format : 0;
    if (d->image) {
        if (d->width < w) w = d->width;
        if (d->height < h) h = d->height;
        if (d->layers < layers) layers = d->layers;
        if (!samples) samples = d->samples;
        if (d->scale > scale) scale = d->scale;
    }
    ex->pass_noatt = w == UINT32_MAX;
    if (ex->pass_noatt) {
        vk_exec_noatt_dims(ex, &w, &h);
        layers = 1;
        static int logged;
        if (!peek && logged < 8 && ex->has_vp && (ex->vp[0][0] != 0 || ex->vp[0][1] != 0 || ex->vp[0][2] != w || ex->vp[0][3] != h))
            logged++, rt_log("vk_exec: pass without attachments %ux%u for viewport %g,%g %gx%g", w, h, ex->vp[0][0],
                             ex->vp[0][1], ex->vp[0][2], ex->vp[0][3]);
    }
    ex->pass_w = w, ex->pass_h = h, ex->pass_layers = layers;
    ex->pass_scale = ex->pass_noatt ? 1 : scale;
    ex->pass_ncolor = (uint32_t)n;
    ex->pass_samples = samples ? samples : 1;
    ex->pipe.samples = (uint8_t)ex->pass_samples;
}

static void resolve_rt(VkExec *ex) { resolve_rt_as(ex, 0); }

// The attachment view of a target. A registered texture's view lives as long as its image (vk_exec_target_view); any
// other image's view is made for this command buffer and freed at its fence.
static VkImageView view_of(VkExec *ex, const VkExecTarget *t) {
    if (t->img && t->img->image == t->image)
        return vk_exec_target_view(t->img, t->format, t->view_type, t->aspect, t->level, t->base_layer, t->layers, t->blocks);
    ViewEnt *v = ex->views;
    for (int i = 0; i < ex->nviews; i++)
        if (v[i].image == t->image && v[i].format == t->format && v[i].type == t->view_type && v[i].aspect == t->aspect &&
            v[i].level == t->level && v[i].base == t->base_layer && v[i].layers == t->layers)
            return v[i].view;
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->image,
                                .viewType = t->view_type, .format = t->format,
                                .subresourceRange = {t->aspect, t->level, 1, t->base_layer, t->layers}};
    VkImageViewUsageCreateInfo use = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    if (t->blocks) ci.pNext = &use;   // not the BC image's sampling and transfer usages
    VkImageView view;
    if (vkCreateImageView(vk_ctx()->device, &ci, NULL, &view) != VK_SUCCESS)
        rt_fatal("vk: render target view (format %d, %ux%u) could not be created", (int)t->format, t->width, t->height);
    if (ex->nviews == ex->cap_views) {
        ex->cap_views = ex->cap_views ? ex->cap_views * 2 : 16;
        ex->views = realloc(ex->views, sizeof(ViewEnt) * (size_t)ex->cap_views);
        if (!ex->views) rt_fatal("vk: out of memory");
        v = ex->views;
    }
    v[ex->nviews++] = (ViewEnt){t->image, t->format, t->view_type, t->aspect, t->level, t->base_layer, t->layers, view};
    return view;
}

typedef struct { int n; VkImageView v[]; } ViewList;

static void free_views(void *arg) {   // completion thread, once the submission that used them is done
    ViewList *l = arg;
    VkDevice dev = vk_ctx()->device;
    for (int i = 0; i < l->n; i++) vkDestroyImageView(dev, l->v[i], NULL);
    free(l);
}

// ---- target history
// The presented frame (gfx_counters.frames + 1; 0: never) each target subresource was last rendered into, and the one
// before. prepare_draw lets a draw into a target the previous frame did not render (a one-shot draw: a volume or LUT
// generated once) wait for its shaders and pipeline instead of skipping it. Per subresource, not per image: a volume
// may be generated a few slices per frame. TFR_WAYS-way sets: a new subresource takes the way rendered longest ago, so
// the targets a frame renders keep their entries; a direct-mapped table lost a few each frame to collisions, and a draw
// into a lost target waited as one-shot instead of skipping a frame.
#define TFR_SETS 512
#define TFR_WAYS 8
typedef struct { VkImage image; uint32_t level, layer; uint64_t last, prev; } TargetFrames;
static TargetFrames tfr[TFR_SETS][TFR_WAYS];
static PlatMutex tfr_lock = PLAT_MUTEX_INIT;   // the recording threads of a chunked replay

static TargetFrames *tfr_set(const VkExecTarget *t) {
    uint64_t h = ((uint64_t)(uintptr_t)t->image ^ (uint64_t)t->level << 56 ^ (uint64_t)t->base_layer << 40) *
                 0x9E3779B97F4A7C15ull;
    return tfr[h >> 55];
}

static int tfr_match(const TargetFrames *e, const VkExecTarget *t) {
    return e->image == t->image && e->level == t->level && e->layer == t->base_layer;
}

// The entry of t, NULL when it has none (tfr_lock held).
static TargetFrames *tfr_find(const VkExecTarget *t) {
    TargetFrames *s = tfr_set(t);
    for (int i = 0; i < TFR_WAYS; i++)
        if (tfr_match(&s[i], t)) return &s[i];
    return NULL;
}

static void tfr_note(const VkExecTarget *t, uint64_t stamp) {
    plat_mutex_lock(&tfr_lock);
    TargetFrames *e = tfr_find(t);
    if (!e) {
        // the way rendered longest ago, indexed from the set's start: the ways past the last set are RP's words
        TargetFrames *s = tfr_set(t);
        e = s;
        for (int i = 1; i < TFR_WAYS; i++)
            if (s[i].last < e->last) e = &s[i];
        *e = (TargetFrames){t->image, t->level, t->base_layer, 0, 0};
    }
    if (e->last != stamp) e->prev = e->last, e->last = stamp;
    plat_mutex_unlock(&tfr_lock);
}

// --probe-vkexec: a new target noted into a full set takes the way rendered longest ago, and the set after it keeps
// its entries. The ways are stamped so that a search stepping from the way it picked last would leave the set. 1 when
// it holds; the two sets are cleared afterwards.
int vk_exec_tfr_check(void) {
    VkExecTarget t[TFR_WAYS + 1];
    for (int n = 0, k = 1; n <= TFR_WAYS; k++) {
        t[n] = (VkExecTarget){.image = (VkImage)(uintptr_t)k};
        n += tfr_set(&t[n]) == tfr[0];
    }
    static const uint64_t last[TFR_WAYS] = {8, 7, 100, 6, 100, 100, 5, 100};
    TargetFrames next[TFR_WAYS];
    for (int i = 0; i < TFR_WAYS; i++) tfr[0][i] = (TargetFrames){t[i].image, 0, 0, last[i], 0};
    memcpy(next, tfr[1], sizeof next);
    tfr_note(&t[TFR_WAYS], 200);
    plat_mutex_lock(&tfr_lock);
    int ok = tfr_find(&t[TFR_WAYS]) == &tfr[0][6] && !memcmp(next, tfr[1], sizeof next);
    plat_mutex_unlock(&tfr_lock);
    memset(tfr, 0, sizeof tfr[0] * 2);
    return ok;
}

static int tfr_recent(const VkExecTarget *t, uint64_t stamp) {
    plat_mutex_lock(&tfr_lock);
    const TargetFrames *e = tfr_find(t);
    int r = e && (e->last == stamp ? e->prev : e->last) == stamp - 1;
    plat_mutex_unlock(&tfr_lock);
    return r;
}

// No frame before the current one rendered the target.
static int tfr_first(const VkExecTarget *t, uint64_t stamp) {
    plat_mutex_lock(&tfr_lock);
    const TargetFrames *e = tfr_find(t);
    int r = !e || !(e->last == stamp ? e->prev : e->last);
    plat_mutex_unlock(&tfr_lock);
    return r;
}

// With any, a recent depth target alone does not make the pass per-frame when a colour target is rendered for the
// first time: the game's item icon capture (an inventory or Fuse icon) draws into a new colour target with the
// per-frame depth buffer, once. Its draws skipped there stay missing from the icon until the item is captured again.
int vk_exec_targets_recent(VkExec *ex, int any) {
    resolve_rt(ex);
    uint64_t stamp = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    int n = 0, color = 0, depth = 0, color_first = 0;
    for (int i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if (!t->image) continue;
        n++;
        if (tfr_recent(t, stamp)) *(i < 8 ? &color : &depth) = 1;
        else if (!any) return 0;
        else if (i < 8 && tfr_first(t, stamp)) color_first = 1;
    }
    return any ? color || (depth && !color_first) : n > 0;
}

// One line naming each target of the current pass: format, extent, layer and the two frames it was last rendered in
// (prepare_draw's pipeline wait log).
void vk_exec_targets_describe(VkExec *ex, char *buf, size_t cap) {
    resolve_rt(ex);
    size_t n = 0;
    buf[0] = 0;
    for (int i = 0; i < 9 && n < cap; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if (!t->image) continue;
        plat_mutex_lock(&tfr_lock);
        const TargetFrames *e = tfr_find(t);
        uint64_t last = e ? e->last : 0, prev = e ? e->prev : 0;
        plat_mutex_unlock(&tfr_lock);
        int w = snprintf(buf + n, cap - n, "%s%c%d fmt %d %ux%u L%u seen %llu/%llu", n ? "; " : "", i < 8 ? 'c' : 'd',
                         i < 8 ? i : 0, (int)t->format, t->width, t->height, t->base_layer,
                         (unsigned long long)last, (unsigned long long)prev);
        if (w < 0) break;
        n += (size_t)w;
    }
    if (!n) snprintf(buf, cap, "no targets");
}

// ---- passes and barriers

static int has_image(const VkImage *list, int n, VkImage img) {
    for (int i = 0; i < n; i++)
        if (list[i] == img) return 1;
    return 0;
}

static VkImageMemoryBarrier2 layout_barrier(VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
    return (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                                   .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
}

// Full barrier. The images in the feedback layout become want[] (pass begin) or none (every other barrier): the others
// go back to GENERAL and the new ones leave it, in the same command.
static void barrier_to(VkExec *ex, const VkImage *want, const VkImageAspectFlags *wasp, int nwant) {
    vk_exec_pass_end(ex);
    vk_exec_ub_flush(ex);   // uniform updates overlaid in the pass reach their buffers before the barrier below
    VkImageMemoryBarrier2 ib[18];
    uint32_t n = 0;
    for (int i = 0; i < ex->nfbl; i++)
        if (!has_image(want, nwant, ex->fbl_img[i]))
            ib[n++] = layout_barrier(ex->fbl_img[i], ex->fbl_aspect[i], VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT,
                                     VK_IMAGE_LAYOUT_GENERAL);
    for (int i = 0; i < nwant; i++)
        if (!has_image(ex->fbl_img, ex->nfbl, want[i]))
            ib[n++] = layout_barrier(want[i], wasp[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT);
    for (int i = 0; i < nwant; i++) ex->fbl_img[i] = want[i], ex->fbl_aspect[i] = wasp[i];
    ex->nfbl = nwant;
    if (ex->barrier_at == ex->recorded && !n) return;
    ex->fr.fb_layouts += n;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb,
                           .imageMemoryBarrierCount = n, .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->barrier_at = ++ex->recorded;
}

void vk_exec_barrier(VkExec *ex) { barrier_to(ex, NULL, NULL, 0); }

void vk_exec_pass_begin(VkExec *ex) {
    if (ex->in_pass) return;
    resolve_rt(ex);
    vk_contact_pass_begin(ex);
    vk_dlaa_pass(ex);   // DLAA runs before the bloom chain's first pass (vk_dlaa.c)
    // Learned targets (VkTexImage.fb_learned) are attached in the feedback layout
    VkImage want[9];
    VkImageAspectFlags wasp[9];
    int nwant = 0;
    uint32_t fbl = 0;
    for (uint32_t i = 0; i < 9; i++) {
        const VkExecTarget *t = i < 8 ? &ex->color[i] : &ex->depth;
        if ((i < 8 && i >= ex->pass_ncolor) || !t->image || !t->img || !t->img->fb_learned) continue;
        fbl |= 1u << i;
        if (has_image(want, nwant, t->image)) continue;
        want[nwant] = t->image, wasp[nwant++] = i < 8 ? VK_IMAGE_ASPECT_COLOR_BIT : t->img->aspect;
    }
    barrier_to(ex, want, wasp, nwant);   // passes, clears, copies and dispatches see each other's results
    ex->fbl_mask = fbl;
    ex->fb_dirty = 0;
    ex->pass_serial++;
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    for (uint32_t i = 0; i < ex->pass_ncolor; i++) {
        const VkExecTarget *t = &ex->color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (!t->image) continue;
        ca[i].imageView = view_of(ex, t);
        ca[i].imageLayout = fbl >> i & 1 ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT : VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;    }
    const VkExecTarget *d = &ex->depth;
    if (d->image) {
        VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view_of(ex, d),
                                       .imageLayout = fbl >> 8 & 1 ? VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
                                                                   : VK_IMAGE_LAYOUT_GENERAL,
                                       .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        if (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) da = a;
        if (d->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) sa = a;
    }
    // The start mark goes between the barrier and vkCmdBeginRendering, its end right after vkCmdEndRendering
    // (vk_exec_pass_end): outside the pass, and without breaking it.
    VkPassMark *m = ex->mark = vk_pass_mark_begin(ex->cb, VK_MARK_PASS, ex->fence);
    if (m) {
        m->step = ex->mark_passes++;
        m->w = ex->pass_w, m->h = ex->pass_h, m->d = ex->pass_layers;
        m->ncolor = (uint8_t)(ex->pass_ncolor < 8 ? ex->pass_ncolor : 8);
        for (int i = 0; i < m->ncolor; i++) m->color[i] = ex->pass_color[i];
        m->depth = ex->pass_depth ? ex->pass_depth : ex->pass_stencil;
        // The shaders bound now are the first draw's when it repeats the pipeline state; when it does not,
        // vk_exec_draw.c prepare_draw replaces them as it records the new state.
        vk_exec_draw_shaders(ex, &m->first[0], &m->first[1]);
        ex->mark_draws0 = ex->fr.draws;
    }
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {ex->pass_w, ex->pass_h}},
                          .layerCount = ex->pass_layers, .colorAttachmentCount = ex->pass_ncolor,
                          .pColorAttachments = ca,
                          .pDepthAttachment = ex->pass_depth ? &da : NULL,
                          .pStencilAttachment = ex->pass_stencil ? &sa : NULL};
    vkCmdBeginRendering(ex->cb, &ri);
    ex->fr.passes++;
    ex->recorded++;
    ex->in_pass = 1;
}

void vk_exec_pass_end(VkExec *ex) {
    if (!ex->in_pass) return;
    vk_exec_oq_pass_end(ex);
    vkCmdEndRendering(ex->cb);
    ex->recorded++;
    ex->in_pass = 0;
    if (ex->mark) {
        // frame_add empties the context's counters at the end of a submission: then only the draws since it count
        uint64_t d = ex->fr.draws, d0 = ex->mark_draws0;
        ex->mark->draws = (uint32_t)(d >= d0 ? d - d0 : d);
        if (ex->mark->draws) vk_exec_draw_shaders(ex, &ex->mark->last[0], &ex->mark->last[1]);
        vk_pass_mark_end(ex->cb, ex->mark);
        ex->mark = NULL;
    }
    for (int i = 0; i < ex->pass_ncolor && i < 8; i++)   // stand-ins of 3D BC targets go to their textures
        if (ex->color[i].image && ex->color[i].img && ex->color[i].img->image != ex->color[i].image)
            bc_copy_back(ex, &ex->color[i]);
    if (vk_dump_armed) vk_dump_pass_end(ex);
    uint64_t stamp = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 1;
    for (int i = 0; i < 8; i++)
        if (ex->color[i].image) vk_texture_gpu_written(ex->color[i].tex), tfr_note(&ex->color[i], stamp);
    if (ex->depth.image) vk_texture_gpu_written(ex->depth.tex), tfr_note(&ex->depth, stamp);
}

VkCommandBuffer vk_exec_inline_begin(int *reopen) {
    VkExec *ex = x_cur;
    *reopen = 0;
    if (!ex || !ex->cb) return VK_NULL_HANDLE;
    *reopen = ex->in_pass;
    vk_exec_barrier(ex);   // ends the pass; feedback-layout targets go back to GENERAL
    return ex->cb;
}

void vk_exec_inline_end(int reopen) {
    x_cur->recorded++;
    if (reopen) vk_exec_pass_begin(x_cur);
}

typedef struct { VkBuffer b; VmaAllocation a; } InlineBuf;
static void free_inline_buf(void *arg) {   // completion thread
    InlineBuf *t = arg;
    vmaDestroyBuffer(vk_ctx()->vma, t->b, t->a);
    free(t);
}

VkBuffer vk_exec_inline_buffer(VkDeviceSize size) {
    InlineBuf *t = calloc(1, sizeof *t);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (!t || vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &t->b, &t->a, NULL) != VK_SUCCESS) { free(t); return VK_NULL_HANDLE; }
    vk_completion_add(x_cur->fence, free_inline_buf, t);
    return t->b;
}

VkRect2D vk_exec_scissor(const VkExec *ex) {
    int64_t w = ex->pass_w, h = ex->pass_h;
    if (!ex->has_scissor) return (VkRect2D){{0, 0}, {(uint32_t)w, (uint32_t)h}};
    int64_t k = ex->pass_scale > 1 ? ex->pass_scale : 1;
    int64_t x0 = ex->scissor.x * k, y0 = ex->scissor.y * k;
    int64_t x1 = x0 + (int64_t)ex->scissor.w * k, y1 = y0 + (int64_t)ex->scissor.h * k;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x1 <= x0 || y1 <= y0) return (VkRect2D){{0, 0}, {0, 0}};
    return (VkRect2D){{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
}

// ---- clears

// Outside a pass, a clear of a whole target (no narrower scissor, the target as large as the render area) clears the
// image directly instead of opening a pass for it.
static int whole_target(const VkExec *ex, const VkExecTarget *t) {
    // vkCmdClear*Image addresses a 3D level whole, not one depth plane: slices of a 3D target clear in a pass; it
    // cannot clear a compressed image: views of BC blocks clear in a pass too
    if (ex->in_pass || t->is3d || t->blocks || t->width != ex->pass_w || t->height != ex->pass_h) return 0;
    if (!ex->has_scissor) return 1;
    const p_scissor *s = &ex->scissor;
    return s->x <= 0 && s->y <= 0 && (int64_t)s->x + s->w >= t->width && (int64_t)s->y + s->h >= t->height;
}

static void clear_attachment(VkExec *ex, const VkClearAttachment *a) {
    vk_exec_pass_begin(ex);
    VkClearRect cr = {vk_exec_scissor(ex), 0, ex->pass_layers};
    if (!cr.rect.extent.width || !cr.rect.extent.height) return;
    vkCmdClearAttachments(ex->cb, 1, a, 1, &cr);
    ex->recorded++;
    ex->fb_dirty |= a->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT ? 1u << a->colorAttachment : 0x100u;
}

static void clear_color(VkExec *ex, const p_clear_color *c) {
    ex->fr.clears++;
    if (ex->render_disabled) return;
    resolve_rt(ex);
    int i = c->index;
    if (i < 0 || i >= (int)ex->pass_ncolor || !ex->color[i].image) {
        warn_once(W_CLEAR_TARGET, "colour clear of target %d, which is not bound or not renderable: skipped", i);
        return;
    }
    uint32_t mask = (uint32_t)c->mask & 0xf;
    if (!mask) return;
    if (mask != 0xf) {   // vkCmdClearAttachments ignores write masks; a masked clear needs a draw (vk_exec_draw.c)
        warn_once(W_CLEAR_MASK, "colour clear with channel mask %#x is not replayed: skipped", mask);
        return;
    }
    const VkExecTarget *t = &ex->color[i];
    VkClearColorValue cv;
    memcpy(cv.float32, c->rgba, sizeof cv.float32);   // integer targets: the payload's bits as the front end stored them
    if (whole_target(ex, t)) {
        vk_exec_barrier(ex);
        VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, t->level, 1, t->base_layer, t->layers};
        vkCmdClearColorImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &r);
        ex->recorded++;
        vk_texture_gpu_written(t->tex);
        return;
    }
    VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)i};
    a.clearValue.color = cv;
    clear_attachment(ex, &a);
}

static void clear_ds(VkExec *ex, const p_clear_ds *c) {
    if (ex->render_disabled) return;
    resolve_rt(ex);
    const VkExecTarget *t = &ex->depth;
    if (!t->image) return;
    VkImageAspectFlags asp = 0;
    if (c->depth_mask && (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    uint32_t sm = (uint32_t)c->stencil_mask & 0xff;
    if (sm && (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
        if (sm == 0xff) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
        else warn_once(W_STENCIL_MASK, "stencil clear with mask %#x is not replayed: skipped", sm);
    }
    if (!asp) return;
    float d = c->depth < 0.0f ? 0.0f : c->depth > 1.0f ? 1.0f : c->depth;
    VkClearDepthStencilValue v = {d, (uint32_t)c->stencil & 0xff};
    if (whole_target(ex, t)) {
        vk_exec_barrier(ex);
        VkImageSubresourceRange r = {asp, t->level, 1, t->base_layer, t->layers};
        vkCmdClearDepthStencilImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
        ex->recorded++;
        vk_texture_gpu_written(t->tex);
        return;
    }
    VkClearAttachment a = {.aspectMask = asp};
    a.clearValue.depthStencil = v;
    clear_attachment(ex, &a);
}

// ---- the op loop

static void set_rt(VkExec *ex, const p_set_rt *r) {
    if (!memcmp(&ex->rt, r, sizeof *r)) return;   // the same targets again: the pass goes on
    vk_exec_pass_end(ex);
    ex->rt = *r;
    ex->rt_resolved = 0;
}

// ---- command buffer parts
// NVN starts the GPU as soon as commands are submitted; one Vulkan command buffer per submission starts it only after
// the whole replay. Between passes, once TOTK_SPLIT_DRAWS (64; 0 off) draws and dispatches were recorded since the
// last part, the part goes to the queue (vk_sync_split) and the replay continues in a new command buffer. Only
// command-buffer state is lost there (bindings and dynamic state: vk_exec_draw_new_cb, ex->dirty); barriers, layouts and
// queries are not: a barrier's first scope is everything earlier in submission order, image layouts belong to the image,
// and occlusion queries end with their pass. Fences, completion callbacks and reports stay per submission: only the last
// part signals the value. nxnative commits every 64 draws between passes; MacNativeGames after 30, then every 40.
// A part also spans at least TOTK_SPLIT_US (1000) of replay time: a split costs about 40 us of the render thread
// (vkQueueSubmit2, a command buffer, the mirror prologue), and a fight frame of 2,000 draws made 15 parts on the 7900X;
// a millisecond of recording is a fraction of a millisecond of GPU work, so the GPU still starts early.
static int split_n = -1;
static uint64_t split_ns;
static uint64_t n_parts, n_subs;

static void maybe_split(VkExec *ex) {
    if (split_n < 0) {
        split_n = cfg_int("TOTK_SPLIT_DRAWS");
        if (split_n < 0) split_n = 0;
        int us = cfg_int("TOTK_SPLIT_US");
        split_ns = us > 0 ? (uint64_t)us * 1000 : 0;
        rt_log("vk: replay command buffer split every %d draws between passes, parts of at least %d us%s", split_n,
               us > 0 ? us : 0, split_n ? "" : " (off)");
    }
    if (!split_n || ex->in_pass || ex->split_draws < split_n) return;
    if (split_ns && kern_now_ns() - ex->part_t0 < split_ns) return;   // split_draws stays: the next draw asks again
    ex->split_draws = 0;
    if (ex->recorded == ex->part_start) return;
    ex->cb = vk_sync_split();
    ex->part_start = ex->recorded;
    ex->part_t0 = kern_now_ns();
    ex->dirty = VK_EXEC_DIRTY_ALL;
    vk_exec_draw_new_cb(ex);
    n_parts++;
}

// The fixed-function state ops: writes into ex alone, applied by the replay and the state scan. 0: not one of them.
static int state_op(VkExec *ex, uint32_t op, const void *a) {
    switch (op) {
    case OP_VIEWPORTS: {
        const p_viewports *v = a;
        for (int i = 0; i < v->n && v->first + i < 16; i++)
            if (v->first + i >= 0) memcpy(ex->vp[v->first + i], v->v[i], sizeof ex->vp[0]);
        ex->has_vp = 1;
        ex->dirty |= VK_EXEC_DIRTY_VIEWPORT;
        break;
    }
    case OP_SCISSOR:
        ex->scissor = *(const p_scissor *)a;
        ex->has_scissor = 1;
        ex->dirty |= VK_EXEC_DIRTY_SCISSOR;
        break;
    case OP_DEPTH_RANGE: {
        const p_depth_range *r = a;
        ex->depth_range[0] = r->n, ex->depth_range[1] = r->f;
        ex->dirty |= VK_EXEC_DIRTY_VIEWPORT;
        break;
    }
    case OP_STENCIL: {
        const p_stencil *s = a;
        for (int f = 0; f < 2; f++) {
            if (!(s->face & (1 << f))) continue;
            if (s->which == 0) ex->st_vmask[f] = (uint32_t)s->value_mask;
            else if (s->which == 1) ex->st_wmask[f] = (uint32_t)s->mask;
            else ex->st_ref[f] = (uint32_t)s->ref;
        }
        ex->dirty |= VK_EXEC_DIRTY_STENCIL;
        break;
    }
    case OP_POLY_OFFSET:
        ex->poly_offset = *(const p_poly_offset *)a;
        ex->dirty |= VK_EXEC_DIRTY_DEPTH_BIAS;
        break;
    case OP_BLEND_COLOR:
        memcpy(ex->blend_color, ((const p_blend_color *)a)->rgba, sizeof ex->blend_color);
        ex->dirty |= VK_EXEC_DIRTY_BLEND_CONST;
        break;
    case OP_DEPTH_CLAMP: ex->pipe.depth_clamp = ((const p_flag *)a)->value != 0; break;
    case OP_RASTER_DISCARD: ex->pipe.raster_discard = ((const p_flag *)a)->value != 0; break;
    case OP_PRIM_RESTART: ex->prim_restart = *(const p_prim_restart *)a; break;
    case OP_COPY_STRIDE: ex->copy_stride = ((const p_copy_stride *)a)->stride; break;
    case OP_SET_TEX_POOL: ex->tex_pool = ((const p_set_pool *)a)->pool, ex->tex_serial++; break;
    case OP_SET_SAMP_POOL: ex->samp_pool = ((const p_set_pool *)a)->pool; break;
    default: return 0;
    }
    return 1;
}

// A record the scan planned for this point (vk_contact_plan, vk_dlaa_plan): recorded before the op that triggers it, the
// draw's pass begun after it as in prepare_draw. The records share their module's pipelines and state, so they go in
// the scan's order; chunks are taken in order, so the chunk of an earlier record is being recorded or done. DLSS's
// evaluation reads what the scene's draws noted on the chunks before it (the camera block, the jitter used, the
// per-object motion list), so it waits until they are recorded; the chunks after it wait for it (a hold, scan_job).
static uint32_t job_turn;
static void chunks_wait(int k);

static void run_job(VkExec *ex) {
    uint32_t turn = ex->job_i;
    VkExecJob j = ex->jobs[ex->job_i++];
    if (j.kind == VK_JOB_DLSS) chunks_wait(j.chunk);
    while (__atomic_load_n(&job_turn, __ATOMIC_ACQUIRE) != turn) plat_yield();
    vk_exec_pass_end(ex);
    if (j.kind != VK_JOB_CONTACT) {
        vk_dlaa_exec(ex, &j);
    } else {
        // The scan resolved the scene and its depth without creating entries (vk_texture_revive): their images now.
        const VkTexImage *si = vk_texture_get(j.scene.tex), *di = vk_texture_get(j.depth.tex);
        if (si && di) {
            j.scene.img = si, j.scene.image = si->image, j.depth.img = di, j.depth.image = di->image;
            vk_contact_exec(ex, &j);
        }
    }
    __atomic_store_n(&job_turn, turn + 1, __ATOMIC_RELEASE);
    ex->dirty = VK_EXEC_DIRTY_ALL;
    vk_exec_draw_new_cb(ex);
}

// Replays from h to OP_END; 1 when it stopped at stop instead, an op at depth 0 (a chunk's end, record_chunks).
// The ops that may make the GPU write a uniform range the submission's snapshot copied (vk_usnap_drop_at).
static int drop_op(uint32_t op) { return op == OP_UPDATE_UB || op == OP_COPY_B2B || op == OP_COPY_T2B || op == OP_CLEAR_BUF; }

static int walk(VkExec *ex, const op_hdr *h, int depth, const op_hdr *stop) {
    for (;;) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_)
            rt_fatal("vk: corrupt command record at %p: op %u size %u", (const void *)h, h->op, h->size);
        if (h == stop) return 1;
        while (ex->job_i < ex->njobs && h == ex->jobs[ex->job_i].op) run_job(ex);
        const void *a = nvn_op_payload(h);
        if (h->op == OP_END) return 0;
        if (ex->chunked) {
            ex->drop_n += (uint32_t)drop_op(h->op), ex->pos++;
            if (ex->wpos_i < ex->wpos_end && ex->wpos[ex->wpos_i] == ex->pos) ex->drop_n++, ex->wpos_i++;
        }
        switch (h->op) {
        case OP_BEGIN: break;
        case OP_JUMP: h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target; continue;
        case OP_CALL:
            if (depth >= CALL_DEPTH) rt_fatal("vk: command buffer calls nested deeper than %d", CALL_DEPTH);
            walk(ex, nvn_op_first(((const p_jump *)a)->target), depth + 1, NULL);
            break;
        case OP_SET_RT: set_rt(ex, a); break;
        case OP_CLEAR_COLOR: clear_color(ex, a); break;
        case OP_CLEAR_DS: clear_ds(ex, a); break;
        case OP_BARRIER: vk_exec_barrier(ex); break;
        case OP_DRAW: case OP_DRAW_IDX: case OP_DISPATCH: case OP_UPDATE_UB: case OP_DRAW_IDX_IND: case OP_DISPATCH_IND:
            if (h->op == OP_DRAW || h->op == OP_DRAW_IDX || h->op == OP_DRAW_IDX_IND) ex->fr.draws_seen++, ex->split_draws++;
            if (h->op == OP_DISPATCH || h->op == OP_DISPATCH_IND) ex->fr.dispatches++, ex->split_draws++;
        case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
        case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG:
            vk_exec_op_draw(ex, h->op, a);
            break;
        case OP_COPY_B2B: case OP_COPY_B2T: case OP_COPY_T2B: case OP_COPY_T2T: case OP_CLEAR_TEX:
            if (h->op != OP_CLEAR_TEX) ex->fr.copies++;
            if (vk_dump_armed) vk_dump_copy(ex, h->op, a);
            // fall through
        case OP_SIGNAL_EVENT: case OP_REPORT_COUNTER: case OP_RESET_COUNTER: case OP_RENDER_ENABLE: case OP_RENDER_COND:
        case OP_CLEAR_BUF:
            vk_exec_op_xfer(ex, h->op, a);
            break;
        default:
            if (!state_op(ex, h->op, a)) rt_fatal("vk: op %s has no replay handler", nvn_op_name(h->op));
        }
        if (ex->split_draws >= split_n && !ex->chunked) maybe_split(ex);
        h = nvn_op_next(h);
    }
}

// ---- chunked recording (TOTK_REPLAY_WORKERS)
// A submission is recorded in chunks that start between passes, by TOTK_REPLAY_WORKERS threads (0: the sequential
// replay, the default) and twice as many chunks (TOTK_REPLAY_CHUNKS), so a thread
// that finishes early takes another: in the town at 4K on an RTX 5070 the replay goes from 8.2 to 4.5 ms
// and the frame rate from 71 to 79; more threads add little, since the frame then waits for the GPU. A sequential scan on
// the submitting thread applies only what is queue state: the fixed-function and bind ops, render targets, render
// enable and conditions, samples-passed counting, the contact shadows' marches (vk_contact_plan) and DLSS's decisions
// (vk_dlaa_plan); it looks up no
// shader, pipeline or descriptor and makes no Vulkan call. Where a chunk starts it keeps a copy of that state
// (VK_EXEC_STATE_BEGIN..END) and how far the scan's lists got: the submission's inline uniform updates (ubo_ptr),
// RENDER_COND results, planned records (contact shadows, DLSS) and the ops that drop uniform copies (vk_usnap_drop_at,
// scan_writes), which a chunk takes
// from the lists instead of from earlier chunks. A chunk starts with every image in GENERAL and ends with a full
// barrier, as a part of vk_sync_split does, so its recording depends only on the copy and the lists.
// A chunk can be taken as soon as the scan has placed the next one and the threads finished what the chunks share, which
// they do beside the scan (prep_run); they record into command buffers of their own, each with a context of its own
// (memos, descriptor pools, transient memory), and make their Vulkan calls themselves. The submitting thread records none while there are such threads: it submits the chunks in order as they
// are done, between the scan's steps and after it (vk_sync_part: the uploads and mirror copies queued so far go first),
// with their completion callbacks, so the callbacks keep the order of the sequential replay. The first chunk is a
// quarter of the others: the GPU starts sooner. A chunk ends where the replay ends a pass once it has its share of
// draws, or inside a pass at half as many more (TotK's G-buffer and shadow passes have 1,000 to 2,000 draws): the next
// chunk begins the pass again on the loaded attachments.
// The scan resolves render targets without creating images and revives finalized ones at their op (vk_texture_revive);
// a chunk, which the scan is ahead of, asks for a texture slot as of its own op (VkExec.pos, vk_exec_replay_pos).
// A texture that has no entry yet is made by the chunk that reaches it, and only that chunk has its position: its
// content from a live texture at the storage (vk_texture.c alias_copy) goes at that op. A later chunk sampling it first
// would follow another texture at the storage or copy over what the earlier chunk renders, so the chunks after the
// first use wait until that chunk is recorded (SC.holds). TotK makes a few such targets a frame: thumbnails, and depth
// targets whose size follows the camera; a wrong one darkened the whole scene for some frames.
// TOTK_REPLAY_STATE_CHECK=1 compares the state after the last chunk with the scan's.
#define MAX_JOBS 64
#define MAX_RECS 16

typedef struct {
    int hi;                              // handle index and record of the first op
    const op_hdr *op;
    uint32_t upd_n, cond_i, job_i, job_end, drop_n, pos;   // the scan's lists, drop and op positions before it; its records
    uint32_t wpos_i, wpos_end;           // its writing draws and dispatches in SC.wpos
    VkExec st;                           // queue state at its start
    VkCommandBuffer cb;                  // recorded, ended, submitted in its turn
    VkDeferred defer;                    // its completion callbacks
    VkExec *by;                          // the context that recorded it
    uint64_t ns, start;                  // its recording time, and when it began after SC.t0
    int draws;                           // draws and dispatches in it
    volatile int32_t done;
} Chunk;

typedef struct {
    VkExec ex;                           // the thread's recording context
    VkCommandPool pool;
    struct { VkCommandBuffer cb; uint64_t fence; } *cbs;   // reusable once the timeline reached fence
    int ncbs;
    int64_t traffic[VK_MCLS_N][VK_MUSE_N];   // vk_mem_count_local
    PlatThread *t;
} Rec;

typedef struct { uint64_t addr, size; uint32_t at; } ScanDrop;

static struct {
    int workers, chunks, min_draws, check;
    int skew_us;                         // TOTK_REPLAY_SKEW_US (chunk_record)
    VkExec s;                            // the scan's state
    Chunk *ch;                           // chunks + 1 entries, never moved while threads record
    int nch;
    VkExecUpd *upd;                      // sized before the scan (count_ops): threads read them while it appends
    uint32_t nupd, cupd;
    uint8_t *cond;
    uint32_t ncond, ccond;
    VkExecJob jobs[MAX_JOBS];
    uint32_t njobs;
    uint32_t drop_n;                     // scan: ops so far that may write a uniform range
    uint32_t *wpos;                      // scan: the positions of the draws and dispatches among them, sized like upd
    uint32_t nwpos, cwpos;
    ScanDrop *dr;                        // scan: its uniform drops, applied once the snapshot exists (chunks_publish)
    uint32_t ndr, cdr, dr_done;
    int pub;                             // chunks published
    uint64_t holds;                      // chunks that make a new texture or record writes the scan does not know: the
                                         // ones after them wait until they are done
    int per, open, in_chunk;             // scan: draws for this chunk, a pass of the replay may be open, draws in it
    int sub;                             // chunks submitted
    uint64_t t0;                         // the submission's start
    // TOTK_TRACE=perf, per chunked submission: the scan, the first and the last chunk submitted after t0, the chunks'
    // recording summed and the longest
    uint64_t subs, chunked, nchunks, scan_ns, first_ns, last_ns, rec_ns, rec_max_ns, bad, prep_ns, start0_ns;
    uint64_t rec_t0;                     // vk_exec_record's start
    uint64_t held[HOLD_N];               // TOTK_TRACE=perf: chunks held, by scan_hold's reason
    uint64_t settles, settle_ns;         // TOTK_TRACE=perf: vk_exec_chunk_settle waits (atomic: recording threads)
    uint64_t mirror_ns, usnap_ns, ready_ns;   // vk_mem_mirror_begin, vk_usnap_begin, both done after t0
} SC = {.workers = -1};

static Rec recs[MAX_RECS + 1];           // [0]: the submitting thread's, used without recording threads
static int nrecs, nthreads;
static struct {
    PlatMutex m;
    PlatCond go, done;                   // more chunks can be taken or a new submission; a chunk is done
    uint32_t gen;                        // bumped for every chunked submission
    // gen << 32 | scan done << 31 | chunks that can be taken << 16 | next to take
    volatile uint64_t take;
    const uint64_t *handles;
    int n, prio;
    uint64_t fence;
    int prep_next, prep_left;            // prep_run: the next job, the jobs not done
} RP;

static void *grow_n(void *p, uint32_t *cap, size_t elem) {
    *cap = *cap ? *cap * 2 : 64;
    p = realloc(p, elem * *cap);
    if (!p) rt_fatal("vk: out of memory");
    return p;
}

static void copy_state(VkExec *dst, const VkExec *src) {
    memcpy((char *)dst + VK_EXEC_STATE_BEGIN, (const char *)src + VK_EXEC_STATE_BEGIN, VK_EXEC_STATE_END - VK_EXEC_STATE_BEGIN);
}

static int draw_op(uint32_t op) { return op == OP_DRAW || op == OP_DRAW_IDX || op == OP_DRAW_IDX_IND; }

static void check_op(const op_hdr *h) {
    if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_)
        rt_fatal("vk: corrupt command record at %p: op %u size %u", (const void *)h, h->op, h->size);
}

// Draws and dispatches, inline uniform updates and conditions of a command stream.
typedef struct { uint32_t draws, upd, cond; } OpCounts;

static void count_ops(const op_hdr *h, int depth, OpCounts *c) {
    for (;;) {
        check_op(h);
        const void *a = nvn_op_payload(h);
        if (h->op == OP_END) return;
        if (h->op == OP_JUMP) {
            h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target;
            continue;
        }
        if (h->op == OP_CALL && depth < CALL_DEPTH) count_ops(nvn_op_first(((const p_jump *)a)->target), depth + 1, c);
        c->draws += draw_op(h->op) || h->op == OP_DISPATCH || h->op == OP_DISPATCH_IND;
        c->upd += h->op == OP_UPDATE_UB;
        c->cond += h->op == OP_RENDER_COND;
        h = nvn_op_next(h);
    }
}

// The take word: chunks [0, avail) can be taken, final once the scan placed them all.
static void take_publish(int avail, int final) {
    plat_mutex_lock(&RP.m);
    uint64_t w = __atomic_load_n(&RP.take, __ATOMIC_RELAXED);
    while (!__atomic_compare_exchange_n(&RP.take, &w, (w & ~(0xffffull << 16 | 1ull << 31)) | (uint64_t)avail << 16 | (uint64_t)final << 31,
                                        0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {}
    plat_cond_broadcast(&RP.go);
    plat_mutex_unlock(&RP.m);
}

// What every chunk needs before it records, independent of each other and of the scan, so they run beside it on the
// recording threads and the submission's chunks start sooner: the mirrors' write-watch queries and the uniform snapshot.
static void prep_run(void) {
    for (;;) {
        int j = __atomic_fetch_add(&RP.prep_next, 1, __ATOMIC_ACQ_REL);
        if (j >= 2) return;
        uint64_t t0 = kern_now_ns();
        if (j == 0) vk_mem_mirror_begin();
        else vk_usnap_begin(RP.handles, RP.n, RP.fence);
        uint64_t t1 = kern_now_ns();
        if (j == 0) SC.mirror_ns += t1 - t0;
        else SC.usnap_ns += t1 - t0;
        if (__atomic_load_n(&RP.prep_left, __ATOMIC_RELAXED) == 1) SC.ready_ns += t1 - SC.t0;
        __atomic_sub_fetch(&RP.prep_left, 1, __ATOMIC_RELEASE);
    }
}

// Chunks [0, avail) less those behind a hold that is not done.
static int chunks_allowed(int avail) {
    for (uint64_t m = SC.holds; m; m &= m - 1) {
        int k = __builtin_ctzll(m);
        if (k + 1 >= avail) break;
        if (!__atomic_load_n(&SC.ch[k].done, __ATOMIC_ACQUIRE)) return k + 1;
    }
    return avail;
}

// The chunks placed so far can be taken once prep_run is done, with the uniform drops the scan found up to them.
static void chunks_publish(int final) {
    int all = final ? SC.nch : SC.nch - 1, avail = chunks_allowed(all);
    final = final && avail == all;   // the threads go to sleep on the final word: only with nothing held back
    if ((avail == SC.pub && !final) || __atomic_load_n(&RP.prep_left, __ATOMIC_ACQUIRE)) return;
    for (; SC.dr_done < SC.ndr; SC.dr_done++) vk_usnap_drop_at(SC.dr[SC.dr_done].addr, SC.dr[SC.dr_done].size, SC.dr[SC.dr_done].at);
    SC.pub = avail;
    take_publish(avail, final);
}

// TOTK_TRACE=perf counts the chunks held per reason (SC.held).
static void scan_hold(int why) {
    if (SC.nch <= 0 || SC.nch > 64 || SC.holds >> (SC.nch - 1) & 1) return;
    SC.holds |= 1ull << (SC.nch - 1);
    SC.held[why]++;
}

static void scan_drop(uint64_t addr, uint64_t size) {
    if (SC.ndr == SC.cdr) SC.dr = grow_n(SC.dr, &SC.cdr, sizeof *SC.dr);
    SC.dr[SC.ndr++] = (ScanDrop){addr, size, SC.drop_n};
}

// A draw or dispatch whose shaders write storage buffers drops the uniform copies they cover from its own op on, as
// the sequential replay does in bind_range. The scan looks up no shader: it takes the ones the contexts translated
// (vk_exec_draw_writes). With one not translated yet the op still counts, the chunk that records it drops what the
// shader writes at its position (bind_range), and the chunks after it wait until it is recorded. Without this a chunk
// recorded before an earlier chunk's write bound the copy taken at the submit call, and on an RX 7800 XT the grass
// near the camera came out dark or unshaded in some runs.
static void scan_writes(VkExec *s, uint32_t op) {
    uint64_t w[16][2];
    s->chunked = 1, s->upd = SC.upd, s->upd_n = SC.nupd;   // ubo_ptr reads the scan's updates
    int n = vk_exec_draw_writes(s, op == OP_DISPATCH || op == OP_DISPATCH_IND, w, 16);
    s->chunked = 0;
    if (!n) return;
    SC.drop_n++;
    SC.wpos[SC.nwpos++] = s->pos;
    if (n < 0) scan_hold(HOLD_WRITES);
    for (int i = 0; i < n; i++) scan_drop(w[i][0], w[i][1]);
}

// The next chunk of submission gen nobody took: -1 none yet, -2 none left (or a later submission).
static int chunk_take(uint32_t gen) {
    uint64_t w = __atomic_load_n(&RP.take, __ATOMIC_ACQUIRE);
    for (;;) {
        if ((uint32_t)(w >> 32) != gen) return -2;
        if ((w & 0xffff) >= (w >> 16 & 0x7fff)) return w >> 31 & 1 ? -2 : -1;
        if (__atomic_compare_exchange_n(&RP.take, &w, w + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return (int)(w & 0xffff);
    }
}

static void views_release(VkExec *ex) {
    if (!ex->nviews) return;
    ViewList *l = malloc(sizeof *l + sizeof(VkImageView) * (size_t)ex->nviews);
    if (!l) rt_fatal("vk: out of memory");
    l->n = ex->nviews;
    for (int i = 0; i < ex->nviews; i++) l->v[i] = ((ViewEnt *)ex->views)[i].view;
    vk_completion_add(ex->fence, free_views, l);
    ex->nviews = 0;
}

static VkCommandBuffer rec_cb(Rec *r, uint64_t fence) {
    VkCtx *c = vk_ctx();
    if (!r->pool) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = c->gfx_family};
        if (vkCreateCommandPool(c->device, &pi, NULL, &r->pool) != VK_SUCCESS) rt_fatal("vk: chunk command pool");
    }
    uint64_t gpu = vk_sync_fence_done();
    int i = 0;
    while (i < r->ncbs && r->cbs[i].fence > gpu) i++;
    if (i == r->ncbs) {
        r->cbs = realloc(r->cbs, sizeof *r->cbs * (size_t)(r->ncbs + 1));
        if (!r->cbs) rt_fatal("vk: out of memory (command buffers)");
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = r->pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        if (vkAllocateCommandBuffers(c->device, &ai, &r->cbs[i].cb) != VK_SUCCESS) rt_fatal("vk: command buffer allocation");
        r->ncbs++;
    }
    r->cbs[i].fence = fence;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(r->cbs[i].cb, &bi);
    return r->cbs[i].cb;
}

// Chunk k on the calling thread with context r: from its state copy up to the next chunk's first op, into a command
// buffer of r's pool, ending with every image in GENERAL.
static void chunk_record(Rec *r, int k) {
    // TOTK_REPLAY_SKEW_US (debug): the even chunks wait this long before they record, so with several threads the
    // chunk after each one records first. A recording that depends on the order the threads get to the chunks, rather
    // than on replay order, then shows in every submission instead of a few launches in thirty.
    if (SC.skew_us > 0 && nthreads && !(k & 1)) plat_sleep_ns((uint64_t)SC.skew_us * 1000);
    uint64_t t0 = kern_now_ns();
    VkExec *ex = &r->ex;
    Chunk *c = &SC.ch[k];
    c->start = t0 - SC.t0;
    const Chunk *next = k + 1 < __atomic_load_n(&SC.nch, __ATOMIC_ACQUIRE) ? &SC.ch[k + 1] : NULL;
    ex->cb = rec_cb(r, RP.fence);
    ex->fence = RP.fence, ex->recorded = 0, ex->in_pass = 0, ex->nviews = 0, ex->barrier_at = -1, ex->nfbl = 0;
    ex->part_start = 0;
    x_cur = ex;
    chunk_k = k;
    vk_completion_defer(&c->defer);
    vk_mem_count_local(r->traffic);
    int ts = vk_sync_ts_open(ex->cb);
    copy_state(ex, &c->st);
    ex->rt_resolved = 0, ex->dirty = VK_EXEC_DIRTY_ALL, ex->tex_serial++, ex->fb_fs = NULL;
    vk_exec_draw_new_cb(ex);
    ex->chunked = 1, ex->upd = SC.upd, ex->cond = SC.cond, ex->jobs = SC.jobs, ex->njobs = c->job_end;
    ex->upd_n = c->upd_n, ex->cond_i = c->cond_i, ex->job_i = c->job_i, ex->drop_n = c->drop_n, ex->pos = c->pos;
    ex->wpos = SC.wpos, ex->wpos_i = c->wpos_i, ex->wpos_end = c->wpos_end;
    for (int i = c->hi; i < RP.n; i++)
        if (walk(ex, i == c->hi ? c->op : nvn_op_first(RP.handles[i]), 0, next && next->hi == i ? next->op : NULL)) break;
    vk_exec_barrier(ex);
    vk_exec_oq_flush(ex);
    ex->chunked = 0, ex->njobs = ex->job_i = 0;
    views_release(ex);
    vk_sync_ts_close(ex->cb, ts, RP.fence);
    vk_completion_defer(NULL);
    vk_mem_count_local(NULL);
    vk_mem_count_flush(r->traffic);
    x_cur = NULL;
    chunk_k = -1;
    vkEndCommandBuffer(ex->cb);
    c->cb = ex->cb, c->by = ex, c->ns = kern_now_ns() - t0;
    plat_mutex_lock(&RP.m);
    __atomic_store_n(&c->done, 1, __ATOMIC_RELEASE);
    plat_cond_broadcast(&RP.done);
    plat_mutex_unlock(&RP.m);
}

static void rec_main(void *arg) {
    Rec *r = arg;
    plat_thread_priority(RP.prio);
    uint32_t seen = 0;
    for (;;) {
        plat_mutex_lock(&RP.m);
        while (RP.gen == seen) plat_cond_wait(&RP.go, &RP.m);
        seen = RP.gen;
        plat_mutex_unlock(&RP.m);
        prep_run();
        for (;;) {
            uint64_t w = __atomic_load_n(&RP.take, __ATOMIC_ACQUIRE);
            int k = chunk_take(seen);
            if (k >= 0) chunk_record(r, k);
            if (k != -1) {
                if (k == -2) break;
                continue;
            }
            plat_mutex_lock(&RP.m);
            while (__atomic_load_n(&RP.take, __ATOMIC_ACQUIRE) == w) plat_cond_wait(&RP.go, &RP.m);
            plat_mutex_unlock(&RP.m);
        }
    }
}

// TOTK_REPLAY_WORKERS threads record; with one, the submitting thread does.
static void recs_start(void) {
    if (nrecs) return;
    nthreads = SC.workers > 1 ? (SC.workers < MAX_RECS ? SC.workers : MAX_RECS) : 0;
    nrecs = nthreads + 1;
    RP.prio = plat_thread_priority_get();
    for (int i = 0; i < nrecs; i++) {
        state_reset(&recs[i].ex);
        if (i && !(recs[i].t = plat_thread_new(rec_main, &recs[i], "vk replay"))) rt_fatal("vk: no replay thread");
    }
}

// Submits the chunks that are done, in order; wait: until all are.
static void chunks_submit(int wait) {
    while (SC.sub < SC.nch) {
        Chunk *c = &SC.ch[SC.sub];
        if (wait) chunks_publish(1);   // a hold may have ended
        if (!__atomic_load_n(&c->done, __ATOMIC_ACQUIRE)) {
            if (!wait) return;
            int k = nthreads ? -1 : chunk_take(RP.gen);
            if (k >= 0) {
                chunk_record(&recs[0], k);
                continue;
            }
            plat_mutex_lock(&RP.m);
            while (!__atomic_load_n(&c->done, __ATOMIC_ACQUIRE)) plat_cond_wait(&RP.done, &RP.m);
            plat_mutex_unlock(&RP.m);
        }
        vk_completion_flush(&c->defer);
        vk_sync_part(c->cb);
        uint64_t t = kern_now_ns() - SC.t0;
        if (!SC.sub) SC.first_ns += t, SC.start0_ns += c->start;
        SC.last_ns += SC.sub == SC.nch - 1 ? t : 0;
        SC.rec_ns += c->ns;
        if (c->ns > SC.rec_max_ns) SC.rec_max_ns = c->ns;
        SC.sub++;
        n_parts++;
    }
}

// 1 when the replay ends an open pass at this op, so a chunk starting here splits no pass.
static int scan_ends_pass(const VkExec *s, uint32_t op, const void *a) {
    switch (op) {
    case OP_SET_RT: return memcmp(&s->rt, a, sizeof s->rt) != 0;
    case OP_BARRIER: case OP_DISPATCH: case OP_DISPATCH_IND: case OP_UPDATE_UB: case OP_COPY_B2B: case OP_COPY_B2T:
    case OP_COPY_T2B: case OP_COPY_T2T: case OP_CLEAR_TEX: case OP_CLEAR_BUF: return 1;
    default: return 0;
    }
}

// Places chunk nch at h; the chunk before it can be taken from now on.
static void chunk_start(const VkExec *s, int hi, const op_hdr *h) {
    Chunk *c = &SC.ch[SC.nch];
    c->hi = hi, c->op = h, c->upd_n = SC.nupd, c->cond_i = SC.ncond, c->job_i = SC.njobs, c->drop_n = SC.drop_n, c->pos = s->pos;
    c->wpos_i = SC.nwpos;
    c->done = 0, c->cb = VK_NULL_HANDLE, c->by = NULL;
    copy_state(&c->st, s);
    if (SC.nch) SC.ch[SC.nch - 1].job_end = SC.njobs, SC.ch[SC.nch - 1].wpos_end = SC.nwpos, SC.ch[SC.nch - 1].draws = SC.in_chunk;
    SC.in_chunk = 0;
    __atomic_store_n(&SC.nch, SC.nch + 1, __ATOMIC_RELEASE);
    if (SC.nch > 1) chunks_publish(0);
    if (SC.nch == 2) SC.per *= 4;   // the first chunk was a quarter
    chunks_submit(0);
}

// The chunks before chunk k are recorded.
static void chunks_wait(int k) {
    plat_mutex_lock(&RP.m);
    for (int i = 0; i < k; i++)
        while (!__atomic_load_n(&SC.ch[i].done, __ATOMIC_ACQUIRE)) plat_cond_wait(&RP.done, &RP.m);
    plat_mutex_unlock(&RP.m);
}

int vk_exec_chunk_behind(void) {
    for (int i = chunk_k - 1; i >= 0; i--)
        if (!__atomic_load_n(&SC.ch[i].done, __ATOMIC_ACQUIRE)) return 1;
    return 0;
}

// The chunks before take no lock this one holds and wait only for chunks before them, so they all get done: a chunk
// is taken only after every chunk before it was (chunk_take), and the DLSS records wait the same way (chunks_wait).
void vk_exec_chunk_settle(void) {
    if (chunk_k <= 0) return;
    uint64_t t0 = kern_now_ns();
    chunks_wait(chunk_k);
    __atomic_add_fetch(&SC.settles, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&SC.settle_ns, kern_now_ns() - t0, __ATOMIC_RELAXED);
}

static void scan_job(VkExecJob *j, const op_hdr *h) {
    static int full;
    j->chunk = SC.nch - 1;
    if (SC.njobs < MAX_JOBS) SC.jobs[SC.njobs] = *j, SC.jobs[SC.njobs++].op = h;
    else if (!full++) rt_log("vk_exec: more than %d planned records in a submission: the others are left out", MAX_JOBS);
    if (j->kind == VK_JOB_DLSS) scan_hold(HOLD_DLSS);   // the chunks after it read DLSS's output (vk_dlaa_swap_img)
}

// What vk_contact_pass_begin (pass) and vk_contact_flush do in the sequential replay, in replay order. A pass of the
// replay may start at any draw or clear here: the plan only looks at the targets, so asking again changes nothing.
static void scan_contact(const VkExec *s, int pass, const op_hdr *h) {
    VkExecJob j;
    if (vk_contact_plan(s, pass, &j)) scan_job(&j, h);
}

// What vk_exec_pass_begin asks of contact shadows and DLSS. DLSS's plan, too, gives the same answer for a pass asked
// again: it moves on only at the passes that arm, end or scale the scene.
static void scan_pass(const VkExec *s, const op_hdr *h) {
    scan_contact(s, 1, h);
    VkExecJob j;
    if (vk_dlaa_plan(s, &j)) scan_job(&j, h);
}

// A draw as prepare_draw sees it: its pass begins, and with it a contact shadow march or a DLSS record may be due.
static void scan_draw(VkExec *s, uint32_t op, const void *a, const op_hdr *h) {
    if (s->render_disabled) return;
    if (op == OP_DRAW && ((const p_draw *)a)->count <= 0) return;
    if (op == OP_DRAW_IDX && (((const p_draw_idx *)a)->itype < 0 || ((const p_draw_idx *)a)->itype > 2 ||
                              ((const p_draw_idx *)a)->count <= 0))
        return;
    if (op == OP_DRAW_IDX_IND && ((const p_draw_idx_ind *)a)->itype != NVN_INDEX_TYPE_UNSIGNED_SHORT &&
        ((const p_draw_idx_ind *)a)->itype != NVN_INDEX_TYPE_UNSIGNED_INT)
        return;
    if (s->pass_noatt) s->rt_resolved = 0;   // sized by this draw's viewport
    resolve_rt_as(s, 1);
    scan_pass(s, h);
    vk_dlaa_scan_draw(s);
}

static void scan(VkExec *s, int hi, const op_hdr *h, int depth) {
    for (;;) {
        check_op(h);
        const void *a = nvn_op_payload(h);
        if (h->op == OP_END) return;
        if (!depth && SC.in_chunk >= SC.per && SC.nch < SC.chunks &&
            (!SC.open || scan_ends_pass(s, h->op, a) || 2 * SC.in_chunk >= 3 * SC.per))
            chunk_start(s, hi, h);
        if (drop_op(h->op)) SC.drop_n++;
        s->pos++;
        if ((draw_op(h->op) && !s->render_disabled) || h->op == OP_DISPATCH || h->op == OP_DISPATCH_IND) scan_writes(s, h->op);
        switch (h->op) {   // the ops vk_exec_op_xfer and op_dispatch flush the contact shadows at
        case OP_DISPATCH: case OP_DISPATCH_IND: case OP_COPY_B2B: case OP_COPY_B2T: case OP_COPY_T2B: case OP_COPY_T2T:
        case OP_CLEAR_TEX: case OP_REPORT_COUNTER: case OP_RESET_COUNTER: case OP_RENDER_COND: case OP_CLEAR_BUF:
            scan_contact(s, 0, h);
            break;
        default: break;
        }
        switch (h->op) {
        case OP_BEGIN: break;
        case OP_JUMP: h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target; continue;
        case OP_CALL:
            if (depth < CALL_DEPTH) scan(s, hi, nvn_op_first(((const p_jump *)a)->target), depth + 1);   // else walk stops
            break;
        case OP_SET_RT:
            if (memcmp(&s->rt, a, sizeof s->rt)) {
                s->rt = *(const p_set_rt *)a, s->rt_resolved = 0, SC.open = 0;
                resolve_rt_as(s, 1);   // here, not at the first draw: a clear or a later chunk may need them live
                chunks_publish(0);
            }
            break;
        case OP_CLEAR_COLOR: case OP_CLEAR_DS:
            SC.open |= !s->render_disabled;
            if (!s->render_disabled) resolve_rt_as(s, 1), scan_pass(s, h);
            break;
        case OP_DRAW: case OP_DRAW_IDX: case OP_DRAW_IDX_IND:
            SC.in_chunk++, SC.open = 1;
            scan_draw(s, h->op, a, h);
            break;
        case OP_DISPATCH: case OP_DISPATCH_IND: SC.in_chunk++, SC.open = 0; break;
        case OP_UPDATE_UB: {   // listed where op_update_ub notes it
            const p_update_ub *u = a;
            VkMemSpan sp;
            if (u->size && vk_mem_resolve(u->dst, u->size, &sp)) {
                SC.upd[SC.nupd++] = (VkExecUpd){u->dst, u->dst + u->size, u->data};
                scan_drop(u->dst, u->size);
            }
            SC.open = 0;
            break;
        }
        case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
        case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG:
            vk_exec_bind(s, h->op, a);
            break;
        case OP_COPY_B2B: case OP_COPY_T2B: case OP_CLEAR_BUF: {
            uint64_t at, size = vk_exec_xfer_written(s, h->op, a, &at);
            if (size) scan_drop(at, size);
            if (vk_exec_xfer_revive(h->op, a, s->pos)) scan_hold(HOLD_TEX);
            SC.open = 0;
            break;
        }
        case OP_BARRIER: case OP_COPY_B2T: case OP_COPY_T2T: case OP_CLEAR_TEX:
            if (vk_exec_xfer_revive(h->op, a, s->pos)) scan_hold(HOLD_TEX);
            SC.open = 0;
            break;
        case OP_SIGNAL_EVENT: break;
        case OP_RENDER_COND: SC.cond[SC.ncond++] = (uint8_t)vk_exec_xfer_scan(s, h->op, a); break;
        case OP_REPORT_COUNTER: case OP_RESET_COUNTER: case OP_RENDER_ENABLE: vk_exec_xfer_scan(s, h->op, a); break;
        default: state_op(s, h->op, a);   // walk stops on an op without a handler
        }
        h = nvn_op_next(h);
    }
}

// The queue state of a and b without what resolve_rt derives from rt and the images.
static int state_differs(const VkExec *a, const VkExec *b) {
    static VkExec x, y;
    copy_state(&x, a), copy_state(&y, b);
    VkExec *e[2] = {&x, &y};
    for (int i = 0; i < 2; i++) {
        e[i]->rt_resolved = 0;
        memset(e[i]->color, 0, sizeof e[i]->color), memset(&e[i]->depth, 0, sizeof e[i]->depth);
        e[i]->pass_scale = e[i]->pass_w = e[i]->pass_h = e[i]->pass_layers = 0, e[i]->pass_noatt = 0;
        memset(e[i]->pass_color, 0, sizeof e[i]->pass_color);
        e[i]->pass_depth = e[i]->pass_stencil = VK_FORMAT_UNDEFINED, e[i]->pass_ncolor = e[i]->pass_samples = 0;
        memset(e[i]->pipe.color_fmt, 0, sizeof e[i]->pipe.color_fmt);
        e[i]->pipe.depth_fmt = 0, e[i]->pipe.samples = 0;
    }
    const uint8_t *p = (const uint8_t *)&x, *q = (const uint8_t *)&y;
    for (size_t o = VK_EXEC_STATE_BEGIN; o < VK_EXEC_STATE_END; o++)
        if (p[o] != q[o]) return (int)o;
    return 0;
}

static void record_chunks(VkExec *ex, const uint64_t *handles, int n, const OpCounts *cnt) {
    uint64_t t0 = SC.t0 = kern_now_ns();
    SC.prep_ns += t0 - SC.rec_t0;
    recs_start();
    if (!SC.ch && !(SC.ch = calloc((size_t)SC.chunks + 1, sizeof *SC.ch))) rt_fatal("vk: out of memory");
    while (SC.cupd < cnt->upd) SC.upd = grow_n(SC.upd, &SC.cupd, sizeof *SC.upd);
    while (SC.ccond < cnt->cond) SC.cond = grow_n(SC.cond, &SC.ccond, sizeof *SC.cond);
    while (SC.cwpos < cnt->draws) SC.wpos = grow_n(SC.wpos, &SC.cwpos, sizeof *SC.wpos);
    vk_exec_oq_flush(ex);   // queries a sequential submission closed count before the chunks'
    VkExec *s = &SC.s;
    copy_state(s, ex);
    s->fence = ex->fence, s->rt_resolved = 0, s->pos = 0;
    SC.holds = 0;
    SC.nch = SC.sub = SC.pub = 0, SC.nupd = SC.ncond = SC.njobs = SC.drop_n = SC.ndr = SC.dr_done = 0, SC.open = 0;
    SC.nwpos = 0;
    job_turn = 0;
    SC.per = (SC.per + 3) / 4;
    plat_mutex_lock(&RP.m);
    RP.handles = handles, RP.n = n, RP.fence = ex->fence, RP.prep_next = 0, RP.prep_left = 2;
    if (!++RP.gen) RP.gen = 1;
    __atomic_store_n(&RP.take, (uint64_t)RP.gen << 32, __ATOMIC_RELEASE);
    plat_cond_broadcast(&RP.go);
    plat_mutex_unlock(&RP.m);
    if (!nthreads) prep_run();
    chunk_start(s, 0, nvn_op_first(handles[0]));
    for (int i = 0; i < n; i++) scan(s, i, nvn_op_first(handles[i]), 0);
    SC.ch[SC.nch - 1].job_end = SC.njobs, SC.ch[SC.nch - 1].wpos_end = SC.nwpos, SC.ch[SC.nch - 1].draws = SC.in_chunk;
    SC.scan_ns += kern_now_ns() - t0;
    prep_run();   // threads that wake late leave it to the scan's end
    while (__atomic_load_n(&RP.prep_left, __ATOMIC_ACQUIRE)) plat_yield();
    chunks_publish(1);
    SC.chunked++, SC.nchunks += (uint64_t)SC.nch;
    chunks_submit(1);
    x_cur = ex;
    for (int i = 0; i < nrecs; i++) frame_add(&recs[i].ex);
    int o = SC.check ? state_differs(SC.ch[SC.nch - 1].by, s) : 0;
    if (o && SC.bad++ < 16)
        rt_log("vk_exec: chunked replay: the state after the last chunk differs from the scan's at byte %d of VkExec", o);
    copy_state(ex, s);
    ex->rt_resolved = 0;
}

int vk_exec_replay_workers(void) { return SC.workers; }

void vk_exec_contexts(void (*fn)(VkExec *ex)) {
    fn(&X);
    for (int i = 0; i < nrecs; i++) fn(&recs[i].ex);
}

static int chunked_on(void) {
    if (SC.workers < 0) {
        SC.workers = cfg_int("TOTK_REPLAY_WORKERS");
        if (SC.workers < 0) SC.workers = 0;
        SC.chunks = cfg_int("TOTK_REPLAY_CHUNKS");
        if (SC.chunks <= 0) SC.chunks = 2 * SC.workers;
        SC.min_draws = cfg_int("TOTK_REPLAY_CHUNK_DRAWS");
        SC.check = cfg_bool("TOTK_REPLAY_STATE_CHECK");
        SC.skew_us = cfg_int("TOTK_REPLAY_SKEW_US");
        if (SC.workers && SC.skew_us > 0)
            rt_log("vk: TOTK_REPLAY_SKEW_US: the even chunks wait %d us before they record", SC.skew_us);
        if (SC.workers)
            rt_log("vk: replay in up to %d chunks per submission of at least %d draws each, %d recording thread%s%s",
                   SC.chunks, SC.min_draws, SC.workers, SC.workers > 1 ? "s" : "", SC.check ? ", state checked" : "");
    }
    return SC.workers > 0 && SC.chunks > 1 && !vk_dump_armed;
}

int vk_exec_record(VkCommandBuffer cb, const uint64_t *handles, int n, uint64_t fence) {
    VkExec *ex = &X;
    SC.rec_t0 = kern_now_ns();
    if (!x_ready) state_reset(ex), vk_dump_init(), x_ready = 1;
    const tex_t *keep[9];   // the render targets carry over to this submission (set_rt compares them by pointer)
    memcpy(keep, ex->rt.color, sizeof ex->rt.color);
    keep[8] = ex->rt.depth;
    nvn_pins_sweep(nvn_submit_count(), keep, 9);
    ex->cb = cb;
    ex->fence = fence;
    ex->recorded = 0;
    ex->in_pass = 0;
    ex->mark = NULL;
    ex->mark_passes = ex->mark_dispatches = 0;
    ex->dirty = VK_EXEC_DIRTY_ALL;
    ex->rt_resolved = 0;   // images may have changed since the last command buffer
    ex->nviews = 0;
    ex->barrier_at = -1;
    ex->nfbl = 0;   // the previous command buffer ended with a full barrier: every image is in GENERAL
    ex->fr.submits++;
    ex->split_draws = ex->part_start = 0;
    ex->part_t0 = kern_now_ns();
    x_cur = ex;
    for (int i = 0; i < n; i++)
        if (!handles[i]) rt_fatal("vk: command handle %d of a submission is null", i);
    OpCounts cnt = {0};
    if (n && chunked_on())
        for (int i = 0; i < n; i++) count_ops(nvn_op_first(handles[i]), 0, &cnt);
    SC.subs += n != 0;
    if (cnt.draws && cnt.draws >= 2 * (uint32_t)SC.min_draws) {
        SC.per = (int)((cnt.draws * 4 + 4u * SC.chunks - 4) / (4u * SC.chunks - 3));   // the first chunk a quarter
        if (SC.per < SC.min_draws) SC.per = SC.min_draws;
        record_chunks(ex, handles, n, &cnt);
    } else {
        if (n) vk_mem_mirror_begin(), vk_usnap_begin(handles, n, fence);
        for (int i = 0; i < n; i++) walk(ex, nvn_op_first(handles[i]), 0, NULL);
    }
    vk_exec_pass_end(ex);
    x_cur = NULL;
    if (ex->recorded) vk_exec_barrier(ex);   // later submissions see this one's writes
    if (n) ex->recorded += vk_mem_mirror_end(ex->cb, fence), vk_usnap_end(fence);   // VRAM mirror bytes the GPU wrote go back to guest memory
    views_release(ex);
    ex->cb = VK_NULL_HANDLE;
    frame_add(ex);
    if (n) n_subs++;
    if (n && (rt_trace & RT_TRACE_PERF)) vk_exec_ub_perf();
    if ((rt_trace & RT_TRACE_PERF) && n_subs >= 240) {
        rt_log("perf: replay command buffer parts per submission %.2f", (double)(n_subs + n_parts) / (double)n_subs);
        if (SC.chunked) {
            double k = 1e6 * (double)SC.chunked;
            rt_log("perf: chunked replay: %llu of %llu submissions, %.2f chunks each; per submission %.3f ms before the scan, "
                   "scan %.3f ms, first chunk begun at %.3f ms and submitted at %.3f ms, last at %.3f ms, recording %.3f ms "
                   "in all, longest chunk %.3f ms; beside the scan: mirrors %.3f ms, uniform snapshot %.3f ms, both done at %.3f ms",
                   (unsigned long long)SC.chunked, (unsigned long long)SC.subs, (double)SC.nchunks / SC.chunked,
                   SC.prep_ns / k, SC.scan_ns / k, SC.start0_ns / k, SC.first_ns / k, SC.last_ns / k, SC.rec_ns / k,
                   SC.rec_max_ns / 1e6, SC.mirror_ns / k, SC.usnap_ns / k, SC.ready_ns / k);
            rt_log("perf: chunked replay: chunks held per submission: %.2f for a new texture, %.2f for DLSS, %.2f for "
                   "unknown storage writes", SC.held[HOLD_TEX] / (double)SC.chunked, SC.held[HOLD_DLSS] / (double)SC.chunked,
                   SC.held[HOLD_WRITES] / (double)SC.chunked);
            rt_log("perf: chunked replay: %llu chunks waited for the chunks before them to make a texture (%.3f ms in all)",
                   (unsigned long long)__atomic_exchange_n(&SC.settles, 0, __ATOMIC_RELAXED),
                   __atomic_exchange_n(&SC.settle_ns, 0, __ATOMIC_RELAXED) / 1e6);
            memset(SC.held, 0, sizeof SC.held);
            char b[768];
            int o = 0;
            for (int i = 0; i < SC.nch && o < 700; i++)
                o += snprintf(b + o, sizeof b - o, " %d:%.2f@%.2f%s", SC.ch[i].draws, SC.ch[i].ns / 1e6, SC.ch[i].start / 1e6,
                              SC.holds >> i & 1 ? "h" : "");
            rt_log("perf: chunked replay: the last submission's chunks (draws:ms@begun, h held)%s", b);
        }
        n_subs = n_parts = 0;
        SC.subs = SC.chunked = SC.nchunks = SC.scan_ns = SC.first_ns = SC.last_ns = SC.rec_ns = SC.rec_max_ns = 0;
        SC.prep_ns = SC.start0_ns = SC.mirror_ns = SC.usnap_ns = SC.ready_ns = 0;
    }
    return ex->recorded - ex->part_start;   // what the last part holds
}
