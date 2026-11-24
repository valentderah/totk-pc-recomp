// Point lights at full resolution (TOTK_LIGHT_FULL: game, full; applies in play).
// TotK draws its point lights (lightroots, the Brightbloom's light, glowing items) at half the size of its G-buffer per
// axis, in one pass of light volumes (1.4.3: targets 0 and 1 the two layers of a B10G11R11 light image, target 2 an
// RGBA8 occlusion image, a D32F depth that the pass before builds from the half-size linear depth), and the full-size
// lighting composite reads the light image with one bilinear sample. A light near the ground reaches it almost sideways,
// so the ground's normal map turns into streaks thinner than the half-size pixels where the ground is seen at a shallow
// angle, and the lit ground shimmers while the view moves; filtering the light afterwards only blurs its detail away.
// With full, both passes are drawn at the G-buffer's size:
// - vk_lightfull_retarget, as a pass's targets are resolved, recognises the light pass (three colour targets: both
//   layers of one two-layer B10G11R11 image and an RGBA8 one, a D32F depth, all at half the G-buffer's size) and its
//   depth pass (one R32F target and a D32F depth at that size, after the frame's main half-size lighting pass of six or
//   more targets: a pass of the same shape early in the frame writes the half-size linear depth itself, which the whole
//   half-size lighting reads, and must stay the game's) and puts full-size images of ours in place of their targets, with
//   VkTexImage-style scale 2: the executor doubles the game's viewport and scissor (vk_exec.c resolve_rt_as,
//   vk_exec_scissor; vk_exec_draw.c draw_viewport). The light pass is redirected only after its depth pass was;
//   the passes after it that test against the game's half-size depth read the one the early pass wrote;
// - in those passes the half-size linear depth and the half-size copy of the normals are sampled as the G-buffer's
//   full-size ones (vk_lightfull_sampled, from vk_exec_draw.c tex_info), and the support buffer's render scale is 2
//   (vk_exec_draw.c support_in): the shaders divide gl_FragCoord by it before scaling by the half-size texel size, so the
//   same coordinates land on full-size texels;
// - our light and occlusion images start the pass cleared, so they hold the lights alone. After the pass
//   runtime/shaders/light_full.comp adds their 2x2 averages to the game's half-size images (the half-size passes after
//   it read those) and adds the game's light image, enlarged bilinearly as the composite would, to ours;
// - draws in full-size passes then sample ours in place of the game's light image (vk_lightfull_swap_img and
//   vk_lightfull_swap_view, applied by tex_info), until a pass writes the game's image again or a new frame begins.
// The light shaders seen in 1.4.3 (fragment SPIR-V 9b1af94d1cc8e81f and fb38a8425a4c8eea) read their textures at
// gl_FragCoord / render scale, never with texelFetch, which is what makes this work.
#include "vk.h"

#include <string.h>

#include "cfg.h"
#include "host.h"
#include "light_full_spv.h"
#include "vk_exec.h"
#include "vk_rec.h"

typedef struct {
    VkImage img;
    VmaAllocation alloc;
    VkImageView view, layer[2];   // view: the whole image (2D array for two layers); layer: each one as 2D
} Img;

enum { I_LIGHT, I_OCCL, I_DEPTH, I_LDEPTH, I_HLIGHT, I_HOCCL, NIMG };   // full: light, occlusion, depth, linear depth
                                                                        // scratch; half: light, occlusion
const VkTexImage *vk_lightfull_swap_img;
VkImageView vk_lightfull_swap_view;

static struct {
    int tried, on, ok, init_tried, logged, fresh;
    unsigned cfg_ver;
    uint64_t frame, pass_frame;         // presents so far; the frame of the last pass seen
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkSampler linear;
    Img img[NIMG], old[8][NIMG];        // old: earlier sizes, which recorded work may still use (freed at release)
    int nold;
    uint32_t w, h, hw, hh;              // the G-buffer's size; the game's
    const VkTexImage *gdepth, *gnormal; // this frame's G-buffer linear depth and normals
    int armed;                          // this frame's main half-size lighting pass ran: the next depth pass is the
                                        // light pass's (an earlier one also writes the half-size linear depth)
    int depth_done;                     // the light pass's depth pass was drawn at full size, into ours
    const VkTexImage *game_light, *game_occl;   // the last light pass's game targets
    const VkTexImage *game_depth;       // the depth pass's game D32F: ours starts as it, enlarged (the pass may draw
                                        // only part of it; the rest holds what an earlier pass of the frame wrote)
    int pending;                        // a light pass was drawn: the merge is due before the next other pass (the
                                        // executor can split one game pass in several; they all add to ours)
} F;

static void swap_off(VkExec *ex) {
    if (!vk_lightfull_swap_img) return;
    vk_lightfull_swap_img = NULL, vk_lightfull_swap_view = VK_NULL_HANDLE;
    vk_exec_draw_new_cb(ex);   // a draw with the same bindings as the one before must not keep the swapped view
}

static void update_mode(VkExec *ex) {
    unsigned v = cfg_version();
    if (F.tried && v == F.cfg_ver) return;
    int on = !strcmp(cfg_value("TOTK_LIGHT_FULL"), "full"), first = !F.tried;
    F.tried = 1, F.cfg_ver = v;
    if (on == F.on && !first) return;
    F.on = on;
    if (!on) swap_off(ex);
    rt_log(on ? "vk: point lights drawn at full resolution" : "vk: point lights drawn at the game's half resolution");
}

static int init(VkCtx *c) {
    F.init_tried = 1;
    VkFormatProperties fp;   // the depth pass's start: a blit of the game's D32F into ours
    vkGetPhysicalDeviceFormatProperties(c->phys, VK_FORMAT_D32_SFLOAT, &fp);
    VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((fp.optimalTilingFeatures & blit) != blit) {
        rt_log("vk: full-resolution point lights off: the GPU cannot blit D32F depth");
        return 0;
    }
    F.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[6];
    for (uint32_t i = 0; i < 6; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorCount = 1,
                                              .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                              .descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                      : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 6, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &F.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = sizeof light_full_spv, .pCode = light_full_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (!F.push || vkCreateDescriptorSetLayout(c->device, &dci, NULL, &F.dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &li, NULL, &F.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &F.linear) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS)
        goto fail;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = F.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &F.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) goto fail;
    return F.ok = 1;
fail:
    rt_log("vk: full-resolution point lights off: the pipeline could not be created");
    return 0;
}

static void free_image(VkCtx *c, Img *m) {
    for (int k = 0; k < 2; k++)
        if (m->layer[k] && m->layer[k] != m->view) vkDestroyImageView(c->device, m->layer[k], NULL);
    if (m->view) vkDestroyImageView(c->device, m->view, NULL);
    if (m->img) vmaDestroyImage(c->vma, m->img, m->alloc);
    memset(m, 0, sizeof *m);
}

static int make_image(VkCtx *c, Img *m, VkFormat fmt, uint32_t w, uint32_t h, uint32_t layers, VkImageUsageFlags use,
                      VkImageAspectFlags aspect) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = use};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ci, &ai, &m->img, &m->alloc, NULL) != VK_SUCCESS) {
        m->img = VK_NULL_HANDLE;
        return 0;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->img,
                                .viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt, .subresourceRange = {aspect, 0, 1, 0, layers}};
    if (vkCreateImageView(c->device, &vi, NULL, &m->view) != VK_SUCCESS) return 0;
    for (uint32_t k = 0; k < 2; k++) {
        if (layers == 1) {
            m->layer[k] = m->view;
            continue;
        }
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.subresourceRange.baseArrayLayer = k, vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(c->device, &vi, NULL, &m->layer[k]) != VK_SUCCESS) return 0;
    }
    return 1;
}

static int ensure_images(VkCtx *c, uint32_t w, uint32_t h, uint32_t hw, uint32_t hh) {
    if (F.img[0].img && F.w == w && F.h == h && F.hw == hw && F.hh == hh) return 1;
    if (F.img[0].img) {
        if (F.nold == 8) {   // a full list of retired sizes: wait for the GPU, free them (as vk_dlaa.c does)
            vkDeviceWaitIdle(c->device);
            for (int i = 0; i < F.nold; i++)
                for (int k = 0; k < NIMG; k++) free_image(c, &F.old[i][k]);
            F.nold = 0;
        }
        memcpy(F.old[F.nold++], F.img, sizeof F.img);
        memset(F.img, 0, sizeof F.img);
    }
    const VkImageUsageFlags col = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkImageAspectFlags ca = VK_IMAGE_ASPECT_COLOR_BIT;
    int ok = make_image(c, &F.img[I_LIGHT], VK_FORMAT_B10G11R11_UFLOAT_PACK32, w, h, 2, col, ca) &&
             make_image(c, &F.img[I_OCCL], VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, col, ca) &&
             make_image(c, &F.img[I_DEPTH], VK_FORMAT_D32_SFLOAT, w, h, 1,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_DEPTH_BIT) &&
             make_image(c, &F.img[I_LDEPTH], VK_FORMAT_R32_SFLOAT, w, h, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, ca) &&
             make_image(c, &F.img[I_HLIGHT], VK_FORMAT_B10G11R11_UFLOAT_PACK32, hw, hh, 2, col, ca) &&
             make_image(c, &F.img[I_HOCCL], VK_FORMAT_R8G8B8A8_UNORM, hw, hh, 1, col, ca);
    if (!ok) {
        for (int k = 0; k < NIMG; k++) free_image(c, &F.img[k]);
        rt_log("vk: full-resolution point lights off: %ux%u images could not be created", w, h);
        F.ok = 0;
        return 0;
    }
    F.w = w, F.h = h, F.hw = hw, F.hh = hh, F.fresh = 1;
    rt_log("vk: full-resolution point lights at %ux%u (the game's %ux%u)", w, h, hw, hh);
    return 1;
}

static int half_of(uint32_t half, uint32_t full) { return half * 2 + 1 >= full && half * 2 <= full + 1; }

static int half_target(const VkExecTarget *t) {
    return t->image && t->img && !t->level && half_of(t->width, F.gdepth->width) &&
           half_of(t->height, F.gdepth->height);
}

// A G-buffer pass: a D32F depth and, at the pass's size, an R32F target (the linear depth) and an RGBA8 target 3 (the
// normals).
static int gbuffer(const VkExec *ex, const VkTexImage **normals, const VkTexImage **depth) {
    *normals = *depth = NULL;
    if (ex->pass_noatt || ex->pass_w < 640 || ex->depth.format != VK_FORMAT_D32_SFLOAT || ex->depth.level ||
        ex->pass_ncolor < 4)
        return 0;
    const VkExecTarget *n = &ex->color[3];
    if (n->image && n->img && !n->level && n->width == ex->pass_w && n->height == ex->pass_h &&
        n->format == VK_FORMAT_R8G8B8A8_UNORM)
        *normals = n->img;
    for (uint32_t i = 0; i < ex->pass_ncolor && i < 8; i++) {
        const VkExecTarget *t = &ex->color[i];
        if (t->image && t->img && !t->level && t->width == ex->pass_w && t->height == ex->pass_h &&
            t->format == VK_FORMAT_R32_SFLOAT)
            *depth = t->img;
    }
    return *normals && *depth;
}

static void put(VkExecTarget *t, const Img *m, VkImage image, uint32_t layer) {
    (void)m;
    t->image = image, t->img = NULL;
    t->view_type = VK_IMAGE_VIEW_TYPE_2D, t->level = 0, t->base_layer = layer, t->layers = 1;
    t->width = F.w, t->height = F.h, t->scale = 2, t->is3d = 0, t->blocks = 0;
}

int vk_lightfull_retarget(VkExec *ex) {
    if (!F.on || !F.gdepth || !F.gnormal) return 0;
    int n = ex->rt.n < 0 ? 0 : ex->rt.n > 8 ? 8 : ex->rt.n;
    VkExecTarget *c = ex->color, *d = &ex->depth;
    if (!d->image || !d->img || d->format != VK_FORMAT_D32_SFLOAT || !half_target(d)) return 0;
    int mode = 0;
    if (n == 3 && F.depth_done && half_target(&c[0]) && half_target(&c[1]) && half_target(&c[2]) && c[0].img == c[1].img &&
        c[0].format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && c[0].img->layers == 2 && c[0].base_layer == 0 &&
        c[1].base_layer == 1 && c[0].layers == 1 && c[1].layers == 1 && c[2].format == VK_FORMAT_R8G8B8A8_UNORM)
        mode = 2;
    else if (n == 1 && F.armed && half_target(&c[0]) && c[0].format == VK_FORMAT_R32_SFLOAT)
        mode = 1;
    if (!mode) return 0;
    VkCtx *vc = vk_ctx();
    if (!F.init_tried) init(vc);
    if (!F.ok || !ensure_images(vc, F.gdepth->width, F.gdepth->height, d->img->width, d->img->height)) return 0;
    if (mode == 2) {
        F.game_light = c[0].img, F.game_occl = c[2].img;
        put(&c[0], &F.img[I_LIGHT], F.img[I_LIGHT].img, 0);
        put(&c[1], &F.img[I_LIGHT], F.img[I_LIGHT].img, 1);
        put(&c[2], &F.img[I_OCCL], F.img[I_OCCL].img, 0);
    } else {
        F.game_depth = d->img;
        put(&c[0], &F.img[I_LDEPTH], F.img[I_LDEPTH].img, 0);
    }
    put(d, &F.img[I_DEPTH], F.img[I_DEPTH].img, 0);
    if (!(F.logged & mode)) {   // once per pass kind
        F.logged |= mode;
        rt_log("vk: the point lights' %s pass drawn at %ux%u", mode == 2 ? "light" : "depth", F.w, F.h);
    }
    return mode;
}

// In a pass drawn at full size: the half-size linear depth and the half-size normals as the G-buffer's.
VkImageView vk_lightfull_sampled(const VkExec *ex, const VkTexImage *img) {
    if (!F.gdepth || !F.gnormal || !half_of(img->width, F.gdepth->width) || !half_of(img->height, F.gdepth->height))
        return VK_NULL_HANDLE;
    if (img->format == VK_FORMAT_R32_SFLOAT)
        return vk_exec_target_view(F.gdepth, VK_FORMAT_R32_SFLOAT, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                   0, 1, 0);
    if (ex->pass_lightfull == 2 && img->format == VK_FORMAT_R8G8B8A8_UNORM && img != F.game_occl)
        return vk_exec_target_view(F.gnormal, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D,
                                   VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0);
    return VK_NULL_HANDLE;
}

void vk_lightfull_present(uint64_t frame) { __atomic_store_n(&F.frame, frame, __ATOMIC_RELAXED); }

static void to_general(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                               .subresourceRange = {aspect, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                           .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

static void all_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// Before a pass opens (no pass is open): the first pass of a new frame forgets the last frame's lighting passes; our
// images are made usable, and before the light pass ours are cleared, so they hold the lights alone.
static void merge(VkExec *ex);

void vk_lightfull_pass_begin(VkExec *ex) {
    update_mode(ex);
    if (!F.on) {
        F.pending = 0;
        return;
    }
    if (F.pending && ex->pass_lightfull != 2) merge(ex);
    // A new frame disarms here, not at its G-buffer: in some shrines the first pass with the G-buffer's R32F target
    // comes after the early pass that writes the half-size linear depth, and the depth pass before the light pass has
    // no colour target, so nothing else disarms. A lighting pass of the last frame then made that early pass the
    // light pass's depth pass, drawn into ours, and the game's half-size linear depth kept an old frame's contents.
    // Present and passes run in order on the replay thread, so the count changes between two frames' passes.
    uint64_t f = __atomic_load_n(&F.frame, __ATOMIC_RELAXED);
    if (f != F.pass_frame) {
        F.pass_frame = f, F.armed = F.depth_done = 0;
        swap_off(ex);
    }
    const VkTexImage *n, *d;
    if (gbuffer(ex, &n, &d)) {
        F.gnormal = n, F.gdepth = d;
        return;
    }
    if (!ex->pass_lightfull) return;
    VkCommandBuffer cb = ex->cb;
    vk_exec_barrier(ex);
    vk_exec_draw_new_cb(ex);   // draws here get the full-size textures (vk_lightfull_sampled), not cached descriptors
    if (F.fresh) {
        for (int k = 0; k < NIMG; k++)
            to_general(cb, F.img[k].img, k == I_DEPTH ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
        F.fresh = 0;
        ex->recorded++;
    }
    if (ex->pass_lightfull == 1 && F.game_depth) {   // the depth pass: ours starts as the game's, enlarged
        VkImageBlit r = {.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                         .srcOffsets = {{0, 0, 0}, {(int32_t)F.game_depth->width, (int32_t)F.game_depth->height, 1}},
                         .dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                         .dstOffsets = {{0, 0, 0}, {(int32_t)F.w, (int32_t)F.h, 1}}};
        vkCmdBlitImage(cb, F.game_depth->image, VK_IMAGE_LAYOUT_GENERAL, F.img[I_DEPTH].img, VK_IMAGE_LAYOUT_GENERAL, 1,
                       &r, VK_FILTER_NEAREST);
        all_barrier(cb);
        ex->recorded += 2;
    }
    if (ex->pass_lightfull == 2 && !F.pending) {   // the first part of the light pass: ours start empty
        VkClearColorValue zero = {.float32 = {0, 0, 0, 0}};
        VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(cb, F.img[I_LIGHT].img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
        vkCmdClearColorImage(cb, F.img[I_OCCL].img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
        all_barrier(cb);
        ex->recorded += 3;
    }
}

static void copy_layers(VkCommandBuffer cb, VkImage src, VkImage dst, uint32_t w, uint32_t h, uint32_t layers) {
    VkImageCopy r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers}, .extent = {w, h, 1}};
    vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
}

static void run(VkExec *ex, VkImageView game_light, VkImageView game_occl, uint32_t layer, int mode) {
    VkDescriptorImageInfo ii[6] = {{F.linear, game_light, VK_IMAGE_LAYOUT_GENERAL},
                                   {F.linear, game_occl, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, F.img[I_LIGHT].layer[layer], VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, F.img[I_OCCL].view, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, F.img[I_HLIGHT].layer[layer], VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, F.img[I_HOCCL].view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[6];
    for (uint32_t i = 0; i < 6; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = i,
                                      .descriptorCount = 1,
                                      .descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                              : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      .pImageInfo = &ii[i]};
    int32_t pc[6] = {(int32_t)F.w, (int32_t)F.h, (int32_t)F.hw, (int32_t)F.hh, mode, mode == 0 && layer == 0};
    rec_push(F.push, ex->cb, VK_PIPELINE_BIND_POINT_COMPUTE, F.layout, 0, 6, w);
    vkCmdPushConstants(ex->cb, F.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    uint32_t gw = mode ? F.w : F.hw, gh = mode ? F.h : F.hh;
    vkCmdDispatch(ex->cb, (gw + 7) / 8, (gh + 7) / 8, 1);
}

// The light pass is over (no pass is open): the game's half-size images get the lights and ours the game's light.
static void merge(VkExec *ex) {
    F.pending = 0, F.depth_done = 0;
    if (!F.game_light || !F.game_occl) return;
    VkImageView gl[2], go = vk_exec_target_view(F.game_occl, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D,
                                                VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0);
    for (uint32_t k = 0; k < 2; k++)
        gl[k] = vk_exec_target_view(F.game_light, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_IMAGE_VIEW_TYPE_2D,
                                    VK_IMAGE_ASPECT_COLOR_BIT, 0, k, 1, 0);
    if (!gl[0] || !gl[1] || !go) return;
    vk_exec_barrier(ex);
    VkCommandBuffer cb = ex->cb;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, F.pipe);
    for (uint32_t k = 0; k < 2; k++) run(ex, gl[k], go, k, 0);   // the half-size sums, into our half-size images
    all_barrier(cb);
    for (uint32_t k = 0; k < 2; k++) run(ex, gl[k], go, k, 1);   // the game's light into ours, from its unchanged images
    all_barrier(cb);
    copy_layers(cb, F.img[I_HLIGHT].img, F.game_light->image, F.hw, F.hh, 2);
    copy_layers(cb, F.img[I_HOCCL].img, F.game_occl->image, F.hw, F.hh, 1);
    all_barrier(cb);
    ex->recorded += 10;
    // the game's bound compute pipeline and descriptors were replaced: bind them again; from here full-size passes
    // sample ours
    vk_exec_draw_new_cb(ex);
    vk_lightfull_swap_img = F.game_light, vk_lightfull_swap_view = F.img[I_LIGHT].view;
}

// After a pass: the light pass leaves the merge pending; any other write to the game's light image makes ours out of
// date.
void vk_lightfull_pass_end(VkExec *ex) {
    if (!F.on) return;
    if (ex->pass_lightfull) vk_exec_draw_new_cb(ex);   // its draws' descriptors held the swapped textures
    if (ex->pass_lightfull == 1) F.depth_done = 1, F.armed = 0;
    if (ex->pass_lightfull == 2) {
        F.pending = 1;
        return;
    }
    // the main half-size lighting pass: six or more targets at half the G-buffer's size, one a two-layer B10G11R11
    if (F.gdepth && ex->pass_ncolor >= 6 && !ex->pass_lightfull)
        for (uint32_t i = 0; i < ex->pass_ncolor && i < 8; i++)
            if (half_target(&ex->color[i]) && ex->color[i].format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 &&
                ex->color[i].img->layers == 2)
                F.armed = 1, F.depth_done = 0;
    if (vk_lightfull_swap_img)
        for (uint32_t i = 0; i < ex->pass_ncolor && i < 8; i++)
            if (ex->color[i].img == vk_lightfull_swap_img) swap_off(ex);
}

void vk_lightfull_release(void) {
    VkCtx *c = vk_ctx();
    vk_lightfull_swap_img = NULL, vk_lightfull_swap_view = VK_NULL_HANDLE;
    if (!c || !F.tried) return;
    for (int i = 0; i < F.nold; i++)
        for (int k = 0; k < NIMG; k++) free_image(c, &F.old[i][k]);
    for (int k = 0; k < NIMG; k++) free_image(c, &F.img[k]);
    if (F.pipe) vkDestroyPipeline(c->device, F.pipe, NULL);
    if (F.linear) vkDestroySampler(c->device, F.linear, NULL);
    if (F.layout) vkDestroyPipelineLayout(c->device, F.layout, NULL);
    if (F.dsl) vkDestroyDescriptorSetLayout(c->device, F.dsl, NULL);
    memset(&F, 0, sizeof F);
}
