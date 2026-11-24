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
#include "vk.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "contact_shadow_spv.h"
#include "host.h"
#include "vk_exec.h"

static struct {
    int read, tried, ok, strength, debug, logged;
    PFN_vkCmdPushDescriptorSetKHR push;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkSampler sampler;
    VkExecTarget scene, depth;         // the last G-buffer's scene colour and linear depth
    VkExecTarget sun;                  // this frame's half-resolution lighting target (is_lighting); no img: none seen
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
    const char *e = cfg_get("TOTK_CONTACT_DEBUG");   // 1: the contact shadow factor in grey, 2: the sun visibility
    C.debug = e ? atoi(e) : 0;
    C.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    VkDescriptorSetLayoutBinding b[3] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 3, .pBindings = b};
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
           C.debug == 2 ? " (debug view: sun visibility)" : C.debug ? " (debug view)" : "");
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
    *j = (VkExecJob){.scene = C.scene, .depth = C.depth, .sun = C.sun,
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
    // A full barrier that also takes the images in the feedback layout back to GENERAL (vk_exec.c barrier_to): the
    // G-buffer's linear depth and scene colour are such images once a draw sampled them in their own pass, and the march
    // binds them in GENERAL. A memory barrier alone left them in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT.
    vk_exec_barrier(ex);
    VkCommandBuffer cb = ex->cb;
    float pcv[16];
    memcpy(pcv, j->pc, sizeof pcv);
    pcv[12] = (float)C.debug;
    pcv[13] = sun ? 1.0f : 0.0f;
    if (!C.logged) {
        C.logged = 1;
        rt_log("vk: contact shadows on a %ux%u scene, sun toward %.3f %.3f %.3f (view space), far %g; %s", j->scene.width,
               j->scene.height, pcv[4], pcv[5], pcv[6], pcv[2],
               sun ? "sunlit pixels only" : "no sun visibility found: every pixel");
    }
    VkDescriptorImageInfo ii = {C.sampler, in, VK_IMAGE_LAYOUT_GENERAL}, oi = {VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL},
                          si = {C.sampler, sun ? sun : in, VK_IMAGE_LAYOUT_GENERAL};   // not read without a sun view
    VkWriteDescriptorSet w[3] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &si}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, C.pipe);
    rec_push(C.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, C.layout, 0, 3, w);
    vkCmdPushConstants(cb, C.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pcv, pcv);
    vkCmdDispatch(cb, (j->scene.width + 7) / 8, (j->scene.height + 7) / 8, 1);
    vk_texture_storage_written(j->scene.img);   // the march wrote the scene colour: a fence write-back snapshot made before is stale
    mem_barrier(cb);   // and what follows sees the darkened colour
    ex->recorded += 6;
    vk_exec_draw_new_cb(ex);   // the game's bound compute pipeline and descriptors were replaced: bind them again
}

// The sequential replay's hooks; a chunk leaves the marches to the scan.
// ---- aim tracker: where the game's aim ray hits the scene, for the 3D reticle.
// The ray runs from the game camera along its forward axis (the game aims through its view's centre; the head is not
// in the game camera). When the G-buffer pass ends, AIM_N points along the ray, projected into the frame's head
// camera, are copied from the G-buffer's linear depth into a host buffer; AIM_RING frames later the first point that lies
// behind the scene there gives the hit distance (vk_xr.c places the reticle layer at it).
#define AIM_N 64
#define AIM_RING 4
int vk_exec_aim_cams(float G[12], float HL[12]);
float vr_far_plane(void);
void vk_xr_game_tan(float *th, float *tv);
void vk_xr_set_reticle_dist(float d);
int vr_orbit_target(float t[3]);   // vr_camera.c
int vr_player_pos(float xyz[3]);   // frame_rate.c: Link's position (his feet)
int vr_aim_ray(float o[3], float f[3]);   // vr_camera.c: the game's aim ray while head aim holds the camera
// Link as a vertical capsule around his position (body, gear, the drawn bow): the aim never stops on him (aiming
// past Link would put the bow / throw reticle on his back). March points inside it are skipped, and a sample
// where the eye sees Link himself says nothing about the ray (it is hidden behind him there).
#define LINK_R 0.6f
#define LINK_Y0 (-0.2f)
#define LINK_Y1 2.1f
static int in_link(const float lp[3], const float P[3]) {
    float dx = P[0] - lp[0], dz = P[2] - lp[2], dy = P[1] - lp[1];
    return dx * dx + dz * dz < LINK_R * LINK_R && dy > LINK_Y0 && dy < LINK_Y1;
}
static struct {
    VkExecTarget depth;
    int pending, ok, slot;
    VkBuffer buf[AIM_RING];
    VmaAllocation al[AIM_RING];
    const float *map[AIM_RING];
    float t[AIM_RING][AIM_N], z[AIM_RING][AIM_N];   // ray distance and the point's depth in front of the eye
    float P[AIM_RING][AIM_N][3], eye[AIM_RING][3], link[AIM_RING][3];   // the points, the eye, Link (when have_link)
    int have_link[AIM_RING];
    int n[AIM_RING], used[AIM_RING];
    float far[AIM_RING];
    unsigned long hits, misses;
} AIM;
// A coarse grid of the same linear depth (DG_N x DG_N points over the frame's view), copied with the aim points: the
// world markers' depths (vk_xr.c places each marker at the depth of what it floats over). Normalised view coordinates.
#define DG_N 48
static struct { VkBuffer buf[AIM_RING]; VmaAllocation al[AIM_RING]; const float *map[AIM_RING]; int used[AIM_RING]; float far[AIM_RING];
                float H[AIM_RING][12], th[AIM_RING], tv[AIM_RING], lp[AIM_RING][3]; int hl[AIM_RING]; } DG;
static float DG_m[DG_N * DG_N], DG_a[DG_N * DG_N];   // metres (0: no data / sky); DG_a without Link
static int DG_ok;
// The nearest depth (m) over the grid cells inside [u0,u1] x [v0,v1]; 0 when the grid has none there.
static float dg_min(const float *grid, float u0, float v0, float u1, float v1);
float vk_contact_depth_min(float u0, float v0, float u1, float v1) { return dg_min(DG_m, u0, v0, u1, v1); }
// Same without Link (the bow / Ultrahand reticle's depth).
float vk_contact_depth_min_aim(float u0, float v0, float u1, float v1) { return dg_min(DG_a, u0, v0, u1, v1); }
static float dg_min(const float *grid, float u0, float v0, float u1, float v1) {
    if (!__atomic_load_n(&DG_ok, __ATOMIC_ACQUIRE)) return 0;
    int x0 = (int)(u0 * DG_N), x1 = (int)(u1 * DG_N), y0 = (int)(v0 * DG_N), y1 = (int)(v1 * DG_N);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > DG_N - 1) x1 = DG_N - 1;
    if (y1 > DG_N - 1) y1 = DG_N - 1;
    float m = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float d = grid[y * DG_N + x];
            if (d > 0.05f && (m == 0 || d < m)) m = d;
        }
    return m;
}
static void dg_read(int s) {
    if (!DG.used[s] || !DG.map[s]) return;
    DG.used[s] = 0;
    for (int i = 0; i < DG_N * DG_N; i++) {
        float v = DG.map[s][i];
        DG_m[i] = v > 0 && v < 0.999f ? v * DG.far[s] : 0;
        DG_a[i] = DG_m[i];
        if (DG.hl[s] && DG_m[i] > 0.05f) {   // the surface point of this cell inside Link: not a target for the reticle
            int x = i % DG_N, y = i / DG_N;
            float z = DG_m[i], u = (x + 0.5f) / DG_N, v2 = (y + 0.5f) / DG_N, vx = (2 * u - 1) * DG.th[s] * z, vy = -(2 * v2 - 1) * DG.tv[s] * z;
            const float *H = DG.H[s];
            float S[3];
            for (int k = 0; k < 3; k++) S[k] = -(H[k] * H[3] + H[4 + k] * H[7] + H[8 + k] * H[11]) + H[k] * vx + H[4 + k] * vy - H[8 + k] * z;
            if (in_link(DG.lp[s], S)) DG_a[i] = 0;
        }
    }
    __atomic_store_n(&DG_ok, 1, __ATOMIC_RELEASE);
}
static void dg_sample(VkExec *ex, int s, const float H[12], float th, float tv, const float lp[3], int hl) {
    dg_read(s);
    if (!DG.buf[s]) {
        VkCtx *c = vk_ctx();
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = DG_N * DG_N * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST, .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                      .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(c->vma, &bi, &ai, &DG.buf[s], &DG.al[s], &info) != VK_SUCCESS) { DG.buf[s] = VK_NULL_HANDLE; return; }
        DG.map[s] = info.pMappedData;
    }
    static VkBufferImageCopy rc[DG_N * DG_N];
    uint32_t W = AIM.depth.width, Hh = AIM.depth.height;
    for (int y = 0; y < DG_N; y++)
        for (int x = 0; x < DG_N; x++)
            rc[y * DG_N + x] = (VkBufferImageCopy){.bufferOffset = (VkDeviceSize)(y * DG_N + x) * 4, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                                   .imageOffset = {(int32_t)((x + 0.5f) * W / DG_N), (int32_t)((y + 0.5f) * Hh / DG_N), 0}, .imageExtent = {1, 1, 1}};
    vk_exec_barrier(ex);
    vkCmdCopyImageToBuffer(ex->cb, AIM.depth.image, VK_IMAGE_LAYOUT_GENERAL, DG.buf[s], DG_N * DG_N, rc);
    vk_exec_barrier(ex);
    ex->recorded += 3;
    DG.used[s] = 1, DG.far[s] = vr_far_plane();
    memcpy(DG.H[s], H, 48), DG.th[s] = th, DG.tv[s] = tv, DG.hl[s] = hl;
    if (hl) memcpy(DG.lp[s], lp, 12);
}
static float aim_t(int k) { return 0.4f * powf(1.12f, (float)k); }   // 0.4 m .. ~570 m
static void aim_read(int s) {
    if (!AIM.used[s] || !AIM.map[s]) return;
    AIM.used[s] = 0;
    // A hit is where the ray crosses a surface: one sample in front of what is seen there, the next behind it (the ray
    // passes right behind Link as the VR camera sees him, so its first samples lie behind his back). Samples that start
    // out behind a surface (hidden by Link, a ledge) are skipped until the ray comes out in front of the scene again.
    float prev_t = 0, prev_gap = -1;
    int seen_front = 0;
    for (int i = 0; i < AIM.n[s]; i++) {
        float scene = AIM.map[s][i] * AIM.far[s];
        float gap = AIM.z[s][i] - scene;   // > 0: the ray point is behind the surface seen there
        float tol = 0.02f * AIM.z[s][i] + 0.05f;
        if (!(scene > 0.05f)) continue;
        if (AIM.have_link[s]) {   // the surface seen here: along the eye's ray through the point, at the scene's depth
            float k = scene / AIM.z[s][i], S[3];
            for (int c = 0; c < 3; c++) S[c] = AIM.eye[s][c] + (AIM.P[s][i][c] - AIM.eye[s][c]) * k;
            if (in_link(AIM.link[s], S)) continue;   // Link covers the ray here: unknown, keep the last sample's state
        }
        if (gap <= tol) {   // in front of (or on) the surface seen here
            if (gap > -tol && seen_front) {   // on it
                vk_xr_set_reticle_dist(AIM.t[s][i]);
                AIM.hits++;
                return;
            }
            seen_front = 1, prev_t = AIM.t[s][i], prev_gap = gap;
            continue;
        }
        if (seen_front && prev_gap <= tol) {   // crossed it between the last sample and this one
            float th = AIM.t[s][i];
            if (prev_gap < 0) th = prev_t + (AIM.t[s][i] - prev_t) * (-prev_gap) / (gap - prev_gap);
            vk_xr_set_reticle_dist(th);
            AIM.hits++;
            return;
        }
        prev_gap = gap;   // behind something nearer: hidden, go on
    }
    vk_xr_set_reticle_dist(150);   // nothing hit (sky): far away
    AIM.misses++;
}
static void aim_sample(VkExec *ex) {
    float G[12], H[12], th, tv;
    if (!vk_exec_aim_cams(G, H) || !AIM.depth.image) return;
    vk_xr_game_tan(&th, &tv);
    if (!(th > 0.1f) || !(tv > 0.1f)) return;
    VkCtx *c = vk_ctx();
    int s = AIM.slot;
    AIM.slot = (AIM.slot + 1) % AIM_RING;
    aim_read(s);   // this slot's copy is AIM_RING frames old: done
    if (!AIM.buf[s]) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = AIM_N * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST, .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                      .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(c->vma, &bi, &ai, &AIM.buf[s], &AIM.al[s], &info) != VK_SUCCESS) { AIM.buf[s] = VK_NULL_HANDLE; return; }
        AIM.map[s] = info.pMappedData;
    }
    float cam[3], fwd[3] = {-G[8], -G[9], -G[10]};
    for (int k = 0; k < 3; k++) cam[k] = -(G[k] * G[3] + G[4 + k] * G[7] + G[8 + k] * G[11]);
    float ro[3], rf[3];
    if (vr_aim_ray(ro, rf)) memcpy(cam, ro, 12), memcpy(fwd, rf, 12);   // head aim: the ray the game aims along, not the un-turned camera's
    // the ray from Link on: seen from the VR camera, ~5 m behind Link, Link stands in front of the ray's
    // first metres (the game camera is ~1 m behind him while aiming), so every frame "hit" Link (6.8 m, a sample point).
    // The arrow flies from Link to where this ray meets the scene: the march starts 0.6 m past the look-at point.
    float tgt[3], t_min = 0;
    if (vr_orbit_target(tgt)) {
        float a = (tgt[0] - cam[0]) * fwd[0] + (tgt[1] - cam[1]) * fwd[1] + (tgt[2] - cam[2]) * fwd[2];
        if (a > 0 && a < 8) t_min = a + 0.6f;
    }
    float lp[3];
    int hl = vr_player_pos(lp);
    dg_sample(ex, s, H, th, tv, lp, hl);
    AIM.have_link[s] = hl;
    if (hl) memcpy(AIM.link[s], lp, 12);
    for (int k = 0; k < 3; k++) AIM.eye[s][k] = -(H[k] * H[3] + H[4 + k] * H[7] + H[8 + k] * H[11]);
    VkBufferImageCopy rc[AIM_N];
    int n = 0;
    float W = (float)AIM.depth.width, Hh = (float)AIM.depth.height;
    for (int k = 0; k < AIM_N; k++) {
        float t = aim_t(k), P[3] = {cam[0] + fwd[0] * t, cam[1] + fwd[1] * t, cam[2] + fwd[2] * t};
        if (t < t_min) continue;
        if (hl && in_link(lp, P)) continue;
        float vx = H[0] * P[0] + H[1] * P[1] + H[2] * P[2] + H[3];
        float vy = H[4] * P[0] + H[5] * P[1] + H[6] * P[2] + H[7];
        float vz = H[8] * P[0] + H[9] * P[1] + H[10] * P[2] + H[11];
        float z = -vz;
        if (!(z > 0.05f)) continue;
        float u = (vx / z / th + 1) * 0.5f, v = (-vy / z / tv + 1) * 0.5f;
        if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;
        rc[n] = (VkBufferImageCopy){.bufferOffset = (VkDeviceSize)n * 4, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                     .imageOffset = {(int32_t)(u * W), (int32_t)(v * Hh), 0}, .imageExtent = {1, 1, 1}};
        AIM.t[s][n] = t, AIM.z[s][n] = z, memcpy(AIM.P[s][n], P, 12), n++;
    }
    if (!n) return;
    vk_exec_barrier(ex);
    vkCmdCopyImageToBuffer(ex->cb, AIM.depth.image, VK_IMAGE_LAYOUT_GENERAL, AIM.buf[s], (uint32_t)n, rc);
    vk_exec_barrier(ex);
    ex->recorded += 3;
    AIM.n[s] = n, AIM.used[s] = 1, AIM.far[s] = vr_far_plane();
    static unsigned long nlog;
    if (++nlog % 3600 == 1) rt_log("vr: aim tracker: %lu hits, %lu misses (G-buffer linear depth %ux%u, far %.0f)", AIM.hits, AIM.misses, AIM.depth.width, AIM.depth.height, AIM.far[s]);
}
static int aim_gbuffer(const VkExec *ex, const VkExecTarget **depth) {   // is_gbuffer without its storage condition
    const VkExecTarget *scene = NULL;
    *depth = NULL;
    if (ex->pass_noatt || ex->pass_w < 640 || ex->depth.format != VK_FORMAT_D32_SFLOAT || ex->depth.level) return 0;
    for (uint32_t i = 0; i < ex->pass_ncolor && i < 8; i++) {
        const VkExecTarget *t = &ex->color[i];
        if (!t->image || t->level || t->width != ex->pass_w || t->height != ex->pass_h) continue;
        if (t->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && !scene) scene = t;
        if (t->format == VK_FORMAT_R32_SFLOAT && !*depth) *depth = t;
    }
    return scene && *depth;
}
static void aim_pass(VkExec *ex) {
    if (vk_exec_walk2() || vk_exec_hud_replay()) return;   // the frame's own passes only, not the HUD replay
    const VkExecTarget *depth;
    int gb = aim_gbuffer(ex, &depth);
    if (AIM.pending && !gb) AIM.pending = 0, aim_sample(ex);   // the G-buffer pass has ended
    if (gb) AIM.depth = *depth, AIM.pending = 1;
}

void vk_contact_pass_begin(VkExec *ex) {
    aim_pass(ex);
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
