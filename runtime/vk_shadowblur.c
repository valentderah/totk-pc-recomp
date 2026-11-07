// Depth-aware versions of the game's two blurs of its shadow mask (TOTK_SHADOW_BLUR: game, depth; applies in play).
// TotK works out the sun's shadow into a quarter-size RGBA8 mask and blurs it twice without reading depth: one
// full-screen draw along each row (1.4.3: guest fragment shader FS_BLUR), then along each column inside the draw that
// merges the mask's channels (FS_MERGE). A foreground object's lit value spreads about four texels over the shadowed
// background behind it, which shows as a light strip around silhouettes, 30 pixels wide at a 2560x1440 scene. This is
// the game's own look, so the setting defaults to game.
// With depth, the game's draws still run. The row blur's source is the one texture the draw samples
// (vk_shadowblur_sampled, from vk_exec_draw.c tex_info). Before the next pass, runtime/shaders/shadow_blur.comp blurs
// that source again into an image of ours, counting only taps on the pixel's own surface, and the merge samples ours in
// place of the game's result (vk_shadowblur_swap_img and vk_shadowblur_swap_view, applied by tex_info). The merge's
// red is then the plain column blur of ours: measured in the Depths it matches to 0.04 of 255 on average. After the
// merge the shader replaces that blur in red by the depth-aware one, and the result is copied over the game's merged
// mask, which the lighting reads.
// The merge also classifies the lighting's screen tiles from the row's result, so the row's red keeps the plain blur's
// verdict of lit, dark or in between (shadow_blur.comp).
// The depth is the last single-target R32F pass before the blur, which the game downsamples for this mask.
#include "vk.h"

#include <string.h>

#include "cfg.h"
#include "host.h"
#include "shadow_blur_spv.h"
#include "vk_exec.h"
#include "vk_rec.h"

// The guest fragment shaders, FNV-1a 64 of their code (vk_exec_fs_hash), the same on the surface and in the Depths.
#define FS_BLUR 0x68aa9c5e8632e31cull    // the row blur
#define FS_MERGE 0xfdb731cd52e458b2ull   // the merge, with the column blur
// How far a tap's depth may stray from the pixel's, per texel of distance, as a fraction of the pixel's depth, before
// it starts to fade. A wall at 80 degrees to the view at a field of view of 100 changes about 0.015 per texel.
#define SLOPE 0.02f

typedef struct {
    VkImage img;
    VmaAllocation alloc;
    VkImageView view;
} Img;

const VkTexImage *vk_shadowblur_swap_img;
VkImageView vk_shadowblur_swap_view;
uint32_t vk_shadowblur_swap_hits;
int vk_shadowblur_watch;

static struct {
    int tried, on, ok, init_tried, logged;
    unsigned cfg_ver;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;          // sampled at 0 and 1, storage at 2
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkSampler point;
    Img out[2], old[8][2];              // out: the row's and the column's result; old: earlier sizes, which recorded
                                        // work may still use (freed at release)
    int nold, fresh;                    // fresh: the image is still in UNDEFINED layout
    uint32_t w, h;                      // our image's size
    uint64_t frame, swap_frame;         // presents so far; the frame the swap began in
    const VkTexImage *depth;            // the last single-target R32F pass's image
    const VkTexImage *src, *dst;        // the blur draw's source and result, set until the flush: pending
    const VkTexImage *merged;           // the merge's target, set until the flush: pending
    int row_done;                       // out[0] holds this frame's row blur
} S;

static void swap_off(void) {
    if (!vk_shadowblur_swap_img) return;
    vk_shadowblur_swap_img = NULL, vk_shadowblur_swap_view = VK_NULL_HANDLE;
    vk_exec_draw_new_cb();   // a draw with the same bindings as the one before must not keep the swapped view
}

static void update_mode(void) {
    unsigned v = cfg_version();
    if (S.tried && v == S.cfg_ver) return;
    int on = !strcmp(cfg_value("TOTK_SHADOW_BLUR"), "depth"), first = !S.tried;
    S.tried = 1, S.cfg_ver = v;
    if (on == S.on && !first) return;
    S.on = on;
    if (!on) S.src = S.dst = S.merged = NULL, S.row_done = 0, vk_shadowblur_watch = 0, swap_off();
    rt_log(on ? "vk: the shadow mask's blur stops at depth edges" : "vk: the shadow mask keeps the game's blur");
}

static int init(VkCtx *c) {
    S.init_tried = 1;
    S.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[4];
    for (uint32_t i = 0; i < 4; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = i, .descriptorCount = 1,
                                              .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                                              .descriptorType = i != 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                       : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 4, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 20};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &S.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = sizeof shadow_blur_spv, .pCode = shadow_blur_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (!S.push || vkCreateDescriptorSetLayout(c->device, &dci, NULL, &S.dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &li, NULL, &S.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &S.point) != VK_SUCCESS ||
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
    rt_log("vk: depth-aware shadow blur off: the pipeline could not be created");
    return 0;
}

static void free_image(VkCtx *c, Img *m) {
    if (m->view) vkDestroyImageView(c->device, m->view, NULL);
    if (m->img) vmaDestroyImage(c->vma, m->img, m->alloc);
    memset(m, 0, sizeof *m);
}

static int make_image(VkCtx *c, Img *m, uint32_t w, uint32_t h) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vmaCreateImage(c->vma, &ci, &ai, &m->img, &m->alloc, NULL) != VK_SUCCESS) {
        m->img = VK_NULL_HANDLE;
        return 0;
    }
    vi.image = m->img;
    return vkCreateImageView(c->device, &vi, NULL, &m->view) == VK_SUCCESS;
}

static int ensure_images(VkCtx *c, uint32_t w, uint32_t h) {
    if (S.out[0].img && S.w == w && S.h == h) return 1;
    if (S.out[0].img) {
        if (S.nold == 8) return 0;
        memcpy(S.old[S.nold++], S.out, sizeof S.out);
        memset(S.out, 0, sizeof S.out);
    }
    if (!make_image(c, &S.out[0], w, h) || !make_image(c, &S.out[1], w, h)) {
        free_image(c, &S.out[0]), free_image(c, &S.out[1]);
        rt_log("vk: depth-aware shadow blur off: %ux%u images could not be created", w, h);
        S.ok = 0;
        return 0;
    }
    S.w = w, S.h = h, S.fresh = 1;
    return 1;
}

static void to_general(VkCommandBuffer cb, VkImage img, int fresh) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT,   // earlier work read ours
                                .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                .oldLayout = fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                                .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                           .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
}

static void all_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo md = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &md);
}

// One pass of shadow_blur.comp along the axis into ours; every image in the GENERAL layout.
static void run(VkCommandBuffer cb, VkImageView source, VkImageView depth, VkImageView keep, VkImageView result,
                int vertical) {
    VkDescriptorImageInfo ii[4] = {{S.point, source, VK_IMAGE_LAYOUT_GENERAL}, {S.point, depth, VK_IMAGE_LAYOUT_GENERAL},
                                   {VK_NULL_HANDLE, result, VK_IMAGE_LAYOUT_GENERAL},
                                   {S.point, keep, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet w[4];
    for (uint32_t i = 0; i < 4; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = i,
                                      .descriptorCount = 1,
                                      .descriptorType = i != 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                               : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      .pImageInfo = &ii[i]};
    struct { int32_t size[2], axis[2]; float slope; } pc = {{(int32_t)S.w, (int32_t)S.h}, {!vertical, vertical}, SLOPE};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.pipe);
    rec_push(S.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, S.layout, 0, 4, w);
    vkCmdPushConstants(cb, S.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (S.w + 7) / 8, (S.h + 7) / 8, 1);
    all_barrier(cb);
}

static VkImageView rgba8_view(const VkTexImage *img) {
    return vk_exec_target_view(img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0);
}

static VkImageView depth_view(void) {
    return vk_exec_target_view(S.depth, VK_FORMAT_R32_SFLOAT, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0);
}

// After the row blur: ours, and the merge samples it.
static void flush_row(VkExec *ex) {
    const VkTexImage *src = S.src, *dst = S.dst;
    S.src = S.dst = NULL, S.row_done = 0;
    VkCtx *c = vk_ctx();
    if (!S.init_tried) init(c);
    if (!S.ok || !ensure_images(c, dst->width, dst->height)) return;
    VkImageView m = rgba8_view(src), d = depth_view();
    if (!m || !d) return;
    vk_exec_barrier(ex);   // the game's writes; images in the feedback layout go back to GENERAL
    VkCommandBuffer cb = ex->cb;
    to_general(cb, S.out[0].img, S.fresh), to_general(cb, S.out[1].img, S.fresh);
    S.fresh = 0;
    run(cb, m, d, m, S.out[0].view, 0);
    ex->recorded += 7;
    if (!S.logged) S.logged = 1, rt_log("vk: depth-aware shadow blur runs at %ux%u", S.w, S.h);
    // the game's bound compute pipeline and descriptors were replaced: bind them again; from here the merge samples ours
    vk_exec_draw_new_cb();
    vk_shadowblur_swap_img = dst, vk_shadowblur_swap_view = S.out[0].view;
    vk_shadowblur_swap_hits = 0;
    S.swap_frame = __atomic_load_n(&S.frame, __ATOMIC_RELAXED);
    S.row_done = 1;
}

// After the merge: the column blur in red, over the game's mask.
static void flush_column(VkExec *ex) {
    const VkTexImage *mask = S.merged;
    S.merged = NULL;
    if (!S.row_done || !S.ok || mask->width != S.w || mask->height != S.h) return;
    S.row_done = 0;
    VkImageView m = rgba8_view(mask), d = depth_view();
    if (!m || !d) return;
    vk_exec_barrier(ex);   // the merge's writes
    VkCommandBuffer cb = ex->cb;
    to_general(cb, S.out[1].img, 0);
    run(cb, S.out[0].view, d, m, S.out[1].view, 1);
    VkImageCopy r = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {S.w, S.h, 1}};
    vkCmdCopyImage(cb, S.out[1].img, VK_IMAGE_LAYOUT_GENERAL, mask->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    all_barrier(cb);
    ex->recorded += 8;
    vk_exec_draw_new_cb();   // the game's bound compute pipeline and descriptors were replaced: bind them again
}

// Outside a pass: the blur's result and the merged mask are finished by now.
void vk_shadowblur_flush(VkExec *ex) {
    if (ex->in_pass) return;
    if (S.dst) flush_row(ex);
    if (S.merged) flush_column(ex);
}

void vk_shadowblur_present(uint64_t frame) { __atomic_store_n(&S.frame, frame, __ATOMIC_RELAXED); }

void vk_shadowblur_pass_begin(VkExec *ex) {
    update_mode();
    if (!S.on) return;
    // a frame where nothing read the swap (the mask pass did not run): the image may mean something else by now
    if (vk_shadowblur_swap_img && S.swap_frame != __atomic_load_n(&S.frame, __ATOMIC_RELAXED)) swap_off();
    const VkExecTarget *t = &ex->color[0];
    if (ex->pass_ncolor == 1 && t->img && !t->level && t->format == VK_FORMAT_R32_SFLOAT) S.depth = t->img;
    vk_shadowblur_flush(ex);
}

// The pass that ends: once the merge has sampled ours, the swap is over.
void vk_shadowblur_pass_end(VkExec *ex) {
    (void)ex;
    if (vk_shadowblur_swap_img && vk_shadowblur_swap_hits) swap_off();
}

// A draw about to be recorded, its pass open.
void vk_shadowblur_draw(VkExec *ex) {
    if (!S.tried) update_mode();
    vk_shadowblur_watch = 0;
    if (!S.on || !ex->pass_ncolor) return;
    const VkExecTarget *d = &ex->color[0];
    if (!d->img || d->level || d->format != VK_FORMAT_R8G8B8A8_UNORM) return;
    uint32_t len;
    uint64_t h = vk_exec_fs_hash(ex, &len);
    if (h == FS_MERGE && S.row_done) S.merged = d->img;
    if (h != FS_BLUR) return;
    if (!S.depth || S.depth->width != d->width || S.depth->height != d->height) return;
    vk_shadowblur_watch = 1;
}

// A texture the watched blur draw samples: its source.
void vk_shadowblur_sampled(VkExec *ex, const VkTexImage *img) {
    const VkExecTarget *d = &ex->color[0];
    if (!img || img->width != d->width || img->height != d->height) return;
    vk_shadowblur_watch = 0;
    S.src = img, S.dst = d->img;
}

void vk_shadowblur_release(void) {
    VkCtx *c = vk_ctx();
    vk_shadowblur_swap_img = NULL, vk_shadowblur_swap_view = VK_NULL_HANDLE, vk_shadowblur_watch = 0;
    if (!c || !S.tried) return;
    for (int i = 0; i < S.nold; i++) free_image(c, &S.old[i][0]), free_image(c, &S.old[i][1]);
    free_image(c, &S.out[0]), free_image(c, &S.out[1]);
    if (S.pipe) vkDestroyPipeline(c->device, S.pipe, NULL);
    if (S.point) vkDestroySampler(c->device, S.point, NULL);
    if (S.layout) vkDestroyPipelineLayout(c->device, S.layout, NULL);
    if (S.dsl) vkDestroyDescriptorSetLayout(c->device, S.dsl, NULL);
    memset(&S, 0, sizeof S);
}
