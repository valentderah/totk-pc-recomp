// Motion blur (TOTK_MOTION_BLUR, totk.ini motion_blur: the percentage of a frame's motion the blur spans, 0 = off,
// read each frame). It works from what DLSS is given: the scene's motion vectors (vk_dlaa.c mv_pass: the camera's
// motion, and with dlaa_objmv the characters' own over it) and the scene depth. With DLSS it runs right after DLSS's
// evaluation (vk_dlaa.c evaluate) on DLSS's picture, at the output size. Without DLSS (dlaa = 0, or a GPU without it)
// vk_dlaa.c still follows the scene and makes the vectors while the blur is on, and the blur runs on the scene's light
// target at the end of the scene (vk_dlaa.c blur_only). Either way the picture is scene light before the tonemap and
// before the HUD, which the game draws later and which stays sharp.
//
// Two compute passes, recorded like the replay's own commands:
//   1. runtime/shaders/motion_blur_tiles.comp: the longest motion vector of each tile of the motion image.
//   2. runtime/shaders/motion_blur.comp: each pixel of the picture averaged along the longest motion near it, with
//      samples weighted by depth and by their own motion (the filter of McGuire et al. 2012; the shader's header
//      explains it). A pixel with no motion near it is copied.
// The result goes into an image of this file's, which vk_dlaa.c then hands on in place of DLSS's output or copies back
// into the light target. That image only grows: TotK's dynamic resolution changes the picture's size from frame to
// frame in the DLAA mode and without DLSS, and a smaller picture uses the top left of the image.
//
// Sizes. A tile is 8 * ceil(h / 180) scene pixels, 1/22.5 of the scene's height or more. The longest blur is 1/15 of
// the picture's height (72 pixels at 1080p), so a smear reaches at most 1/30 of the height to either side of what
// moves: within one tile even after the blur pass's lookup jitter of a quarter tile, which is what lets a pixel find
// everything that can sweep over it in the 3 x 3 tiles around it. The tile image is TILES_W x TILES_H, enough for any
// picture up to 5.6:1; only its top left ceil(w / tile) x ceil(h / tile) is written and read.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_exec.h"
#include "vk_rec.h"
#include "motion_blur_spv.h"
#include "motion_blur_tiles_spv.h"

#define TILES_W 128u
#define TILES_H 32u
#define TILE_ROWS 180u    // a tile's edge is 8 * ceil(scene height / TILE_ROWS)
#define MAX_SHARE 15.0f   // the longest blur: the picture's height / MAX_SHARE

static struct {
    int ok;                      // -1 not built, 0 failed, 1 ready
    int percent;                 // the setting as last logged, -1: not yet
    VkDescriptorSetLayout tile_dsl, blur_dsl;
    VkPipelineLayout tile_layout, blur_layout;
    VkPipeline tile_pipe, blur_pipe;
    VkSampler nearest, linear;
    VkImage tiles;               // RG16F, TILES_W x TILES_H
    VmaAllocation tiles_mem;
    VkImageView tiles_view;
    int tiles_new;               // created, not yet in GENERAL
    struct Out {                 // the blurred picture (RGBA16F, GENERAL), in the top left of an image at least its size
        uint32_t w, h;
        VkImage img;
        VmaAllocation mem;
        VkImageView view;
    } out;
    uint64_t frames;
} M = {.ok = -1, .percent = -1};

static int image2d(uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage, VkImage *img, VmaAllocation *mem,
                   VkImageView *view) {
    const VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, img, mem, NULL) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(c->device, &vi, NULL, view) == VK_SUCCESS;
}

// A new image into GENERAL, the layout every image of the back end is kept in.
static void to_general(VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                           .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
}

static void mem_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static int compute_pipe(const uint32_t *code, size_t bytes, uint32_t nbind, const VkDescriptorSetLayoutBinding *b,
                        uint32_t push_bytes, VkDescriptorSetLayout *dsl, VkPipelineLayout *layout, VkPipeline *pipe) {
    const VkCtx *c = vk_ctx();
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = nbind, .pBindings = b};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, dsl) != VK_SUCCESS) return 0;
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes,
                                   .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(c->device, &li, NULL, layout) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS)
        return 0;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = *layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    return r == VK_SUCCESS;
}

// Once, at the first frame with the setting on. The passes store into RG16F and RGBA16F images, filter the picture
// (RGBA16F from DLSS, else the game's light target, R11G11B10F or RGBA16F) and their result is blitted into the game's
// target; a device whose formats cannot do that keeps the picture as it is.
static int init(void) {
    if (M.ok >= 0) return M.ok;
    M.ok = 0;
    const VkCtx *c = vk_ctx();
    VkFormatProperties rg, rgba, r11;
    vkGetPhysicalDeviceFormatProperties(c->phys, VK_FORMAT_R16G16_SFLOAT, &rg);
    vkGetPhysicalDeviceFormatProperties(c->phys, VK_FORMAT_R16G16B16A16_SFLOAT, &rgba);
    vkGetPhysicalDeviceFormatProperties(c->phys, VK_FORMAT_B10G11R11_UFLOAT_PACK32, &r11);
    VkFormatFeatureFlags st = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    VkFormatFeatureFlags pic = VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((rg.optimalTilingFeatures & st) != st || (rgba.optimalTilingFeatures & st) != st ||
        !(rgba.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        (rgba.optimalTilingFeatures & pic) != pic || (r11.optimalTilingFeatures & pic) != pic) {
        rt_log("mblur: motion blur off: the device cannot store into, filter or blit the float formats it works in");
        return 0;
    }
    VkDescriptorSetLayoutBinding tb[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutBinding bb[5] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkSamplerCreateInfo sl = si;
    sl.magFilter = sl.minFilter = VK_FILTER_LINEAR;
    if (!vk_pipeline_push_fn() ||
        !compute_pipe(motion_blur_tiles_spv, sizeof motion_blur_tiles_spv, 2, tb, 16, &M.tile_dsl, &M.tile_layout,
                      &M.tile_pipe) ||
        !compute_pipe(motion_blur_spv, sizeof motion_blur_spv, 5, bb, 48, &M.blur_dsl, &M.blur_layout, &M.blur_pipe) ||
        vkCreateSampler(c->device, &si, NULL, &M.nearest) != VK_SUCCESS ||
        vkCreateSampler(c->device, &sl, NULL, &M.linear) != VK_SUCCESS ||
        !image2d(TILES_W, TILES_H, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                 &M.tiles, &M.tiles_mem, &M.tiles_view)) {
        rt_log("mblur: motion blur off: its passes could not be set up");
        return 0;
    }
    M.tiles_new = 1;
    return M.ok = 1;
}

static void out_free(void *arg) {
    struct Out *o = arg;
    const VkCtx *c = vk_ctx();
    if (o->view) vkDestroyImageView(c->device, o->view, NULL);
    if (o->img) vmaDestroyImage(c->vma, o->img, o->mem);
    free(o);
}

// The image the blurred picture is written to: M.out when it is large enough (exact: just that size), else a new one
// that covers both sizes, in GENERAL. The one it replaces was read by the frames before this one, so it goes when the
// GPU has finished this submission, which comes after them. 0: no memory.
static int out_for(VkExec *ex, uint32_t w, uint32_t h, int exact) {
    struct Out *o = &M.out;
    if (o->img && (exact ? o->w == w && o->h == h : o->w >= w && o->h >= h)) return 1;
    struct Out n = {.w = exact || o->w < w ? w : o->w, .h = exact || o->h < h ? h : o->h};
    // TRANSFER_SRC: vk_dlaa.c blits it into the game's target
    int ok = image2d(n.w, n.h, VK_FORMAT_R16G16B16A16_SFLOAT,
                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &n.img,
                     &n.mem, &n.view);
    struct Out *old = malloc(sizeof *old);
    if (!ok || !old) {
        const VkCtx *c = vk_ctx();
        if (n.view) vkDestroyImageView(c->device, n.view, NULL);
        if (n.img) vmaDestroyImage(c->vma, n.img, n.mem);
        free(old);
        return 0;
    }
    *old = *o, *o = n;
    if (old->img) vk_completion_add(ex->fence, out_free, old);
    else free(old);
    to_general(ex->cb, o->img), ex->recorded++;
    return 1;
}

static int percent(void) {
    int p = cfg_int("TOTK_MOTION_BLUR");
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

int vk_mblur_wanted(void) { return M.ok != 0 && percent() > 0; }

int vk_mblur_apply(VkExec *ex, VkImage *pic, VkImageView *pic_view, uint32_t ow, uint32_t oh, VkImageView mv,
                   VkImageView depth, uint32_t w, uint32_t h, const double inv_w[4], int exact) {
    int pct = percent();
    if (pct != M.percent) {
        if (pct) rt_log("mblur: motion blur over %d %% of each frame's motion", pct);
        else if (M.percent > 0) rt_log("mblur: motion blur off");
        M.percent = pct;
    }
    if (!pct || !init()) return 0;
    uint32_t cell = (h + TILE_ROWS - 1) / TILE_ROWS, tile = 8 * cell;
    uint32_t tx = (w + tile - 1) / tile, ty = (h + tile - 1) / tile;
    if (tx > TILES_W || ty > TILES_H) return 0;   // a picture wider than 5.6:1
    if (!out_for(ex, ow, oh, exact)) {
        rt_log("mblur: motion blur off: no memory for a %ux%u picture", ow, oh);
        return M.ok = 0;
    }
    struct Out *o = &M.out;
    VkCommandBuffer cb = ex->cb;
    if (M.tiles_new) to_general(cb, M.tiles), M.tiles_new = 0, ex->recorded++;

    // 1. the tiles' longest motion; vectors past a quarter of the scene in one frame are a cut, not motion
    VkDescriptorImageInfo mi = {M.nearest, mv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo ti = {VK_NULL_HANDLE, M.tiles_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet tws[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &mi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ti}};
    float cut = 0.25f * (float)(w > h ? w : h);
    struct { int32_t w, h, cell; float cut; } tp = {(int32_t)w, (int32_t)h, (int32_t)cell, cut * cut};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, M.tile_pipe);
    rec_push(vk_pipeline_push_fn(), cb, VK_PIPELINE_BIND_POINT_COMPUTE, M.tile_layout, 0, 2, tws);
    vkCmdPushConstants(cb, M.tile_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof tp, &tp);
    vkCmdDispatch(cb, tx, ty, 1);
    mem_barrier(cb);

    // 2. the blur. 1 / clip w = row . (ndc.x, ndc.y, 2 * depth - 1, 1), folded into a row for the depth as stored,
    // in double precision: near the far plane the last two terms nearly cancel
    VkDescriptorImageInfo si = {M.linear, *pic_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo zi = {M.nearest, depth, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo tsi = {M.nearest, M.tiles_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo oi = {VK_NULL_HANDLE, o->view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet bw[5] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &si},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &mi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &zi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &tsi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 4, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
    float noise = (float)fmod((double)(M.frames % 64) * 0.6180339887, 1.0);
    float bp[12] = {(float)inv_w[0], (float)inv_w[1], (float)(2.0 * inv_w[2]), (float)(inv_w[3] - inv_w[2]),
                    (float)ow, (float)oh, (float)w, (float)h,
                    (float)pct / 100.0f, (float)oh / MAX_SHARE, (float)tile, noise};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, M.blur_pipe);
    rec_push(vk_pipeline_push_fn(), cb, VK_PIPELINE_BIND_POINT_COMPUTE, M.blur_layout, 0, 5, bw);
    vkCmdPushConstants(cb, M.blur_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof bp, bp);
    vkCmdDispatch(cb, (ow + 7) / 8, (oh + 7) / 8, 1);
    mem_barrier(cb);
    ex->recorded += 10;
    M.frames++;
    *pic = o->img, *pic_view = o->view;
    return 1;
}
