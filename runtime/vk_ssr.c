// Screen-space reflections (TOTK_WATER_REFLECTIONS: off, natural, clear; TOTK_SURFACE_REFLECTIONS: off, soft, sharp;
// both read at each present): the world mirrored on TotK's water and on its smooth surfaces. What reflects, the ray
// and its limits: runtime/shaders/ssr.comp.
//
// Where it runs: at the point of the contact shadows (vk_contact.c), which follows the frame for both: after the pass
// that composes the game's lighting into the scene colour and before whatever comes next (the sky, the transparents,
// the bloom, DLSS). vk_contact_plan hands over the scene colour, the linear depth and the G-buffer's targets 0 (object
// classes) and 3 (normal and smoothness) with the projection, and vk_contact_exec calls vk_ssr_exec after its march.
//
// The passes, recorded like the replay's own commands:
//   1. The lit scene is copied into S.copy (RGBA16F), which the rays read while the picture is rewritten.
//   2. For soft surfaces, three blurred mips of the copy, each a bilinear blit of the one above.
//   3. ssr.comp writes every pixel of the scene, with its reflection mixed in where there is one, into S.out.
//   4. S.out is blitted back into the scene colour.
// The game's scene colour is not written in place: it has no STORAGE usage unless contact shadows were on at start,
// and the setting here switches while the game runs.
// Both images only grow: TotK's dynamic resolution changes the scene's size from frame to frame, and a smaller scene
// uses the top left of them.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_exec.h"
#include "vk_rec.h"
#include "ssr_spv.h"

#define MIPS 4u          // the copy's levels: full size and three blurred ones
#define STEPS 32.0f
#define NEAR_M 0.1f      // rays stop 1.5 x this far in front of the camera
#define RAY_M 2000.0f    // the longest ray; the picture's edge cuts nearly all of them first

static struct {
    int ok;                      // -1 not built, 0 failed, 1 ready
    int water, surf, debug;      // the settings as of the last present (vk_ssr_frame): 0 off, 1 natural / soft, 2 clear / sharp
    int logged_water, logged_surf;
    int no_gbuf;                 // the G-buffer lacked target 0 or 3: logged once
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkSampler nearest, linear;
    struct Work {
        uint32_t w, h, mips;
        VkImage copy, out;
        VmaAllocation copy_mem, out_mem;
        VkImageView copy_view, out_view;
    } work;
    uint64_t runs;
} S = {.ok = -1, .logged_water = -1, .logged_surf = -1};

static int choice(const char *name, const char *one, const char *two) {
    const char *v = cfg_value(name);
    return !strcmp(v, one) ? 1 : !strcmp(v, two) ? 2 : 0;
}

void vk_ssr_frame(void) {
    S.water = choice("TOTK_WATER_REFLECTIONS", "natural", "clear");
    S.surf = choice("TOTK_SURFACE_REFLECTIONS", "soft", "sharp");
    const char *d = cfg_get("TOTK_SSR_DEBUG");   // 1: what reflects (water magenta, surfaces cyan); 2: the reflections alone
    S.debug = d && *d ? atoi(d) : 0;
}

int vk_ssr_wanted(void) { return S.ok != 0 && (S.water || S.surf); }

static int image2d(uint32_t w, uint32_t h, uint32_t mips, VkImageUsageFlags usage, VkImage *img, VmaAllocation *mem,
                   VkImageView *view) {
    const VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {w, h, 1}, .mipLevels = mips,
                            .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, img, mem, NULL) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1}};
    return vkCreateImageView(c->device, &vi, NULL, view) == VK_SUCCESS;
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

// Once, at the first frame with a setting on. The copy is filtered and blitted from and to, the result stored into and
// blitted into the game's scene colour (R11G11B10F); a device whose formats cannot do that keeps the game's picture.
static int init(void) {
    if (S.ok >= 0) return S.ok;
    S.ok = 0;
    const VkCtx *c = vk_ctx();
    VkFormatProperties rgba, r11;
    vkGetPhysicalDeviceFormatProperties(c->phys, VK_FORMAT_R16G16B16A16_SFLOAT, &rgba);
    vkGetPhysicalDeviceFormatProperties(c->phys, VK_FORMAT_B10G11R11_UFLOAT_PACK32, &r11);
    VkFormatFeatureFlags work = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                VK_FORMAT_FEATURE_BLIT_DST_BIT;
    VkFormatFeatureFlags scene = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((rgba.optimalTilingFeatures & work) != work || (r11.optimalTilingFeatures & scene) != scene) {
        rt_log("ssr: reflections off: the device cannot store into, filter or blit the float formats they work in");
        return 0;
    }
    VkDescriptorSetLayoutBinding b[5] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 5, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 96};
    VkSamplerCreateInfo sn = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkSamplerCreateInfo sl = sn;
    sl.magFilter = sl.minFilter = VK_FILTER_LINEAR, sl.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sl.maxLod = VK_LOD_CLAMP_NONE;
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof ssr_spv,
                                   .pCode = ssr_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (!vk_pipeline_push_fn() || vkCreateDescriptorSetLayout(c->device, &dci, NULL, &S.dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &S.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &S.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &sn, NULL, &S.nearest) != VK_SUCCESS ||
        vkCreateSampler(c->device, &sl, NULL, &S.linear) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS)
        goto fail;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = S.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &S.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) goto fail;
    return S.ok = 1;
fail:
    rt_log("ssr: reflections off: their pass could not be set up");
    return 0;
}

static void work_free(void *arg) {
    struct Work *k = arg;
    const VkCtx *c = vk_ctx();
    if (k->copy_view) vkDestroyImageView(c->device, k->copy_view, NULL);
    if (k->out_view) vkDestroyImageView(c->device, k->out_view, NULL);
    if (k->copy) vmaDestroyImage(c->vma, k->copy, k->copy_mem);
    if (k->out) vmaDestroyImage(c->vma, k->out, k->out_mem);
    free(k);
}

// S.work large enough for a w x h scene, in GENERAL: as it is, or new images that cover both sizes. The ones they
// replace were read by the frames before this one, so they go when the GPU has finished this submission, which comes
// after them. 0: no memory.
static int work_for(VkExec *ex, uint32_t w, uint32_t h) {
    struct Work *k = &S.work;
    if (k->copy && k->w >= w && k->h >= h) return 1;
    struct Work n = {.w = k->w > w ? k->w : w, .h = k->h > h ? k->h : h, .mips = MIPS};
    while (n.mips > 1 && ((n.w >> (n.mips - 1)) == 0 || (n.h >> (n.mips - 1)) == 0)) n.mips--;
    struct Work *old = malloc(sizeof *old);
    VkImageUsageFlags xfer = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!old ||
        !image2d(n.w, n.h, n.mips, VK_IMAGE_USAGE_SAMPLED_BIT | xfer, &n.copy, &n.copy_mem, &n.copy_view) ||
        !image2d(n.w, n.h, 1, VK_IMAGE_USAGE_STORAGE_BIT | xfer, &n.out, &n.out_mem, &n.out_view)) {
        if (old) *old = n, work_free(old);
        return 0;
    }
    *old = *k, *k = n;
    if (old->copy) vk_completion_add(ex->fence, work_free, old);
    else free(old);
    VkImageMemoryBarrier2 ib[2];
    VkImage im[2] = {k->copy, k->out};
    for (int i = 0; i < 2; i++)
        ib[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im[i],
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2,
                           .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(ex->cb, &di);
    ex->recorded++;
    return 1;
}

static void blit(VkCommandBuffer cb, VkImage src, uint32_t sl, uint32_t slayer, uint32_t sw, uint32_t sh, VkImage dst,
                 uint32_t dl, uint32_t dlayer, uint32_t dw, uint32_t dh, VkFilter f) {
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, sl, slayer, 1},
                     .srcOffsets = {{0, 0, 0}, {(int32_t)sw, (int32_t)sh, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dl, dlayer, 1},
                     .dstOffsets = {{0, 0, 0}, {(int32_t)dw, (int32_t)dh, 1}}};
    vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &b, f);
    mem_barrier(cb);
}

void vk_ssr_exec(VkExec *ex, const VkExecJob *j) {
    int water = S.water, surf = S.surf;
    if (water != S.logged_water || surf != S.logged_surf) {
        static const char *const wn[3] = {"off", "natural", "clear"}, *const sn[3] = {"off", "soft", "sharp"};
        if (S.logged_water >= 0 || water || surf) rt_log("ssr: water reflections %s, surface reflections %s", wn[water], sn[surf]);
        S.logged_water = water, S.logged_surf = surf;
    }
    if ((!water && !surf) || !init()) return;
    const VkExecTarget *scene = &j->scene, *depth = &j->depth, *cls = &j->aux[0], *nrm = &j->aux[1];
    if (!scene->img || !depth->img || !cls->img || !nrm->img) {
        if (!S.no_gbuf) rt_log("ssr: no reflections: the G-buffer has no RGBA8 target 0 (object classes) or 3 (normal)");
        S.no_gbuf = 1;
        return;
    }
    uint32_t w = scene->width, h = scene->height;
    if (!work_for(ex, w, h)) {
        rt_log("ssr: reflections off: no memory for a %ux%u scene", w, h);
        S.ok = 0;
        return;
    }
    const struct Work *k = &S.work;
    VkImageView dv = vk_exec_target_view(depth->img, VK_FORMAT_R32_SFLOAT, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
                                         0, depth->base_layer, 1, 0);
    VkImageView cv = vk_exec_target_view(cls->img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D,
                                         VK_IMAGE_ASPECT_COLOR_BIT, 0, cls->base_layer, 1, 0);
    VkImageView nv = vk_exec_target_view(nrm->img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D,
                                         VK_IMAGE_ASPECT_COLOR_BIT, 0, nrm->base_layer, 1, 0);
    if (!dv || !cv || !nv) return;
    // A full barrier that also takes the G-buffer's images out of the feedback layout (vk_contact_exec says why).
    vk_exec_barrier(ex);
    VkCommandBuffer cb = ex->cb;
    // 1. the lit scene into the copy, 2. its blurred mips where soft surfaces read them
    blit(cb, scene->image, 0, scene->base_layer, w, h, k->copy, 0, 0, w, h, VK_FILTER_NEAREST);
    uint32_t last = 0;
    if (surf == 1)
        for (uint32_t l = 1; l < k->mips && (w >> l) && (h >> l); l++, last++)
            blit(cb, k->copy, l - 1, 0, w >> (l - 1), h >> (l - 1), k->copy, l, 0, w >> l, h >> l, VK_FILTER_LINEAR);
    // 3. the reflections. The noise moves from frame to frame only where DLSS averages it away.
    VkDescriptorImageInfo si = {S.linear, k->copy_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo di = {S.nearest, dv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo ci = {S.nearest, cv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo ni = {S.nearest, nv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo oi = {VK_NULL_HANDLE, k->out_view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo *infos[5] = {&si, &di, &ci, &ni, &oi};
    VkWriteDescriptorSet wr[5];
    for (uint32_t i = 0; i < 5; i++)
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = i,
                                       .descriptorCount = 1,
                                       .descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                               : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .pImageInfo = infos[i]};
    float noise = vk_dlaa_resolving() ? (float)fmod((double)(S.runs % 64) * 0.6180339887, 1.0) : 0.0f;
    float pc[24] = {j->pc[0], j->pc[1], j->pc[2], NEAR_M,
                    (float)w, (float)h, 1.0f / (float)k->w, 1.0f / (float)k->h,
                    STEPS, (float)S.debug, noise, RAY_M,
                    water ? 1.0f : 0.0f, 0.02f, water == 2 ? 0.08f : 0.25f, 12.0f,
                    surf ? 1.0f : 0.0f, 0.5f, 0.04f, (float)last,
                    surf == 1 ? 0.15f : 0.0f, 0, 0, 0};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.pipe);
    rec_push(vk_pipeline_push_fn(), cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.layout, 0, 5, wr);
    vkCmdPushConstants(cb, S.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    mem_barrier(cb);
    // 4. the picture back into the game's scene colour
    blit(cb, k->out, 0, 0, w, h, scene->image, 0, scene->base_layer, w, h, VK_FILTER_NEAREST);
    ex->recorded += 9 + 2 * (int)last;
    if (!S.runs++)
        rt_log("ssr: reflections on a %ux%u scene (projection %.3f %.3f, far %g)", w, h, pc[0], pc[1], pc[2]);
    vk_exec_draw_new_cb(ex);   // the game's bound compute pipeline and descriptors were replaced: bind them again
}

void vk_ssr_release(void) {
    VkCtx *c = vk_ctx();
    if (!c || S.ok < 0) return;
    struct Work *k = &S.work;
    if (k->copy_view) vkDestroyImageView(c->device, k->copy_view, NULL);
    if (k->out_view) vkDestroyImageView(c->device, k->out_view, NULL);
    if (k->copy) vmaDestroyImage(c->vma, k->copy, k->copy_mem);
    if (k->out) vmaDestroyImage(c->vma, k->out, k->out_mem);
    if (S.pipe) vkDestroyPipeline(c->device, S.pipe, NULL);
    if (S.nearest) vkDestroySampler(c->device, S.nearest, NULL);
    if (S.linear) vkDestroySampler(c->device, S.linear, NULL);
    if (S.layout) vkDestroyPipelineLayout(c->device, S.layout, NULL);
    if (S.dsl) vkDestroyDescriptorSetLayout(c->device, S.dsl, NULL);
    memset(&S, 0, sizeof S);
    S.ok = S.logged_water = S.logged_surf = -1;
}
