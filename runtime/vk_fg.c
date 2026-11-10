// Frame generation with AMD FSR 3.1 (TOTK_FRAME_GEN, totk.ini frame_gen): between every two frames the game draws,
// one more is made from them and shown halfway in time, so the picture moves twice as often as the game draws. It is
// AMD's frame interpolation of the FidelityFX SDK 1.1.4, from the library vk_fsr3.c loads (vk_ffx.h), and runs on any
// GPU. A generated frame is a guess: what the two frames around it do not show (a place something uncovered) is filled
// in, and the less the game draws per second, the further apart the two frames are.
//
// The library offers a replacement swapchain that makes, paces and presents the generated frames on threads of its own.
// It is not used: the port already presents on a thread of its own that holds each frame until its display time
// (vk_backend.c present_main), and the library's frame generation context also works alone
// (FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY; ffx-api/src/ffx_provider_framegeneration.cpp Dispatch needs
// nothing of the swapchain). So this file only makes the picture between the last frame and this one, and
// gfx_present.c shows it before this frame's.
//
// Three steps a frame:
// 1. vk_fg_scene, in the replay, where the game is about to draw its HUD (vk_dlaa.c finds the place): the game's float
//    target is copied, and the library is given that copy with the scene's depth and motion vectors, the ones the
//    upscaler takes. It keeps a copy as the next call's "last frame", estimates by optical flow the motion the
//    vectors do not cover (shadows, particles, what the game animates in a shader), moves both pictures halfway and
//    blends them. The HUD is in neither picture: hearts and the map would otherwise be dragged along with whatever
//    moves behind them, which is the best known flaw of generated frames.
// 2. vk_fg_take, when the present is queued: the frame's two pictures go along with it.
// 3. vk_fg_compose, on the present thread: this frame's HUD goes onto the generated picture (fg_compose.comp).
// The game's float target is linear light with the HUD's white at 1; its last draw stores it sRGB-encoded in the window
// texture, whose bytes the present shows. For the library the pictures are therefore scRGB with 80 nits per unit,
// the transfer function that leaves linear values as they are (ffx_frameinterpolation_common.h ffxscRGBToLinear), and
// vk_fg_compose encodes the generated picture as that last draw would. (The first build of this passed the float
// values to the screen unencoded: every generated frame came out dark and the picture flickered.)
//
// A frame is not generated, and the game's own frame is shown alone, when the scene was not drawn or has no camera
// (menus, loading), when the picture does not continue the last one (the first frame, another size, a frame without
// motion before it), with HDR output (TOTK_HDR), and for the rest of the run once the library refused a call.
//
// The library's context is made for one picture size and made again when that or the depth convention changes or the
// scene grows. Its calls all come from the replay, as the upscaler's do.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "fg_compose_spv.h"
#include "gfx.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_exec.h"
#include "vk_ffx.h"
#include "vk_fg.h"
// AMD's header declares an enumeration without values (FfxApiConfigureFrameGenerationKey), which C++ compilers let
// pass and C does not. The header stays as AMD ships it: for its length the name stands for an enumeration with one
// value followed by the tag of an empty structure, which takes the header's empty braces.
#define FfxApiConfigureFrameGenerationKey FfxApiConfigureFrameGenerationKey { FFX_API_CONFIGURE_FG_KEY_NONE }; struct ffx_api_fg_no_keys
#include "third_party/ffx_api/ffx_framegeneration.h"
#undef FfxApiConfigureFrameGenerationKey

#define SLOTS 8

struct Pic {
    VkImage img;
    VmaAllocation mem;
    VkImageView view;
};

static struct {
    // ---- the library
    ffxContext ctx;
    uint32_t ow, oh, mw, mh;   // the picture's size; the largest scene
    int inverted, infinite;
    uint64_t id;               // the library's frame number: one more per call
    uint64_t made, frames, shown;
    volatile int32_t failed;
    volatile int32_t live;     // above 0: one of the last 8 frames queued for the present had a generated picture
    // ---- the replay's pictures
    struct Pic before, gen;    // the picture before the HUD, the generated one: RGBA16F, w x h
    uint32_t w, h;
    int fed;                   // this frame's picture went to the library
    int chain;                 // so did the frame's before it: the generated picture is the one between the two
    PlatMutex lock;            // cur: the replay writes it, the thread that queues the present takes it
    VkFgFrame cur;
    int cur_ok;
    // ---- the present's pass
    int tried, ok;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkSampler nearest;
    struct Pic out;            // RGBA8, ow x oh
    uint32_t out_w, out_h;
    VkImageView pic_view[SLOTS], win_view[SLOTS];   // the views each slot's last frame read
} G = {.lock = PLAT_MUTEX_INIT};

int vk_fg_wanted(void) {
    if (!cfg_bool("TOTK_FRAME_GEN") || __atomic_load_n(&G.failed, __ATOMIC_RELAXED)) return 0;
    if (vk_hdr_wanted()) {
        static int said;
        if (!said) said = 1, rt_log("fg: frame generation is not built for HDR output (hdr = 1): off");
        return 0;
    }
    return vk_ffx() != NULL;
}

// TOTK_FG_DEBUG (test, docs/settings.md), read at each frame: 1 and 2 are fg_compose.comp's views, 3 the library's.
static int debug_view(void) {
    const char *d = cfg_get("TOTK_FG_DEBUG");
    return d ? atoi(d) : 0;
}

static void fail(const char *what, unsigned code) {
    if (!plat_xchg32(&G.failed, 1)) rt_log("fg: %s (%u): frame generation off for this run", what, code);
}

// ---------------------------------------------------------------- images
static int pic_make(struct Pic *p, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage) {
    const VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    *p = (struct Pic){0};
    if (vmaCreateImage(c->vma, &ii, &ai, &p->img, &p->mem, NULL) != VK_SUCCESS) return p->img = VK_NULL_HANDLE, 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = p->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(c->device, &vi, NULL, &p->view) == VK_SUCCESS;
}

static void pic_free(struct Pic *p) {
    const VkCtx *c = vk_ctx();
    if (p->view) vkDestroyImageView(c->device, p->view, NULL);
    if (p->img) vmaDestroyImage(c->vma, p->img, p->mem);
    *p = (struct Pic){0};
}

static void pics_free(void *arg) {
    struct Pic *p = arg;
    pic_free(&p[0]), pic_free(&p[1]);
    free(p);
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

static void memory_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// The two pictures of the replay at w x h, in GENERAL. The pair they replace was read by the presents of the frames
// before this one, so it goes when the GPU has finished this submission, which comes after them. 0: no memory.
static int pics_for(VkExec *ex, uint32_t w, uint32_t h) {
    if (G.before.img && G.w == w && G.h == h) return 1;
    const VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    struct Pic n[2], *old = malloc(2 * sizeof *old);
    int ok = pic_make(&n[0], w, h, VK_FORMAT_R16G16B16A16_SFLOAT, u);
    ok = ok && pic_make(&n[1], w, h, VK_FORMAT_R16G16B16A16_SFLOAT, u);
    if (!ok || !old) {
        pic_free(&n[0]);
        if (ok || n[1].img) pic_free(&n[1]);
        free(old);
        return 0;
    }
    old[0] = G.before, old[1] = G.gen;
    G.before = n[0], G.gen = n[1], G.w = w, G.h = h;
    if (old[0].img) vk_completion_add(ex->fence, pics_free, old);
    else free(old);
    image_barrier(ex->cb, G.before.img, VK_IMAGE_LAYOUT_UNDEFINED);
    image_barrier(ex->cb, G.gen.img, VK_IMAGE_LAYOUT_UNDEFINED);
    ex->recorded += 2;
    G.chain = G.fed = 0;   // the library's last picture is another size's
    return 1;
}

// ---------------------------------------------------------------- the library
static void ctx_destroy(void) {
    if (!G.ctx) return;
    vkDeviceWaitIdle(vk_ctx()->device);   // its images may still be read by the frames before this one
    vk_ffx()->destroy(&G.ctx, NULL);
    G.ctx = NULL;
}

static int ctx_for(const VkFgScene *s, uint32_t ow, uint32_t oh, int infinite) {
    if (G.ctx && ow == G.ow && oh == G.oh && s->w <= G.mw && s->h <= G.mh && s->inverted == G.inverted &&
        infinite == G.infinite)
        return 1;
    int same = G.ctx && ow == G.ow && oh == G.oh;
    ctx_destroy();
    const VkFfx *fx = vk_ffx();
    uint32_t mw = same && G.mw > s->w ? G.mw : s->w, mh = same && G.mh > s->h ? G.mh : s->h;
    struct ffxCreateBackendVKDesc backend;
    vk_ffx_backend(&backend);
    struct ffxCreateContextDescFrameGeneration cd = {
        .header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, .pNext = &backend.header},
        .flags = (s->inverted ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED : 0) |
                 (infinite ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE : 0),
        .displaySize = {ow, oh}, .maxRenderSize = {mw, mh},
        .backBufferFormat = FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT};
    ffxReturnCode_t r = fx->create(&G.ctx, &cd.header, NULL);
    if (r != FFX_API_RETURN_OK || !G.ctx) {
        G.ctx = NULL;
        return fail("the library made no frame generation context", (unsigned)r), 0;
    }
    struct ffxConfigureDescGlobalDebug1 dbg = {.header = {.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1},
                                               .fpMessage = vk_ffx_message,
                                               .debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS};
    fx->configure(&G.ctx, &dbg.header);
    G.ow = ow, G.oh = oh, G.mw = mw, G.mh = mh, G.inverted = s->inverted, G.infinite = infinite;
    G.chain = 0;   // a new context has no last picture
    rt_log("fg: context %llu for %ux%u pictures and scenes up to %ux%u%s%s", (unsigned long long)++G.made, ow, oh, mw, mh,
           s->inverted ? ", inverted depth" : "", infinite ? ", no far plane" : "");
    return 1;
}

// The picture between the library's last one and G.before, into G.gen. 0: the library refused.
static int generate(VkCommandBuffer cb, const VkFgScene *s, uint32_t ow, uint32_t oh, int reset, float dt_ms) {
    const VkFfx *fx = vk_ffx();
    if (!ctx_for(s, ow, oh, s->far_m >= 1e8f)) return 0;
    uint64_t id = ++G.id;
    int view = debug_view() == 3;   // the library's debug view
    uint32_t flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY |
                     (view ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW : 0);
    // nothing of a swapchain, and no picture with the HUD: the HUD goes back on in vk_fg_compose
    struct ffxConfigureDescFrameGeneration cf = {.header = {.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION},
                                                 .frameGenerationEnabled = true, .flags = flags,
                                                 .generationRect = {0, 0, (int32_t)ow, (int32_t)oh}, .frameID = id};
    ffxReturnCode_t r = fx->configure(&G.ctx, &cf.header);
    if (r != FFX_API_RETURN_OK) return fail("the library refused the frame's settings", (unsigned)r), 0;
    struct ffxDispatchDescFrameGenerationPrepare p;
    memset(&p, 0, sizeof p);
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    p.frameID = id;
    p.flags = flags;
    p.commandList = (void *)cb;
    p.renderSize.width = s->w, p.renderSize.height = s->h;
    // as vk_fsr3.c gives them to the upscaler: the picture moved by the jitter, x right and y down; the vectors are
    // last frame's window position minus this frame's in scene pixels, at a scale of one
    p.jitterOffset.x = s->jx, p.jitterOffset.y = s->jy;
    p.motionVectorScale.x = p.motionVectorScale.y = 1.0f;
    p.frameTimeDelta = dt_ms;
    p.cameraNear = s->near_m, p.cameraFar = s->far_m, p.cameraFovAngleVertical = s->fov_y;
    p.viewSpaceToMetersFactor = 1.0f;
    p.depth = vk_ffx_resource(s->depth, VK_FORMAT_D32_SFLOAT, s->w, s->h, 0);
    p.motionVectors = vk_ffx_resource(s->mv, VK_FORMAT_R16G16_SFLOAT, s->w, s->h, 0);
    r = fx->dispatch(&G.ctx, &p.header);
    if (r != FFX_API_RETURN_OK) return fail("the library refused the scene's depth and motion", (unsigned)r), 0;
    struct ffxDispatchDescFrameGeneration d;
    memset(&d, 0, sizeof d);
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    d.commandList = (void *)cb;
    d.presentColor = vk_ffx_resource(G.before.img, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, 0);
    d.outputs[0] = vk_ffx_resource(G.gen.img, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, 1);
    d.numGeneratedFrames = 1;
    d.reset = reset != 0;
    d.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SCRGB;   // linear light
    d.minMaxLuminance[0] = 0.0f, d.minMaxLuminance[1] = 80.0f;                   // 1.0 is the picture's white
    d.generationRect = cf.generationRect;
    d.frameID = id;
    r = fx->dispatch(&G.ctx, &d.header);
    if (r != FFX_API_RETURN_OK) return fail("the library refused the frame", (unsigned)r), 0;
    if (!G.frames++)
        rt_log("fg: generating %ux%u pictures from %ux%u scenes (near %g m, far %g m, vertical field of view %.1f "
               "degrees)", ow, oh, s->w, s->h, s->near_m, s->far_m, s->fov_y * 57.29578f);
    return 1;
}

// ---------------------------------------------------------------- the replay
void vk_fg_scene(VkExec *ex, const VkFgScene *s) {
    const VkExecTarget *t = s->target;
    uint32_t w = t->width, h = t->height;
    VkCommandBuffer cb = ex->cb;
    if (!pics_for(ex, w, h)) return fail("no memory for the pictures", 0);
    // The picture so far, exactly: RGBA16F holds every value of the game's R11G11B10F target.
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, t->level, t->base_layer, 1},
                     .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .dstOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}};
    vkCmdBlitImage(cb, t->image, VK_IMAGE_LAYOUT_GENERAL, G.before.img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    memory_barrier(cb);
    ex->recorded += 2;
    int made = 0, reset = s->reset || !G.chain;
    if (s->have_mv) {
        static uint64_t last_ns;
        uint64_t now = plat_now_ns();
        double dt = last_ns ? (double)(now - last_ns) / 1e6 : 0.0;
        last_ns = now;
        rec_drain();   // the library records into the command buffer itself: the recorder must have made every call so far
        made = generate(cb, s, w, h, reset, dt > 0.0 && dt < 100.0 ? (float)dt : 16.67f);
        // generate() may have made the context again, which leaves it without a last picture
        reset = reset || !G.chain;
        memory_barrier(cb);
        ex->recorded++;
    }
    G.fed = made;
    plat_mutex_lock(&G.lock);
    G.cur_ok = made && !reset;
    G.cur = (VkFgFrame){.target = t->image, .before_hud = G.before.view, .generated = G.gen.view, .w = w, .h = h};
    plat_mutex_unlock(&G.lock);
}

int vk_fg_take(VkFgFrame *f) {
    plat_mutex_lock(&G.lock);
    int ok = G.cur_ok;
    *f = G.cur;
    G.cur_ok = 0;
    G.chain = G.fed, G.fed = 0;   // a frame that gave the library nothing breaks the chain
    plat_mutex_unlock(&G.lock);
    // a single frame without one (a camera cut) does not move the frame limit (frame_rate.c frame_step_cap_ns)
    int32_t live = __atomic_load_n(&G.live, __ATOMIC_RELAXED);
    plat_xchg32(&G.live, ok ? 8 : live > 0 ? live - 1 : 0);
    return ok;
}

int gfx_frame_gen_live(void) { return __atomic_load_n(&G.live, __ATOMIC_RELAXED) > 0; }

// frame_gen = 1: the frame limit counts the frames shown, so the game draws half of it (frame_rate.c); x2: the limit
// stays the game's and twice as many frames are shown.
int gfx_frame_gen_halves(void) { return gfx_frame_gen_live() && !strcmp(cfg_value("TOTK_FRAME_GEN"), "1"); }

// ---------------------------------------------------------------- the present
static int init(const VkCtx *c) {
    G.tried = 1;
    G.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[5];
    for (uint32_t i = 0; i < 5; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                              .descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                      : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 5, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof fg_compose_spv,
                                   .pCode = fg_compose_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    int ok = G.push && vkCreateDescriptorSetLayout(c->device, &dci, NULL, &G.dsl) == VK_SUCCESS;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &G.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    ok = ok && vkCreatePipelineLayout(c->device, &li, NULL, &G.layout) == VK_SUCCESS &&
         vkCreateSampler(c->device, &si, NULL, &G.nearest) == VK_SUCCESS &&
         vkCreateShaderModule(c->device, &mi, NULL, &m) == VK_SUCCESS;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = G.layout};
    ok = ok && vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &G.pipe) == VK_SUCCESS;
    if (m) vkDestroyShaderModule(c->device, m, NULL);
    if (!ok) return fail("the pass that puts the HUD on generated frames could not be made", 0), 0;
    return G.ok = 1;
}

static int out_for(const VkCtx *c, uint32_t w, uint32_t h) {
    if (G.out.img && G.out_w == w && G.out_h == h) return 1;
    // another picture size: earlier presents may still read the old image (as vk_fsr.c ensure_images)
    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    vk_queue_unlock();
    pic_free(&G.out);
    if (!pic_make(&G.out, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        pic_free(&G.out);
        return 0;
    }
    G.out_w = w, G.out_h = h;
    return 1;
}

// The window texture's bytes as they are: its sRGB formats are viewed as their UNORM twins (the back end's colour
// images take views of another format, vk_texture.c).
static VkFormat bytes_format(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : f == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : f;
}

static VkImageView view_of(const VkCtx *c, VkImageView *slot, VkImage img, VkFormat fmt) {
    if (*slot) vkDestroyImageView(c->device, *slot, NULL);   // the present waited for this slot's fence
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, slot) != VK_SUCCESS) *slot = VK_NULL_HANDLE;
    return *slot;
}

VkImage vk_fg_compose(VkCommandBuffer cb, unsigned slot, const VkFgFrame *f, VkImage pic, VkFormat pic_fmt, VkImage win,
                      VkFormat win_fmt) {
    const VkCtx *c = vk_ctx();
    if (__atomic_load_n(&G.failed, __ATOMIC_RELAXED) || (!G.tried && !init(c)) || !G.ok) return VK_NULL_HANDLE;
    if (pic != f->target || !out_for(c, f->w, f->h)) return VK_NULL_HANDLE;
    slot %= SLOTS;
    VkImageView pv = view_of(c, &G.pic_view[slot], pic, pic_fmt), wv = view_of(c, &G.win_view[slot], win, bytes_format(win_fmt));
    if (!pv || !wv) return VK_NULL_HANDLE;
    // what the replay wrote becomes visible; the last present's read of the output is over
    memory_barrier(cb);
    image_barrier(cb, G.out.img, VK_IMAGE_LAYOUT_UNDEFINED);
    int debug = debug_view();   // fg_compose.comp's views
    VkDescriptorImageInfo ii[5] = {{G.nearest, pv, VK_IMAGE_LAYOUT_GENERAL},
                                   {G.nearest, f->before_hud, VK_IMAGE_LAYOUT_GENERAL},
                                   {G.nearest, f->generated, VK_IMAGE_LAYOUT_GENERAL},
                                   {G.nearest, wv, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, G.out.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet ws[5];
    for (uint32_t i = 0; i < 5; i++)
        ws[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = i,
                                       .descriptorCount = 1, .pImageInfo = &ii[i],
                                       .descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                               : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    int32_t pc[3] = {(int32_t)f->w, (int32_t)f->h, debug == 1 || debug == 2 ? debug : 0};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.pipe);
    rec_push(G.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, G.layout, 0, 5, ws);
    vkCmdPushConstants(cb, G.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (f->w + 7) / 8, (f->h + 7) / 8, 1);
    memory_barrier(cb);
    if (!G.shown++)
        rt_log("fg: generated frames are shown (%ux%u, the HUD from the game's frame; float target format %d, window "
               "texture format %d)", f->w, f->h, (int)pic_fmt, (int)win_fmt);
    return G.out.img;
}

void vk_fg_release(void) {
    const VkCtx *c = vk_ctx();
    if (!c) return;
    ctx_destroy();
    pic_free(&G.before), pic_free(&G.gen), pic_free(&G.out);
    for (int i = 0; i < SLOTS; i++) {
        if (G.pic_view[i]) vkDestroyImageView(c->device, G.pic_view[i], NULL);
        if (G.win_view[i]) vkDestroyImageView(c->device, G.win_view[i], NULL);
        G.pic_view[i] = G.win_view[i] = VK_NULL_HANDLE;
    }
    if (G.pipe) vkDestroyPipeline(c->device, G.pipe, NULL);
    if (G.nearest) vkDestroySampler(c->device, G.nearest, NULL);
    if (G.layout) vkDestroyPipelineLayout(c->device, G.layout, NULL);
    if (G.dsl) vkDestroyDescriptorSetLayout(c->device, G.dsl, NULL);
    G.pipe = VK_NULL_HANDLE, G.nearest = VK_NULL_HANDLE, G.layout = VK_NULL_HANDLE, G.dsl = VK_NULL_HANDLE;
    G.tried = G.ok = 0, G.w = G.h = G.out_w = G.out_h = 0, G.chain = G.fed = G.cur_ok = 0;
}
