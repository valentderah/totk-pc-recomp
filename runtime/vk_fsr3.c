// AMD FSR 3.1 temporal upscaler for vk_dlaa.c's DLAA / DLSS point (vk_fsr3.h), through the C bridge of
// UniversalScalers-Vulkan (FireBurn/UniversalScalers-Vulkan, formerly FSR-Vulkan; MIT) to the FSR 3.1.5 upscaler of
// AMD's FidelityFX SDK 2.3 (MIT). The libraries it needs are built from that repository into runtime/deps
// (tools/make_release.py build_ffx, tools/ffx/README.md); their shaders are SPIR-V compiled into them.
//
// The bridge records plain Vulkan commands into the command buffer it is given and binds pipelines and descriptor sets
// of its own: vk_dlaa.c drains the recorder before the call and makes the next draw bind everything again, as it does
// for NGX. The bridge reads a fixed list of formats, which has neither the scene's R11G11B10F light nor D32F depth, so
// each frame copies them into an RGBA16F and an R32F image of this context first (the depth through a buffer: Vulkan
// copies a depth aspect into a buffer, not into a colour image). Every image FSR sees is then the context's own or one
// of vk_dlaa.c's work images of this size, imported once; the SDK's per-frame descriptor sets and constants are retired
// once the submission that used them is done on the GPU (its timeline value).
//
// Inputs, as vk_dlaa.c makes them for DLSS: the scene's light before the bloom (linear, so HDR input), its depth
// (OpenGL depth range, not inverted, finite far plane, which is what the bridge assumes), motion vectors in render
// pixels from this frame's position to last frame's (FSR's convention, scale 1), and the jitter the scene was drawn
// with. FSR also wants the camera's near and far planes and vertical field of view to judge depth differences;
// vk_dlaa.c only has the combined view-projection on the GPU, so fixed values stand in for them.
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_fsr3.h"

// A build without the libraries in its deps folder (an MSYS2 source build that never ran tools/ffx/README.md) has no
// bridge header: FSR 3.1 is then left out and aa_engine offers DLSS alone.
#if !__has_include("ffx_vk_fsr3_3_1_5_bridge.h")
int fsr3_available(void) { return 0; }
Fsr3 *fsr3_create(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, VkImage mv, VkImage out) {
    (void)w, (void)h, (void)ow, (void)oh, (void)mv, (void)out;
    return NULL;
}
void fsr3_destroy(Fsr3 *f) { (void)f; }
int fsr3_dispatch(Fsr3 *f, VkCommandBuffer cb, const Fsr3Frame *in) { (void)f, (void)cb, (void)in; return 0; }
#else
#include "ffx_vk_fsr3_3_1_5_bridge.h"

int fsr3_available(void) { return 1; }

enum { IN_COLOR, IN_DEPTH, IN_MV, OUT_IMG, DIL_DEPTH, DIL_MV, PREV_DEPTH, NRES };

struct Fsr3 {
    FfxVkFsr3_3_1_5UpscalerContext *ctx;
    FfxVkFsr3_3_1_5Bridge *bridge;
    uint32_t w, h, ow, oh;
    VkImage img[NRES];              // IN_COLOR, IN_DEPTH and the three shared images are this context's
    VmaAllocation mem[NRES];
    FfxVkFsr3_3_1_5Resource res[NRES];
    VkBuffer zbuf;                  // the depth on its way from the D32F target to IN_DEPTH
    VmaAllocation zbuf_mem;
    int general;                    // own images moved from UNDEFINED to GENERAL (first dispatch)
};

static int own_image(Fsr3 *f, int i, VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags u) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(vk_ctx()->vma, &ii, &ai, &f->img[i], &f->mem[i], NULL) == VK_SUCCESS) return 1;
    rt_log("fsr3: image %d (%ux%u, format %d) could not be created", i, w, h, (int)fmt);
    return 0;
}

static int import(Fsr3 *f, int i, VkFormat fmt, uint32_t w, uint32_t h, uint32_t state, VkImageUsageFlags u) {
    FfxVkFsr3_3_1_5ImportedImageDescription d = {.image = f->img[i], .format = fmt, .width = w, .height = h,
                                                 .mipCount = 1, .arrayLayers = 1, .layout = VK_IMAGE_LAYOUT_GENERAL,
                                                 .state = state, .usage = u};
    f->res[i] = ffxVkFsr3_3_1_5BridgeImportImage(f->bridge, &d);
    if (f->res[i].resource) return 1;
    rt_log("fsr3: image %d (format %d) could not be imported", i, (int)fmt);
    return 0;
}

Fsr3 *fsr3_create(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, VkImage mv, VkImage out) {
    const VkCtx *c = vk_ctx();
    Fsr3 *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->w = w, f->h = h, f->ow = ow, f->oh = oh;
    static int hdr = -1;
    if (hdr < 0) hdr = rt_knob("FSR3_HDR", "1");   // TOTK_FSR3_HDR=0: tell FSR the colour is display-referred (test)
    FfxVkFsr3_3_1_5UpscalerCreateInfo ci = {.physicalDevice = c->phys, .device = c->device, .maxRenderWidth = w,
                                            .maxRenderHeight = h, .maxUpscaleWidth = ow, .maxUpscaleHeight = oh,
                                            .hdrColorInput = hdr ? VK_TRUE : VK_FALSE, .autoExposure = VK_FALSE};
    FfxVkFsr3_3_1_5Result r = ffxVkFsr3_3_1_5UpscalerContextCreate(&ci, &f->ctx);
    if (r != FFX_VK_FSR3_3_1_5_OK || !f->ctx) {
        rt_log("fsr3: context creation failed (%d) at %ux%u -> %ux%u", (int)r, w, h, ow, oh);
        free(f);
        return NULL;
    }
    f->bridge = ffxVkFsr3_3_1_5UpscalerContextGetBridge(f->ctx);
    FfxVkFsr3_3_1_5SharedResourceDescriptions sd;
    if (ffxVkFsr3_3_1_5UpscalerContextGetSharedResourceDescriptions(f->ctx, &sd) != FFX_VK_FSR3_3_1_5_OK) {
        rt_log("fsr3: no shared resource descriptions");
        fsr3_destroy(f);
        return NULL;
    }
    const uint32_t rd = FFX_VK_FSR3_3_1_5_RESOURCE_STATE_COMPUTE_READ, ua = FFX_VK_FSR3_3_1_5_RESOURCE_STATE_UNORDERED_ACCESS;
    const VkImageUsageFlags samp = VK_IMAGE_USAGE_SAMPLED_BIT, rw = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    const FfxVkFsr3_3_1_5SharedResourceDescription *s[3] = {&sd.dilatedDepth, &sd.dilatedMotionVectors,
                                                           &sd.reconstructedPrevNearestDepth};
    int ok = own_image(f, IN_COLOR, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, samp | VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
             own_image(f, IN_DEPTH, VK_FORMAT_R32_SFLOAT, w, h, samp | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    // the SDK clears these at a history reset (vkCmdClearColorImage), so they take transfers too
    const VkImageUsageFlags xfer = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    for (int i = 0; ok && i < 3; i++)
        ok = own_image(f, DIL_DEPTH + i, s[i]->format, s[i]->width, s[i]->height, rw | xfer);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)w * h * 4,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    ok = ok && vmaCreateBuffer(c->vma, &bi, &ai, &f->zbuf, &f->zbuf_mem, NULL) == VK_SUCCESS;
    f->img[IN_MV] = mv, f->img[OUT_IMG] = out;   // vk_dlaa.c's, not destroyed here
    ok = ok && import(f, IN_COLOR, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, rd, samp) &&
         import(f, IN_DEPTH, VK_FORMAT_R32_SFLOAT, w, h, rd, samp) &&
         import(f, IN_MV, VK_FORMAT_R16G16_SFLOAT, w, h, rd, rw) &&
         import(f, OUT_IMG, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, ua, rw);
    for (int i = 0; ok && i < 3; i++) ok = import(f, DIL_DEPTH + i, s[i]->format, s[i]->width, s[i]->height, ua, rw);
    if (!ok) {
        fsr3_destroy(f);
        return NULL;
    }
    FfxVkFsr3_3_1_5MemoryUsage mu = {0};
    ffxVkFsr3_3_1_5UpscalerContextGetMemoryUsage(f->ctx, &mu);
    rt_log("fsr3: FSR 3.1.5 context %ux%u -> %ux%u (%s colour), %.1f MiB", w, h, ow, oh, hdr ? "HDR" : "display",
           mu.totalUsageInBytes / 1048576.0);
    return f;
}

void fsr3_destroy(Fsr3 *f) {
    if (!f) return;
    const VkCtx *c = vk_ctx();
    for (int i = 0; i < NRES; i++)
        if (f->res[i].resource) ffxVkFsr3_3_1_5BridgeReleaseImportedImage(f->bridge, f->res[i]);
    if (f->ctx) ffxVkFsr3_3_1_5UpscalerContextDestroy(f->ctx);
    for (int i = 0; i < NRES; i++)
        if (f->img[i] && f->mem[i]) vmaDestroyImage(c->vma, f->img[i], f->mem[i]);
    if (f->zbuf) vmaDestroyBuffer(c->vma, f->zbuf, f->zbuf_mem);
    free(f);
}

static void barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static float knob_float(const char *name, float def) {
    const char *s = getenv(name);   // test-only switches, read like rt_knob's: the environment, no settings row
    double v = s ? strtod(s, NULL) : 0;
    return v > 0 ? (float)v : def;
}

int fsr3_dispatch(Fsr3 *f, VkCommandBuffer cb, const Fsr3Frame *in) {
    ffxVkFsr3_3_1_5UpscalerContextRetireFrame(f->ctx, vk_sync_fence_done());
    if (!f->general) {   // the context's own images start in UNDEFINED; they live in GENERAL like every other image
        static const int own[] = {IN_COLOR, IN_DEPTH, DIL_DEPTH, DIL_MV, PREV_DEPTH};
        VkImageMemoryBarrier2 ib[5];
        for (int k = 0; k < 5; k++)
            ib[k] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                            .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = f->img[own[k]],
                                            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 5,
                               .pImageMemoryBarriers = ib};
        vkCmdPipelineBarrier2(cb, &di);
        f->general = 1;
    }
    // the scene's colour and depth into formats the bridge reads
    VkImageBlit cb_blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .srcOffsets = {{0, 0, 0}, {(int32_t)f->w, (int32_t)f->h, 1}},
                           .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .dstOffsets = {{0, 0, 0}, {(int32_t)f->w, (int32_t)f->h, 1}}};
    vkCmdBlitImage(cb, in->color, VK_IMAGE_LAYOUT_GENERAL, f->img[IN_COLOR], VK_IMAGE_LAYOUT_GENERAL, 1, &cb_blit,
                   VK_FILTER_NEAREST);
    VkBufferImageCopy zc = {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .imageExtent = {f->w, f->h, 1}};
    vkCmdCopyImageToBuffer(cb, in->depth, VK_IMAGE_LAYOUT_GENERAL, f->zbuf, 1, &zc);
    barrier(cb);
    zc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vkCmdCopyBufferToImage(cb, f->zbuf, f->img[IN_DEPTH], VK_IMAGE_LAYOUT_GENERAL, 1, &zc);
    barrier(cb);
    // ponytail: fixed camera planes and field of view (TOTK_FSR3_NEAR / FAR / FOV override them); reading them from the
    // camera block would need the projection on the CPU.
    FfxVkFsr3_3_1_5UpscalerDispatchInfo d = {
        .commandBuffer = cb, .color = f->res[IN_COLOR], .depth = f->res[IN_DEPTH], .motionVectors = f->res[IN_MV],
        .output = f->res[OUT_IMG], .dilatedDepth = f->res[DIL_DEPTH], .dilatedMotionVectors = f->res[DIL_MV],
        .reconstructedPrevNearestDepth = f->res[PREV_DEPTH],
        .jitterOffsetX = in->jx, .jitterOffsetY = in->jy, .motionVectorScaleX = 1.0f, .motionVectorScaleY = 1.0f,
        .renderWidth = f->w, .renderHeight = f->h, .upscaleWidth = f->ow, .upscaleHeight = f->oh,
        .frameTimeMilliseconds = 16.67f, .preExposure = 1.0f, .enableSharpening = VK_FALSE, .sharpness = 0.0f,
        .reset = in->reset ? VK_TRUE : VK_FALSE, .cameraNear = knob_float("TOTK_FSR3_NEAR", 0.1f),
        .cameraFar = knob_float("TOTK_FSR3_FAR", 10000.0f),
        .cameraVerticalFovRadians = knob_float("TOTK_FSR3_FOV", 50.0f) * 3.14159265f / 180.0f,
        .viewSpaceToMeters = 1.0f, .flags = 0, .frameId = in->frame};
    FfxVkFsr3_3_1_5Result res = ffxVkFsr3_3_1_5UpscalerContextRecordDispatch(f->ctx, &d);
    if (res == FFX_VK_FSR3_3_1_5_OK) return 1;
    static int errors;
    if (errors++ < 5) rt_log("fsr3: dispatch failed (%d)", (int)res);
    return 0;
}
#endif   // the bridge header
