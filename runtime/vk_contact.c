// Screen-space contact shadows (TOTK_CONTACT_SHADOWS, percent strength, 0 = off). TotK fills a full-resolution
// G-buffer whose targets include a B10G11R11 scene colour, an R32F linear depth (view distance / far) and a D32F depth
// (is_gbuffer), lights it in half resolution, and then composites the lighting into that scene colour in one pass
// whose only target is the scene colour, with a D16 depth that sorts pixels into material classes (is_composite).
// When the pass after the composite begins, or a dispatch or copy comes first, runtime/shaders/contact_shadow.comp
// marches each pixel of a surface facing the sun (normal from the linear depth) a short way toward the sun through the
// linear depth and darkens the scene colour where the ray passes behind nearby geometry: the small shadows at feet,
// under stones and in grass that the shadow maps are too coarse for. Surfaces turned away from the sun are left alone:
// they get no direct light to take away, and the colour the composite wrote already holds their ambient light and fog.
// Transparent surfaces, the sky and post-processing come after, untouched.
//
// vk_contact_plan decides that in replay order and vk_contact_exec records it: a chunked replay's scan
// plans the march (vk_exec.c scan_contact) and the chunk that holds its op records it.
//
// Inputs from the game, read where the march is planned from the fragment stage's uniform block c4 bound in the
// composite pass (guest memory, so it may be one frame newer than the replay): the projection scales c4[7].x and
// c4[8].y, the far plane c4[14].y, and the first sun cascade's depth row c4[44].xyz, whose negated direction points from
// a view-space position toward
// the sun (the G-buffer shaders compare dot(c4[44].xyz, pos) + c4[44].w with the cascade map). The scene colour images
// get STORAGE usage when the setting is on (vk_texture.c create_tex).
//
// Only sunlit pixels are darkened. The composite reads a half-resolution lighting pass (eight targets at half the scene
// size, the first RGBA8) whose first target's alpha is the sun's visibility: 1 in the sun, 0 in the shadow maps'
// shadow and everywhere underground (found by comparing frame dumps of a sunny path and a cave). The march's darkening
// is scaled by it, so a contact shadow takes away direct sunlight and leaves the ambient light and textures alone;
// darkening the whole colour turned caves flat, where nearly every ray toward the sun meets the rock overhead. Without
// that pass in the frame every pixel is darkened as before. TOTK_CONTACT_DEBUG=2 shows the visibility in grey.
//
// Some caves get a sun visibility of 1 all the same (the Rauru Settlement Ruins well: every wall took contact shadows).
// Each frame, before the G-buffer, the game renders one face of a cube around the camera into a 64x64 depth and reduces
// it into one layer of a 16x16 R16F cube (is_enclosure): per texel clamp((distance - 5 m) / 5 m), 0 where geometry is
// within 5 m. The march samples the upward face (layer 2) and leaves pixels alone while rock covers most of it.
// TOTK_CONTACT_DEBUG=3 shows that cover in red (magenta: no cube in the frame).
#include "vk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "contact_shadow_spv.h"
#include "host.h"
#include "nvn_api.h"
#include "vk_exec.h"

static struct {
    int read, tried, ok, strength, debug, logged, logged_up;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkSampler sampler;
    VkExecTarget scene, depth;         // the last G-buffer's scene colour and linear depth
    VkExecTarget sun;                  // this frame's half-resolution lighting target (is_lighting); no img: none seen
    VkExecTarget encl;                 // the enclosure cube (is_enclosure), last seen at frame encl_frame; no img: none
    uint64_t encl_frame;
    int pending;                       // the composite into scene ran; the march waits for the next pass
    uint64_t c4;
} C;

int vk_contact_strength(void) {
    if (!C.read) {   // once: the scan asks at every draw
        int s = cfg_int("TOTK_CONTACT_SHADOWS");
        C.strength = s < 0 ? 0 : s > 100 ? 100 : s;
        C.read = 1;
    }
    return C.strength;
}

static int init(VkCtx *c) {
    C.tried = 1;
    vk_contact_strength();
    if (!C.strength) return 0;
    const char *e = cfg_get("TOTK_CONTACT_DEBUG");   // 1: the contact shadow factor in grey, 2: the sun visibility,
                                                      // 3: the overhead cover
    C.debug = e ? atoi(e) : 0;
    C.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[4] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 4, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &C.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = sizeof contact_shadow_spv, .pCode = contact_shadow_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (!C.push || vkCreateDescriptorSetLayout(c->device, &dci, NULL, &C.dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &li, NULL, &C.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &C.sampler) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) {
        rt_log("vk: contact shadows off: their pipeline could not be created");
        return 0;
    }
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = C.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &C.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) {
        rt_log("vk: contact shadows off: their pipeline could not be created");
        return 0;
    }
    rt_log("vk: contact shadows on, strength %d%%%s", C.strength,
           C.debug == 3 ? " (debug view: overhead cover)" : C.debug == 2 ? " (debug view: sun visibility)" : C.debug ? " (debug view)" : "");
    return C.ok = 1;
}

// A G-buffer pass: a B10G11R11 scene colour, an R32F linear depth and a D32F depth, all one size, at least 640 wide.
static int is_gbuffer(const VkExec *ex, const VkExecTarget **scene, const VkExecTarget **depth) {
    *scene = *depth = NULL;
    if (ex->pass_noatt || ex->pass_w < 640 || ex->depth.format != VK_FORMAT_D32_SFLOAT || ex->depth.level) return 0;
    for (uint32_t i = 0; i < ex->pass_ncolor && i < 8; i++) {
        const VkExecTarget *t = &ex->color[i];
        if (!t->image || t->level || t->width != ex->pass_w || t->height != ex->pass_h) continue;
        if (t->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && !*scene) *scene = t;
        if (t->format == VK_FORMAT_R32_SFLOAT && !*depth) *depth = t;
    }
    return *scene && *depth && (*scene)->img && ((*scene)->img->usage & VK_IMAGE_USAGE_STORAGE_BIT);
}

static void mem_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &d);
}

// The half-resolution lighting pass after the G-buffer: eight targets at half the scene's size, the first RGBA8.
static int is_lighting(const VkExec *ex) {
    const VkExecTarget *t = &ex->color[0];
    return C.scene.image && ex->pass_ncolor == 8 && t->image && t->img && !t->level &&
           t->format == VK_FORMAT_R8G8B8A8_UNORM && ex->pass_w * 2 == C.scene.width && ex->pass_h * 2 == C.scene.height;
}

// The game's enclosure cube: one 16x16 R16F target, a layer of a six-layer image, the pass's only target.
static int is_enclosure(const VkExec *ex) {
    const VkExecTarget *t = &ex->color[0];
    return ex->pass_ncolor == 1 && t->image && t->img && !t->level && t->format == VK_FORMAT_R16_SFLOAT &&
           t->width == 16 && t->height == 16 && t->img->layers == 6 && t->base_layer < 6;
}

// The lighting composite: the remembered scene colour as the only target, with a D16 depth of its size.
static int is_composite(const VkExec *ex) {
    return C.scene.image && ex->pass_ncolor >= 1 && ex->color[0].image == C.scene.image && !ex->color[0].level &&
           ex->depth.format == VK_FORMAT_D16_UNORM && ex->pass_w == C.scene.width && ex->pass_h == C.scene.height;
}

int vk_contact_plan(const VkExec *ex, int pass, VkExecJob *j) {
    if (!vk_contact_strength()) return 0;
    if (pass) {
        const VkExecTarget *scene, *depth;
        // The camera block: c4 of the fragment stage, 0x940 bytes. The composite binds it only after its pass begins, so
        // the newest one seen at any pass start serves.
        const p_bind_buf *b = &ex->ubo[1][1];
        if (b->addr && b->size == 0x940) C.c4 = b->addr;
        if (is_gbuffer(ex, &scene, &depth)) {
            C.pending = 0;
            C.scene = *scene, C.depth = *depth, C.sun = (VkExecTarget){0};
            return 0;
        }
        if (is_enclosure(ex)) {
            C.encl = ex->color[0];   // a frame may hold more than one G-buffer pass: kept by frame, not reset there
            C.encl_frame = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
            return 0;
        }
        if (is_lighting(ex)) {
            C.sun = ex->color[0];
            return 0;
        }
        if (is_composite(ex)) {
            C.pending = C.c4 != 0;
            return 0;
        }
    }
    if (!C.pending) return 0;
    C.pending = 0;
    const float *u = (const float *)(uintptr_t)C.c4;
    float lx = -u[44 * 4 + 0], ly = -u[44 * 4 + 1], lz = -u[44 * 4 + 2], ll = sqrtf(lx * lx + ly * ly + lz * lz);
    float px = u[7 * 4 + 0], py = u[8 * 4 + 1], zfar = u[14 * 4 + 1];
    if (!(ll > 1e-6f) || !(px > 0) || !(py > 0) || !(zfar > 1)) return 0;
    float w = (float)C.scene.width, h = (float)C.scene.height;
    uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    *j = (VkExecJob){.scene = C.scene, .depth = C.depth, .sun = C.sun,
                     // The game renders one face per frame but skips some frames (about one in nine in the
                     // Rauru well): a cube from up to 30 frames back serves, or the march flickered on and off.
                     .encl = fr - C.encl_frame <= 30 ? C.encl : (VkExecTarget){0},
                     .pc = {px, py, zfar, (float)C.strength / 100.0f,
                            lx / ll, ly / ll, lz / ll, 0.6f,
                            w, h, 1.0f / w, 1.0f / h,
                            0, 0, 0, 0}};
    return 1;
}

void vk_contact_exec(VkExec *ex, const VkExecJob *j) {
    if (!C.tried) init(vk_ctx());
    if (!C.ok) return;
    VkImageView in = vk_exec_target_view(j->depth.img, VK_FORMAT_R32_SFLOAT, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
                                         0, 0, 1, 0);
    VkImageView out = vk_exec_target_view(j->scene.img, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_IMAGE_VIEW_TYPE_2D,
                                          VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0);
    if (!in || !out) return;
    VkImageView sun = j->sun.img ? vk_exec_target_view(j->sun.img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D,
                                                       VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0)
                                 : VK_NULL_HANDLE;
    VkImageView up = j->encl.img ? vk_exec_target_view(j->encl.img, VK_FORMAT_R16_SFLOAT, VK_IMAGE_VIEW_TYPE_2D,
                                                       VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 1, 0)   // layer 2: +Y, up
                                 : VK_NULL_HANDLE;
    // A full barrier that also takes the images in the feedback layout back to GENERAL (vk_exec.c barrier_to): the
    // G-buffer's linear depth and scene colour are such images once a draw sampled them in their own pass, and the march
    // binds them in GENERAL. A memory barrier alone left them in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT.
    vk_exec_barrier(ex);
    VkCommandBuffer cb = ex->cb;
    float pcv[16];
    memcpy(pcv, j->pc, sizeof pcv);
    pcv[12] = (float)C.debug;
    pcv[13] = sun ? 1.0f : 0.0f;
    pcv[14] = up ? 1.0f : 0.0f;
    if (!C.logged) {
        C.logged = 1;
        rt_log("vk: contact shadows on a %ux%u scene, sun toward %.3f %.3f %.3f (view space), far %g; %s", j->scene.width,
               j->scene.height, pcv[4], pcv[5], pcv[6], pcv[2],
               sun ? "sunlit pixels only" : "no sun visibility found: every pixel");
    if (up && !C.logged_up) {
        C.logged_up = 1;
        rt_log("vk: contact shadows: the game's enclosure cube found; none under rock overhead");
    }
    }
    VkDescriptorImageInfo ii = {C.sampler, in, VK_IMAGE_LAYOUT_GENERAL}, oi = {VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL},
                          si = {C.sampler, sun ? sun : in, VK_IMAGE_LAYOUT_GENERAL},   // not read without a sun view
                          ui = {C.sampler, up ? up : in, VK_IMAGE_LAYOUT_GENERAL};     // not read without an up view
    VkWriteDescriptorSet w[4] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &si},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ui}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, C.pipe);
    rec_push(C.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, C.layout, 0, 4, w);
    vkCmdPushConstants(cb, C.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pcv, pcv);
    vkCmdDispatch(cb, (j->scene.width + 7) / 8, (j->scene.height + 7) / 8, 1);
    vk_texture_storage_written(j->scene.img);   // the march wrote the scene colour: a fence write-back snapshot made before is stale
    mem_barrier(cb);   // and what follows sees the darkened colour
    ex->recorded += 6;
    vk_exec_draw_new_cb(ex);   // the game's bound compute pipeline and descriptors were replaced: bind them again
}

// The sequential replay's hooks; a chunk leaves the marches to the scan.
void vk_contact_pass_begin(VkExec *ex) {
    VkExecJob j;
    if (!ex->chunked && vk_contact_plan(ex, 1, &j)) vk_contact_exec(ex, &j);
}

void vk_contact_flush(VkExec *ex) {
    VkExecJob j;
    if (!ex->chunked && C.pending && vk_contact_plan(ex, 0, &j)) vk_contact_exec(ex, &j);
}

void vk_contact_release(void) {
    VkCtx *c = vk_ctx();
    if (!c || !C.tried) return;
    if (C.pipe) vkDestroyPipeline(c->device, C.pipe, NULL);
    if (C.sampler) vkDestroySampler(c->device, C.sampler, NULL);
    if (C.layout) vkDestroyPipelineLayout(c->device, C.layout, NULL);
    if (C.dsl) vkDestroyDescriptorSetLayout(c->device, C.dsl, NULL);
    memset(&C, 0, sizeof C);
}
