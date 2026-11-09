// Vulkan textures of the graphics layer: a VkImage cache keyed by storage
// GPU address plus descriptor, uploads through staging buffers with block-linear deswizzle and ASTC decode (vk_astc.c),
// re-uploads when page write generations (vk_memory.c) show the storage written, ReadTexels / WriteTexels, the texture
// and sampler descriptor slots, and VkSamplers.
//
// Behaviour ported from MacNativeGames games/totk/third_party/d2r_gfx/gfx/nvn @ b75be8cf (MIT, Copyright (c) 2026
// jhaizhou; origin d2r-mobile): which textures mirror their pool memory (nvn_mtl.m tex_mirrored, 3347-3362), the
// "GPU-owned" rule that stops re-uploads once the GPU rendered into a mirrored texture (nvn_mtl.m tex_mtl, 3366-3372),
// the live texture at a storage address that a slot of a finalized texture follows (hle_nvn.c:373-393), and
// ReadTexels / WriteTexels (nvn_mtl.m:10105-10195). Image creation, upload regions and samplers are ported from LAS
// Recomp runtime/nvn/vulkan/textures.c:235-330, 778-844 @ da35b83a (MIT, Copyright (c) 2026 Peiikai).
//
// Every image lives in VK_IMAGE_LAYOUT_GENERAL. Uploads are recorded into the upload batch of vk_memory.c; the replay
// submits it (vk_upload_flush) before a submission that samples the textures.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "nvn_hmap.h"
#include "nvn_tex.h"
#include "tex.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_exec.h"
#include "platform/platform.h"

typedef struct {
    uint64_t addr;
    nvn_texdesc d;
    uint8_t has_layout, layout[8], pad[7];
} TexKey;

typedef struct Tex {
    VkTexImage pub;               // first: vk_texture_get hands out &pub
    TexKey key;
    tex_t t;                      // the newest texture object seen with this key
    TexVkFormat f;
    VmaAllocation alloc;          // own memory; NULL when bound into pool memory
    VmaAllocation bound_to;       // the pool memory it is bound into
    uint32_t wgen;                // page generation of the last upload check
    uint32_t checked;             // submit_epoch of the last check from a draw binding
    int uploaded, released;
    uint32_t rev_epoch, rev_pos;  // the state scan made it live in that submission at that op (vk_texture_revive)
    uint32_t own_epoch, own_pos;  // a chunk made it GPU-owned in that submission at that op (vk_texture_gpu_written)
    uint32_t n_up;                // uploads of this entry (vk_texture_debug)
    int last_ch;                  // last vk_mem_changed result of make_fresh: 1 written, 0 clean, -1 not tracked
    struct Tex *next_at;          // other entries at the same storage address
    uint8_t gpu_dirty;            // mirrored, and the GPU wrote the image after the last write-back to the storage
    uint32_t gpu_gen;             // page generation when the GPU write that set gpu_dirty was recorded
    uint64_t gpu_seq;             // order of that GPU write among all of them (gpu_writes)
    uint64_t gpu_fence;           // fence of the submission that made that write (0: not recorded in one)
    uint64_t run_frame;           // mirrored: the frame of the newest GPU write
    uint8_t run;                  // frames in a row up to run_frame with a GPU write, counted up to WB_RUN
    uint64_t wr_fence;            // any entry: newest submission recorded to write its image (render, copy, clear,
                                  // storage binding); ReadTexels waits for it instead of draining the replay queue
    uint8_t listed;               // in owned[]
    uint8_t *snap;                // a texture that fills no page: its storage bytes when the GPU write was recorded
    uint32_t wt_n;                // WriteTexels into the storage since the last upload
    struct Tex *rel_prev, *rel_next;   // while released: the list of released entries, oldest first
    uint64_t rel_ms;              // while released: when (coarse_ms)
    uint64_t use_seq;             // order of the last creation or take-back among all entries
    uint64_t rel_seq;             // while released: use_seq of the newest entry placed before this release; an entry
                                  // with a later use_seq on overlapping storage replaced this one's bytes
} Tex;

// A slot keeps what vk_texture_by_id resolved for it, so a draw binding skips the key lookup and the address chain: the
// entry (ce), valid while the generation of its address (agen) is still cgen, the view and has_view handed out with it,
// and the submit_epoch of the last freshness check through the slot (cepoch). Hot fields first: a lookup that hits
// touches one or two cache lines.
typedef struct {
    struct Tex *ce;               // NULL: nothing cached (registered since, or never resolved)
    uint32_t cgen;                // agen of the slot's address when ce was resolved
    uint32_t cepoch;
    view_t cview;
    uint8_t chas_view, used, has_view;
    view_t v;
    tex_t t;
} TexSlot;
typedef struct { samp_par p; VkSampler s, sp; uint8_t used; } SampSlot;   // s: vk_sampler_for(&p) once looked up; sp:
                                                                           // its point-sampled form (vk_sampler_point_by_id_locked)

static PlatRMutex tex_lock;
static PlatOnce tex_once = PLAT_ONCE_INIT;
static nvn_hmap by_key = {.klen = sizeof(TexKey)};
static nvn_hmap by_addr = {.klen = 8};
static TexSlot *tslots;
static int ntslots;
static SampSlot *sslots;
static int nsslots;
static nvn_hmap samplers = {.klen = sizeof(samp_par)};
static int nreleased;
static Tex *rel_head, *rel_tail;   // released entries in release order (sweep_released drops from the head)
static uint64_t n_uploads, n_reuploads;
// Re-uploads since the last perf line (tex_lock): all, and those of textures a WriteTexels wrote since their last upload.
static uint64_t ru_n, ru_bytes, ru_ns, ru_wt_n, ru_wt_bytes, ru_wt_ns;
// TOTK_TEX_ALIAS (default on; =0: a new image over live storage starts empty): a new descriptor over the storage of a
// live image that the GPU wrote gets that image's content (alias_copy). Counts since the last log line.
static int tex_alias = -1;
static uint64_t n_alias_copied, n_alias_upload, n_alias_uninit;
static uint32_t submit_epoch = 1;   // bumped per queue submit: draw bindings check a texture's storage once per submit
// Mirrored textures the GPU wrote into, sorted by address (vk_texture_cpu_stale, vk_texture_write_back). Changed under
// tex_lock and owned_lock; read under either.
static PlatMutex owned_lock = PLAT_MUTEX_INIT;
static Tex **owned;
static int nowned, cowned;
static uint64_t max_owned;   // largest storage size in owned[]
static uint64_t gpu_writes;  // GPU writes into mirrored textures so far: Tex.gpu_seq
// Bumped whenever the entry a slot resolves to can change: an entry created or destroyed, released or taken back into
// use (the slot lookup and the follow of a finalized slot read only by_key, by_addr and the released flags).
static uint32_t tex_gen = 1;
// Bumped with tex_gen and whenever a slot is registered: what vk_texture_by_id returns for any slot may have changed.
static uint32_t slots_gen = 1;
static uint32_t samp_gen = 1;   // bumped whenever a sampler slot is registered: vk_sampler_by_id may answer otherwise
static uint64_t n_checks;    // draw-binding storage checks (make_fresh_snap) since the last perf line
// tex_gen per storage address (hashed): a slot resolves through the entries at its own address only (slot_resolve), so
// a slot's resolution stays valid while its address's generation does. TotK finalizes and initializes transient
// textures every frame: about 90 tex_gen and 160 slots_gen bumps per frame in the monsters fight, one every 16 draws,
// which emptied every cache keyed on the global generations.
#define AGEN_N 4096
static uint32_t agen[AGEN_N];
static uint32_t arev[AGEN_N];   // per storage address (hashed): the submit_epoch of the state scan's last revival there
// Per slot id, for vk_texture_slot_token without the lock: the registration serial (bits 16-47; 0: never registered)
// and the address generation index (bits 0-15), one word so a reader gets a consistent pair. Fixed-size (slot ids are
// 20 bits), unlike tslots, which moves when it grows.
static uint64_t *sinfo;
static uint32_t *sepoch;   // per slot: the submit_epoch its entry was checked in (vk_texture_slot_checked), 0 none
static uint32_t reg_serial;

static uint32_t agen_idx(uint64_t addr) { return (uint32_t)(addr * 0x9E3779B97F4A7C15ull >> 52); }

static void tex_gen_bump(uint64_t addr) {
    if (!++tex_gen) tex_gen = 1;
    slots_gen++;
    uint32_t *g = &agen[agen_idx(addr)];
    __atomic_store_n(g, *g + 1, __ATOMIC_RELEASE);
}

static volatile int32_t tex_ready;
static void tex_init(void *p) {
    (void)p;
    __atomic_store_n(&tex_ready, 1, __ATOMIC_RELEASE);
    return;
}
static _Thread_local int tex_depth;   // how often this thread holds tex_lock (entry_of releases it for a wait)
static _Thread_local uint32_t tex_settles;   // entry_of waits of this thread (vk_texture_by_id_locked)
static void lock(void) {
    if (!__atomic_load_n(&tex_ready, __ATOMIC_ACQUIRE)) plat_once(&tex_once, tex_init, NULL);
    plat_rmutex_lock(&tex_lock);
    tex_depth++;
}
static void unlock(void) {
    tex_depth--;
    plat_rmutex_unlock(&tex_lock);
}

static uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }
static uint32_t mip(uint32_t v, unsigned lv) { return umax(v >> lv, 1); }

static void make_key(const tex_t *t, TexKey *k) {
    memset(k, 0, sizeof *k);
    k->addr = (uintptr_t)t->mem;
    k->d = t->d;
    k->has_layout = t->has_layout;
    memcpy(k->layout, t->layout, 8);
}

// Keys equal apart from the descriptor's swizzle selectors: the same storage, format and geometry.
// The colour format without its sRGB decode: a target and its sRGB view share one texel layout (slot_resolve).
static int linear_format(int f) {
    switch (f) {
    case NVN_FORMAT_RGBA8_SRGB: return NVN_FORMAT_RGBA8;
    case NVN_FORMAT_RGBX8_SRGB: return NVN_FORMAT_RGBX8;
    case NVN_FORMAT_BGRA8_SRGB: return NVN_FORMAT_BGRA8;
    case NVN_FORMAT_BGRX8_SRGB: return NVN_FORMAT_BGRX8;
    default: return f;
    }
}

static int same_but_swizzle(const TexKey *a, const TexKey *b) {
    TexKey x = *a, y = *b;
    x.d.swizzle_r = x.d.swizzle_g = x.d.swizzle_b = x.d.swizzle_a = 0;
    y.d.swizzle_r = y.d.swizzle_g = y.d.swizzle_b = y.d.swizzle_a = 0;
    return !memcmp(&x, &y, sizeof x);
}

// MNG nvn_mtl.m tex_mirrored: textures whose content the CPU provides in pool memory. Display, compressible, zcull,
// depth, multisample and GPU-only storage stays GPU-owned.
static int tex_mirrored(const tex_t *t) {
    if (!t->mem || nvn_gpu_only((uintptr_t)t->mem)) return 0;
    if (t->has_layout) return 1;
    if (!(t->pool_flags & (NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_CPU_CACHED))) return 0;
    if (t->d.flags & (NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_ADAPTIVE_ZCULL |
                      NVN_TEXTURE_FLAGS_DEPTH16_PREFER_FAST_CLEAR | NVN_TEXTURE_FLAGS_ZCULL_SUPPORT_STENCIL))
        return 0;
    if (t->d.samples > 1 || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return 0;
    if (t->d.format >= 0x31 && t->d.format <= 0x36) return 0;   // stencil / depth formats
    return 1;
}

static void log_limited(volatile int32_t *n, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void log_limited(volatile int32_t *n, const char *fmt, ...) {
    if (plat_inc32(n) > 16) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rt_log("vk: %s", buf);
}

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                    VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
#define ALL_RW VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT

// ---------------------------------------------------------------- images
static VkFormatFeatureFlags features_of(VkFormat f) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(vk_ctx()->phys, f, &p);
    return p.optimalTilingFeatures;
}

static VkImageAspectFlags aspect_of(const TexVkFormat *f) {
    VkImageAspectFlags a = 0;
    if (f->depth) a |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (f->stencil) a |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return a ? a : VK_IMAGE_ASPECT_COLOR_BIT;
}

// Binds img into the pool memory behind its GPU address (physical pools, virtual pools through the page table) when
// the memory type, alignment and contiguous size allow; 0 otherwise.
// A depth / stencil image that needs more bytes than its NVN storage (guest_size) is not bound: AMD's RDNA2 driver
// (26.10.1) puts the depth compression metadata after the depth data, and above 1080p that tail lies over the next
// texture of the game's heap; each depth write then scrambled it (garbage band at the top, pink glow, blocks in the
// grass, a blue checkered scene in the menu). Colour images keep their place: the RX 9070 also sizes some of them
// above their storage, and nothing there showed that the GPU writes the excess.
static int format_compressed(VkFormat f) {   // vkCmdClearColorImage takes no block-compressed format
    return (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) ||
           (f >= VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK && f <= VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK);
}

static int bind_pool_memory(VkCtx *c, Tex *e, VkImage img, uint64_t guest_size) {
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(c->device, img, &req);
    if ((e->f.depth || e->f.stencil) && req.size > guest_size) return 0;
    VkMemSpan sp;
    if (!req.size) return 0;   // an image the device could not lay out (image_size)
    if (!vk_mem_resolve(e->pub.addr, req.size, &sp) || !sp.alloc || sp.alloc_offset % req.alignment) return 0;
    VmaAllocationInfo info;
    vmaGetAllocationInfo(c->vma, sp.alloc, &info);
    if (!(req.memoryTypeBits >> info.memoryType & 1)) return 0;
    if (vmaBindImageMemory2(c->vma, sp.alloc, sp.alloc_offset, img, NULL) != VK_SUCCESS) return 0;
    e->bound_to = sp.alloc;
    return 1;
}

// agl's BC4 encoder draws a BC texture's blocks through an RGBA16UI view, one texel per 8-byte block (refs/nxnative
// docs/agents/04-graphics.md, Breath of the Wild and Tears of the Kingdom sections). Vulkan does it with an uncompressed
// view of a BLOCK_TEXEL_VIEW_COMPATIBLE image; EXTENDED_USAGE lets the BC image carry the COLOR_ATTACHMENT usage that
// only such views support. Whether the device takes that combination for this format and shape:
static int block_views_ok(const VkImageCreateInfo *ci) {
    VkPhysicalDeviceImageFormatInfo2 fi = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                                           .format = ci->format, .type = ci->imageType, .tiling = ci->tiling,
                                           .usage = ci->usage, .flags = ci->flags};
    VkImageFormatProperties2 p = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    if (vkGetPhysicalDeviceImageFormatProperties2(vk_ctx()->phys, &fi, &p) != VK_SUCCESS) return 0;
    const VkImageFormatProperties *q = &p.imageFormatProperties;
    return ci->extent.width <= q->maxExtent.width && ci->extent.height <= q->maxExtent.height &&
           ci->extent.depth <= q->maxExtent.depth && ci->mipLevels <= q->maxMipLevels &&
           ci->arrayLayers <= q->maxArrayLayers;
}

// The memory the device wants for an image made from ci: 0 when it cannot lay it out. RADV on GFX9 (Vega, Renoir)
// hands back a 0-byte image for a mipmapped block-compressed 3D image at least as deep as it is wide in blocks, with
// COLOR_ATTACHMENT usage and no STORAGE: that usage asks for a thin 3D layout (Mesa radv_image.c), for which the GFX9
// address library has no swizzle mode left (gfx9addrlib.cpp), and the error is dropped; VMA then refuses the 0-byte
// allocation. The format properties call it supported, so only the size shows it.
static VkDeviceSize image_size(const VkImageCreateInfo *ci) {
    VkDeviceImageMemoryRequirements q = {.sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, .pCreateInfo = ci};
    VkMemoryRequirements2 r = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetDeviceImageMemoryRequirements(vk_ctx()->device, &q, &r);
    return r.memoryRequirements.size;
}

// UNDEFINED -> GENERAL; new depth / stencil images start cleared (LAS textures.c: the game copies depth
// without stencil, and the stencil of those images must not be whatever the memory held).
static void init_image(Tex *e, VkCommandBuffer cb) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = e->pub.image, .subresourceRange = {e->pub.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
    if (e->f.depth || e->f.stencil) {
        VkClearDepthStencilValue zero = {0.0f, 0};
        VkImageSubresourceRange rg = {e->pub.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearDepthStencilImage(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        barrier(cb, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    } else if (vk_fix_zero() && !e->bound_to && !e->pub.mirrored && !format_compressed(e->f.vk)) {
        // GPU-only storage that got memory of its own starts at zero, as the pool it stands for (vk_memory.c
        // alloc_physical). Images aliasing pool memory are not cleared: that would erase what other textures rendered
        // there; mirrored images are uploaded from their storage on first use.
        VkClearColorValue zero = {0};
        VkImageSubresourceRange rg = {e->pub.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        barrier(cb, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    }
}

// TOTK_SHADOW_SCALE (1 to 4): the game's shadow maps are made this many times larger on each side. TotK renders its
// sun cascades into a 960x960 DEPTH16 2D array of 4 layers and its static shadows into 1440x1440 and 720x720 arrays of 2,
// whatever the output resolution, and shrinks the cascades under load (872, 848, 800, 480 and so on). Those are made
// larger here: render passes into them scale the game's viewports and scissors (vk_exec.c resolve_rt, pass_scale),
// copies scale their regions (vk_exec_xfer.c sub_resolve), and shaders sample them with normalised coordinates, so the
// game sees nothing of it. A map whose storage the CPU writes (mirrored) or that aliases pool memory keeps its size,
// as its texels must match the guest layout.
static int shadow_scale(void) {
    static int scale = -1;
    if (scale < 0) {
        int s = cfg_int("TOTK_SHADOW_SCALE");
        s = s < 1 ? 1 : s > 4 ? 4 : s;
        rt_log("vk: shadow map scale %d", s);
        scale = s;
    }
    return scale;
}

static int shadow_scale_of(const nvn_texdesc *d, const TexVkFormat *f, uint32_t samples, int mirrored) {
    int scale = shadow_scale();
    if (scale == 1 || mirrored || samples > 1 || d->format != NVN_FORMAT_DEPTH16 || !f->depth) return 1;
    if (d->target != NVN_TEXTURE_TARGET_2D_ARRAY || d->width != d->height || d->width < 256 || d->depth < 2 || d->depth > 4)
        return 1;
    uint32_t most = vk_ctx()->props.limits.maxImageDimension2D;
    return d->width * (uint32_t)scale <= most ? scale : 1;
}

// defer_init: the caller records the image's first transition (init_image or alias_copy).
static Tex *create_tex(const tex_t *t, const TexKey *k, int defer_init) {
    static volatile int32_t n_unmapped, n_unsupported, n_failed, n_unbound, n_thick;
    VkCtx *c = vk_ctx();
    const nvn_texdesc *d = &t->d;
    if (!c || d->target == NVN_TEXTURE_TARGET_BUFFER) return NULL;
    TexVkFormat f;
    if (!tex_vk_format(d->format, &f)) {
        log_limited(&n_unmapped, "texture format %#x has no Vulkan format: %s", d->format, f.reason);
        return NULL;
    }
    if (!tex_vk_format_supported(&f)) {
        log_limited(&n_unsupported, "texture format %#x: VkFormat %d lacks features %#x", d->format, (int)f.vk, (unsigned)f.need);
        return NULL;
    }
    nvn_texlayout L;
    nvn_tex_layout(d, &L);
    uint32_t w = umax(d->width, 1), h = umax(d->height, 1), depth = 1, layers = 1;
    VkImageType type = VK_IMAGE_TYPE_2D;
    switch (d->target) {
    case NVN_TEXTURE_TARGET_1D: type = VK_IMAGE_TYPE_1D; h = 1; break;
    case NVN_TEXTURE_TARGET_1D_ARRAY: type = VK_IMAGE_TYPE_1D; h = 1; layers = L.layers; break;
    case NVN_TEXTURE_TARGET_3D: type = VK_IMAGE_TYPE_3D; depth = umax(d->depth, 1); break;
    case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY:
    case NVN_TEXTURE_TARGET_CUBEMAP: case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: layers = L.layers; break;
    default: break;
    }
    int scale = shadow_scale_of(d, &f, d->samples > 1 ? (uint32_t)d->samples : 1, tex_mirrored(t));
    w *= (uint32_t)scale, h *= (uint32_t)scale;
    uint32_t levels = umax(d->levels, 1), most = 1;
    for (uint32_t s = umax(umax(w, h), depth); s > 1; s >>= 1) most++;
    if (levels > most) levels = most;
    uint32_t samples = d->samples > 1 ? (uint32_t)d->samples : 1;
    if (samples > 1) levels = 1;
    VkFormatFeatureFlags ff = features_of(f.vk);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    int ds = f.depth || f.stencil;
    if (ds && (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (!ds && (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((d->flags & NVN_TEXTURE_FLAGS_IMAGE) && (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && samples == 1)
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    // contact shadows (vk_contact.c) darken the game's B10G11R11 scene colour in place from a compute shader
    if (f.vk == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && samples == 1 &&
        (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && w >= 640 && vk_contact_strength())
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageCreateFlags flags = ds ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;   // format-reinterpreting views
    // Cube compatible only for cube targets: the replay makes cube views of nothing else (vk_exec_draw.c get_view checks
    // the image's own view type). The flag on every square array of 6+ layers changed how AMD's Windows driver lays such
    // an image out, and terrain material arrays sampled through it came out in rainbow blocks that moved with the mip
    // level (Savage Wind Recomp issue #59, BotW on an RX 580 and a 7800 XT).
    if (type == VK_IMAGE_TYPE_2D && layers >= 6 && w == h && samples == 1 &&
        (d->target == NVN_TEXTURE_TARGET_CUBEMAP || d->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY))
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    // NVN renders into a 3D texture slice by slice (a view's min layer is the depth plane, nvn_mtl.m), e.g. the
    // grading LUT of the menu background blur. Vulkan needs a 2D view of the slice for that.
    if (type == VK_IMAGE_TYPE_3D && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = type, .format = f.vk,
                            .extent = {w, h, depth}, .mipLevels = levels, .arrayLayers = layers,
                            .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    int bw, bh, block_views = 0;
    // TOTK_BLOCK_VIEWS_3D (default 0): a 3D BC image also gets block views. On RDNA2 cards (driver 26.10.1, issue #5)
    // the 32x32x32 BC4 volume the game creates at the logo, made BLOCK_TEXEL_VIEW_COMPATIBLE and 2D_ARRAY_COMPATIBLE,
    // took a GPU write just past the end of its memory: device lost at the first submit after it when the image ended
    // its memory block. Draws into a 3D BC image go through a stand-in instead (vk_exec.c bc_target).
    static int bv3d = -1;
    if (bv3d < 0) {
        bv3d = rt_knob("BLOCK_VIEWS_3D", "0");
        rt_log("vk: 3D BC textures %s (TOTK_BLOCK_VIEWS_3D=%d)", bv3d ? "get block views (renderable)" : "have no block views",
               bv3d);
    }
    if (!ds && samples == 1 && f.vk == f.ideal && nvn_fmt_bpb(d->format, &bw, &bh) && bw == 4 && bh == 4 &&
        (type != VK_IMAGE_TYPE_3D || bv3d)) {
        VkImageCreateInfo bc = ci;   // BC formats: renderable through views of their blocks (block_views_ok)
        bc.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        bc.flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        if (type == VK_IMAGE_TYPE_3D) bc.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        VkImageUsageFlags hidden = 0;   // creation usage the replay never uses (views restrict theirs, vk_exec.c)
        int ok = block_views_ok(&bc);
        if (ok && type == VK_IMAGE_TYPE_3D && !image_size(&bc)) {
            // STORAGE usage keeps RADV on GFX9 from the thin layout (image_size), so agl's BC4 encoder still renders
            // into the volume; else the image is made without block views and its encoder passes are left out
            bc.usage |= VK_IMAGE_USAGE_STORAGE_BIT, hidden = VK_IMAGE_USAGE_STORAGE_BIT;
            ok = block_views_ok(&bc) && image_size(&bc);
            log_limited(&n_thick, "texture fmt %#x %ux%ux%u: the device cannot lay out a 3D image with block views; %s",
                        d->format, w, h, depth, ok ? "made with storage usage (a thick layout)" : "made without them");
        }
        if (ok) ci = bc, usage = bc.usage & ~hidden, flags = bc.flags, block_views = 1;
    }
    // Feedback loops (vk.h VkCtx.feedback): every attachable image may be sampled by a draw of a pass that renders it,
    // so it gets the usage now; adding it when a loop shows up would mean recreating the image (that lost the
    // grass tile maps once).
    int fb_usage = c->feedback == VK_FEEDBACK_LAYOUT && !block_views &&
                   (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
    if (fb_usage) ci.usage = usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    Tex *e = calloc(1, sizeof *e);
    if (!e) rt_fatal("vk: out of memory for a texture");
    e->key = *k;
    e->t = *t;
    e->f = f;
    e->pub.addr = k->addr;
    VkImage img = VK_NULL_HANDLE;
    if (scale == 1 && nvn_gpu_only(k->addr)) {   // storage in a physical or virtual pool: alias the pool memory (section 3.2)
        if (vkCreateImage(c->device, &ci, NULL, &img) == VK_SUCCESS && !bind_pool_memory(c, e, img, L.size)) {
            vkDestroyImage(c->device, img, NULL);
            img = VK_NULL_HANDLE;
            log_limited(&n_unbound, "texture %#llx (%ux%u fmt %#x) cannot alias its pool memory; own memory instead",
                        (unsigned long long)k->addr, w, h, d->format);
        }
    }
    if (!img) {
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkResult r = vmaCreateImage(c->vma, &ci, &ai, &img, &e->alloc, NULL);
        if (r != VK_SUCCESS) {
            log_limited(&n_failed, "image creation failed (%d): nvn fmt %#x %ux%ux%u target %d levels %u layers %u", (int)r,
                        d->format, w, h, depth, d->target, levels, layers);
            free(e);
            return NULL;
        }
    }
    VkTexImage *p = &e->pub;
    p->image = img;
    p->format = f.vk;
    p->type = type;
    p->usage = usage;
    p->aspect = aspect_of(&f);
    p->width = w, p->height = h, p->depth = depth, p->levels = levels, p->layers = layers, p->samples = samples;
    p->view_type = type == VK_IMAGE_TYPE_1D ? (d->target == NVN_TEXTURE_TARGET_1D_ARRAY ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                 : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                 : d->target == NVN_TEXTURE_TARGET_CUBEMAP && (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ? VK_IMAGE_VIEW_TYPE_CUBE
                 : d->target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY && (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                 : layers > 1 || d->target == NVN_TEXTURE_TARGET_2D_ARRAY || d->target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY
                     ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    p->alpha_one = f.alpha_one;
    memcpy(p->chan, f.chan, sizeof p->chan);
    p->swz[0] = (uint8_t)d->swizzle_r, p->swz[1] = (uint8_t)d->swizzle_g;
    p->swz[2] = (uint8_t)d->swizzle_b, p->swz[3] = (uint8_t)d->swizzle_a;
    if (!(p->swz[0] | p->swz[1] | p->swz[2] | p->swz[3]))   // descriptors built without nvnTextureBuilderSetDefaults
        p->swz[0] = NVN_TEXTURE_SWIZZLE_R, p->swz[1] = NVN_TEXTURE_SWIZZLE_G, p->swz[2] = NVN_TEXTURE_SWIZZLE_B,
        p->swz[3] = NVN_TEXTURE_SWIZZLE_A;
    p->mirrored = (uint8_t)tex_mirrored(t);
    p->bound = e->bound_to != NULL;
    p->block_views = (uint8_t)block_views;
    p->fb_usage = (uint8_t)fb_usage;
    p->scale = (uint8_t)scale;

    if (!defer_init) {
        init_image(e, vk_upload_begin());
        vk_upload_end();
    }
    return e;
}

static int owned_lower(uint64_t a) {
    int lo = 0, hi = nowned;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (owned[m]->pub.addr < a) lo = m + 1;
        else hi = m;
    }
    return lo;
}

static void own_add(Tex *e) {
    if (e->listed) return;
    plat_mutex_lock(&owned_lock);
    if (nowned == cowned) {
        cowned = cowned ? 2 * cowned : 256;
        if (!(owned = realloc(owned, sizeof *owned * (size_t)cowned))) rt_fatal("vk: out of memory for the owned-texture list");
    }
    int i = owned_lower(e->pub.addr);
    memmove(owned + i + 1, owned + i, sizeof *owned * (size_t)(nowned - i));
    owned[i] = e;
    __atomic_store_n(&nowned, nowned + 1, __ATOMIC_RELEASE);
    if (e->t.L.size > max_owned) max_owned = e->t.L.size;
    e->listed = 1;
    plat_mutex_unlock(&owned_lock);
}

static void own_remove(Tex *e) {
    if (!e->listed) return;
    plat_mutex_lock(&owned_lock);
    for (int i = owned_lower(e->pub.addr); i < nowned && owned[i]->pub.addr == e->pub.addr; i++)
        if (owned[i] == e) {
            memmove(owned + i, owned + i + 1, sizeof *owned * (size_t)(nowned - i - 1));
            __atomic_store_n(&nowned, nowned - 1, __ATOMIC_RELEASE);
            break;
        }
    e->listed = 0;
    plat_mutex_unlock(&owned_lock);
}

static void rel_append(Tex *e) {
    e->rel_next = NULL;
    e->rel_prev = rel_tail;
    if (rel_tail) rel_tail->rel_next = e;
    else rel_head = e;
    rel_tail = e;
}

static void rel_unlink(Tex *e) {
    if (e->rel_prev) e->rel_prev->rel_next = e->rel_next;
    else rel_head = e->rel_next;
    if (e->rel_next) e->rel_next->rel_prev = e->rel_prev;
    else rel_tail = e->rel_prev;
    e->rel_prev = e->rel_next = NULL;
}

// A clock read per submit, for stamps on paths that run many times a frame.
static uint64_t coarse_ms;
static uint64_t use_counter;

// How long released entries wait until the game takes them back, split into those holding GPU-written content and the
// others (TOTK_TRACE=perf, vk_texture_released_log): where transient textures end and the tail that can go begins.
enum { AGE_1S, AGE_5S, AGE_30S, AGE_120S, AGE_LONG, AGE_N };
static const char *const age_name[AGE_N] = {"<1s", "<5s", "<30s", "<120s", ">=120s"};
static uint64_t takeback_n[2][AGE_N];
static int age_bucket(uint64_t ms) {
    return ms < 1000 ? AGE_1S : ms < 5000 ? AGE_5S : ms < 30000 ? AGE_30S : ms < 120000 ? AGE_120S : AGE_LONG;
}

// Keys of entries the pressure path dropped, to count the ones the game asks for again soon after: a drop that buys
// nothing and, for GPU-written content, loses it. Direct-mapped, a lost record only undercounts.
#define DROPPED_SLOTS 4096u
static struct { uint64_t hash, ms; } dropped[DROPPED_SLOTS];
static uint64_t n_recreated[2], n_recreated_written[2];   // index: within 10 s, later
static uint64_t key_hash(const TexKey *k) {
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *b = (const uint8_t *)k;
    for (size_t i = 0; i < sizeof *k; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h | 1;
}
static void dropped_note(const Tex *e) {
    uint64_t h = key_hash(&e->key);
    dropped[h >> 3 & (DROPPED_SLOTS - 1)].hash = h ^ (uint64_t)(e->pub.gpu_owned != 0) << 1;
    dropped[h >> 3 & (DROPPED_SLOTS - 1)].ms = coarse_ms;
}
static void dropped_check(const TexKey *k) {
    uint64_t h = key_hash(k);
    unsigned i = h >> 3 & (DROPPED_SLOTS - 1);
    uint64_t was = dropped[i].hash;
    if (!was || (was & ~2ull) != (h & ~2ull)) return;
    int later = coarse_ms - dropped[i].ms >= 10000;
    n_recreated[later]++;
    if (was != h) n_recreated_written[later]++;
    dropped[i].hash = 0;
}

static void destroy_tex(Tex *e) {
    own_remove(e);
    tex_gen_bump(e->key.addr);
    nvn_hm_del(&by_key, &e->key);
    Tex *head = nvn_hm_get(&by_addr, &e->key.addr);
    if (head == e) {
        if (e->next_at) nvn_hm_put(&by_addr, &e->key.addr, e->next_at);
        else nvn_hm_del(&by_addr, &e->key.addr);
    } else {
        for (Tex *p = head; p; p = p->next_at)
            if (p->next_at == e) { p->next_at = e->next_at; break; }
    }
    if (e->released) nreleased--, rel_unlink(e);
    // Another thread may hold &e->pub, taken under the lock and used after it (vk_texture_get, vk_texture_by_id: render
    // targets for a whole pass, vk_exec.c resolve_target, vk_texture_storage_bound_locked writes through it). Freed
    // here, that thread read a stale VkImage from freed memory, made views of it and cached them by that handle. The
    // record goes with the image, once the submission being recorded is done; gone keeps views of it out of the caches.
    e->pub.gone = 1;
    vk_exec_image_gone(e->pub.image);
    vk_defer_image(e->pub.image, e->alloc);
    free(e->snap);
    e->snap = NULL;
    vk_defer_free(e);
}

// Released entries with another descriptor at the same address are replaced; past RELEASED_MAX released entries the
// oldest one goes at each release (it comes back on demand, a mirrored one re-uploaded from its storage). TotK
// finalizes and initializes transient textures every frame, so a released entry is mostly taken back within frames.
// Dropping all of them at once when the count passed 4096 destroyed thousands of images in one frame: 333-422 ms
// frames every 40-50 s on a GTX 1050 Ti, then the textures still in use were created and uploaded again.
#define RELEASED_MAX 2048
static void sweep_released(void) {
    while (nreleased > RELEASED_MAX && rel_head) destroy_tex(rel_head);
}

// TOTK_TEX_VRAM_PRESSURE (percent of the device-local budget, default 90; 0 = off): above it, each budget query drops up
// to PRESSURE_STEP released entries, oldest first, until the use is back under the line; a few at a time, for the reason
// above. Two kinds come back without loss:
//
// Overwritten: the game placed another texture on the entry's storage after releasing it, so on the Switch its bytes are
// gone too, and it has waited DEAD_AGE_MS. Transient render targets are taken back within a second or two (a stand-still
// field window: about 19000 take-backs under 1 s, 2 later than that, none after 30 s), while their leftovers at
// addresses the game's allocator moved on from stay released for good: 1.9 GiB of 2.0 GiB, nearly all of it render targets
// no storage can refill. Not bound through a finalized slot for DEAD_QUIET_SUBMITS either: such a slot still samples the
// entry (slot_resolve).
//
// Clean: mirrored ones the GPU never wrote, re-uploaded from their storage, at the cost of the upload, so after
// CLEAN_AGE_MS: without the wait a fight re-created about 500 of them per 10 s. Any other render target lives only in its
// image: dropping one that was merely old brought the texture back empty (black flashes in a Depths boss fight, ~170
// "left uninitialised" aliases a minute).
#define PRESSURE_STEP 32
#define CLEAN_AGE_MS 5000u
#define DEAD_AGE_MS 30000u
#define DEAD_QUIET_SUBMITS 600u
#define DEAD_RETRY_QUERIES 8   // budget queries to skip after a search that found nothing
static int vram_pressure = -1;
static uint64_t n_pressure_drops, n_pressure_overwritten;

static int tex_by_addr(const void *a, const void *b) {
    const Tex *x = *(Tex *const *)a, *y = *(Tex *const *)b;
    return x->pub.addr < y->pub.addr ? -1 : x->pub.addr > y->pub.addr;
}

// All entries sorted by storage address, with the highest storage end up to each one: the input of storage_taken.
typedef struct { Tex **e; uint64_t *end; int n; } AddrIndex;

static int addr_index(AddrIndex *x) {
    x->n = 0;
    x->e = malloc(sizeof *x->e * (size_t)(by_key.n + 1));
    x->end = malloc(sizeof *x->end * (size_t)(by_key.n + 1));
    if (!x->e || !x->end) { free(x->e), free(x->end); return 0; }
    for (uint32_t i = 0; i < by_key.cap; i++)
        if (by_key.vals[i]) x->e[x->n++] = by_key.vals[i];
    qsort(x->e, (size_t)x->n, sizeof *x->e, tex_by_addr);
    uint64_t top = 0;
    for (int i = 0; i < x->n; i++) {
        uint64_t end = x->e[i]->pub.addr + x->e[i]->t.L.size;
        x->end[i] = top = end > top ? end : top;
    }
    return 1;
}

static void addr_index_free(AddrIndex *x) { free(x->e), free(x->end); }

// A texture placed after the release of x->e[i] overlaps its storage.
static int storage_taken(const AddrIndex *x, int i) {
    const Tex *e = x->e[i];
    for (int j = i; --j >= 0 && x->end[j] > e->pub.addr;)
        if (x->e[j]->pub.addr + x->e[j]->t.L.size > e->pub.addr && x->e[j]->use_seq > e->rel_seq) return 1;
    for (int j = i; ++j < x->n && x->e[j]->pub.addr < e->pub.addr + e->t.L.size;)
        if (x->e[j]->use_seq > e->rel_seq) return 1;
    return 0;
}

static uint64_t own_bytes(const Tex *e) {
    VkCtx *c = vk_ctx();
    if (!e->alloc || !c) return 0;
    VmaAllocationInfo ai;
    vmaGetAllocationInfo(c->vma, e->alloc, &ai);
    return ai.size;
}

static int dead_candidate(const Tex *e) {
    return e->alloc && (!e->checked || submit_epoch - e->checked >= DEAD_QUIET_SUBMITS);
}

// Drops overwritten entries until `need` bytes are free or the step is used up. The sort runs only under pressure, while
// an old enough entry exists, and rests for a while when it finds nothing to drop. Returns the bytes freed.
static uint64_t pressure_search_ns;   // longest search since the last mem: released line
static uint64_t drop_overwritten(uint64_t need, int *dropped) {
    static int rest;
    if (rest > 0) { rest--; return 0; }
    if (!rel_head || coarse_ms - rel_head->rel_ms < DEAD_AGE_MS) return 0;
    uint64_t t0 = plat_now_ns(), freed = 0;
    AddrIndex x;
    if (!addr_index(&x)) return 0;
    Tex *cand[PRESSURE_STEP];
    int nc = 0;
    for (Tex *e = rel_head; e && nc < PRESSURE_STEP && coarse_ms - e->rel_ms >= DEAD_AGE_MS; e = e->rel_next) {
        if (!dead_candidate(e)) continue;
        int lo = 0, hi = x.n;   // the first entry at e's address, then e among those
        while (lo < hi) {
            int m = (lo + hi) / 2;
            if (x.e[m]->pub.addr < e->pub.addr) lo = m + 1;
            else hi = m;
        }
        while (lo < x.n && x.e[lo] != e) lo++;
        if (lo < x.n && storage_taken(&x, lo)) cand[nc++] = e;
    }
    addr_index_free(&x);
    for (int i = 0; i < nc && freed < need; i++) {
        freed += own_bytes(cand[i]);
        dropped_note(cand[i]);
        destroy_tex(cand[i]);
        (*dropped)++, n_pressure_drops++, n_pressure_overwritten++;
    }
    if (!nc) rest = DEAD_RETRY_QUERIES;
    uint64_t ns = plat_now_ns() - t0;
    if (ns > pressure_search_ns) pressure_search_ns = ns;
    return freed;
}

static void sweep_pressure(void) {   // tex_lock held
    if (vram_pressure < 0) {
        const char *s = cfg_get("TOTK_TEX_VRAM_PRESSURE");
        vram_pressure = s && *s ? atoi(s) : 90;
        if (vram_pressure < 0 || vram_pressure > 100) vram_pressure = 90;
        rt_log("vk: released textures under VRAM pressure: %s%d%%", vram_pressure ? "dropped above " : "off", vram_pressure);
    }
    if (!vram_pressure || !rel_head || submit_epoch & 7) return;   // the budget query every 8 submits
    uint64_t used, budget;
    int exact;
    if (!vk_mem_vram(&used, &budget, &exact)) return;
    uint64_t line = budget / 100 * (uint64_t)vram_pressure;
    if (used <= line) return;
    int dropped = 0;
    uint64_t need = used - line, freed = drop_overwritten(need, &dropped);
    for (Tex *e = rel_head, *next; e && freed < need && dropped < PRESSURE_STEP && coarse_ms - e->rel_ms >= CLEAN_AGE_MS; e = next) {
        next = e->rel_next;
        if (!e->pub.mirrored || e->pub.gpu_owned || e->gpu_dirty) continue;
        freed += own_bytes(e);
        dropped_note(e);
        destroy_tex(e);
        dropped++, n_pressure_drops++;
    }
}

// ---------------------------------------------------------------- uploads
static uint32_t texel_alpha_one(int format, uint32_t *elem) {
    switch (format) {
    case 0x19: *elem = 1; return 0xFF;
    case 0x1A: *elem = 1; return 0x7F;
    case 0x1B: case 0x1C: *elem = 1; return 1;
    case 0x1D: *elem = 2; return 0x3C00;
    case 0x1E: *elem = 2; return 0xFFFF;
    case 0x1F: *elem = 2; return 0x7FFF;
    case 0x20: case 0x21: *elem = 2; return 1;
    case 0x22: *elem = 4; return 0x3F800000;
    default: *elem = 4; return 1;   // 0x23, 0x24
    }
}

// 3-component texels to 4 (n texels).
static void expand_rgb(int format, const uint8_t *src, uint8_t *dst, size_t n) {
    uint32_t e, one = texel_alpha_one(format, &e);
    for (size_t i = 0; i < n; i++, src += 3 * e, dst += 4 * e) {
        memcpy(dst, src, 3 * e);
        memcpy(dst + 3 * e, &one, e);
    }
}

static void shrink_rgb(int format, const uint8_t *src, uint8_t *dst, size_t n) {
    uint32_t e;
    texel_alpha_one(format, &e);
    for (size_t i = 0; i < n; i++, src += 4 * e, dst += 3 * e) memcpy(dst, src, 3 * e);
}

// TOTK_ASTC=check: the compute decoder's texels of n regions (gpu: the regions one after the other) against
// astc_decode_ldr of the same raw blocks (raw: the staging bytes the regions point into).
static void astc_check(const Tex *e, const uint8_t *raw, const VkAstcRegion *ar, int n, const uint8_t *gpu) {
    static volatile int32_t n_bad;
    static uint64_t n_tex, n_texels, n_diff_tex;
    uint32_t bw = e->f.astc_bw, bh = e->f.astc_bh;
    uint64_t diff = 0, first = UINT64_MAX;
    uint32_t want = 0, got = 0;
    int first_k = 0;
    for (int k = 0; k < n; k++) {
        const VkAstcRegion *r = &ar[k];
        size_t sb = (size_t)((r->w + bw - 1) / bw) * ((r->h + bh - 1) / bh) * 16, st = (size_t)r->w * r->h * 4;
        uint8_t *cpu = malloc(st);
        for (uint32_t z = 0; z < r->slices; z++, gpu += st) {
            astc_decode_ldr(raw + r->src + z * sb, r->w, r->h, bw, bh, e->f.astc_srgb, cpu);
            if (!memcmp(cpu, gpu, st)) continue;
            for (size_t i = 0; i < st / 4; i++)
                if (memcmp(cpu + 4 * i, gpu + 4 * i, 4)) {
                    if (first == UINT64_MAX) {
                        first = i, first_k = k;
                        memcpy(&want, cpu + 4 * i, 4), memcpy(&got, gpu + 4 * i, 4);
                    }
                    diff++;
                }
        }
        free(cpu);
        n_texels += (uint64_t)r->w * r->h * r->slices;
    }
    n_tex++;
    if (diff) {
        n_diff_tex++;
        log_limited(&n_bad, "ASTC check: texture %#llx fmt %#x %ux%u: %llu texels differ, first region %d texel %llu "
                    "cpu %08x gpu %08x", (unsigned long long)e->pub.addr, e->t.d.format, bw, bh,
                    (unsigned long long)diff, first_k, (unsigned long long)first, want, got);
    }
    if (n_tex == 1 || !(n_tex & 63) || diff)
        rt_log("vk: ASTC check: %llu textures, %llu texels compared, %llu textures differ", (unsigned long long)n_tex,
               (unsigned long long)n_texels, (unsigned long long)n_diff_tex);
}

// Staging bytes of one part of a texture upload decoded or copied on the CPU (see upload). With TOTK_ASTC=cpu a
// large ASTC array decodes to RGBA8 in one go: a staging buffer of 676682336 bytes failed (-2) on an Intel UHD with
// 8 GB of shared memory and the game closed before the title screen.
#define UPLOAD_PART (32ull << 20)   // fits the staging pool (vk_staging takes up to half of its 64 MiB block)
// The open upload batch is submitted between chunks (and parts of the CPU path) once its staging and scratch pass this:
// on fight2's first world frame one batch took hundreds of MiB, and the staging and scratch pools grew by 64 MiB blocks
// on the replay thread. Submitting earlier keeps the queue order (the batch still precedes the part that samples the
// images) and lets the GPU finish and return the buffers sooner.
#define UPLOAD_HELD_MAX (48ull << 20)

static int upload_gpu(Tex *e);

// The CPU side of upload: deswizzles (and decodes or expands) into staging buffers, then copies them into the image.
static void upload_cpu(Tex *e) {
    static volatile int32_t n_bad, n_undecoded, n_split;
    const tex_t *t = &e->t;
    const nvn_texdesc *d = &t->d;
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    int linear = nvn_tex_is_linear(d), is3d = d->target == NVN_TEXTURE_TARGET_3D, one_d = e->pub.type == VK_IMAGE_TYPE_1D;
    uint32_t levels = linear ? 1 : e->pub.levels, layers = linear ? 1 : e->pub.layers;
    int astc = e->f.astc_bw != 0, rgb = e->f.expand_rgb;
    int agpu = astc && vk_astc_gpu(), acheck = agpu && vk_astc_mode() == VK_ASTC_CHECK;   // raw blocks to the GPU
    uint32_t n = levels * layers, k = 0;
    VkBufferImageCopy *rg = calloc(n, sizeof *rg);
    VkAstcRegion *ar = agpu ? calloc(n, sizeof *ar) : NULL;
    VkDeviceSize total = 0, decoded = 0;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++) {
            uint32_t w = mip(d->width, lv), h = one_d ? 1 : mip(d->height, lv), sl = is3d ? mip(d->depth, lv) : 1;
            uint64_t texels = (uint64_t)w * h * sl, raw = (uint64_t)(w + bw - 1) / bw * bpb * ((h + bh - 1) / bh) * sl;
            uint64_t out = agpu ? raw : astc ? texels * 4 : rgb ? texels * (uint64_t)bpb / 3 * 4 : raw;
            rg[k++] = (VkBufferImageCopy){.bufferOffset = total, .imageSubresource = {e->pub.aspect, lv, ly, 1},
                                          .imageExtent = {w, h, sl}};
            total += (out + 15) & ~15ull;
            decoded += texels * 4;
        }
    VkBuffer sb, rbuf = VK_NULL_HANDLE;
    VmaAllocation ralloc = NULL;
    uint8_t *rmap = NULL, *raw_copy = NULL;
    int checked = 0, parts = 1;
    // A CPU decode or copy stages at most UPLOAD_PART bytes per staging buffer (a region larger than that alone) and
    // records one copy per buffer; every part stays in the same upload batch, submitted as before. The GPU decoder
    // takes the raw blocks of every region from one buffer.
    for (uint32_t k0 = 0, k1; k0 < n; k0 = k1) {
        VkDeviceSize base = rg[k0].bufferOffset, size;
        for (k1 = k0 + 1; k1 < n && (agpu || (k1 + 1 < n ? rg[k1 + 1].bufferOffset : total) - base <= UPLOAD_PART);) k1++;
        size = (k1 < n ? rg[k1].bufferOffset : total) - base;
        if (k0) parts++;
        VkCommandBuffer cb = vk_upload_begin();
        uint8_t *stg = vk_staging(size, &sb, NULL);
        if (acheck) rmap = vk_staging(decoded, &rbuf, &ralloc);
        for (k = k0; k < k1; k++) {
            uint32_t lv = rg[k].imageSubresource.mipLevel, ly = rg[k].imageSubresource.baseArrayLayer;
            uint32_t w = rg[k].imageExtent.width, h = rg[k].imageExtent.height, sl = rg[k].imageExtent.depth;
            uint32_t rb = (w + bw - 1) / bw * (uint32_t)bpb, rows = (h + bh - 1) / bh;
            size_t raw = (size_t)rb * rows * sl;
            rg[k].bufferOffset -= base;
            uint8_t *dst = stg + rg[k].bufferOffset, *tmp = (astc && !agpu) || rgb ? malloc(raw) : NULL;
            if (nvn_deswizzle(d, lv, ly, t->mem, tmp ? tmp : dst, rb)) {
                log_limited(&n_bad, "texture %#llx fmt %#x target %d: level %u layer %u has no CPU layout; uploaded as zeros",
                            (unsigned long long)e->pub.addr, d->format, d->target, lv, ly);
                memset(tmp ? tmp : dst, 0, raw);
            }
            if (agpu) {
                ar[k] = (VkAstcRegion){.src = rg[k].bufferOffset, .w = w, .h = h, .slices = sl, .level = lv, .layer = ly};
            } else if (astc) {
                uint64_t a0 = perf_begin();
                for (uint32_t z = 0; z < sl; z++)
                    astc_decode_ldr_mt(tmp + (size_t)z * rb * rows, w, h, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb,
                                       dst + (size_t)z * w * h * 4);
                perf_end(PERF_ASTC, a0);
                rg[k].bufferRowLength = w, rg[k].bufferImageHeight = h;
            } else if (rgb) {
                expand_rgb(d->format, tmp, dst, (size_t)w * h * sl);
                rg[k].bufferRowLength = w, rg[k].bufferImageHeight = h;
            } else {
                rg[k].bufferRowLength = rb / (uint32_t)bpb * (uint32_t)bw, rg[k].bufferImageHeight = rows * (uint32_t)bh;
            }
            free(tmp);
        }
        if (acheck) {
            raw_copy = malloc(size);
            memcpy(raw_copy, stg, size);
        }
        barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (agpu) {
            uint64_t a0 = perf_begin();
            checked = vk_astc_record(cb, sb, size, ar, (int)n, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb, e->f.astc_bc,
                                     e->pub.image, e->pub.aspect, rbuf);
            perf_end(PERF_ASTC, a0);
            if (!checked)
                log_limited(&n_undecoded, "ASTC texture %#llx (fmt %#x %ux%u) not decoded on the GPU: left as it was",
                            (unsigned long long)e->pub.addr, d->format, d->width, d->height);
        } else {
            vkCmdCopyBufferToImage(cb, sb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, k1 - k0, rg + k0);
        }
        barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
        vk_upload_end();
        if (!acheck && vk_upload_held() > UPLOAD_HELD_MAX) vk_upload_flush(0);
    }
    if (parts > 1)
        log_limited(&n_split, "texture %#llx fmt %#x %ux%u, %u levels, %u layers: %.1f MiB staged in %d parts",
                    (unsigned long long)e->pub.addr, d->format, d->width, d->height, levels, layers, total / 1048576.0,
                    parts);
    if (acheck) {   // waits for the GPU
        vk_upload_flush(1);
        if (checked) {
            vmaInvalidateAllocation(vk_ctx()->vma, ralloc, 0, VK_WHOLE_SIZE);
            astc_check(e, raw_copy, ar, (int)n, rmap);
        }
        vmaDestroyBuffer(vk_ctx()->vma, rbuf, ralloc);
    }
    free(raw_copy);
    free(ar);
    free(rg);
}

// Block-linear storage in an imported pool, deswizzled by the GPU (vk_deswizzle.c): the replay thread only records
// the dispatches, the image copies and (ASTC) the decode into the upload batch the CPU path also uses, which runs before
// the part that samples the image. The GPU reads the pool when the batch runs, as the Switch's GPU reads it at
// execution; a CPU write after make_fresh's write-watch reset still marks the pages and brings another upload. Each
// chunk of regions gets a device-local scratch buffer of at most DESWZ_CHUNK bytes (one region, when larger), freed
// once the batch completes. TOTK_DESWIZZLE=check compares every chunk with the CPU deswizzle (waits for the GPU).
// 0 when not handled, before anything is recorded: the CPU path takes the texture.
#define DESWZ_CHUNK (32ull << 20)

// Uploads by path since the last perf summary (TOTK_TRACE=perf): 0 GPU, else why the CPU took it.
enum { UP_GPU, UP_MODE, UP_LINEAR, UP_RGB, UP_ASTC_CPU, UP_BPB, UP_NOT_LIVE, UP_GEOM, UP_LIMITS, UP_SCRATCH, UP_WHY };
static const char *const up_why_name[UP_WHY] = {"gpu", "cpu mode", "linear", "rgb", "astc on cpu", "bytes per block",
                                                "not an imported pool", "layout", "device limits", "scratch"};
static uint64_t up_n[UP_WHY], up_bytes[UP_WHY], up_ns[UP_WHY];
static int up_why;   // upload_gpu's reason for returning 0 (texture lock held)

static int upload_gpu(Tex *e) {
    static volatile int32_t n_scratch_fail, n_undecoded, n_bad;
    static volatile int64_t n_checked, n_differ;
    const tex_t *t = &e->t;
    const nvn_texdesc *d = &t->d;
    int mode = vk_deswz_mode();
    up_why = mode == VK_DESWZ_CPU ? UP_MODE : nvn_tex_is_linear(d) ? UP_LINEAR : e->f.expand_rgb ? UP_RGB : UP_GPU;
    if (up_why) return 0;
    int astc = e->f.astc_bw != 0, agpu = astc && vk_astc_gpu();
    if (astc && (!agpu || vk_astc_mode() == VK_ASTC_CHECK)) return up_why = UP_ASTC_CPU, 0;   // or ASTC's check mode
    int bw, bh, bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    if (!bpb || 16 % bpb) return up_why = UP_BPB, 0;   // the rows of the linear copy are padded to 16 bytes
    VkMemSpan sp;
    if (!vk_mem_resolve(e->pub.addr, t->L.size, &sp) || !vk_mem_span_live(&sp)) return up_why = UP_NOT_LIVE, 0;
    int one_d = e->pub.type == VK_IMAGE_TYPE_1D, is3d = d->target == NVN_TEXTURE_TARGET_3D;
    uint32_t levels = e->pub.levels, layers = e->pub.layers, n = levels * layers, k = 0;
    VkDeswzRegion *dr = calloc(n, sizeof *dr);
    VkBufferImageCopy *rg = calloc(n, sizeof *rg);
    VkAstcRegion *ar = agpu ? calloc(n, sizeof *ar) : NULL;
    if (!dr || !rg || (agpu && !ar)) rt_fatal("vk: out of memory (texture upload regions)");
    int ok = 1;
    VkDeviceSize max_dst = DESWZ_CHUNK;
    for (uint32_t lv = 0; ok && lv < levels; lv++)
        for (uint32_t ly = 0; ok && ly < layers; ly++) {
            uint32_t row_bytes, rows, slices, gbpb;
            uint64_t base;
            unsigned th, td;
            uint32_t w = mip(d->width, lv), h = one_d ? 1 : mip(d->height, lv), sl = is3d ? mip(d->depth, lv) : 1;
            if (nvn_deswizzle_geom(d, lv, ly, &row_bytes, &rows, &slices, &base, &th, &td, &gbpb) || gbpb != (uint32_t)bpb ||
                base >= t->L.size || rows != (h + bh - 1) / bh || slices != sl) {
                ok = 0;
                break;
            }
            uint64_t bhg = 1ull << th, bdg = 1ull << td, gobs_x = (row_bytes + 63) / 64;
            uint64_t blocks_y = (rows + 8 * bhg - 1) / (8 * bhg), blocks_z = (slices + bdg - 1) / bdg;
            uint64_t segs = gobs_x * blocks_y * blocks_z * (32ull << (th + td)), room = (t->L.size - base) / 16;
            if (segs > room) segs = room;
            uint32_t pitch16 = (row_bytes + 15) / 16;
            VkDeviceSize out = (VkDeviceSize)pitch16 * 16 * rows * slices;
            if (segs > UINT32_MAX || gobs_x * blocks_y > UINT32_MAX) { ok = 0; break; }
            if (out > max_dst) max_dst = out;
            dr[k] = (VkDeswzRegion){.src = base, .segs = (uint32_t)segs, .pitch16 = pitch16, .rows = rows, .slices = slices,
                                    .th = th, .td = td, .gobs_x = (uint32_t)gobs_x, .blocks_y = (uint32_t)blocks_y};
            rg[k] = (VkBufferImageCopy){.bufferRowLength = pitch16 * 16 / (uint32_t)bpb * (uint32_t)bw,
                                        .bufferImageHeight = rows * (uint32_t)bh,
                                        .imageSubresource = {e->pub.aspect, lv, ly, 1}, .imageExtent = {w, h, sl}};
            if (agpu) ar[k] = (VkAstcRegion){.w = w, .h = h, .slices = sl, .level = lv, .layer = ly};
            k++;
        }
    if (!ok || k != n || !vk_deswz_fits(sp.offset, t->L.size, dr, (int)n, max_dst)) {
        free(dr), free(rg), free(ar);
        return up_why = !ok || k != n ? UP_GEOM : UP_LIMITS, 0;
    }
    VkCtx *c = vk_ctx();
    VkCommandBuffer cb = vk_upload_begin();
    // Earlier GPU writes into the pool (copies, shaders) before the reads; the image's earlier uses before its copy.
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (uint32_t k0 = 0, k1; k0 < n; k0 = k1) {
        VkDeviceSize size = 0;
        for (k1 = k0; k1 < n; k1++) {
            VkDeviceSize out = (VkDeviceSize)dr[k1].pitch16 * 16 * dr[k1].rows * dr[k1].slices;
            if (k1 > k0 && size + out > DESWZ_CHUNK) break;
            dr[k1].dst = size, rg[k1].bufferOffset = size;
            if (agpu) ar[k1].src = size;
            size += out;
        }
        VkBuffer sb;
        VmaAllocation sa;
        if (vk_scratch_buffer(size, &sb, &sa) != VK_SUCCESS) {
            log_limited(&n_scratch_fail, "texture %#llx: deswizzle scratch of %llu bytes failed%s",
                        (unsigned long long)e->pub.addr, (unsigned long long)size,
                        k0 ? "; its later levels left as they were" : "; deswizzled on the CPU");
            if (!k0) {
                vk_upload_end();
                free(dr), free(rg), free(ar);
                return up_why = UP_SCRATCH, 0;
            }
            break;
        }
        if (!vk_deswz_record(cb, sp.buffer, sp.offset, t->L.size, sb, size, dr + k0, (int)(k1 - k0))) {
            log_limited(&n_bad, "texture %#llx: deswizzle of levels %u-%u not recorded; left as they were",
                        (unsigned long long)e->pub.addr, k0, k1 - 1);   // vk_deswz_fits passed: not expected
            vk_defer_buffer(sb, sa);
            break;
        }
        barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        if (agpu) {
            uint64_t a0 = perf_begin();
            if (!vk_astc_record(cb, sb, size, ar + k0, (int)(k1 - k0), e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb,
                                e->f.astc_bc, e->pub.image, e->pub.aspect, VK_NULL_HANDLE))
                log_limited(&n_undecoded, "ASTC texture %#llx (fmt %#x %ux%u) not decoded on the GPU: left as it was",
                            (unsigned long long)e->pub.addr, d->format, d->width, d->height);
            perf_end(PERF_ASTC, a0);
        } else {
            vkCmdCopyBufferToImage(cb, sb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, k1 - k0, rg + k0);
        }
        if (mode == VK_DESWZ_CHECK) {   // the linear copy back to the host, compared with the CPU deswizzle
            VkBuffer rb;
            VmaAllocation ra;
            const uint8_t *rmap = vk_staging(size, &rb, &ra);
            VkBufferCopy cp = {0, 0, size};
            vkCmdCopyBuffer(cb, sb, rb, 1, &cp);
            barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                    VK_ACCESS_2_HOST_READ_BIT);
            vk_defer_buffer(sb, sa);
            vk_upload_end();
            vk_upload_flush(1);
            vmaInvalidateAllocation(c->vma, ra, 0, VK_WHOLE_SIZE);
            for (uint32_t i = k0; i < k1; i++) {
                const VkDeswzRegion *r = &dr[i];
                size_t pitch = (size_t)r->pitch16 * 16, bytes = pitch * r->rows * r->slices;
                uint8_t *cpu = malloc(bytes);
                if (!cpu) rt_fatal("vk: out of memory (deswizzle check)");
                uint32_t row_bytes = 0, rows, slices, gbpb;
                uint64_t base;
                unsigned th, td;
                nvn_deswizzle_geom(d, rg[i].imageSubresource.mipLevel, rg[i].imageSubresource.baseArrayLayer, &row_bytes,
                                   &rows, &slices, &base, &th, &td, &gbpb);
                nvn_deswizzle(d, rg[i].imageSubresource.mipLevel, rg[i].imageSubresource.baseArrayLayer, t->mem, cpu, pitch);
                uint64_t bad = 0, first = UINT64_MAX;
                for (uint64_t row = 0; row < (uint64_t)r->rows * r->slices; row++)
                    if (memcmp(cpu + row * pitch, rmap + r->dst + row * pitch, row_bytes)) {
                        bad++;
                        if (first == UINT64_MAX) first = row;
                    }
                free(cpu);
                plat_add64(&n_checked, 1);
                if (bad) {
                    plat_add64(&n_differ, 1);
                    rt_log("vk: deswizzle check: texture %#llx fmt %#x %ux%u level %u layer %u: %llu of %u rows differ, the "
                           "first %llu (th %u td %u, %u rows of %u bytes, %u slices)", (unsigned long long)e->pub.addr,
                           d->format, d->width, d->height, rg[i].imageSubresource.mipLevel,
                           rg[i].imageSubresource.baseArrayLayer, (unsigned long long)bad, r->rows * r->slices,
                           (unsigned long long)first, r->th, r->td, r->rows, row_bytes, r->slices);
                }
            }
            vmaDestroyBuffer(c->vma, rb, ra);
            if ((n_checked & 1023) == 0 || n_checked < 2)
                rt_log("vk: deswizzle check: %lld regions compared, %lld differ", (long long)n_checked, (long long)n_differ);
            cb = vk_upload_begin();
            barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
            continue;
        }
        vk_defer_buffer(sb, sa);
        if (k1 < n && vk_upload_held() > UPLOAD_HELD_MAX) {   // submit what is recorded, go on in a new batch
            barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ALL_RW);
            vk_upload_end();
            vk_upload_flush(0);
            cb = vk_upload_begin();
            barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        }
    }
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, ALL_RW);
    vk_upload_end();
    if (vk_upload_held() > UPLOAD_HELD_MAX) vk_upload_flush(0);
    free(dr), free(rg), free(ar);
    return 1;
}

// Copies the whole storage into the image (every level and layer the image has): on the GPU where it can
// (upload_gpu), else on the CPU.
static void upload(Tex *e) {
    const tex_t *t = &e->t;
    const nvn_texdesc *d = &t->d;
    int bw, bh;
    if (!nvn_fmt_bpb(d->format, &bw, &bh) || !t->mem) return;
    uint64_t p0 = perf_begin(), u0 = kern_now_ns();
    int why = UP_GPU;
    if (!upload_gpu(e)) why = up_why, upload_cpu(e);
    uint64_t dt = kern_now_ns() - u0;
    up_n[why]++, up_bytes[why] += t->L.size, up_ns[why] += dt;
    if (e->uploaded) {
        n_reuploads++, perf_count(PERF_CNT_TEX_REUP);
        ru_n++, ru_bytes += t->L.size, ru_ns += dt;
        if (e->wt_n) {
            ru_wt_n++, ru_wt_bytes += t->L.size, ru_wt_ns += dt;
            if (rt_trace & RT_TRACE_PERF)
                rt_log("perf: frame %llu: re-upload after %u WriteTexels: texture %#llx fmt %#x %ux%u, %.1f KiB, %.3f ms",
                       (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), e->wt_n,
                       (unsigned long long)e->pub.addr, d->format, d->width, d->height, t->L.size / 1024.0, dt / 1e6);
        }
    } else {
        n_uploads++, perf_count(PERF_CNT_TEX_UP);
    }
    e->wt_n = 0;
    e->uploaded = 1;
    e->n_up++;
    perf_end(PERF_TEX, p0);
}

// Whether e's storage fills no 4 KiB page of its own, so the write tracking can only see writes to pages it shares.
static int fills_no_page(const Tex *e) {
    return ((e->pub.addr + e->t.L.size) & ~0xfffull) <= ((e->pub.addr + 0xfff) & ~0xfffull);
}

// A texture that fills no page keeps a copy of its storage bytes from when a GPU write to it was recorded
// (vk_texture_gpu_written), up to SNAP_MAX bytes; wb_exact (TOTK_WB_EXACT, default on) enables it.
enum { SNAP_MAX = 4096 };
static int wb_exact = -1;
static volatile int32_t n_kept;

// vk_mem_changed over the pages wholly inside e's storage (all of its pages when it fills none): a neighbour that shares
// the first or last page does not count as a write to e. A texture that fills no page shares all of its pages, so
// there a page write counts only when e's own bytes differ from the copy taken at the GPU write. TotK's Purah Pad
// camera renders the ID, screen rect and depth of what is under the reticle into 4x4 targets of 512 bytes, two
// slots used in turn, and the CPU reads them a frame later. One slot's rect target shares its page with data the CPU
// writes every frame: without the copy its renders were never written back, the game read a zero rect every other
// frame and the target's name label blinked. TOTK_WB_EXACT=0 restores the page-only decision.
static int storage_rewritten(const Tex *e, uint32_t since, uint32_t *now) {
    uint64_t lo = (e->pub.addr + 0xfff) & ~0xfffull, hi = (e->pub.addr + e->t.L.size) & ~0xfffull;
    if (hi <= lo) lo = e->pub.addr, hi = e->pub.addr + e->t.L.size;
    int ch = vk_mem_changed(lo, hi - lo, since, now);
    if (ch != 1 || !since || !e->snap || memcmp(e->snap, (const void *)(uintptr_t)e->pub.addr, e->t.L.size)) return ch == 1;
    log_limited(&n_kept, "write-back of %#llx kept: fmt %#x %ux%u, %llu bytes; a neighbour wrote its page",
                (unsigned long long)e->pub.addr, e->t.d.format, e->t.d.width, e->t.d.height,
                (unsigned long long)e->t.L.size);
    return 0;
}

// A mirrored texture is uploaded on first use and again when its storage pages carry a newer write generation.
static void make_fresh(Tex *e) {
    if (!e->pub.mirrored || e->pub.gpu_owned) return;
    uint64_t size = e->t.L.size;
    if (!e->uploaded) {
        vk_mem_changed(e->pub.addr, size, 0, &e->wgen);   // resets the write bits before the storage is read
        upload(e);
        return;
    }
    uint32_t now;
    int ch = vk_mem_changed(e->pub.addr, size, e->wgen, &now);
    e->wgen = now;
    e->last_ch = ch;
    if (ch) upload(e);   // 1 written, -1 not tracked: both upload again
}

// make_fresh for draw bindings: the page check answers from this submission's write-watch snapshot (vk_memory.c
// vk_mem_changed_snap) instead of a system call per texture. A write the snapshot of the texture's region missed (made
// after it, during this replay) keeps its write-watch bit and is caught by the next submission's snapshot.
static void make_fresh_snap(Tex *e) {
    if (!e->pub.mirrored || e->pub.gpu_owned) return;
    uint32_t now;
    n_checks++;
    int ch = vk_mem_changed_snap(e->pub.addr, e->t.L.size, e->uploaded ? e->wgen : 0, submit_epoch, &now);
    e->wgen = now;
    if (!e->uploaded) { upload(e); return; }
    e->last_ch = ch;
    if (ch) upload(e);
}

// ---------------------------------------------------------------- aliases (TOTK_TEX_ALIAS)
// Bytes per block and block size of the image's VkFormat (ASTC decodes to RGBA8, RGB expands to RGBA); 0 when the
// VkFormat substitutes another one of unknown size.
static int img_block(const Tex *e, int *bw, int *bh) {
    if (e->f.astc_bc) { *bw = *bh = 4; return 16; }
    if (e->f.astc_bw) { *bw = *bh = 1; return 4; }
    int b = nvn_fmt_bpb(e->t.d.format, bw, bh);
    if (e->f.expand_rgb) { *bw = *bh = 1; return b / 3 * 4; }
    return e->f.vk == e->f.ideal ? b : 0;
}

// The new entry e can take src's content texel for texel: colour, one sample, the same shape and storage layout, and
// image formats of one block size.
static int alias_compatible(const Tex *e, const Tex *src) {
    const VkTexImage *p = &e->pub, *q = &src->pub;
    if (e->f.depth || e->f.stencil || src->f.depth || src->f.stencil || p->samples > 1 || q->samples > 1) return 0;
    if (p->type != q->type || p->width != q->width || p->height != q->height || p->depth != q->depth) return 0;
    if (e->t.L.size != src->t.L.size || nvn_tex_is_linear(&e->t.d) != nvn_tex_is_linear(&src->t.d)) return 0;
    int ew, eh, sw, sh, eb = img_block(e, &ew, &eh), sb = img_block(src, &sw, &sh);
    return eb && eb == sb && ew == sw && eh == sh;
}

// Whether e is live at op pos of a chunked replay: the state scan is ahead of the chunks, and an entry it revived at a
// later op is still released for this one. Two 4K targets at one address, RGBA8 and R8 with memory of their own, were
// swapped whenever the scan got past the second's SET_RT before a chunk sampling the storage got its turn, and large
// parts of the scene flickered. UINT32_MAX: the state now.
static int live_at(const Tex *e, uint32_t pos) {
    return !e->released && !(e->rev_epoch == submit_epoch && e->rev_pos > pos);
}

// src holds content the storage bytes do not, at op pos of a chunked replay (UINT32_MAX: the state now): the GPU wrote
// it and, for a mirrored texture, it was not written back and the CPU has not rewritten the storage since (the new
// entry's upload then reads the newer bytes). A chunk at a later op that rendered src first does not count: in replay
// order that render comes after the new entry's first use.
static int alias_has_gpu_content(const Tex *src, uint32_t pos) {
    if (!src->pub.gpu_owned || (src->own_epoch == submit_epoch && src->own_pos > pos)) return 0;
    if (!src->pub.mirrored) return 1;
    uint32_t now;
    return src->gpu_dirty && !storage_rewritten(src, src->gpu_gen, &now);
}

// Maxwell reads the same bytes through both descriptors: src's content goes into the new image e at the replay's
// current position, before any later command of the submission uses e. Through a buffer, since bound images share
// their memory and image copies may not overlap; e leaves UNDEFINED between the two halves, so its first transition
// cannot clobber src's bytes before they are read. 0 when no replay is recording (e is then not initialised yet).
static int alias_copy(Tex *e, const Tex *src) {
    int reopen;
    VkCommandBuffer cb = vk_exec_inline_begin(&reopen);
    if (!cb) return 0;
    int bw, bh, bpb = img_block(e, &bw, &bh);
    uint32_t levels = e->pub.levels < src->pub.levels ? e->pub.levels : src->pub.levels;
    uint32_t layers = e->pub.layers < src->pub.layers ? e->pub.layers : src->pub.layers, n = levels * layers, k = 0;
    VkBufferImageCopy *rg = calloc(n, sizeof *rg);
    if (!rg) rt_fatal("vk: out of memory for an alias copy");
    VkDeviceSize total = 0;
    for (uint32_t lv = 0; lv < levels; lv++)
        for (uint32_t ly = 0; ly < layers; ly++, k++) {
            uint32_t w = mip(e->pub.width, lv), h = mip(e->pub.height, lv), z = mip(e->pub.depth, lv);
            rg[k] = (VkBufferImageCopy){.bufferOffset = total, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, ly, 1},
                                        .imageExtent = {w, h, z}};
            total += ((VkDeviceSize)(w + bw - 1) / bw * ((h + bh - 1) / bh) * z * bpb + 15) & ~15ull;
        }
    VkBuffer buf = vk_exec_inline_buffer(total);
    if (!buf) {
        free(rg);
        vk_exec_inline_end(reopen);
        return 0;
    }
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(cb, src->pub.image, VK_IMAGE_LAYOUT_GENERAL, buf, n, rg);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = e->pub.image,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb,
                           .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
    vkCmdCopyBufferToImage(cb, buf, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, n, rg);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    vk_exec_inline_end(reopen);
    free(rg);
    return 1;
}

static void entry_live(Tex *e) {
    if (!e->released) return;
    takeback_n[e->pub.gpu_owned != 0][age_bucket(coarse_ms - e->rel_ms)]++;
    e->use_seq = ++use_counter;
    e->released = 0, nreleased--, rel_unlink(e), tex_gen_bump(e->key.addr);
}

// A live colour entry at e's storage of e's size, as of op pos: the new entry may take its content (alias_copy).
static int alias_candidate(const TexKey *k, const tex_t *t, uint32_t pos) {
    for (const Tex *p = nvn_hm_get(&by_addr, &k->addr); p; p = p->next_at)
        if (live_at(p, pos) && !p->f.depth && !p->f.stencil && p->t.L.size == t->L.size) return 1;
    return 0;
}

static Tex *entry_of(const tex_t *t) {
    TexKey k;
    make_key(t, &k);
    Tex *e = nvn_hm_get(&by_key, &k);
    uint32_t pos = vk_exec_replay_pos();
    // A chunk of a chunked replay makes the entry as of its own op: whether it takes the content of a texture at its
    // storage (alias_copy) depends on what the chunks before it render there, and those may still be recording on
    // other threads. Taken from a render they had not recorded yet, the new image got the storage's old bytes instead:
    // on an RX 7800 XT the grass near Link was missing in 3 of 29 launches with 4 threads (a map texture drawn once
    // per tile, kept from then on). With a live colour texture of its size at the storage, the chunk waits until the
    // chunks before it are recorded, the texture lock released for the wait (as often as this thread holds it).
    if (!e && pos != UINT32_MAX && tex_alias != 0 && alias_candidate(&k, t, pos) && vk_exec_chunk_behind()) {
        tex_t own = *t;   // t may point into tslots, which a registration moves while the lock is released
        t = &own;
        int d = tex_depth;
        for (int i = 0; i < d; i++) unlock();
        vk_exec_chunk_settle();
        for (int i = 0; i < d; i++) lock();
        tex_settles++;
        if ((e = nvn_hm_get(&by_key, &k))) {   // another chunk made it meanwhile
            entry_live(e);
            e->t = *t;
            return e;
        }
    }
    if (!e) {
        if (tex_alias < 0) {
            tex_alias = cfg_bool("TOTK_TEX_ALIAS");
            rt_log("vk: texture aliases: %s", tex_alias ? "a new image takes the live image's content" : "a new image starts empty");
        }
        if (!(e = create_tex(t, &k, tex_alias))) return NULL;
        perf_count(PERF_CNT_TEX_NEW);
        dropped_check(&k);
        e->use_seq = ++use_counter;
        // released entries of another descriptor at this address are replaced by the new one; a live one holding
        // GPU-written content is the alias source, preferring the one that differs only in swizzle (same_but_swizzle)
        int alias = 0;
        Tex *src = NULL;
        for (Tex *p = nvn_hm_get(&by_addr, &k.addr), *nx; p; p = nx) {
            nx = p->next_at;
            if (p->released) { destroy_tex(p); continue; }
            alias = 1;
            if (!live_at(p, pos)) continue;   // the scan revives it at a later op: released at this chunk's
            if (tex_alias && (!src || (same_but_swizzle(&p->key, &k) && !same_but_swizzle(&src->key, &k))) &&
                alias_compatible(e, p) && alias_has_gpu_content(p, pos))
                src = p;
        }
        if (alias) perf_count(PERF_CNT_TEX_ALIAS);
        if (tex_alias) {
            if (src && alias_copy(e, src)) {
                n_alias_copied++;
                if (e->pub.mirrored) {   // the content is src's GPU render, newer than the storage bytes
                    vk_mem_changed(e->pub.addr, e->t.L.size, 0, &e->wgen);
                    e->uploaded = 1;
                    e->pub.gpu_owned = 1;
                }
            } else {
                init_image(e, vk_upload_begin());
                vk_upload_end();
                if (alias) {
                    if (e->pub.mirrored) n_alias_upload++;
                    else n_alias_uninit++;
                }
            }
        }
        e->next_at = nvn_hm_get(&by_addr, &k.addr);
        nvn_hm_put(&by_addr, &k.addr, e);
        nvn_hm_put(&by_key, &k, e);
        tex_gen_bump(k.addr);
    }
    entry_live(e);
    e->t = *t;
    return e;
}

// vk_dump: upload bookkeeping of an image vk_texture_get / vk_texture_by_id returned. pages_written: whether its
// storage pages were written since the last check (1, 0, -1 not tracked); the query leaves that check's state alone.
void vk_texture_debug(const VkTexImage *img, uint32_t *uploads, int *last_change, int *pages_written) {
    lock();
    const Tex *e = (const Tex *)img;   // pub is the first member
    uint32_t now;
    *uploads = e->n_up;
    *last_change = e->last_ch;
    *pages_written = e->pub.mirrored && e->uploaded ? vk_mem_changed(e->pub.addr, e->t.L.size, e->wgen, &now) : 0;
    unlock();
}

const VkTexImage *vk_texture_get(const tex_t *t) {
    if (!t || !t->mem) return NULL;
    lock();
    Tex *e = entry_of(t);
    if (e) make_fresh(e);
    unlock();
    return e ? &e->pub : NULL;
}

// The replay's state scan (vk_exec.c): the entry vk_texture_get would return, neither created nor made current; the
// recording does that. It is revived here, in replay order, and remembers the op it happened at: a chunk may bind a
// finalized target's slot before the chunk that renders it reached its SET_RT, or long after the scan went past a later
// target at the same storage, and slot_resolve has to answer as the replay would at the chunk's own op (live_at).
const VkTexImage *vk_texture_revive(const tex_t *t, uint32_t pos) {
    if (!t || !t->mem) return NULL;
    TexKey k;
    make_key(t, &k);
    lock();
    Tex *e = nvn_hm_get(&by_key, &k);
    if (e && e->released) {
        e->rev_epoch = submit_epoch, e->rev_pos = pos;
        __atomic_store_n(&arev[agen_idx(k.addr)], submit_epoch, __ATOMIC_RELEASE);
        entry_live(e);
    }
    unlock();
    return e ? &e->pub : NULL;
}

// Render targets the CPU reads without NVN's invalidate (vk_texture_write_back_small); small_pending: one may be dirty.
// wb_limit: targets up to this size (bytes) always go back when the CPU waits for a fence, TOTK_WB_FENCE_LIMIT, read
// once. A larger one goes back unless the GPU wrote it in each of the last WB_RUN frames (wb_wanted). wb_trace:
// TOTK_TRACE=gfx, read together with it.
enum { WB_SMALL_DEFAULT = 64 << 10, WB_RUN = 16 };
static int small_pending;
static uint64_t wb_limit = WB_SMALL_DEFAULT;
static int wb_trace;
static int wb_cfg_read;

static void wb_cfg_init(void) {
    if (wb_cfg_read) return;
    int v = cfg_int("TOTK_WB_FENCE_LIMIT");
    wb_limit = v < 0 ? 0 : (uint64_t)v;
    wb_trace = (rt_trace & RT_TRACE_GFX) != 0;
    wb_cfg_read = 1;
    if (wb_limit != WB_SMALL_DEFAULT) rt_log("vk: fence write-back limit %llu bytes", (unsigned long long)wb_limit);
}

// Whether the fence write-back takes a dirty target. A target the GPU draws every frame is scratch the CPU does not
// read: in Hyrule Field three 256x256 RGBA8 targets and a 256x256 RG8 one, which cost 2.7 ms a frame to write back.
// TotK's 260x260 RG8 grass maps, which its fire spread reads after a fence wait (#132), are drawn once per change, or
// up to 9 frames in a row while a tile loads.
static int wb_wanted(const Tex *e) { return e->t.L.size <= wb_limit || e->run < WB_RUN; }

void vk_texture_gpu_written(const tex_t *t) {
    if (!t || !t->mem) return;
    vk_exec_frame_written(t);
    lock();
    Tex *e = entry_of(t);
    if (e) {
        make_fresh(e);   // a partial render keeps what the storage held before
        if (!e->pub.gpu_owned) {   // entry_of's alias choice of a chunk at an earlier op leaves this render out
            uint32_t pos = vk_exec_replay_pos();
            e->own_epoch = pos == UINT32_MAX ? 0 : submit_epoch, e->own_pos = pos;
        }
        e->pub.gpu_owned = 1;
        uint64_t f = vk_exec_recording_fence();
        if (f > e->wr_fence) e->wr_fence = f;
        if (e->pub.mirrored) {
            e->gpu_dirty = 1;
            storage_rewritten(e, 0, &e->gpu_gen);   // CPU writes up to here are older than this render
            if (wb_exact < 0) wb_exact = cfg_bool("TOTK_WB_EXACT");
            if (wb_exact && !e->snap && e->t.L.size <= SNAP_MAX && fills_no_page(e) && !(e->snap = malloc(SNAP_MAX)))
                rt_fatal("vk: out of memory for a texture's storage copy");
            if (e->snap) memcpy(e->snap, (const void *)(uintptr_t)e->pub.addr, e->t.L.size);
            e->gpu_seq = ++gpu_writes;
            e->gpu_fence = vk_exec_recording_fence();
            uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
            if (fr != e->run_frame || !e->run) {
                e->run = fr == e->run_frame + 1 && e->run ? (uint8_t)(e->run < WB_RUN ? e->run + 1 : WB_RUN) : 1;
                e->run_frame = fr;
            }
            wb_cfg_init();
            if (wb_wanted(e)) __atomic_store_n(&small_pending, 1, __ATOMIC_RELEASE);
            own_add(e);
        }
    }
    unlock();
}

void vk_texture_release(const tex_t *t) {
    if (!t || !t->mem) return;
    TexKey k;
    make_key(t, &k);
    lock();
    Tex *e = nvn_hm_get(&by_key, &k);
    if (e && !e->released) {
        e->released = 1;
        e->rel_ms = coarse_ms;
        e->rel_seq = use_counter;
        nreleased++;
        rel_append(e);
        tex_gen_bump(k.addr);
        sweep_released();
    }
    unlock();
}

void vk_texture_range_gone(uint64_t lo, uint64_t size, VmaAllocation alloc) {
    lock();
    Tex **list = malloc(sizeof *list * (size_t)(by_key.n + 1));
    int n = 0;
    for (uint32_t i = 0; i < by_key.cap; i++) {
        Tex *e = by_key.vals[i];
        if (e && ((e->key.addr >= lo && e->key.addr - lo < size) || (alloc && e->bound_to == alloc))) list[n++] = e;
    }
    for (int i = 0; i < n; i++) destroy_tex(list[i]);
    free(list);
    unlock();
}

// ---------------------------------------------------------------- descriptor slots
void vk_texture_register(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || id >= (1 << 20)) rt_fatal("vk: texture slot %d out of range", id);
    lock();
    if (id >= ntslots) {
        int n = ntslots ? ntslots : 1024;
        while (n <= id) n *= 2;
        TexSlot *s = realloc(tslots, sizeof *s * (size_t)n);
        if (!s) rt_fatal("vk: out of memory for texture slots");
        memset(s + ntslots, 0, sizeof *s * (size_t)(n - ntslots));
        tslots = s, ntslots = n;
    }
    if (!sinfo) {
        uint64_t *p = calloc(1 << 20, sizeof *p);
        uint32_t *q = calloc(1 << 20, sizeof *q);
        if (!p || !q) rt_fatal("vk: out of memory for texture slot tokens");
        __atomic_store_n(&sepoch, q, __ATOMIC_RELEASE);
        __atomic_store_n(&sinfo, p, __ATOMIC_RELEASE);
    }
    TexSlot *s = &tslots[id];
    s->t = *t;
    s->used = 1;
    s->ce = NULL;
    slots_gen++;
    if (!++reg_serial) reg_serial = 1;
    __atomic_store_n(&sinfo[id], (uint64_t)reg_serial << 16 | agen_idx((uintptr_t)t->mem), __ATOMIC_RELEASE);
    s->has_view = v != NULL;
    if (v) s->v = *v;
    else memset(&s->v, 0, sizeof s->v);
    unlock();
}

// The entry slot s resolves to at op pos, and the view and has_view a caller gets with it (the slot's, with the
// descriptor's swizzle when the entry is another descriptor at the same storage). NULL when the slot names no texture.
static Tex *slot_resolve(TexSlot *s, view_t *view, int *has_view, uint32_t pos) {
    *view = s->v;
    *has_view = s->has_view;
    TexKey k;
    make_key(&s->t, &k);
    Tex *e = nvn_hm_get(&by_key, &k);
    if (!e || !live_at(e, pos)) {   // MNG hle_nvn.c: a finalized slot follows the live texture at its storage
        // Prefer the live entry that differs from the slot descriptor only in its swizzle: transient render targets
        // share storage, and the newest one at the address is usually another size. The map screen samples its
        // 2250x1080 map target through an (R,G,B,1) descriptor and followed a 960x60 target at the same address.
        // Only an entry of the slot's texel format follows (sRGB aside): Maxwell reads the storage through the slot
        // descriptor's format, and an entry of another format holds another texture's texels. The quick menu samples
        // its 768x432 RGB10A2 scene capture through a slot whose entry was released; after a fuse the Autobuild
        // thumbnail of the fused object, RGBA8 at the same storage, was the live entry, and the menu drew it behind
        // the wheel (the object's silhouette, or garbled colours). Without such an entry the released one keeps the
        // slot: its image holds what was last written through that descriptor.
        Tex *own = e, *any = NULL;
        e = NULL;
        for (Tex *p = nvn_hm_get(&by_addr, &k.addr); p; p = p->next_at) {
            if (!live_at(p, pos) || linear_format(p->key.d.format) != linear_format(k.d.format)) continue;
            if (!any) any = p;
            if (same_but_swizzle(&p->key, &k)) { e = p; break; }
        }
        if (!e) e = any ? any : own;
    }
    TexSlot copy;   // entry_of may release the lock (a chunk's wait), and a registration then moves tslots
    if (!e && s->t.mem) copy = *s, s = &copy, e = entry_of(&s->t);
    // A followed texture is another descriptor at the same storage: its image holds the content, but sampling takes the
    // slot descriptor's swizzle. The UI renders text into RGBA8 and samples it through a (A,A,A,A) descriptor; with the
    // render target's RGBA swizzle the text blur saw R = 1 everywhere and drew solid boxes behind tips and labels.
    if (e && !(s->has_view && s->v.set_swizzle)) {
        const nvn_texdesc *d = &s->t.d;
        uint8_t sw[4] = {(uint8_t)d->swizzle_r, (uint8_t)d->swizzle_g, (uint8_t)d->swizzle_b, (uint8_t)d->swizzle_a};
        if (!(sw[0] | sw[1] | sw[2] | sw[3]))   // descriptors built without nvnTextureBuilderSetDefaults
            sw[0] = NVN_TEXTURE_SWIZZLE_R, sw[1] = NVN_TEXTURE_SWIZZLE_G, sw[2] = NVN_TEXTURE_SWIZZLE_B,
            sw[3] = NVN_TEXTURE_SWIZZLE_A;
        if (memcmp(sw, e->pub.swz, 4)) {
            if (!s->has_view) memset(view, 0, sizeof *view);
            memcpy(view->swz, sw, 4);
            view->set_swizzle = 1;
            *has_view = 1;
        }
    }
    return e;
}

void vk_texture_lock(void) { lock(); }
void vk_texture_unlock(void) { unlock(); }
uint32_t vk_texture_slots_gen(void) { return slots_gen; }

uint64_t vk_texture_slot_token(int id) {
    const uint64_t *si = __atomic_load_n(&sinfo, __ATOMIC_ACQUIRE);
    if (!si || id < 0 || id >= (1 << 20)) return 0;
    uint64_t v = __atomic_load_n(&si[id], __ATOMIC_ACQUIRE);
    return v ? (v >> 16) << 32 | __atomic_load_n(&agen[v & 0xffff], __ATOMIC_ACQUIRE) : 0;
}
uint32_t vk_sampler_gen(void) { return __atomic_load_n(&samp_gen, __ATOMIC_RELAXED); }

const VkTexImage *vk_texture_by_id(int id, view_t *view, int *has_view) {
    lock();
    const VkTexImage *r = vk_texture_by_id_locked(id, view, has_view);
    unlock();
    return r;
}

const VkTexImage *vk_texture_by_id_locked(int id, view_t *view, int *has_view) {
    if (id < 0 || id >= ntslots || !tslots[id].used) return NULL;
    TexSlot *s = &tslots[id];
    uint32_t idx = sinfo[id] & 0xffff, g = agen[idx];
    uint32_t pos = arev[idx] == submit_epoch ? vk_exec_replay_pos() : UINT32_MAX;
    if (pos != UINT32_MAX) {   // what the storage holds depends on the op: chunks at other ops share no answer in s
        view_t v;
        int hv;
        Tex *f = slot_resolve(s, &v, &hv, pos);
        s = &tslots[id];   // slot_resolve may have released the lock
        if (f && (!f->uploaded || f->checked != submit_epoch)) {
            make_fresh_snap(f);
            f->checked = submit_epoch;
        }
        if (view) *view = has_view ? v : s->v;
        if (has_view) *has_view = view ? hv : s->has_view;
        return f ? &f->pub : NULL;
    }
    if (!s->ce || s->cgen != g) {   // resolved again: an entry at its address was created, dropped or released, or the
        view_t v;                   // slot registered (s->ce cleared); an empty result is not kept: image creation may
        int hv;                     // succeed later
        uint32_t settles = tex_settles;
        Tex *r = slot_resolve(s, &v, &hv, UINT32_MAX);   // entry_of may create an entry and bump g: g read before it
        s = &tslots[id];
        if (settles != tex_settles) {   // the lock was released for a wait: the slot may be another texture by now
            if (r && (!r->uploaded || r->checked != submit_epoch)) make_fresh_snap(r), r->checked = submit_epoch;
            if (view) *view = has_view ? v : s->v;
            if (has_view) *has_view = view ? hv : s->has_view;
            return r ? &r->pub : NULL;
        }
        s->ce = r;
        s->cview = v, s->chas_view = (uint8_t)hv;
        s->cgen = g;
        s->cepoch = 0;
        __atomic_store_n(&sepoch[id], 0, __ATOMIC_RELAXED);
    }
    Tex *e = s->ce;
    // Bindings of one submit share one check, answered from the submit's write-watch snapshot (guest writes during a
    // replay race the GPU read on hardware too). After one check an entry is either uploaded or never uploads (not
    // mirrored, GPU-owned or no CPU layout), so a slot that checked its entry in this submit has nothing left to do.
    if (e && s->cepoch != submit_epoch) {
        if (!e->uploaded || e->checked != submit_epoch) {
            make_fresh_snap(e);
            e->checked = submit_epoch;
        }
        s->cepoch = submit_epoch;
        __atomic_store_n(&sepoch[id], submit_epoch, __ATOMIC_RELEASE);
    }
    if (view) *view = has_view ? s->cview : s->v;
    if (has_view) *has_view = view ? s->chas_view : s->has_view;
    return e ? &e->pub : NULL;
}

// vk_dump: the swizzle selectors of the descriptor registered in slot id; 0 when the slot is empty.
int vk_texture_slot_swz(int id, uint8_t swz[4]) {
    lock();
    int ok = id >= 0 && id < ntslots && tslots[id].used;
    if (ok) {
        const nvn_texdesc *d = &tslots[id].t.d;
        swz[0] = (uint8_t)d->swizzle_r, swz[1] = (uint8_t)d->swizzle_g, swz[2] = (uint8_t)d->swizzle_b;
        swz[3] = (uint8_t)d->swizzle_a;
    }
    unlock();
    return ok;
}

uint32_t vk_texture_epoch(void) { return __atomic_load_n(&submit_epoch, __ATOMIC_RELAXED); }

int vk_texture_slot_moves(int id) {
    const uint64_t *si = __atomic_load_n(&sinfo, __ATOMIC_ACQUIRE);
    if (!si || id < 0 || id >= (1 << 20)) return 0;
    uint64_t v = __atomic_load_n(&si[id], __ATOMIC_RELAXED);
    return __atomic_load_n(&arev[v & 0xffff], __ATOMIC_ACQUIRE) == __atomic_load_n(&submit_epoch, __ATOMIC_RELAXED);
}

int vk_texture_slot_checked(int id) {
    const uint32_t *e = __atomic_load_n(&sepoch, __ATOMIC_ACQUIRE);
    return e && id >= 0 && id < (1 << 20) &&
           __atomic_load_n(&e[id], __ATOMIC_ACQUIRE) == __atomic_load_n(&submit_epoch, __ATOMIC_RELAXED);
}

void vk_texture_new_submit(void) {
    lock();
    if (!++submit_epoch) submit_epoch = 1;
    coarse_ms = plat_now_ms();
    sweep_pressure();
    static uint64_t log_at;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) / 60;
    if ((rt_trace & RT_TRACE_PERF) && f != log_at && tex_alias > 0) {
        log_at = f;
        rt_log("perf: frame %llu: texture aliases: content copied %llu, filled from storage %llu, left uninitialised %llu",
               (unsigned long long)f * 60, (unsigned long long)n_alias_copied, (unsigned long long)n_alias_upload,
               (unsigned long long)n_alias_uninit);
        n_alias_copied = n_alias_upload = n_alias_uninit = 0;
        rt_log("perf: frame %llu: texture re-uploads %llu (%.1f MiB, %.2f ms), after WriteTexels %llu (%.1f MiB, %.2f ms)",
               (unsigned long long)f * 60, (unsigned long long)ru_n, ru_bytes / 1048576.0, ru_ns / 1e6,
               (unsigned long long)ru_wt_n, ru_wt_bytes / 1048576.0, ru_wt_ns / 1e6);
        ru_n = ru_bytes = ru_ns = ru_wt_n = ru_wt_bytes = ru_wt_ns = 0;
        char ub[512];
        size_t uo = 0;
        for (int i = 0; i < UP_WHY; i++)
            if (up_n[i] && uo < sizeof ub)
                uo += (size_t)snprintf(ub + uo, sizeof ub - uo, "%s %s %llu (%.1f MiB, %.2f ms)", uo ? "," : "",
                                       up_why_name[i], (unsigned long long)up_n[i], up_bytes[i] / 1048576.0, up_ns[i] / 1e6);
        if (uo) rt_log("perf: frame %llu: texture uploads by path:%s", (unsigned long long)f * 60, ub);
        memset(up_n, 0, sizeof up_n), memset(up_bytes, 0, sizeof up_bytes), memset(up_ns, 0, sizeof up_ns);
        uint64_t ck, calls, untr;
        vk_mem_snap_stats(&ck, &calls, &untr);
        rt_log("perf: frame %llu: binding storage checks %llu (texture %llu), write-watch snapshot queries %llu, untracked %llu",
               (unsigned long long)f * 60, (unsigned long long)ck, (unsigned long long)n_checks, (unsigned long long)calls,
               (unsigned long long)untr);
        n_checks = 0;
        rt_log("perf: frame %llu: released textures kept %d, dropped under VRAM pressure %llu (%llu overwritten)",
               (unsigned long long)f * 60, nreleased, (unsigned long long)n_pressure_drops,
               (unsigned long long)n_pressure_overwritten);
        n_pressure_drops = n_pressure_overwritten = 0;
    }
    unlock();
}

// ---------------------------------------------------------------- texel reads and writes
typedef struct { unsigned level, layer0; int nz, is3d, bw, bh, bpb; size_t wb, hb, row, img, x0, y0; } Region;

static int region_of(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint64_t row, uint64_t img, Region *g) {
    g->bpb = nvn_fmt_bpb(t->d.format, &g->bw, &g->bh);
    if (!g->bpb || r->width <= 0 || r->height <= 0 || r->depth < 0 || r->xoffset < 0 || r->yoffset < 0 || r->zoffset < 0 ||
        r->xoffset % g->bw || r->yoffset % g->bh)
        return 0;
    g->level = v && v->set_levels ? v->base_level : 0;
    g->layer0 = v && v->set_layers ? v->min_layer : 0;
    g->nz = r->depth ? r->depth : 1;
    g->is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    g->wb = (size_t)((r->width + g->bw - 1) / g->bw) * (size_t)g->bpb;
    g->hb = (size_t)(r->height + g->bh - 1) / (size_t)g->bh;
    g->row = row ? row : g->wb;
    g->img = img ? img : g->row * g->hb;
    g->x0 = (size_t)(r->xoffset / g->bw) * (size_t)g->bpb;
    g->y0 = (size_t)(r->yoffset / g->bh);
    return 1;
}

// Pool bytes <-> caller rows of the region alone (nvn_texels_box). MacNativeGames' nvn_mtl.m goes through a
// linear copy of the whole level; for a small region of a large texture that cost tens of milliseconds while the guest
// thread held its virtual core and the caller the texture lock (monsters2 replay, frames 765-790).
static int storage_texels(const tex_t *t, const Region *g, const NVNcopyRegion *r, uint8_t *mem, int write) {
    for (int z = 0; z < g->nz; z++) {
        unsigned layer = g->is3d ? 0 : g->layer0 + (unsigned)r->zoffset + (unsigned)z;
        uint32_t slice = g->is3d ? (uint32_t)(r->zoffset + z) : 0;
        if (nvn_texels_box(&t->d, g->level, layer, (uint32_t)g->x0, (uint32_t)g->y0, slice, (uint32_t)g->wb,
                           (uint32_t)g->hb, t->mem, mem + (size_t)z * g->img, g->row, !write))
            return 0;
    }
    return 1;
}

static int read_aspect(const Tex *e, VkImageAspectFlags *a) {
    if (!e->f.depth && !e->f.stencil) { *a = VK_IMAGE_ASPECT_COLOR_BIT; return 1; }
    switch (e->t.d.format) {   // packed depth-stencil storage has no single-aspect copy
    case 0x31: *a = VK_IMAGE_ASPECT_STENCIL_BIT; return 1;
    case 0x32: case 0x34: *a = VK_IMAGE_ASPECT_DEPTH_BIT; return 1;
    default: return 0;
    }
}

// An image region read through a staging buffer. texels_read_record records the copy into the open upload batch;
// texels_read_take copies the rows out once that batch is done (vk_upload_flush(1)) and frees the buffer. Several
// records before one flush cost one GPU round trip instead of one each.
typedef struct {
    VkBuffer sb;
    VmaAllocation sa;
    const uint8_t *stg;
    size_t slice, ib;   // ib: bytes per texel of an expanded RGB image, 0 when the image holds the storage layout
    uint32_t w;
} TexRead;

static int texels_read_record(Tex *e, const Region *g, const NVNcopyRegion *r, TexRead *t) {
    static volatile int32_t n_astc, n_ds;
    VkImageAspectFlags aspect;
    if (e->f.astc_bw) {
        log_limited(&n_astc, "ReadTexels of ASTC texture %#llx: the image holds decoded RGBA8; zeros returned",
                    (unsigned long long)e->pub.addr);
        return 0;
    }
    if (!read_aspect(e, &aspect)) {
        log_limited(&n_ds, "texels of packed depth-stencil format %#x are not copied", e->t.d.format);
        return 0;
    }
    if (g->level >= e->pub.levels) return 0;
    uint32_t w = (uint32_t)r->width, h = (uint32_t)r->height;
    t->w = w;
    t->ib = e->f.expand_rgb ? (size_t)g->bpb / 3 * 4 : 0;
    t->slice = t->ib ? (size_t)w * h * t->ib : g->wb * g->hb;
    t->stg = vk_staging(t->slice * (size_t)g->nz, &t->sb, &t->sa);
    VkBufferImageCopy *rg = calloc((size_t)g->nz, sizeof *rg);
    if (!rg) rt_fatal("vk: out of memory for a texture read");
    for (int z = 0; z < g->nz; z++)
        rg[z] = (VkBufferImageCopy){.bufferOffset = t->slice * (size_t)z,
                                    .imageSubresource = {aspect, g->level, g->is3d ? 0 : g->layer0 + (uint32_t)r->zoffset + (uint32_t)z, 1},
                                    .imageOffset = {r->xoffset, r->yoffset, g->is3d ? r->zoffset + z : 0},
                                    .imageExtent = {w, h, 1}};
    VkCommandBuffer cb = vk_upload_begin();
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    vkCmdCopyImageToBuffer(cb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, t->sb, (uint32_t)g->nz, rg);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_HOST_READ_BIT);
    vk_upload_end();
    free(rg);
    return 1;
}

static void texels_read_take(const Tex *e, const Region *g, const TexRead *t, uint8_t *mem) {
    VkCtx *c = vk_ctx();
    vmaInvalidateAllocation(c->vma, t->sa, 0, VK_WHOLE_SIZE);
    for (int z = 0; z < g->nz; z++) {
        const uint8_t *s = t->stg + t->slice * (size_t)z;
        uint8_t *m = mem + (size_t)z * g->img;
        for (size_t y = 0; y < g->hb; y++) {
            if (t->ib) shrink_rgb(e->t.d.format, s + y * t->w * t->ib, m + y * g->row, t->w);
            else memcpy(m + y * g->row, s + y * g->wb, g->wb);
        }
    }
    vmaDestroyBuffer(c->vma, t->sb, t->sa);
}

// Image <-> caller rows through a staging buffer; the GPU copy is waited for.
static int image_texels(Tex *e, const Region *g, const NVNcopyRegion *r, uint8_t *mem, int write) {
    static volatile int32_t n_astc, n_ds;
    VkCtx *c = vk_ctx();
    VkImageAspectFlags aspect;
    if (!write) {
        TexRead t;
        if (!texels_read_record(e, g, r, &t)) return 0;
        vk_upload_flush(1);
        texels_read_take(e, g, &t, mem);
        return 1;
    }
    if (!read_aspect(e, &aspect)) {
        log_limited(&n_ds, "texels of packed depth-stencil format %#x are not copied", e->t.d.format);
        return 0;
    }
    if (g->level >= e->pub.levels) return 0;
    uint32_t w = (uint32_t)r->width, h = (uint32_t)r->height;
    int rgb = e->f.expand_rgb, astc = e->f.astc_bw != 0, agpu = astc && vk_astc_gpu();
    size_t texels = (size_t)w * h, ib = agpu ? 0 : astc ? 4 : rgb ? (size_t)g->bpb / 3 * 4 : 0;
    size_t slice = ib ? texels * ib : g->wb * g->hb;   // agpu: the raw blocks, decoded by vk_astc_record
    VkBuffer sb;
    VmaAllocation sa;
    uint8_t *stg = vk_staging(slice * (size_t)g->nz, &sb, &sa);
    VkBufferImageCopy *rg = calloc((size_t)g->nz, sizeof *rg);
    for (int z = 0; z < g->nz; z++) {
        uint8_t *s = stg + slice * (size_t)z;
        const uint8_t *m = mem + (size_t)z * g->img;
        if (agpu) {
            for (size_t y = 0; y < g->hb; y++) memcpy(s + y * g->wb, m + y * g->row, g->wb);
        } else if (astc) {
            uint8_t *tight = malloc(g->wb * g->hb);
            for (size_t y = 0; y < g->hb; y++) memcpy(tight + y * g->wb, m + y * g->row, g->wb);
            uint64_t a0 = perf_begin();
            astc_decode_ldr_mt(tight, w, h, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb, s);
            perf_end(PERF_ASTC, a0);
            free(tight);
        } else if (rgb) {
            for (size_t y = 0; y < g->hb; y++) expand_rgb(e->t.d.format, m + y * g->row, s + y * w * ib, w);
        } else {
            for (size_t y = 0; y < g->hb; y++) memcpy(s + y * g->wb, m + y * g->row, g->wb);
        }
        rg[z] = (VkBufferImageCopy){.bufferOffset = slice * (size_t)z,
                                    .imageSubresource = {aspect, g->level, g->is3d ? 0 : g->layer0 + (uint32_t)r->zoffset + (uint32_t)z, 1},
                                    .imageOffset = {r->xoffset, r->yoffset, g->is3d ? r->zoffset + z : 0},
                                    .imageExtent = {w, h, 1}};
    }
    VkCommandBuffer cb = vk_upload_begin();
    vmaFlushAllocation(c->vma, sa, 0, VK_WHOLE_SIZE);
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int done = 0;
    // A BC3 image takes whole 4x4 blocks, or a part that ends on the mip's edge; a region of a 5x5 or 6x6 footprint
    // may end inside a block (vk_astc_record checks the start corner).
    uint32_t mw = e->t.d.width >> g->level, mh = e->t.d.height >> g->level;
    int bc_cut = e->f.astc_bc && (((w & 3) && (uint32_t)r->xoffset + w != (mw ? mw : 1)) ||
                                  ((h & 3) && (uint32_t)r->yoffset + h != (mh ? mh : 1)));
    if (agpu && !bc_cut) {   // bc_cut: nothing recorded, logged below as not decoded
        VkAstcRegion *ar = calloc((size_t)g->nz, sizeof *ar);
        for (int z = 0; z < g->nz; z++)
            ar[z] = (VkAstcRegion){.src = slice * (size_t)z, .w = w, .h = h, .slices = 1, .level = rg[z].imageSubresource.mipLevel,
                                   .layer = rg[z].imageSubresource.baseArrayLayer, .offset = rg[z].imageOffset};
        uint64_t a0 = perf_begin();
        done = vk_astc_record(cb, sb, slice * (size_t)g->nz, ar, g->nz, e->f.astc_bw, e->f.astc_bh, e->f.astc_srgb, e->f.astc_bc,
                              e->pub.image, aspect, VK_NULL_HANDLE);
        perf_end(PERF_ASTC, a0);
        free(ar);
    }
    if (!agpu) vkCmdCopyBufferToImage(cb, sb, e->pub.image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)g->nz, rg);
    else if (!done) log_limited(&n_astc, "WriteTexels of ASTC texture %#llx not decoded on the GPU: left as it was",
                                (unsigned long long)e->pub.addr);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    vk_upload_end();
    vk_upload_flush(1);
    free(rg);
    vmaDestroyBuffer(c->vma, sb, sa);
    return 1;
}

static void zero_rows(const Region *g, uint8_t *dst) {
    for (int z = 0; z < g->nz; z++)
        for (size_t y = 0; y < g->hb; y++) memset(dst + (size_t)z * g->img + y * g->row, 0, g->wb);
}

int vk_texture_read_image(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row, uint64_t img) {
    Region g;
    if (!t || !region_of(t, v, r, row, img, &g)) return 0;
    lock();
    Tex *e = entry_of(t);
    int ok = 0;
    if (e) {
        make_fresh(e);
        ok = image_texels(e, &g, r, dst, 0);
    }
    unlock();
    if (!ok) zero_rows(&g, dst);
    return ok;
}

// ReadTexels: the pool bytes are the content of a mirrored texture the GPU has not rendered into; every other texture
// is read back from its image.
void vk_read_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row, uint64_t img) {
    static volatile int32_t n_bad;
    Region g;
    if (!region_of(t, v, r, row, img, &g)) {
        log_limited(&n_bad, "ReadTexels: invalid region %d,%d,%d %dx%dx%d (format %#x)", r->xoffset, r->yoffset,
                    r->zoffset, r->width, r->height, r->depth, t->d.format);
        return;
    }
    lock();
    Tex *e = entry_of(t);
    int ok = 0;
    if (e && e->pub.mirrored && !e->pub.gpu_owned) ok = storage_texels(t, &g, r, dst, 0);
    else if (e) {
        make_fresh(e);
        ok = image_texels(e, &g, r, dst, 0);
    }
    unlock();
    if (!ok) zero_rows(&g, dst);
}

// WriteTexels: a mirrored texture gets the texels in its pool bytes (and in its image once the GPU owns it); every
// other texture in its image.
void vk_write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row, uint64_t img) {
    static volatile int32_t n_bad;
    Region g;
    if (!region_of(t, v, r, row, img, &g) || (v && v->set_format && v->format != t->d.format)) {
        log_limited(&n_bad, "WriteTexels: invalid region %d,%d,%d %dx%dx%d or view format (format %#x)", r->xoffset,
                    r->yoffset, r->zoffset, r->width, r->height, r->depth, t->d.format);
        return;
    }
    lock();
    Tex *e = entry_of(t);
    if (e && e->pub.mirrored) {
        if (!storage_texels(t, &g, r, (uint8_t *)src, 1))
            log_limited(&n_bad, "WriteTexels: region outside level %u of texture %#llx", g.level, (unsigned long long)e->pub.addr);
        vk_guest_written((uintptr_t)t->mem, t->L.size);
        if (e->pub.gpu_owned) image_texels(e, &g, r, (uint8_t *)src, 1);
        else if (e->uploaded) e->wt_n++;
    } else if (e) {
        image_texels(e, &g, r, (uint8_t *)src, 1);
    }
    unlock();
}

// ---------------------------------------------------------------- CPU reads of GPU-written storage
// A mirrored texture the GPU rendered into holds its texels in its image only; its storage keeps what the CPU last put
// there. The game may still read that storage with the CPU: TotK moves its generated grass maps between pools with
// memcpy once the GPU is done, and the copy then carried zeros. Such a read writes the image back first.
//
// Every level and layer of every texture in v is copied to staging in one upload batch with one wait: one GPU round
// trip per level and layer took 0.1 ms each, and a 260x260 grass map has 9 levels. The storage is then written in the
// order of v, so where two textures share storage the later one wins.
typedef struct {
    Tex *e;
    Region g;
    NVNcopyRegion r;
    TexRead t;
} WbPart;

static void write_back_n(Tex **v, int n) {
    int np = 0, cap = 0;
    WbPart *p = NULL;
    for (int i = 0; i < n; i++) {
        Tex *e = v[i];
        const nvn_texdesc *d = &e->t.d;
        int linear = nvn_tex_is_linear(d), is3d = d->target == NVN_TEXTURE_TARGET_3D;
        uint32_t levels = linear ? 1 : e->pub.levels, layers = linear || is3d ? 1 : e->pub.layers;
        for (uint32_t lv = 0; lv < levels; lv++)
            for (uint32_t ly = 0; ly < layers; ly++) {
                if (np == cap) {
                    cap = cap ? cap * 2 : 16;
                    if (!(p = realloc(p, sizeof *p * (size_t)cap))) rt_fatal("vk: out of memory for a texture write-back");
                }
                WbPart *q = &p[np];
                view_t vw = {.set_levels = 1, .set_layers = 1, .base_level = (uint8_t)lv, .min_layer = (uint16_t)ly};
                q->e = e;
                q->r = (NVNcopyRegion){.width = (int)mip(d->width, lv),
                                       .height = d->target == 0 || d->target == 3 ? 1 : (int)mip(d->height, lv),
                                       .depth = is3d ? (int)mip(d->depth, lv) : 1};
                if (region_of(&e->t, &vw, &q->r, 0, 0, &q->g) && texels_read_record(e, &q->g, &q->r, &q->t)) np++;
            }
    }
    if (np) vk_upload_flush(1);
    for (int i = 0; i < np; i++) {
        WbPart *q = &p[i];
        uint8_t *buf = malloc(q->g.img * (size_t)q->g.nz);
        if (!buf) rt_fatal("vk: out of memory for a texture write-back");
        texels_read_take(q->e, &q->g, &q->t, buf);
        storage_texels(&q->e->t, &q->g, &q->r, buf, 1);
        free(buf);
    }
    free(p);
    for (int i = 0; i < n; i++) v[i]->gpu_dirty = 0;
}

// Mirrored textures overlapping [addr, addr+size) that the GPU wrote since their last write-back.
static int owned_dirty(uint64_t addr, uint64_t size, Tex **hit) {
    int n = 0;
    for (int i = owned_lower(addr > max_owned ? addr - max_owned : 0); i < nowned && owned[i]->pub.addr < addr + size; i++) {
        Tex *e = owned[i];
        if (__atomic_load_n(&e->gpu_dirty, __ATOMIC_RELAXED) && e->pub.addr + e->t.L.size > addr) {
            if (!hit) return 1;
            hit[n++] = e;
        }
    }
    return n;
}

// A storage image may be written by the shaders of the submission being recorded (texture lock held, vk_exec_draw.c
// tex_info). Its storage is not marked GPU-owned: only the write fence for ReadTexels.
void vk_texture_storage_bound_locked(const VkTexImage *img) {
    Tex *e = (Tex *)img;   // pub is the first member
    uint64_t f = vk_exec_recording_fence();
    if (f > e->wr_fence) e->wr_fence = f;
}

uint64_t vk_texture_last_write(const tex_t *t) {
    lock();
    Tex *e = entry_of(t);
    uint64_t f = e ? e->wr_fence : 0;
    unlock();
    return f;
}

size_t vk_texture_texel_span(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint64_t row, uint64_t img) {
    Region g;
    if (!region_of(t, v, r, row, img, &g)) return 0;
    return (size_t)(g.nz - 1) * g.img + (g.hb - 1) * g.row + g.wb;
}

int vk_texture_cpu_stale(uint64_t addr, uint64_t size) {
    if (!size || !__atomic_load_n(&nowned, __ATOMIC_ACQUIRE)) return 0;
    plat_mutex_lock_shared(&owned_lock);
    int dirty = owned_dirty(addr, size, NULL);
    plat_mutex_unlock_shared(&owned_lock);
    return dirty;
}

// The newest submission recorded to write a GPU-written mirrored texture overlapping [addr, addr+size); 0: none.
uint64_t vk_texture_dirty_fence(uint64_t addr, uint64_t size) {
    if (!size || !__atomic_load_n(&nowned, __ATOMIC_ACQUIRE)) return 0;
    lock();
    uint64_t f = 0;
    for (int i = owned_lower(addr > max_owned ? addr - max_owned : 0); i < nowned && owned[i]->pub.addr < addr + size; i++) {
        const Tex *e = owned[i];
        if (!e->gpu_dirty || e->pub.addr + e->t.L.size <= addr) continue;
        if (e->gpu_fence > f) f = e->gpu_fence;
        if (e->wr_fence > f) f = e->wr_fence;
    }
    unlock();
    return f;
}

// The image is the newest content only while nothing wrote the storage since the GPU render: TotK finalizes grass maps,
// reuses their storage for new data and memcpys that data on, and writing the old image back first handed the copy
// a stale map or stale resource bytes (wrong textures in the distance). Every hit is checked before any is written
// back, since a write-back is itself a CPU write to storage another hit may share; the hits then go back oldest render
// first, so where two descriptors share storage the newer render wins.
void vk_texture_write_back(uint64_t addr, uint64_t size) {
    lock();
    Tex **hit = malloc(sizeof *hit * (size_t)(nowned + 1));
    if (!hit) rt_fatal("vk: out of memory for a texture write-back");
    int n = owned_dirty(addr, size, hit), k = 0;   // owned[] cannot change while tex_lock is held
    for (int i = 0; i < n; i++) {
        uint32_t now;
        if (storage_rewritten(hit[i], hit[i]->gpu_gen, &now) == 1) hit[i]->gpu_dirty = 0;
        else hit[k++] = hit[i];
    }
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && hit[j - 1]->gpu_seq > hit[j]->gpu_seq; j--) {
            Tex *x = hit[j];
            hit[j] = hit[j - 1], hit[j - 1] = x;
        }
    write_back_n(hit, k);
    for (int i = 0; i < k; i++) perf_count(PERF_CNT_WB);
    for (int i = k; i < n; i++) perf_count(PERF_CNT_WB_SKIP);
    free(hit);
    unlock();
}

// TotK's grass system renders a 128x128 RGBA8 map of cut and trampled grass into a CPU-cached pool every frame and its
// CPU code reads the pool bytes without nvnMemoryPoolInvalidateMappedRange (cut reactions: blades, insects, the cut
// itself). On the Switch the bytes are there once the frame's fence has passed. So when the
// CPU waits for a fence, mirrored targets rendered by work up to that fence go back to their storage (wb_wanted:
// targets over wb_limit only when not drawn every frame), unless the CPU rewrote it since. Targets drawn every frame
// still go back on an invalidate, memcpy or ReadTexels.
int vk_texture_small_dirty(void) { return __atomic_load_n(&small_pending, __ATOMIC_ACQUIRE); }

// TOTK_TRACE=gfx: a dirty target the fence write-back leaves alone because it is over the limit and drawn every frame
// (what a CPU read after the wait would miss), or sends back although it is over the default limit. One line per
// target address and outcome; the table is small and a full one stops logging.
static void wb_note(const Tex *e, int skipped) {
    static uint64_t seen[2][64];
    static int n_seen[2];
    for (int i = 0; i < n_seen[skipped]; i++)
        if (seen[skipped][i] == e->pub.addr) return;
    if (n_seen[skipped] == 64) return;
    seen[skipped][n_seen[skipped]++] = e->pub.addr;
    const pool_t *p = e->t.pool;
    unsigned fl = p ? p->flags : 0;
    rt_log("vk: fence write-back %s %#llx %ux%u fmt %#x size %llu pool %s (limit %llu)",
           skipped ? "skipped-drawn-every-frame" : "over-default", (unsigned long long)e->pub.addr, e->t.d.width, e->t.d.height,
           e->t.d.format, (unsigned long long)e->t.L.size,
           !p ? "none" : (fl & NVN_MEMORY_POOL_FLAGS_CPU_CACHED) ? "cpu-cached" : (fl & NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED) ? "cpu-uncached" : "other",
           (unsigned long long)wb_limit);
}

void vk_texture_write_back_small(uint64_t fence) {
    lock();
    wb_cfg_init();
    __atomic_store_n(&small_pending, 0, __ATOMIC_RELAXED);
    plat_mutex_lock_shared(&owned_lock);
    int n = 0, later = 0;
    for (int i = 0; i < nowned; i++) n += owned[i]->gpu_dirty && wb_wanted(owned[i]);
    Tex **hit = n ? malloc(sizeof *hit * (size_t)n) : NULL;
    if (n && !hit) rt_fatal("vk: out of memory for a texture write-back");
    n = 0;
    for (int i = 0; i < nowned; i++) {
        Tex *e = owned[i];
        if (!e->gpu_dirty) continue;
        if (!wb_wanted(e)) {
            if (wb_trace && e->gpu_fence <= fence) {
                wb_note(e, 1);
                perf_count(PERF_CNT_WB_SKIP);
            }
            continue;
        }
        if (e->gpu_fence > fence) later = 1;
        else {
            if (wb_trace && e->t.L.size > WB_SMALL_DEFAULT) wb_note(e, 0);
            hit[n++] = e;
        }
    }
    plat_mutex_unlock_shared(&owned_lock);
    if (later) __atomic_store_n(&small_pending, 1, __ATOMIC_RELEASE);
    int k = 0;
    for (int i = 0; i < n; i++) {
        uint32_t now;
        if (storage_rewritten(hit[i], hit[i]->gpu_gen, &now) == 1) hit[i]->gpu_dirty = 0;
        else hit[k++] = hit[i];
    }
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && hit[j - 1]->gpu_seq > hit[j]->gpu_seq; j--) {
            Tex *x = hit[j];
            hit[j] = hit[j - 1], hit[j - 1] = x;
        }
    write_back_n(hit, k);
    for (int i = 0; i < k; i++) perf_count(PERF_CNT_WB);
    for (int i = k; i < n; i++) perf_count(PERF_CNT_WB_SKIP);
    free(hit);
    unlock();
}

// ---------------------------------------------------------------- samplers (LAS textures.c)
static VkCompareOp compare_op(uint32_t f) {
    switch (f) {
    case NVN_COMPARE_FUNC_NEVER: return VK_COMPARE_OP_NEVER;
    case NVN_COMPARE_FUNC_LESS: return VK_COMPARE_OP_LESS;
    case NVN_COMPARE_FUNC_EQUAL: return VK_COMPARE_OP_EQUAL;
    case NVN_COMPARE_FUNC_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case NVN_COMPARE_FUNC_GREATER: return VK_COMPARE_OP_GREATER;
    case NVN_COMPARE_FUNC_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case NVN_COMPARE_FUNC_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static struct { int init, mirror_clamp_edge, minmax, border_no_format; float aniso; } sf;   // aniso: TOTK_ANISO, 0 = the game's

static void sampler_features(VkCtx *c) {
    if (sf.init) return;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .pNext = c->has_custom_border_color ? &cbf : NULL};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);
    sf.mirror_clamp_edge = f12.samplerMirrorClampToEdge;
    sf.minmax = f12.samplerFilterMinmax;
    sf.border_no_format = c->has_custom_border_color && cbf.customBorderColors && cbf.customBorderColorWithoutFormat;
    const char *an = cfg_value("TOTK_ANISO");
    sf.aniso = strcmp(an, "game") ? (float)atoi(an) : 0;
    rt_log("vk: anisotropic filtering: %s", sf.aniso > 1 ? an : "the game's");
    sf.init = 1;
}

static VkSampler make_sampler(VkCtx *c, const samp_par *d) {
    sampler_features(c);
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = d->magf == NVN_MAG_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    uint32_t mf = d->minf;
    si.minFilter = mf == 1 || mf == 3 || mf == 5 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = mf == 4 || mf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode modes[3];
    const uint8_t wrap[3] = {d->ws, d->wt, d->wr};
    int border = 0;
    for (int i = 0; i < 3; i++) {
        switch (wrap[i]) {
        case NVN_WRAP_MODE_CLAMP: case NVN_WRAP_MODE_CLAMP_TO_EDGE: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case NVN_WRAP_MODE_MIRROR_CLAMP: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE:
            modes[i] = sf.mirror_clamp_edge ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            break;
        case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER: case NVN_WRAP_MODE_CLAMP_TO_BORDER:
            modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            border = 1;
            break;
        case NVN_WRAP_MODE_MIRRORED_REPEAT: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        default: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }
    si.addressModeU = modes[0], si.addressModeV = modes[1], si.addressModeW = modes[2];
    si.minLod = d->lod_min;
    si.maxLod = d->lod_max > 1000 ? VK_LOD_CLAMP_NONE : d->lod_max;
    if (mf <= NVN_MIN_FILTER_LINEAR) si.maxLod = si.minLod + 0.25f;   // not mipmapped: stay on the base level
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    float lim = c->props.limits.maxSamplerLodBias;
    si.mipLodBias = fmaxf(-lim, fminf(lim, d->lod_bias));
    // TOTK_ANISO raises the level for filtered, mipmapped colour samplers that repeat on U and V: the surface textures,
    // which tile, so ground and walls seen at a shallow angle stay sharp into the distance. Depth-compare (shadow)
    // samplers, nearest or unmipmapped ones (data maps, LUTs) and clamped ones keep the game's level: clamped samplers
    // read screen-sized buffers, whose coordinates can stretch steeply at an object's edge, where anisotropic taps reach
    // across it and leave a speckled fringe (the inventory's character preview).
    float aniso = d->aniso;
    // The game's own wrap modes: a mirror-clamp one maps to MIRRORED_REPEAT where the device lacks mirror-clamp-to-edge.
    int tiled = (d->ws == NVN_WRAP_MODE_REPEAT || d->ws == NVN_WRAP_MODE_MIRRORED_REPEAT) &&
                (d->wt == NVN_WRAP_MODE_REPEAT || d->wt == NVN_WRAP_MODE_MIRRORED_REPEAT);
    if (sf.aniso > aniso && tiled && !d->cmp_mode && si.magFilter == VK_FILTER_LINEAR && si.minFilter == VK_FILTER_LINEAR &&
        (mf == NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST || mf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR))
        aniso = sf.aniso;
    if (aniso > 1) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = fminf(aniso, c->props.limits.maxSamplerAnisotropy);
    }
    if (d->cmp_mode) {
        si.compareEnable = VK_TRUE;
        si.compareOp = compare_op(d->cmp_func);
        // A depth-compare sampler set to nearest returns 0 or 1 per tap, and the game's shaders smooth shadow edges by
        // a few taps spaced for its own shadow map texels. With TOTK_SHADOW_SCALE those taps cover a fraction of the
        // area and the edges turn hard and stepped; linear filtering makes every tap a 2x2 PCF instead.
        if (shadow_scale() > 1) si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    }
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    VkSamplerReductionModeCreateInfo rm = {.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    const void **tail = &si.pNext;
    if (border) {
        const float *b = d->border;
        if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0) si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (b[0] == 1 && b[1] == 1 && b[2] == 1 && b[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if (sf.border_no_format) {
            memcpy(cb.customBorderColor.float32, b, 16);
            si.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            *tail = &cb;
            tail = &cb.pNext;
        } else si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    if (d->reduction && sf.minmax && !si.compareEnable) {
        rm.reductionMode = d->reduction == NVN_SAMPLER_REDUCTION_MIN ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
        *tail = &rm;
    }
    VkSampler s;
    VkResult r = vkCreateSampler(c->device, &si, NULL, &s);
    if (r != VK_SUCCESS) rt_fatal("vk: vkCreateSampler failed (%d)", (int)r);
    return s;
}

void vk_sampler_register(int id, const samp_par *s) {
    if (id < 0 || id >= (1 << 12)) rt_fatal("vk: sampler slot %d out of range", id);
    lock();
    if (id >= nsslots) {
        int n = nsslots ? nsslots : 256;
        while (n <= id) n *= 2;
        SampSlot *p = realloc(sslots, sizeof *p * (size_t)n);
        if (!p) rt_fatal("vk: out of memory for sampler slots");
        memset(p + nsslots, 0, sizeof *p * (size_t)(n - nsslots));
        sslots = p, nsslots = n;
    }
    sslots[id].p = *s;
    sslots[id].s = sslots[id].sp = VK_NULL_HANDLE;
    sslots[id].used = 1;
    samp_gen++;
    unlock();
}

// DLSS upscaling (vk_sampler_dlss_bias): the scene's material textures, the filtered mipmapped ones that repeat, are
// sampled for the output's resolution; clamped ones (post effects, LUTs, the UI) keep the game's bias.
static float dlss_bias;

static int dlss_biased(const samp_par *k) {
    int rep_s = k->ws == NVN_WRAP_MODE_REPEAT || k->ws == NVN_WRAP_MODE_MIRRORED_REPEAT;
    int rep_t = k->wt == NVN_WRAP_MODE_REPEAT || k->wt == NVN_WRAP_MODE_MIRRORED_REPEAT;
    return !k->cmp_mode && k->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR && (rep_s || rep_t);
}

void vk_sampler_dlss_bias(float b) {
    lock();
    if (b != dlss_bias) {
        dlss_bias = b;
        for (int i = 0; i < nsslots; i++) sslots[i].s = sslots[i].sp = VK_NULL_HANDLE;   // looked up again (the key moves)
        samp_gen++;
    }
    unlock();
}

// Caller holds the lock.
static VkSampler sampler_for(VkCtx *c, const samp_par *s) {
    samp_par k;
    memset(&k, 0, sizeof k);
    k = *s;
    if (dlss_bias != 0 && dlss_biased(&k)) k.lod_bias += dlss_bias;   // part of the key: samplers live until shutdown
    VkSampler v = (VkSampler)nvn_hm_get(&samplers, &k);
    if (!v) {
        v = make_sampler(c, &k);
        nvn_hm_put(&samplers, &k, (void *)v);
    }
    return v;
}

VkSampler vk_sampler_for(const samp_par *s) {
    VkCtx *c = vk_ctx();
    if (!c || !s) return VK_NULL_HANDLE;
    lock();
    VkSampler v = sampler_for(c, s);
    unlock();
    return v;
}

// Samplers live until shutdown, so a slot keeps the one its parameters map to until it is registered again.
VkSampler vk_sampler_by_id_locked(int id) {
    VkCtx *c = vk_ctx();
    if (!c) return VK_NULL_HANDLE;
    VkSampler v = VK_NULL_HANDLE;
    if (id >= 0 && id < nsslots && sslots[id].used) {
        SampSlot *s = &sslots[id];
        if (!s->s) s->s = sampler_for(c, &s->p);
        v = s->s;
    }
    return v;
}

VkSampler vk_sampler_point_by_id_locked(int id) {
    VkCtx *c = vk_ctx();
    if (!c || id < 0 || id >= nsslots || !sslots[id].used) return VK_NULL_HANDLE;
    SampSlot *s = &sslots[id];
    if (!s->sp) {
        samp_par k = s->p;
        k.magf = NVN_MAG_FILTER_NEAREST;
        k.minf = k.minf == NVN_MIN_FILTER_LINEAR ? NVN_MIN_FILTER_NEAREST :
                 k.minf > NVN_MIN_FILTER_LINEAR ? NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST : k.minf;
        k.aniso = 1;
        s->sp = sampler_for(c, &k);
    }
    return s->sp;
}

VkSampler vk_sampler_by_id(int id) {
    lock();
    VkSampler v = vk_sampler_by_id_locked(id);
    unlock();
    return v;
}

int vk_sampler_params(int id, samp_par *out) {
    lock();
    int ok = id >= 0 && id < nsslots && sslots[id].used;
    if (ok) *out = sslots[id].p;
    unlock();
    return ok;
}

// ---------------------------------------------------------------- statistics and shutdown
void vk_texture_stats(uint64_t *images, uint64_t *uploads, uint64_t *reuploads) {
    lock();
    if (images) *images = by_key.n;
    if (uploads) *uploads = n_uploads;
    if (reuploads) *reuploads = n_reuploads;
    unlock();
}

void vk_texture_census(VkTexCensus *s) {
    VkCtx *c = vk_ctx();
    memset(s, 0, sizeof *s);
    lock();
    for (uint32_t i = 0; i < by_key.cap; i++) {
        const Tex *e = by_key.vals[i];
        if (!e) continue;
        uint64_t bytes = 0;
        if (e->alloc && c) {
            VmaAllocationInfo ai;
            vmaGetAllocationInfo(c->vma, e->alloc, &ai);
            bytes = ai.size;
        }
        if (e->released) s->released++, s->own_released += e->alloc != NULL, s->bytes_released += bytes;
        else s->live++, s->own_live += e->alloc != NULL, s->bytes_live += bytes;
    }
    unlock();
}

// TOTK_TRACE=perf, with mem: vram: what the released entries in own memory hold and how long they waited.
void vk_texture_released_log(uint64_t frame) {
    VkCtx *c = vk_ctx();
    if (!c) return;
    enum { CL_CLEAN, CL_BACKED, CL_DIRTY, CL_POOLS, CL_N };
    uint64_t cl_n[CL_N] = {0}, cl_b[CL_N] = {0}, age_b[AGE_N] = {0}, dead_n = 0, dead_b = 0, dead_dirty_b = 0;
    uint64_t bound_n = 0, bound_b = 0, ded_n = 0, ded_b = 0;
    typedef struct { uint32_t w, h, fmt, n; uint64_t b; } Group;
    Group gr[256];
    int ng = 0;
    lock();
    AddrIndex x = {0};
    int indexed = addr_index(&x);
    for (int i = 0; indexed && i < x.n; i++) {
        Tex *e = x.e[i];
        if (!e->released || !e->alloc) continue;
        VmaAllocationInfo2 ai2;
        vmaGetAllocationInfo2(c->vma, e->alloc, &ai2);
        const VmaAllocationInfo ai = ai2.allocationInfo;
        if (ai2.dedicatedMemory) ded_n++, ded_b += ai.size;
        int cl = !e->pub.mirrored ? CL_POOLS : e->gpu_dirty ? CL_DIRTY : e->pub.gpu_owned ? CL_BACKED : CL_CLEAN;
        cl_n[cl]++, cl_b[cl] += ai.size;
        if (cl != CL_CLEAN) age_b[age_bucket(coarse_ms - e->rel_ms)] += ai.size;
        if (e->checked && submit_epoch - e->checked < 600) bound_n++, bound_b += ai.size;
        if (storage_taken(&x, i)) dead_n++, dead_b += ai.size, dead_dirty_b += cl != CL_CLEAN ? ai.size : 0;
        int g = 0;
        while (g < ng && (gr[g].w != e->t.d.width || gr[g].h != e->t.d.height || gr[g].fmt != (uint32_t)e->t.d.format)) g++;
        if (g == ng && ng < 256) gr[ng++] = (Group){e->t.d.width, e->t.d.height, (uint32_t)e->t.d.format, 0, 0};
        if (g < ng) gr[g].n++, gr[g].b += ai.size;
    }
    if (indexed) addr_index_free(&x);
    const double M = 1048576.0;
    rt_log("mem: released frame %llu: own memory: clean %llu %.0f MiB, GPU-written with storage current %llu %.0f MiB, "
           "GPU-written image only %llu %.0f MiB, not mirrored %llu %.0f MiB; all but clean by age <1s %.0f <5s %.0f <30s %.0f "
           "<120s %.0f older %.0f MiB; storage taken by a later texture %llu %.0f MiB (%.0f not clean); bound through a slot "
           "in the last 600 submits %llu %.0f MiB; in dedicated memory, returned to the driver when dropped, %llu %.0f MiB; "
           "longest search %.2f ms",
           (unsigned long long)frame, (unsigned long long)cl_n[CL_CLEAN], cl_b[CL_CLEAN] / M, (unsigned long long)cl_n[CL_BACKED],
           cl_b[CL_BACKED] / M, (unsigned long long)cl_n[CL_DIRTY], cl_b[CL_DIRTY] / M, (unsigned long long)cl_n[CL_POOLS],
           cl_b[CL_POOLS] / M, age_b[AGE_1S] / M, age_b[AGE_5S] / M, age_b[AGE_30S] / M, age_b[AGE_120S] / M,
           age_b[AGE_LONG] / M, (unsigned long long)dead_n, dead_b / M, dead_dirty_b / M, (unsigned long long)bound_n,
           bound_b / M, (unsigned long long)ded_n, ded_b / M, pressure_search_ns / 1e6);
    pressure_search_ns = 0;
    for (int k = 0; k < 2; k++) {
        char line[256];
        size_t o = 0;
        for (int a = 0; a < AGE_N; a++) {
            o += (size_t)snprintf(line + o, sizeof line - o, " %s %llu", age_name[a], (unsigned long long)takeback_n[k][a]);
            takeback_n[k][a] = 0;
        }
        rt_log("mem: released frame %llu: taken back after,%s:%s", (unsigned long long)frame, k ? " GPU-written" : " others", line);
    }
    rt_log("mem: released frame %llu: dropped under pressure and created again: within 10 s %llu (%llu GPU-written), later %llu (%llu)",
           (unsigned long long)frame, (unsigned long long)n_recreated[0], (unsigned long long)n_recreated_written[0],
           (unsigned long long)n_recreated[1], (unsigned long long)n_recreated_written[1]);
    n_recreated[0] = n_recreated[1] = n_recreated_written[0] = n_recreated_written[1] = 0;
    for (int top = 0; top < 4; top++) {
        int best = -1;
        for (int g = 0; g < ng; g++)
            if (gr[g].n && (best < 0 || gr[g].b > gr[best].b)) best = g;
        if (best < 0) break;
        rt_log("mem: released frame %llu: largest %d: %ux%u fmt %#x: %u entries %.0f MiB", (unsigned long long)frame, top + 1,
               gr[best].w, gr[best].h, gr[best].fmt, gr[best].n, gr[best].b / M);
        gr[best].n = 0;
    }
    unlock();
}

void vk_texture_shutdown(void) {
    VkCtx *c = vk_ctx();
    lock();
    Tex **list = malloc(sizeof *list * (size_t)(by_key.n + 1));
    int n = 0;
    for (uint32_t i = 0; i < by_key.cap; i++)
        if (by_key.vals[i]) list[n++] = by_key.vals[i];
    for (int i = 0; i < n; i++) destroy_tex(list[i]);
    free(list);
    for (uint32_t i = 0; i < samplers.cap; i++)
        if (samplers.vals[i] && c) vkDestroySampler(c->device, (VkSampler)samplers.vals[i], NULL);
    free(by_key.keys), free(by_key.vals), free(by_addr.keys), free(by_addr.vals), free(samplers.keys), free(samplers.vals);
    by_key = (nvn_hmap){.klen = sizeof(TexKey)};
    by_addr = (nvn_hmap){.klen = 8};
    samplers = (nvn_hmap){.klen = sizeof(samp_par)};
    free(tslots), free(sslots);
    tslots = NULL, sslots = NULL, ntslots = nsslots = 0, nreleased = 0;
    rel_head = rel_tail = NULL;
    if (sinfo) memset(sinfo, 0, sizeof *sinfo << 20), memset(sepoch, 0, sizeof *sepoch << 20);   // no slot is registered: tokens 0
    unlock();
}
