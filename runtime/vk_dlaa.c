// Scene reconstruction through RSF, with camera/object motion and pre-HUD evaluation.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cfg.h"
#include "rsf_camera.h"
#include "rsf_runtime.h"
#include "rsf_sr.h"
#include <rescaleframe/native_fg_vulkan.h>
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_exec.h"
#include "vk_rec.h"
#include "dlaa_mv_spv.h"

// Reconstruction and FG independently request temporal inputs. RSF owns the
// providers; this renderer supplies scene resources and camera/motion facts.
static struct { int available; } N;
int vk_dlaa_wanted(void) { return cfg_bool("TOTK_RSF_ENABLED"); }
static int temporal_wanted(void) { return vk_dlaa_wanted() || cfg_int("TOTK_RSF_FG") != 0; }
void vk_dlaa_instance_exts(const char **ext, uint32_t *n, uint32_t max) {
    (void)ext; (void)n; (void)max;
}
void vk_dlaa_device_exts(const VkExtensionProperties *have, uint32_t nhave,
                         const char **ext, uint32_t *n, uint32_t max) {
    (void)have; (void)nhave; (void)ext; (void)n; (void)max;
}
void vk_dlaa_init(void) { N.available = temporal_wanted() && rsf_runtime_load(); }
int vk_dlaa_available(void) { return vk_dlaa_wanted() && N.available; }
int vk_dlaa_temporal_available(void) { return temporal_wanted() && N.available; }

// ---------------------------------------------------------------- where DLAA runs in TotK's frame
// Measured (TOTK_DUMP, frame 2100 at Lookout Landing, 2560x1440 scene): the G-buffer passes draw into five targets with
// the scene depth (d32f at the scene size), one of them the R11G11B10F light target; lighting and the transparents
// then draw into that light target alone, the last time with the scene depth attached; the first pass narrower than a
// quarter of the scene starts the bloom chain, which reads it. After the bloom come the tonemap and post passes, then
// the HUD is drawn into the same target and a last pass copies it to the 8-bit output.
// So: after the G-buffer, a pass at the scene size with exactly one colour target (R11G11B10F or RGBA16F) and the D32F
// depth arms the frame with that pair; the next pass at most a quarter as wide ends the scene (no jitter after it) and,
// when the scene is as wide as the output, runs DLAA on the armed target first. Bloom, tonemap, post and HUD then all
// work on the anti-aliased picture.
// Upscaling (a scene smaller than the output: TOTK_DLSS quality/balanced/performance, or the game's own 5/6): measured
// at a 960x536 scene, 1920x1080 output, the bloom and its composite write back into the light target at the scene size,
// then one full-screen draw (the FXAA shader below, which also scales) reads it into an output-sized R11G11B10F target
// with no depth, and the HUD is drawn over that target at the output size. DLSS runs at the begin of that pass: it
// reads the light target and its result is blitted into the pass's target, whose scaling draw is then skipped. The
// HUD, drawn afterwards into the same target, is never jittered nor passed through DLSS.
// The scaling draw is not always that FXAA shader: at a 1280x720 scene and 1920x1080 output (1.4.3, dlss = quality)
// the game's tonemap (guest fs 7e114c4330dc212a) reads the light target and writes the output-sized target itself,
// over DLSS's picture, so the jittered scene reached the screen and the whole picture shook. So from DLSS's evaluation
// to the frame's end, draws in passes larger than the light target sample DLSS's output in its place
// (vk_dlaa_swap_img, applied by vk_exec_draw.c tex_info): whichever shader scales, it scales DLSS's picture.
static struct {
    VkExecTarget color, depth;   // the armed pair, valid in the frame it was armed
    int armed, done, gbuf;       // this frame: armed on the light target, DLAA ran, the G-buffer was drawn
    uint32_t gbuf_w;             // the widest G-buffer's width this frame
    VkImage nodepth;             // this frame: a G-buffer-size target drawn without depth while nothing was armed
    int ended, up;               // this frame: the scene is drawn (jitter over), DLSS wrote the scaled output
    uint64_t end_fence;          // a chunked replay: the submission and op the scan ended the scene at (ended_at)
    uint32_t end_pos;
    struct Size {                // a DLSS feature and its work images per scene size (dynamic resolution flips it)
        uint32_t w, h, ow, oh;
        unsigned fw, fh, gw, gh;   // render size, output size
        VkImage out, mv;
        VmaAllocation out_mem, mv_mem;
        VkImageView out_view, mv_view;
        VkImage device_depth;   // R32F device depth produced with camera motion for RSF
        VmaAllocation device_depth_mem;
        VkImageView device_depth_view;
        VkImage mask, chars[2];  // the bias mask DLSS reads, the character class mask of this and last frame (R8)
        VmaAllocation mask_mem, chars_mem[2];
        VkImageView mask_view, chars_view[2];
        int chars_cur, chars_ok; // chars[chars_cur] is written next; chars_ok: the other holds last frame's
        RsfSrSession *rsf;
        uint32_t rsf_w, rsf_h;
        uint32_t resource_generation; // identity of this allocated work-image set
        VkImage sr_color;
        VmaAllocation sr_color_mem;
        VkImageView sr_color_view;
        VkImage fg_srgb, fg_color; // encoded post-processing result, then byte-preserving UNORM copy
        VmaAllocation fg_srgb_mem, fg_color_mem;
        VkImageView fg_srgb_view, fg_color_view;
        rsf_frame_record fg_record;
        uint64_t fg_copied;
        uint64_t used;           // evals count when it last ran
    } sizes[5], *last;           // last: the one the previous DLAA frame ran with
    VkImage last_color;          // the target DLAA ran on last frame: another one means a new history
    int failed, reset;
    uint64_t evals, frames_missed, guide_frames;
    // jitter (vk_dlaa_viewport)
    VkImage jdepth;              // the scene depth of the last frame DLAA ran on: passes drawing with it are jittered
    float jx, jy;                // the offset the next scene draws take, render pixels (window x right, y down)
    float ux, uy;                // the offset this frame's scene draws took (DLSS and the motion vectors get these)
    uint32_t jframe;
    int jit_ok, jit_used, hold;  // jitter allowed this frame / a draw used it / frames left without it
    uint64_t holds;
    // camera (vk_dlaa_draw) and motion vectors (mv_pass)
    uint64_t cam_addr;           // this frame's scene camera block (c4 of a scene draw's vertex stage), 0: none seen
    float camera_depth_range[2]; // viewport mapping observed with that camera, rather than the later bloom pass
    uint32_t camera_depth_compare;
    VkBuffer camb;               // two CAM_SLOT slots: this frame's block and last frame's
    VmaAllocation camb_mem;
    VkBuffer mvm;                // MVM_SLOTS pairs of blocks for dlaa_mv.comp: the identity and the reprojection (mv_pass)
    VmaAllocation mvm_mem;
    uint8_t *mvm_map;
    uint32_t mvm_slot;
    double cam_prev[16];         // last frame's view-projection (rows 3-6 of its block), for the reprojection
    RsfCameraProjection projection;
    int projection_ok;
    RsfCameraMatrices camera_matrices;
    int camera_matrices_ok;
    uint64_t invalid_camera;
    int cam_slot, cam_prev_ok;   // the slot this frame writes / the other slot holds last frame's camera
    VkDescriptorSetLayout mv_dsl;
    VkPipelineLayout mv_layout;
    VkPipeline mv_pipe;
    VkSampler mv_samp;
    int mv_ok;                   // -1 not built, 0 failed, 1 ready
    uint64_t mv_frames, mv_none, masked;   // masked: frames with the characters' bias mask
    uint64_t why[3];             // history resets: scene size changed, no camera, scene target changed
    int on;                      // the dlaa setting, read at each present (the menu switches it live)
    int fg_on;                   // cached once per present; the draw hook runs on every native draw
    int preset, preset_dirty;    // the DLSS model features are created with (preset_value); a change pending
    VkExecTarget ids;            // this frame's G-buffer target 0 (object classes in green), valid with ids_ok
    int ids_ok;
    unsigned cfg_seen;           // cfg_version when preset was read
    uint64_t last_eval_ns;       // plat_now_ns of the previous evaluation (FrameTimeDeltaInMsec)
} D = {.mv_ok = -1, .cfg_seen = ~0u};
#define DLSS_SIZES (sizeof D.sizes / sizeof D.sizes[0])

const VkTexImage *vk_dlaa_swap_img;
VkImageView vk_dlaa_swap_view;

static int dl_active(void) { return N.available && !D.failed && D.on; }

static void dl_fail(const char *what, int32_t r) {
    rt_log("dlaa: %s failed (%08x): DLAA off for this run", what, (unsigned)r);
    D.failed = 1;
}

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

static int free_size(struct Size *e) {
    const VkCtx *c = vk_ctx();
    if (!vk_present_fg_drain()) return 0;
    vkDeviceWaitIdle(c->device);   // a third scene size: the evicted images may still be in flight
    if (!rsf_sr_close(&e->rsf)) return 0;
    if (e->sr_color_view) vkDestroyImageView(c->device, e->sr_color_view, NULL);
    if (e->sr_color) vmaDestroyImage(c->vma, e->sr_color, e->sr_color_mem);
    if (e->fg_srgb_view) vkDestroyImageView(c->device, e->fg_srgb_view, NULL);
    if (e->fg_color_view) vkDestroyImageView(c->device, e->fg_color_view, NULL);
    if (e->fg_srgb) vmaDestroyImage(c->vma, e->fg_srgb, e->fg_srgb_mem);
    if (e->fg_color) vmaDestroyImage(c->vma, e->fg_color, e->fg_color_mem);
    if (e->out_view) vkDestroyImageView(c->device, e->out_view, NULL);
    if (e->mv_view) vkDestroyImageView(c->device, e->mv_view, NULL);
    if (e->device_depth_view) vkDestroyImageView(c->device, e->device_depth_view, NULL);
    if (e->device_depth) vmaDestroyImage(c->vma, e->device_depth, e->device_depth_mem);
    if (e->out) vmaDestroyImage(c->vma, e->out, e->out_mem);
    if (e->mv) vmaDestroyImage(c->vma, e->mv, e->mv_mem);
    if (e->mask_view) vkDestroyImageView(c->device, e->mask_view, NULL);
    if (e->mask) vmaDestroyImage(c->vma, e->mask, e->mask_mem);
    for (int i = 0; i < 2; i++) {
        if (e->chars_view[i]) vkDestroyImageView(c->device, e->chars_view[i], NULL);
        if (e->chars[i]) vmaDestroyImage(c->vma, e->chars[i], e->chars_mem[i]);
    }
    memset(e, 0, sizeof *e);
    return 1;
}

int vk_dlaa_release(void) {
    rec_drain();
    for (int i = 0; i < 5; i++) {
        if ((D.sizes[i].out || D.sizes[i].rsf) && !free_size(&D.sizes[i])) return 0;
    }
    D.last = NULL;
    return 1;
}

// The entry for a scene and output size: its output (RGBA16F at the output size, written by DLSS, then blitted into
// the target) and motion vectors (RG16F at the scene size, render pixels), both kept in GENERAL like every image of the
// back end. TotK's dynamic resolution switches between a few sizes; each keeps its feature, the least recently used
// goes for a new one.
static uint32_t next_resource_generation(void) {
    static uint32_t generation;
    uint32_t next = __atomic_add_fetch(&generation, 1, __ATOMIC_RELAXED);
    if (!next) rt_fatal("rsf: scene resource generation exhausted");
    return next;
}

static struct Size *size_for(VkCommandBuffer cb, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) {
    unsigned fg[2], game[2], scene[2];
    res_sizes(game, scene);
    vk_present_fg_output(game[0], game[1], fg);
    struct Size *e = NULL, *lru = &D.sizes[0];
    for (int i = 0; i < 5; i++) {
        if (D.sizes[i].w == w && D.sizes[i].h == h && D.sizes[i].ow == ow && D.sizes[i].oh == oh &&
            D.sizes[i].fw == fg[0] && D.sizes[i].fh == fg[1] &&
            D.sizes[i].gw == game[0] && D.sizes[i].gh == game[1]) return &D.sizes[i];
        if (!D.sizes[i].out && !e) e = &D.sizes[i];
        if (D.sizes[i].used < lru->used) lru = &D.sizes[i];
    }
    if (!e) {
        if (D.last == lru) D.last = NULL;
        if (!free_size(e = lru)) return NULL;
    }
    VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageUsageFlags u8 = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!image2d(ow, oh, VK_FORMAT_R16G16B16A16_SFLOAT, u, &e->out, &e->out_mem, &e->out_view) ||
        !image2d(w, h, VK_FORMAT_R16G16_SFLOAT, u | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &e->mv, &e->mv_mem, &e->mv_view) ||
        !image2d(w, h, VK_FORMAT_R32_SFLOAT, u, &e->device_depth, &e->device_depth_mem, &e->device_depth_view) ||
        !image2d(w, h, VK_FORMAT_R8_UNORM, u8, &e->mask, &e->mask_mem, &e->mask_view) ||
        !image2d(w, h, VK_FORMAT_R8_UNORM, u8, &e->chars[0], &e->chars_mem[0], &e->chars_view[0]) ||
        !image2d(w, h, VK_FORMAT_R8_UNORM, u8, &e->chars[1], &e->chars_mem[1], &e->chars_view[1]))
        return NULL;
    VkImageMemoryBarrier2 ib[6];
    VkImage im[6] = {e->out, e->mv, e->mask, e->chars[0], e->chars[1], e->device_depth};
    for (int i = 0; i < 6; i++)
        ib[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im[i],
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 6,
                           .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(cb, &di);
    e->resource_generation = next_resource_generation();
    e->w = w, e->h = h, e->ow = ow, e->oh = oh;
    e->fw = fg[0], e->fh = fg[1], e->gw = game[0], e->gh = game[1];
    return e;
}

static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

// ---------------------------------------------------------------- timing (TOTK_DLAA_TIMES=1)
// GPU timestamps around the camera motion dispatch, the per-object motion pass and DLSS's evaluation, and the CPU
// time of RSF's call and of draining the recorder before it, summed and logged every TIMES_SUMMARY frames:
//   dlaa times: gpu camera motion a ms, object motion b ms, DLSS c ms (frames timed); cpu: recorder drain d ms, RSF
//   evaluate e ms, motion pass recording f ms
// Each frame takes its own slot of TIMES_SLOTS; a slot's results are read when it comes round again (by then the GPU is
// long done with it; not ready: that frame is left out).
enum { T_MV0, T_MV1, T_OBJ, T_DL0, T_DL1, T_N };
#define TIMES_SLOTS 8u
#define TIMES_SUMMARY 600
static struct {
    int on;                      // -1 not read
    VkQueryPool pool;
    uint32_t slot;
    int used[TIMES_SLOTS];       // the slot holds a frame's timestamps (all T_N written)
    double gpu[3];               // ms summed: camera motion, object motion, DLSS
    uint64_t frames, cpu_drain, cpu_ngx, cpu_mv;   // ns summed
} T = {.on = -1};

static int times_on(void) {
    if (T.on < 0) {
        T.on = rt_knob("DLAA_TIMES", "0");
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = TIMES_SLOTS * T_N};
        if (T.on && vkCreateQueryPool(vk_ctx()->device, &qi, NULL, &T.pool) != VK_SUCCESS) T.on = 0;
    }
    return T.on;
}

static void times_mark(VkCommandBuffer cb, int which) {
    if (times_on()) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, T.pool, T.slot * T_N + (uint32_t)which);
}

// At the start of a frame's timing (mv_pass): the slot's last results are read and it is reset.
static void times_begin(VkCommandBuffer cb) {
    if (!times_on()) return;
    T.slot = (T.slot + 1) % TIMES_SLOTS;
    if (T.used[T.slot]) {
        uint64_t q[T_N];
        if (vkGetQueryPoolResults(vk_ctx()->device, T.pool, T.slot * T_N, T_N, sizeof q, q, sizeof q[0],
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            double ns = vk_ctx()->props.limits.timestampPeriod / 1e6;   // ticks to ms
            T.gpu[0] += (double)(q[T_MV1] - q[T_MV0]) * ns, T.gpu[1] += (double)(q[T_OBJ] - q[T_MV1]) * ns;
            T.gpu[2] += (double)(q[T_DL1] - q[T_DL0]) * ns;
            if (++T.frames % TIMES_SUMMARY == 0) {
                rt_log("dlaa times: gpu camera motion %.3f ms, object motion %.3f ms, DLSS %.3f ms (%llu frames timed); "
                       "cpu: recorder drain %.3f ms, RSF evaluate %.3f ms, motion pass recording %.3f ms",
                       T.gpu[0] / TIMES_SUMMARY, T.gpu[1] / TIMES_SUMMARY, T.gpu[2] / TIMES_SUMMARY,
                       (unsigned long long)T.frames, T.cpu_drain / 1e6 / TIMES_SUMMARY, T.cpu_ngx / 1e6 / TIMES_SUMMARY,
                       T.cpu_mv / 1e6 / TIMES_SUMMARY);
                T.gpu[0] = T.gpu[1] = T.gpu[2] = 0, T.cpu_drain = T.cpu_ngx = T.cpu_mv = 0;
            }
        }
    }
    T.used[T.slot] = 0;
    vkCmdResetQueryPool(cb, T.pool, T.slot * T_N, T_N);
}

// ---------------------------------------------------------------- motion vectors
// The camera's motion first (runtime/shaders/dlaa_mv.comp): this frame's scene camera block is copied out of the
// replay's uniform snapshot (vk_usnap.c, as of the submit call) into one slot of camb, last frame's stays in the other,
// and the compute pass reprojects every pixel's depth through the two (inv4 and mv_pass below). Then the per-object
// pass (vk_exec_draw.c, vk_mvobj_execute) writes the motion of whatever moves on its own over it.
#define CAM_BYTES 256u   // vec4 rows 0-15: view, view-projection (3-6), projection, inverse view
#define CAM_BLOCK 0x940u   // the whole block, kept for the per-object motion pass's vertex shaders (vk_exec_draw.c)
#define CAM_SLOT 0xa00u    // camb slot stride
#define MVM_SLOTS 16u      // frames the GPU may still be reading the reprojection blocks of

static int mv_init(void) {
    if (D.mv_ok >= 0) return D.mv_ok;
    D.mv_ok = 0;
    const VkCtx *c = vk_ctx();
    VkDescriptorSetLayoutBinding b[9] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 9, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof dlaa_mv_spv,
                                   .pCode = dlaa_mv_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 2 * CAM_SLOT,
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &D.mv_dsl) != VK_SUCCESS) goto fail;
    VkBufferCreateInfo mbi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = MVM_SLOTS * 2 * CAM_BYTES,
                              .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VmaAllocationCreateInfo mai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                   .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                            VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo minfo;
    if (vmaCreateBuffer(c->vma, &mbi, &mai, &D.mvm, &D.mvm_mem, &minfo) != VK_SUCCESS) goto fail;
    D.mvm_map = minfo.pMappedData;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &D.mv_dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &D.mv_layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &D.mv_samp) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS ||
        vmaCreateBuffer(c->vma, &bi, &ai, &D.camb, &D.camb_mem, NULL) != VK_SUCCESS)
        goto fail;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = D.mv_layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &D.mv_pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) goto fail;
    return D.mv_ok = 1;
fail:
    rt_log("dlaa: the motion-vector pass could not be set up: DLAA runs with zero motion");
    return 0;
}

// The reprojection from this frame's clip space to last frame's, last * inverse(this), in double precision: the shader
// took each pixel back to world space through a float inverse of the view-projection, and with TotK's world
// coordinates in the thousands and its far plane the result was noise of a fraction of a pixel and more on distant
// surfaces, which DLSS followed into a shimmer like heat haze (roofs and trees, most of all in the upscaling modes).
// The shader is unchanged: its "this frame" block gets the identity and its "last frame" block this matrix, so it
// applies just the one matrix to each pixel. A still camera gives the identity, and zero motion exactly.
static int inv4(const double m[16], double o[16]) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 8; k++) a[r][k] = k < 4 ? m[r * 4 + k] : (k - 4 == r);
    for (int col = 0; col < 4; col++) {
        int piv = col;
        for (int r = col + 1; r < 4; r++)
            if (fabs(a[r][col]) > fabs(a[piv][col])) piv = r;
        if (fabs(a[piv][col]) < 1e-300) return 0;
        for (int k = 0; k < 8; k++) { double t = a[col][k]; a[col][k] = a[piv][k]; a[piv][k] = t; }
        double d = a[col][col];
        for (int k = 0; k < 8; k++) a[col][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != col) {
                double f = a[r][col];
                for (int k = 0; k < 8; k++) a[r][k] -= f * a[col][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) o[r * 4 + k] = a[r][k + 4];
    return 1;
}

// The pass, recorded through the recorder like the replay's own commands. 0: no camera this frame (the vectors stay
// zero and DLSS starts a new history). bias > 0 with this frame's G-buffer classes (D.ids) also writes e->mask, the
// bias mask (dlaa_mv.comp); *masked says whether it did.
static int mv_pass(VkExec *ex, struct Size *e, VkImageView zv, float jx, float jy, float bias, int *masked) {
    *masked = 0;
    D.projection_ok = 0;
    D.camera_matrices_ok = 0;
    VkCommandBuffer cb = ex->cb;
    uint32_t w = e->w, h = e->h;
    VkDescriptorBufferInfo cam;
    if (!mv_init() || !D.cam_addr || !vk_usnap_find(D.cam_addr, CAM_BLOCK, UINT32_MAX, 4, &cam)) {
        D.mv_none++;
        D.cam_prev_ok = 0;
        e->chars_ok = 0;
        return 0;
    }
    // this frame's view-projection as the snapshot holds it, for the reprojection; kept as last frame's for the next
    const float *cr = (const float *)vk_usnap_bytes(D.cam_addr + 48, 64);
    const float *projection = (const float *)vk_usnap_bytes(D.cam_addr + 7 * 16, 64);
    if (projection) D.projection_ok = rsf_camera_projection(projection, &D.projection);
    float previous_vp[16];
    if (D.cam_prev_ok) for (int i = 0; i < 16; i++) previous_vp[i] = (float)D.cam_prev[i];
    if (projection && cr) D.camera_matrices_ok = rsf_camera_matrices(projection, cr,
        D.cam_prev_ok && !D.reset ? previous_vp : NULL, &D.camera_matrices);
    double cur_vp[16], m[16], ci[16];
    int have_m = 0;
    if (cr) {
        for (int i = 0; i < 16; i++) cur_vp[i] = cr[i];
        if (D.cam_prev_ok && inv4(cur_vp, ci)) {
            for (int r = 0; r < 4; r++)
                for (int k = 0; k < 4; k++) {
                    double v = 0;
                    for (int j = 0; j < 4; j++) v += D.cam_prev[r * 4 + j] * ci[j * 4 + k];
                    m[r * 4 + k] = v;
                }
            have_m = 1;
        }
    }
    int s = D.cam_slot, had_prev = D.cam_prev_ok;
    VkBufferCopy bc = {cam.offset, (VkDeviceSize)s * CAM_SLOT, CAM_BLOCK};
    vkCmdCopyBuffer(cb, cam.buffer, D.camb, 1, &bc);
    full_barrier(cb);
    D.cam_slot ^= 1, D.cam_prev_ok = cr != NULL;
    if (cr) memcpy(D.cam_prev, cur_vp, sizeof cur_vp);
    if (!had_prev || !have_m) {   // nothing to reproject into: zero motion, new history
        e->chars_ok = 0;
        D.reset = 1;
        // Still dispatch so the current depth reaches RSF on the first frame.
        // Identity reprojection produces zero motion without reading old camera data.
        for (int r = 0; r < 4; r++)
            for (int k = 0; k < 4; k++) m[r * 4 + k] = r == k ? 1.0 : 0.0;
    }
    VkDescriptorImageInfo zi = {D.mv_samp, zv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo oi = {VK_NULL_HANDLE, e->mv_view, VK_IMAGE_LAYOUT_GENERAL};
    uint32_t ms = D.mvm_slot++ % MVM_SLOTS;
    float *blk = (float *)(D.mvm_map + (size_t)ms * 2 * CAM_BYTES);   // rows 3-6 of each block are read
    memset(blk, 0, 2 * CAM_BYTES);
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            blk[12 + r * 4 + k] = r == k ? 1.0f : 0.0f, blk[64 + 12 + r * 4 + k] = (float)m[r * 4 + k];
    vmaFlushAllocation(vk_ctx()->vma, D.mvm_mem, (VkDeviceSize)ms * 2 * CAM_BYTES, 2 * CAM_BYTES);
    VkDescriptorBufferInfo cur = {D.mvm, (VkDeviceSize)ms * 2 * CAM_BYTES, CAM_BYTES};
    VkDescriptorBufferInfo prev = {D.mvm, (VkDeviceSize)ms * 2 * CAM_BYTES + CAM_BYTES, CAM_BYTES};
    // The class mask needs the G-buffer at the scene size; otherwise bindings 4-7 get the mask images as stand-ins
    // (the shader returns before reading them with bias 0).
    const VkExecTarget *ids = &D.ids;
    int use = bias > 0 && D.ids_ok && ids->image && ids->width == w && ids->height == h && !ids->is3d;
    VkImageView iv = use ? vk_exec_target_view(ids->img, ids->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
                                               ids->level, ids->base_layer, 1, 0)
                         : e->mask_view;
    int cur_c = e->chars_cur;
    VkDescriptorImageInfo ii = {D.mv_samp, iv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo mki = {VK_NULL_HANDLE, e->mask_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo cpi = {VK_NULL_HANDLE, e->chars_view[cur_c ^ 1], VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo coi = {VK_NULL_HANDLE, e->chars_view[cur_c], VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo ddi = {VK_NULL_HANDLE, e->device_depth_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[9] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &zi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &cur},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &prev},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 4, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 5, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &mki},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 6, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &cpi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 7, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &coi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 8, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ddi}};
    times_begin(cb);
    times_mark(cb, T_MV0);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, D.mv_pipe);
    rec_push(vk_pipeline_push_fn(), cb, VK_PIPELINE_BIND_POINT_COMPUTE, D.mv_layout, 0, 9, wr);
    float pc[6] = {(float)w, (float)h, jx, jy, use ? bias : 0.0f, use && e->chars_ok ? 1.0f : 0.0f};
    vkCmdPushConstants(cb, D.mv_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    full_barrier(cb);
    times_mark(cb, T_MV1);
    vk_mvcheck_camera(cb, e->mv, w, h, jx, jy);   // TOTK_MV_CHECK (test)
    // the characters' own motion over the camera's (vk_exec_draw.c, per-object motion)
    uint64_t c0 = T.on > 0 ? kern_now_ns() : 0;
    if (had_prev && have_m && vk_mvobj_execute(ex, D.cam_addr, e->mv_view, zv, D.depth.format, w, h)) full_barrier(cb);
    if (T.on > 0) T.cpu_mv += kern_now_ns() - c0;
    times_mark(cb, T_OBJ);
    if (T.on > 0) T.used[T.slot] = 1;   // the DLSS marks follow in evaluate
    vk_mvcheck_final(cb, ex->fence, e->mv, D.color.image, D.color.format, D.color.level, D.color.base_layer, w, h, jx, jy);
    D.mv_frames++;
    if (use) e->chars_cur ^= 1, e->chars_ok = 1, D.masked++;
    else e->chars_ok = 0;
    *masked = use;
    return 1;
}

// Last frame's camera block for the per-object motion pass, recorded before this frame's mv_pass: that pass writes
// this frame's into slot cam_slot and leaves the other slot, last frame's, as it is. 0: none (no history).
int vk_dlaa_prev_camera(VkDescriptorBufferInfo *out, float jitter[2]) {
    if (!dl_active() || !D.cam_prev_ok || !D.camb) return 0;
    *out = (VkDescriptorBufferInfo){D.camb, (VkDeviceSize)(D.cam_slot ^ 1) * CAM_SLOT, CAM_BLOCK};
    jitter[0] = D.jit_ok ? D.jx : 0.0f, jitter[1] = D.jit_ok ? D.jy : 0.0f;
    return 1;
}

// ---------------------------------------------------------------- reactive mask
// Particles, smoke, sparks and other transparents have no motion vectors of their own: the motion image holds the
// surface's behind them, and DLSS carried their old colour along that surface (trails behind sparks, smoke smeared
// with the camera). DLSS takes a mask of where to trust this frame's colour over the history (NGX
// "DLSS.Input.Bias.Current.Color.Mask"). It is made from the picture itself, at no cost on the CPU: the light target is
// copied when the transparents start (the pass that arms DLAA, vk_dlaa_pass) and compared with what DLSS gets
// (runtime/dlaa_mask_spv.h, tools/make_dlaa_mask.py): the share of a pixel's colour that changed, from MASK_THR on,
// times MASK_GAIN. Off by default (TOTK_DLSS_REACTIVE=1 turns it on, TOTK_DLAA_MASKDEBUG=1 shows the mask instead of
// the picture): the pass that arms also draws the sky and blended fog layers over the solid scene, and the mask
// covered the sky, sunlit trees and roofs and a wide fog region besides the particles (seen with MASKDEBUG at 19:45 in
// the first save), where DLSS would give up its anti-aliasing. It needs the particle draws alone (drawn again into the
// mask, like the motion draws) before it can be on. In the upscaling modes the light target DLSS reads also holds the
// bloom by then; the copy would have to be compared at the end of the scene instead.
#include "dlaa_mask_spv.h"
#define MASK_THR 0.1f
#define MASK_GAIN 2.5f
#define SNAPS 3

static struct {
    struct Snap {                // per light target size and format: its copy at arming, and the mask
        uint32_t w, h;
        VkFormat f;
        VkImage before, mask;
        VmaAllocation before_mem, mask_mem;
        VkImageView before_view, mask_view;
        uint64_t used;
    } s[SNAPS];
    struct Snap *cur;            // this frame's copy, NULL: none
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    int ok, knob;                // ok: -1 not built, 0 failed, 1 ready; knob: -1 not read
    uint64_t made, missed;       // frames with a mask; frames DLSS ran without the copy
} R = {.ok = -1, .knob = -1};

static int mask_init(void) {
    if (R.knob < 0) R.knob = rt_knob("DLSS_REACTIVE", "0");
    if (!R.knob) return 0;
    if (R.ok >= 0) return R.ok;
    R.ok = 0;
    const VkCtx *c = vk_ctx();
    VkDescriptorSetLayoutBinding b[3] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 3, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof dlaa_mask_spv,
                                   .pCode = dlaa_mask_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &R.dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &R.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &R.layout) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS)
        goto fail;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = R.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &R.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) goto fail;
    return R.ok = 1;
fail:
    rt_log("dlaa: the reactive mask pass could not be set up: DLSS runs without it");
    return 0;
}

static void free_snap(struct Snap *s) {
    const VkCtx *c = vk_ctx();
    vkDeviceWaitIdle(c->device);   // a fourth size: the evicted images may still be in flight
    if (s->before_view) vkDestroyImageView(c->device, s->before_view, NULL);
    if (s->mask_view) vkDestroyImageView(c->device, s->mask_view, NULL);
    if (s->before) vmaDestroyImage(c->vma, s->before, s->before_mem);
    if (s->mask) vmaDestroyImage(c->vma, s->mask, s->mask_mem);
    memset(s, 0, sizeof *s);
}

static struct Snap *snap_for(VkCommandBuffer cb, uint32_t w, uint32_t h, VkFormat f) {
    static uint64_t tick;
    struct Snap *e = NULL, *lru = &R.s[0];
    for (int i = 0; i < SNAPS; i++) {
        if (R.s[i].w == w && R.s[i].h == h && R.s[i].f == f) return R.s[i].used = ++tick, &R.s[i];
        if (!R.s[i].before && !e) e = &R.s[i];
        if (R.s[i].used < lru->used) lru = &R.s[i];
    }
    if (!e) free_snap(e = lru);
    VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!image2d(w, h, f, u, &e->before, &e->before_mem, &e->before_view) ||
        !image2d(w, h, VK_FORMAT_R8_UNORM, u | VK_IMAGE_USAGE_STORAGE_BIT, &e->mask, &e->mask_mem, &e->mask_view)) {
        free_snap(e);
        return NULL;
    }
    VkImageMemoryBarrier2 ib[2];
    VkImage im[2] = {e->before, e->mask};
    for (int i = 0; i < 2; i++)
        ib[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im[i],
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2,
                           .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(cb, &di);
    e->w = w, e->h = h, e->f = f, e->used = ++tick;
    return e;
}

// At arming: the light target as it is before the transparents, outside any pass.
static void mask_snapshot(VkExec *ex, const VkExecTarget *t) {
    R.cur = NULL;
    if (mask_init() <= 0) return;
    struct Snap *s = snap_for(ex->cb, t->width, t->height, t->format);
    if (!s) return;
    vk_exec_barrier(ex);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, t->level, t->base_layer, 1},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {t->width, t->height, 1}};
    vkCmdCopyImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, s->before, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    full_barrier(ex->cb);
    ex->recorded += 2;
    R.cur = s;
}

// Before DLSS: the mask from the copy and the light target (view cv). NULL: none this frame.
static struct Snap *mask_pass(VkCommandBuffer cb, VkImageView cv, uint32_t w, uint32_t h) {
    struct Snap *s = R.cur;
    R.cur = NULL;
    if (mask_init() <= 0) return NULL;
    if (!s || s->w != w || s->h != h || s->f != D.color.format) return R.missed++, NULL;
    VkDescriptorImageInfo bi = {D.mv_samp, s->before_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo ai = {D.mv_samp, cv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo mi = {VK_NULL_HANDLE, s->mask_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[3] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &bi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ai},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &mi}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.pipe);
    rec_push(vk_pipeline_push_fn(), cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 3, wr);
    float pc[4] = {(float)w, (float)h, MASK_THR, MASK_GAIN};
    vkCmdPushConstants(cb, R.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    full_barrier(cb);
    R.made++;
    return s;
}

// ---------------------------------------------------------------- jitter
// The scene's draws (every pass drawing with the scene depth, until the DLAA point) have their viewport moved by a
// sub-pixel Halton(2,3) offset, which shifts what they rasterise as a jittered projection would; full-screen passes
// that shade by window position are unaffected. Jitter only follows a frame DLSS ran on: a jittered frame that DLSS
// does not resolve shakes on screen, so a missed DLAA point stops the jitter for a second.
static float halton(uint32_t i, uint32_t b) {
    float f = 1, r = 0;
    for (; i; i /= b) f /= (float)b, r += f * (float)(i % b);
    return r;
}

// The scene is drawn at the op ex records. A chunked replay's scan ends it ahead of the chunks that still record the
// scene's draws, so a chunk compares its op with the scan's (the chunks after it are taken once the scan is past their
// ops); a later submission of the frame is past it.
static int ended_at(const VkExec *ex) {
    if (!__atomic_load_n(&D.ended, __ATOMIC_ACQUIRE)) return 0;
    return !ex->chunked || ex->fence != D.end_fence || ex->pos >= D.end_pos;
}

void vk_dlaa_viewport(VkExec *ex, VkViewport *v) {
    if (!D.jit_ok || !ex->depth.image || ex->depth.image != D.jdepth || ended_at(ex)) return;
    v->x += D.jx, v->y += D.jy;
    D.ux = D.jx, D.uy = D.jy;
    __atomic_store_n(&D.jit_used, 1, __ATOMIC_RELAXED);
}

// The scene camera: c4 of the last scene draw before the scene ends. A chunked replay's scan notes it in replay order
// (vk_dlaa_scan_draw): chunks recording side by side would leave the block of whichever drew last.
static void note_camera(const VkExec *ex) {
    const p_bind_buf *b = &ex->ubo[0][1];   // constant bank c4: the replay keeps user banks from c3 on
    const VkExecTarget *z = &ex->depth;
    if (b->size == 0x940 && b->addr && z->image && z->width >= DLAA_SCENE_MIN_W && z->format == VK_FORMAT_D32_SFLOAT &&
        (!D.jdepth || z->image == D.jdepth)) {
        D.cam_addr = b->addr;
        memcpy(D.camera_depth_range, ex->depth_range, sizeof D.camera_depth_range);
        D.camera_depth_compare = ex->pipe.ds.df;
    }
}

void vk_dlaa_draw(VkExec *ex) {
    if (!dl_active() || ended_at(ex)) return;
    static int survey = -1;   // TOTK_DLAA_SURVEY=1 (test): what is drawn into the armed light target
    if (survey < 0) survey = rt_knob("DLAA_SURVEY", "0");
    static uint64_t dfn[16];   // scene draws writing depth, by depth test (reversed depth: GREATER/GEQUAL)
    if (survey && ex->depth.image && ex->depth.image == D.jdepth && ex->pipe.ds.dt && ex->pipe.ds.dw)
        dfn[ex->pipe.ds.df & 15]++;
    if (survey && D.armed && ex->color[0].image == D.color.image) {
        static uint64_t n[2][2][2], arms, frames, last_frame;   // [blend][depth write][depth test]
        const VkPipeState *s = &ex->pipe;
        n[s->color.blend_en & 1][s->ds.dw][s->ds.dt]++;
        uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        if (f != last_frame) last_frame = f, frames++;
        static uint64_t ps;
        if (ex->pass_serial != ps) ps = ex->pass_serial, arms++;
        if (frames % 300 == 0) {
            static uint64_t logged;
            if (logged != frames) {
                logged = frames;
                rt_log("dlaa survey: scene draws writing depth by depth test (NVN func 1-8: never less equal lequal "
                       "greater notequal gequal always): %llu %llu %llu %llu %llu %llu %llu %llu",
                       (unsigned long long)dfn[1], (unsigned long long)dfn[2], (unsigned long long)dfn[3],
                       (unsigned long long)dfn[4], (unsigned long long)dfn[5], (unsigned long long)dfn[6],
                       (unsigned long long)dfn[7], (unsigned long long)dfn[8]);
                rt_log("dlaa survey: %llu frames, %.1f passes a frame into the light target after arming; draws a frame "
                       "(blend/depth write/depth test): 000 %.1f 001 %.1f 010 %.1f 011 %.1f 100 %.1f 101 %.1f 110 %.1f 111 %.1f",
                       (unsigned long long)frames, (double)arms / frames, (double)n[0][0][0] / frames, (double)n[0][0][1] / frames,
                       (double)n[0][1][0] / frames, (double)n[0][1][1] / frames, (double)n[1][0][0] / frames,
                       (double)n[1][0][1] / frames, (double)n[1][1][0] / frames, (double)n[1][1][1] / frames);
            }
        }
    }
    if (!ex->chunked) note_camera(ex);
}

void vk_dlaa_scan_draw(const VkExec *ex) {
    if (dl_active() && !D.ended) note_camera(ex);
}

// The jitter moves on right after DLAA ran, not at the present: the replay can record the next frame's scene before
// the present of this one is handled, and a step taken there landed in the middle of a frame (worse after an off/on
// switch, which shifted that order). Jitter needs a frame DLSS ran on; a frame without the DLAA point stops it.
// The sequence is as long as NVIDIA's DLSS guide asks for the upscaling ratio, 8 x (output / scene)^2 phases (18 in
// Quality, 24 in Balanced, 32 in Performance, 72 in Ultra Performance), and 16 at least, the length DLAA has used from the start.
static void next_jitter(uint32_t w, uint32_t ow) {
    static int nj = -1;
    if (nj < 0) nj = !rt_knob("DLAA_JITTER", "1");   // TOTK_DLAA_JITTER=0: no jitter (test)
    double r = w ? (double)ow / w : 1.0;
    uint32_t phases = (uint32_t)ceil(8.0 * r * r - 1e-6);
    if (phases < 16) phases = 16;
    if (phases > 72) phases = 72;
    uint32_t i = D.jframe++ % phases + 1;
    D.jx = halton(i, 2) - 0.5f, D.jy = halton(i, 3) - 0.5f;
    D.jit_ok = !D.hold && !nj;
}

// The DLSS model (TOTK_DLSS_PRESET, live): default takes NVIDIA's model per mode (evaluate: K for DLAA, Quality and
// Balanced, M for Performance, L for Ultra Performance), 0 here. A letter a..m takes that model in every mode: k pins
// K, j is the other transformer model, which NVIDIA describes as slightly less ghosting at the cost of extra flicker, e
// the older CNN model, cheaper on slower RTX cards. The value is an NVSDK_NGX_DLSS_Hint_Render_Preset (1 A ... 13 M),
// read when a feature is created; a change recreates the features (preset_check).
static int preset_value(void) {
    const char *v = cfg_get("TOTK_DLSS_PRESET");
    int c = v && v[0] && !v[1] ? v[0] | 0x20 : 0;
    return c >= 'a' && c <= 'm' ? c - 'a' + 1 : 0;
}

// How strongly DLSS favours the current frame on characters and where they just were (TOTK_DLSS_CHAR_MASK, percent,
// live; 0 off). Where a character's vectors are the camera's only (dlaa_objmv = 0, or a draw the per-object pass
// skips), one that moves on its own drags a faint copy of itself through DLSS's history; the bias mask makes DLSS take
// less of the history there, at the cost of some of its anti-aliasing on those pixels.
static float char_bias(void) {
    const char *v = cfg_get("TOTK_DLSS_CHAR_MASK");
    long n = v && *v ? strtol(v, NULL, 10) : 65;
    return n <= 0 ? 0.0f : n >= 100 ? 1.0f : (float)n / 100.0f;
}

// A target of the scene's shape (resolution.c's scene size, within 5 %; the dynamic resolution tiers keep it). In the
// inventory the game renders the items' icons as the slots fill in, each through a G-buffer and a light target of its
// own with a D32F depth, square (432x432 or 426x426 at a 3840x2160 scene) and with no wider G-buffer before it in the
// frame; DLSS armed on that, scaled it to the output and wrote it over the menu, so a weapon filled the screen on the
// frames an icon was drawn.
static int scene_shaped(const VkExecTarget *c) {
    unsigned o[2], s[2];
    res_sizes(o, s);
    uint64_t a = (uint64_t)c->width * s[1], b = (uint64_t)c->height * s[0];
    return a * 20 >= b * 19 && b * 20 >= a * 19;
}

static rsf_vk_function rsf_instance_proc(void *instance, const char *name) {
    return (rsf_vk_function)vkGetInstanceProcAddr(instance, name);
}
static rsf_vk_function rsf_device_proc(void *device, const char *name) {
    return (rsf_vk_function)vkGetDeviceProcAddr(device, name);
}

static void rsf_layout(VkCommandBuffer cb, VkImage image, VkImageLayout before, VkImageLayout after) {
    VkImageMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = before, .newLayout = after, .image = image,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(cb, &dependency);
}

static rsf_srvk_image rsf_image(VkImage image, VkFormat format, unsigned w, unsigned h, VkImageLayout layout) {
    return (rsf_srvk_image){vk_ctx()->device, (uint64_t)image, format, w, h, layout, 1};
}

static rsf_frame_record scene_record(VkExec *ex, const struct Size *e, float jx, float jy) {
    uint64_t now = plat_now_ns();
    float dt = D.last_eval_ns ? (float)((now - D.last_eval_ns) / 1e6) : 16.67f;
    D.last_eval_ns = now;
    if (!(dt > 0 && dt < 100)) dt = 16.67f;
    rsf_frame_record record = {.struct_size = sizeof record, .abi_version = RSF_GAME_FRAME_ABI_VERSION,
        .frame_id = ex->source_frame ? ex->source_frame : D.evals + 1,
        .session_id = ex->source_frame ? 1 : 2, .view_id = 1,
        .resource_generation = e->resource_generation,
        .flags = (ex->source_frame ? 0 : RSF_FRAME_FLAG_AMBIGUOUS_ID) |
                 (D.reset ? RSF_FRAME_FLAG_RESET : 0),
        .render_width = e->w, .render_height = e->h, .output_width = e->ow, .output_height = e->oh,
        .frame_time_ms = dt,
        .camera = {.struct_size = sizeof(rsf_camera_frame), .abi_version = RSF_GAME_FRAME_ABI_VERSION,
            .render_width = e->w, .render_height = e->h, .output_width = e->ow, .output_height = e->oh,
            .near_plane = D.projection.near_plane, .far_plane = D.projection.far_plane,
            .vertical_fov_radians = D.projection.vertical_fov, .depth_inverted = D.projection.inverted,
            .jitter_pixels = {jx, jy}, .frame_time_seconds = dt / 1000}};
    memcpy(record.camera.view_to_clip, D.camera_matrices.view_to_clip, sizeof record.camera.view_to_clip);
    memcpy(record.camera.clip_to_view, D.camera_matrices.clip_to_view, sizeof record.camera.clip_to_view);
    memcpy(record.camera.world_to_view, D.camera_matrices.world_to_view, sizeof record.camera.world_to_view);
    memcpy(record.camera.view_to_world, D.camera_matrices.view_to_world, sizeof record.camera.view_to_world);
    memcpy(record.camera.clip_to_previous_clip, D.camera_matrices.clip_to_previous_clip,
           sizeof record.camera.clip_to_previous_clip);
    return record;
}

static void fg_prepare(VkExec *ex, struct Size *e, const rsf_frame_record *record) {
    if (!cfg_int("TOTK_RSF_FG") || !ex->source_frame || !e->fw || !e->fh) return;
    rsf_frame_record output = *record;
    output.output_width = output.camera.output_width = e->fw;
    output.output_height = output.camera.output_height = e->fh;
    record = &output;
    VkCommandBuffer cb = ex->cb;
    rsf_layout(cb, e->device_depth, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    rsf_layout(cb, e->mv, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    rsf_fgvk_frame frame = {.struct_size = sizeof frame, .abi_version = RSF_FGVK_ABI_VERSION, .record = record,
        .depth = rsf_image(e->device_depth, VK_FORMAT_R32_SFLOAT, e->w, e->h, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        .motion = rsf_image(e->mv, VK_FORMAT_R16G16_SFLOAT, e->w, e->h, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        .motion_scale_x = 1, .motion_scale_y = 1};
    rec_drain();   // SDK records directly into the producer's current command buffer.
    int result = vk_present_fg_prepare(cb, &frame);
    e->fg_record = *record;
    if (result) e->fg_record.frame_id = 0;
    rsf_layout(cb, e->device_depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    rsf_layout(cb, e->mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vk_exec_draw_new_cb(ex);
    static uint64_t prepared, refused;
    if (!result) {
        if (++prepared == 1 || !(prepared % 300))
            rt_log("rsf: FG prepared %llu source %llu, render %ux%u output %ux%u; HUD-less admission pending",
                (unsigned long long)prepared, (unsigned long long)record->frame_id, e->w, e->h, e->fw, e->fh);
    } else if (++refused == 1 || !(refused % 300))
        rt_log("rsf: FG prepare refused (%d) source %llu (%llu refusals)", result,
            (unsigned long long)record->frame_id, (unsigned long long)refused);
}

static int rsf_scene(VkExec *ex, struct Size *e, int masked, const rsf_frame_record *source) {
    const VkCtx *c = vk_ctx();
    int split = cfg_int("TOTK_RSF_BACKEND") == RSF_SR_FSR4;
    rsf_fsr4vk_handoff handoff;
    if (split && !c->has_rsf_sr_split) {
        rt_log("rsf: FSR4 unavailable: Win32 shared image/fence imports were not enabled");
        return RSF_SR_FAILED;
    }
    if (split && !vk_sync_sr_prepare(ex->cb, &handoff)) {
        static int warned;
        if (!warned++) rt_log("rsf: FSR4 recording deferred: no serial graphics recording lease");
        return RSF_SR_PENDING;
    }
    if (!e->rsf) {
        rsf_srvk_setup setup = {.struct_size = sizeof setup, .abi_version = RSF_SRVK_ABI_VERSION,
            .instance = c->instance, .physical_device = c->phys, .device = c->device,
            .instance_proc = rsf_instance_proc, .device_proc = rsf_device_proc,
            .backend = (uint32_t)cfg_int("TOTK_RSF_BACKEND"),
            .quality = !strcmp(cfg_value("TOTK_RSF_QUALITY"), "quality") ? 1 :
                       !strcmp(cfg_value("TOTK_RSF_QUALITY"), "balanced") ? 2 :
                       !strcmp(cfg_value("TOTK_RSF_QUALITY"), "performance") ? 3 :
                       !strcmp(cfg_value("TOTK_RSF_QUALITY"), "ultra_performance") ? 4 : 0,
            .output_width = e->ow, .output_height = e->oh,
            .depth_inverted = D.projection.inverted, .depth_infinite = D.projection.infinite,
            .runtime_directory_utf8 = rsf_provider_directory((unsigned)cfg_int("TOTK_RSF_BACKEND"))};
        int opened = split ? rsf_sr_open_split(&setup, &handoff, &e->rsf, &e->rsf_w, &e->rsf_h)
                           : rsf_sr_open(&setup, &e->rsf, &e->rsf_w, &e->rsf_h);
        if (!opened) { if (split) vk_sync_sr_cancel(); return 0; }
    }
    if (e->rsf_w != e->w || e->rsf_h != e->h) {
        if (split) vk_sync_sr_cancel();
        if (res_rsf_scene(e->rsf_w, e->rsf_h)) return RSF_SR_PENDING;
        rt_log("rsf: provider requires %ux%u; scene %ux%u cannot be reconfigured within its heap budget",
               e->rsf_w, e->rsf_h, e->w, e->h);
        return 0;
    }
    VkCommandBuffer cb = ex->cb;
    if (!e->sr_color) {
        if (!image2d(e->w, e->h, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            &e->sr_color, &e->sr_color_mem, &e->sr_color_view)) { if (split) vk_sync_sr_cancel(); return 0; }
        rsf_layout(cb, e->sr_color, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    }
    VkImageBlit copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, D.color.level, D.color.base_layer, 1},
        .srcOffsets = {{0, 0, 0}, {(int)e->w, (int)e->h, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets = {{0, 0, 0}, {(int)e->w, (int)e->h, 1}}};
    vkCmdBlitImage(cb, D.color.image, VK_IMAGE_LAYOUT_GENERAL, e->sr_color, VK_IMAGE_LAYOUT_GENERAL,
                   1, &copy, VK_FILTER_NEAREST);
    rsf_layout(cb, e->sr_color, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    rsf_layout(cb, e->device_depth, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    rsf_layout(cb, e->mv, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (masked) rsf_layout(cb, e->mask, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    rsf_layout(cb, e->out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    rsf_frame_record record = *source;
    record.flags |= RSF_FRAME_FLAG_NO_FG;
    rsf_srvk_frame frame = {.struct_size = sizeof frame, .abi_version = RSF_SRVK_ABI_VERSION, .record = &record,
        .color = rsf_image(e->sr_color, VK_FORMAT_R16G16B16A16_SFLOAT, e->w, e->h, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        .depth = rsf_image(e->device_depth, VK_FORMAT_R32_SFLOAT, e->w, e->h, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        .motion = rsf_image(e->mv, VK_FORMAT_R16G16_SFLOAT, e->w, e->h, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        .output = rsf_image(e->out, VK_FORMAT_R16G16B16A16_SFLOAT, e->ow, e->oh, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        .motion_scale_x = 1, .motion_scale_y = 1, .pre_exposure = 1};
    if (masked) frame.bias_current_color = rsf_image(e->mask, VK_FORMAT_R8_UNORM, e->w, e->h, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    rec_drain();
    uint64_t started = T.on > 0 ? kern_now_ns() : 0;
    void *continuation = cb;
    int result = rsf_sr_record_split(e->rsf, &continuation, &frame);
    if (split) vk_sync_sr_cancel();
    if (!continuation) rt_fatal("rsf: terminal FSR4 split failure; recorded buffers and provider retained");
    cb = ex->cb = continuation;
    if (result == RSF_SR_FAILED && split && continuation != (void *)0) {
        // Keep the SDK failure in provider status. Spatial recovery must be
        // recorded AFTER its done wait, even if the DX12 dispatch was refused.
        __typeof__(&rsf_srvk_spatial_fallback) fallback = rsf_runtime_symbol("rsf_srvk_spatial_fallback");
        rsf_srvk_fallback call = {sizeof call, RSF_SRVK_ABI_VERSION, c->instance, c->phys, c->device,
            rsf_instance_proc, rsf_device_proc, cb, frame.color, frame.output};
        if (fallback && !fallback(&call)) result = RSF_SR_FALLBACK;
    }
    if (T.on > 0) T.cpu_ngx += kern_now_ns() - started;
    rsf_layout(cb, e->sr_color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    rsf_layout(cb, e->device_depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    rsf_layout(cb, e->mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    if (masked) rsf_layout(cb, e->mask, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    rsf_layout(cb, e->out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vk_exec_draw_new_cb(ex);
    return result;
}

// dst: the target DLSS's picture goes to, NULL for the light target itself (DLAA in place).
static void evaluate(VkExec *ex, const VkExecTarget *dst) {
    const VkExecTarget *t = &D.color, *z = &D.depth;
    if (!dst) dst = t;
    uint32_t w = t->width, h = t->height, ow = dst->width, oh = dst->height;
    VkCommandBuffer cb = ex->cb;
    vk_exec_barrier(ex);   // what drew the scene is done (no pass is open here)
    if (D.preset_dirty) {   // another model: features are created with it, so every size gets a new one
        for (size_t i = 0; i < DLSS_SIZES; i++)
            if (D.sizes[i].out && !free_size(&D.sizes[i])) return;
        if (D.failed) return;
        D.last = NULL, D.preset_dirty = 0;
    }
    struct Size *e = size_for(cb, w, h, ow, oh);
    if (!e) return; // Retained SDK leases may become retireable after submission.
    if (e != D.last) D.reset = 1, D.why[0]++, e->chars_ok = 0;   // another size's history is from another moment
    VkImageView zv = vk_exec_target_view(z->img, z->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, z->level,
                                         z->base_layer, 1, 0);
    float jx = D.jit_used ? D.ux : 0.0f, jy = D.jit_used ? D.uy : 0.0f;
    static int no_mv = -1;
    if (no_mv < 0) no_mv = !rt_knob("DLAA_MV", "1");   // TOTK_DLAA_MV=0: zero motion (test)
    int masked = 0;
    // Depth and camera metadata are still needed when the diagnostic requests
    // zero motion. Produce those inputs before clearing just the vector image.
    int motion_ok = mv_pass(ex, e, zv, jx, jy, vk_dlaa_wanted() ? char_bias() : 0.0f, &masked);
    if (no_mv || !motion_ok) {
        VkClearColorValue zero = {{0, 0, 0, 0}};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, e->mv, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
        full_barrier(cb);
        if (!no_mv) D.reset = 1, D.why[1]++;   // no camera: the history cannot be moved along
    }
    VkImageView cv = vk_exec_target_view(t->img, t->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, t->level,
                                         t->base_layer, 1, 0);
    struct Snap *mk = D.mv_samp ? mask_pass(cb, cv, w, h) : NULL;
    int timed = T.on > 0 && T.used[T.slot];   // mv_pass began this frame's slot
    if (timed) times_mark(cb, T_DL0);
    uint64_t c0 = timed ? kern_now_ns() : 0;
    rec_drain();   // RSF records directly: finish all scene and motion commands first
    if (timed) T.cpu_drain += kern_now_ns() - c0;
    if (D.last_color != t->image) D.reset = 1, D.why[2]++, D.last_color = t->image;
    if (!motion_ok || !D.projection_ok || !D.camera_matrices_ok) {
        D.reset = 1;
        D.jit_ok = 0;
        if (motion_ok && ++D.invalid_camera == 1)
            rt_log("rsf: camera projection or transforms unavailable/invalid; skipping reconstruction");
        return;
    }
    rsf_frame_record record = scene_record(ex, e, jx, jy);
    fg_prepare(ex, e, &record);
    if (!vk_dlaa_wanted()) {
        // FG needs scene depth and independent object motion even when the
        // native colour path is retained. Never inject temporal jitter here.
        D.last = e, e->used = ++D.guide_frames, D.jdepth = z->image;
        D.reset = 0, D.jit_ok = 0;
        full_barrier(cb);
        vk_exec_draw_new_cb(ex);
        if (D.guide_frames == 1 || !(D.guide_frames % 300))
            rt_log("rsf: FG guides %llu source %llu, render %ux%u output %ux%u; depth reversed %d infinite %d near %g far %g viewport %g..%g compare %#x; HUD-less admission pending",
                   (unsigned long long)D.guide_frames, (unsigned long long)ex->source_frame, w, h, ow, oh,
                   D.projection.inverted, D.projection.infinite, D.projection.near_plane, D.projection.far_plane,
                   D.camera_depth_range[0], D.camera_depth_range[1], D.camera_depth_compare);
        return;
    }
    int result = rsf_scene(ex, e, masked, &record);
    cb = ex->cb; // the split service may have ended the scene producer
    if (result == RSF_SR_PENDING) { D.reset = 1; D.jit_ok = 0; return; }
    if (!result) return dl_fail("RSF evaluation", 0);
    if (timed) times_mark(cb, T_DL1);   // through the recorder, after RSF recording
    D.reset = result == RSF_SR_FALLBACK;
    D.last = e, e->used = D.evals + 1;
    full_barrier(cb);
    vk_mvcheck_output(cb, e->out, ow, oh);   // TOTK_MV_CHECK (test)
    // the result into the scene target (DLAA: the bloom, the post passes and the HUD read it and draw over it) or into
    // the scaling pass's target (the HUD draws over it)
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .srcOffsets = {{0, 0, 0}, {(int32_t)ow, (int32_t)oh, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dst->level, dst->base_layer, 1},
                     .dstOffsets = {{0, 0, 0}, {(int32_t)ow, (int32_t)oh, 1}}};
    // TOTK_DLAA_MVDEBUG=1 (test): the motion vectors in place of the picture, x in red and y in green, render pixels
    // as scene light (the post passes still run over them)
    static int mvdebug = -1;
    if (mvdebug < 0) mvdebug = rt_knob("DLAA_MVDEBUG", "0");
    static int maskdebug = -1;
    if (maskdebug < 0) maskdebug = rt_knob("DLAA_MASKDEBUG", "0");
    if (mvdebug || (maskdebug && mk)) b.srcOffsets[1] = (VkOffset3D){(int32_t)w, (int32_t)h, 1};
    vkCmdBlitImage(cb, mvdebug ? e->mv : maskdebug && mk ? mk->mask : e->out, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                   VK_FILTER_NEAREST);
    full_barrier(cb);
    if (dst != t) D.up = 1, vk_dlaa_swap_img = t->img, vk_dlaa_swap_view = e->out_view;
    ex->recorded += 4;
    vk_exec_draw_new_cb(ex);   // RSF binds its own pipelines/descriptors; force the next draw to bind everything
    D.jdepth = z->image;
    next_jitter(w, ow);
    if (result == RSF_SR_FALLBACK) {
        static uint64_t fallbacks;
        if (++fallbacks == 1 || !(fallbacks % 300))
            rt_log("rsf: FSR4 spatial fallback %llu; temporal dispatch remains refused", (unsigned long long)fallbacks);
        return;
    }
    if (++D.evals == 1 || D.evals == 300 || D.evals % 3600 == 0)   // 300: one early line with the frames counted
        rt_log("dlaa: %llu frames anti-aliased (scene %ux%u, output %ux%u; %llu frames without the DLAA point, %llu with camera "
               "motion, %llu with the characters' mask, %llu without a camera, %llu jitter holds; resets: %llu size, %llu "
               "camera, %llu target; reactive mask %llu, without its copy %llu)",
               (unsigned long long)D.evals, w, h, ow, oh, (unsigned long long)D.frames_missed,
               (unsigned long long)D.mv_frames, (unsigned long long)D.masked, (unsigned long long)D.mv_none,
               (unsigned long long)D.holds, (unsigned long long)D.why[0], (unsigned long long)D.why[1],
               (unsigned long long)D.why[2], (unsigned long long)R.made, (unsigned long long)R.missed);
}

// More than half the scene size resolution.c set, on each side (the dynamic resolution tiers go down to 80 %). Around
// the quick weapon menu the only G-buffer before the scene's end is at times the half-size one (1920x1080 at a
// 3840x2160 scene), and the half-size RGBA16F effects target with its D32F depth armed; DLSS scaled the effects to the
// output in place of the scene, which showed dark and blurred for that frame.
static int scene_large(const VkExecTarget *c) {
    unsigned o[2], s[2];
    res_sizes(o, s);
    return c->width * 2 > s[0] && c->height * 2 > s[1];
}

static int scene_color(VkFormat f) {
    return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT;
}

// The bloom chain's first pass: at most a quarter of the scene's width, into level 0 of a scene colour target. At some
// views (seen in the yard of Lookout Landing) the game draws a full-screen pass without depth into the light target,
// then a half-size scene colour target and its mips, and only then the light target with the scene depth. Level 1 of
// those mips is a quarter of the scene's width; taken for the bloom it ended the scene before anything was armed, DLSS
// skipped those frames, and the picture flickered between anti-aliased frames and plain ones as the view changed.
// The pass also has the scene's shape (resolution.c's scene size, within 5 %) and no depth. In rain by a waterfall the
// game drew two transparents into the light target with the scene depth before its sky lookups (25x25 and 256x256
// RGBA16F) and its quarter-size particles (960x540 RGBA16F with a depth of their own), which then ended the armed
// scene: DLSS ran before the mist and most transparents, which were drawn over its picture unprocessed, on runs of
// frames, and the mist in the distance darkened or brightened for a frame wherever a frame in between came out whole.
static int bloom_start(const VkExec *ex, uint32_t scene_w) {
    const VkExecTarget *c = &ex->color[0];
    if (!ex->pass_w || ex->pass_w * 4 > scene_w || !c->image || !scene_color(c->format) || c->level || ex->depth.image)
        return 0;
    unsigned o[2], s[2];
    res_sizes(o, s);
    uint64_t a = (uint64_t)ex->pass_w * s[1], b = (uint64_t)ex->pass_h * s[0];
    return a * 20 >= b * 19 && b * 20 >= a * 19;
}

int vk_dlaa_plan(const VkExec *ex, VkExecJob *j) {
    const VkExecTarget *c = &ex->color[0], *z = &ex->depth;
    if (!dl_active() || D.done) return 0;
    // The G-buffer: several colour targets with a D32F depth of at least DLAA_SCENE_MIN_W wide. Only the light target drawn after it
    // arms the frame; at night the game draws into the light target alone with the scene depth before the G-buffer too,
    // and DLAA then ran at a small pass ahead of the whole scene, which reached the screen jittered.
    if (ex->pass_ncolor >= 3 && z->image && z->format == VK_FORMAT_D32_SFLOAT && z->width >= DLAA_SCENE_MIN_W) {
        D.gbuf_w = D.gbuf && D.gbuf_w > z->width ? D.gbuf_w : z->width;
        D.gbuf = 1, D.armed = 0;
        // target 0 is the object-class buffer (RGBA8, the scene size): the characters' bias mask reads its green. A
        // smaller pass of the same shape (640x360, seen while loading) does not replace the scene's in the same frame.
        if (c->image && c->format == VK_FORMAT_R8G8B8A8_UNORM && c->width == z->width && c->height == z->height &&
            c->samples <= 1 && !c->is3d && (!D.ids_ok || c->width >= D.ids.width))
            D.ids = *c, D.ids_ok = 1;
        return 0;
    }
    // A pass into one scene colour target of the G-buffer's size or larger without depth while nothing is armed ends
    // the scene once the bloom chain follows with still nothing armed: in the inventory (1.4.3) the preview's light
    // target is drawn that way before the bloom, and with the scene depth only after it, while the menu is composed;
    // DLAA ran on the finished menu picture with the previous frame's history, which left stray dark pixels around Link.
    // Arming cancels the end, on that target too: at some camera angles the field's effects composite (a full-screen
    // draw without depth into the light target) comes before the first pass with the scene depth, and with that target
    // barred from arming DLSS stopped for as long as the view held (in the inventory the bloom chain always ended the
    // scene first, in both modes, menu opening included). While the scope zooms in, a second G-buffer pass disarms the
    // frame, the scene is copied into an output-sized target without depth, and only then is the light target drawn
    // again with the scene depth.
    // The G-buffer's width is the widest of the frame: the game also draws a half-size G-buffer, after which a
    // half-size pass without depth ended the scene at some camera angles.
    if (D.gbuf && !D.armed && !D.ended && !z->image && ex->pass_ncolor == 1 && c->image && scene_color(c->format) &&
        c->width >= D.gbuf_w && !c->level)
        D.nodepth = c->image;
    if (D.nodepth && !D.armed && !D.ended && bloom_start(ex, D.gbuf_w)) {
        D.end_fence = ex->fence, D.end_pos = ex->pos;
        __atomic_store_n(&D.ended, 1, __ATOMIC_RELEASE);
    }
    // the scene depth is D32F; a D16 target of the scene size also serves one composite pass before the transparents.
    // A smaller pair never takes over an armed one: the camera rune draws a half-size RGBA16F target with a half-size
    // D32F depth after the lighting (852x480 under a 1704x960 scene), and DLSS ran on that and wrote its picture into
    // the scene's own light target, which left every object black. Nor does a pair narrower than the frame's widest
    // G-buffer arm at all: the game draws its particles into a half-size RGBA16F target with a half-size D32F depth, and
    // with the scope open, where the light target is drawn with a D16 depth and never arms, that effects pass was the
    // first pair to arm; DLSS then scaled the effects into the light target, which filled the lens with garbage in the
    // DLAA mode.
    if (D.gbuf && !D.ended && ex->pass_ncolor == 1 && c->image && scene_color(c->format) && z->image && z->format == VK_FORMAT_D32_SFLOAT &&
        c->width == z->width && c->height == z->height && c->width >= DLAA_SCENE_MIN_W && c->width >= D.gbuf_w && scene_shaped(c) &&
        (!D.armed || (c->width >= D.color.width && c->height >= D.color.height)) &&
        c->samples <= 1 && !c->is3d && scene_large(c)) {
        int first = !D.armed;
        D.color = *c, D.depth = *z, D.armed = 1, D.nodepth = VK_NULL_HANDLE;
        if (!first) return 0;
        *j = (VkExecJob){.kind = VK_JOB_DLSS_MASK, .scene = *c};   // the lit scene, before the transparents (reactive mask)
        return 1;
    }
    if (!D.armed) return 0;
    if (!D.ended) {
        // The bloom chain's first pass ends the scene (bloom_start). With the camera rune or the scope open the game
        // draws a 160-wide depth-only pass and 160x90 RG16F and RGBA8 passes right after the lighting, then more
        // geometry and the transparents; DLSS ended the scene at those, the next G-buffer pass disarmed it and the
        // scaling pass found nothing to evaluate.
        if (!bloom_start(ex, D.color.width)) return 0;
        D.end_fence = ex->fence, D.end_pos = ex->pos;
        __atomic_store_n(&D.ended, 1, __ATOMIC_RELEASE);
        unsigned o[2], s[2];
        res_sizes(o, s);
        if (D.color.width < o[0]) return 0;   // wait for the scaling pass
        D.done = 1;
        *j = (VkExecJob){.kind = VK_JOB_DLSS};   // DLAA in place
        return 1;
    }
    // the game's scaling pass: one colour target of the output size, no depth
    unsigned o[2], s[2];
    res_sizes(o, s);
    if (ex->pass_ncolor == 1 && c->image && !z->image && scene_color(c->format) && c->width > D.color.width &&
        c->width == o[0] && c->height == o[1] && c->samples <= 1 && !c->is3d) {
        D.done = 1;
        *j = (VkExecJob){.kind = VK_JOB_DLSS, .scene = *c};
        return 1;
    }
    return 0;
}

// The scan resolved its targets without making entries (vk_texture_revive); a record made on a chunk takes their images
// now, as the scan's pointers may be stale by then.
static void target_now(VkExecTarget *t) {
    const VkTexImage *i = t->tex ? vk_texture_get(t->tex) : NULL;
    if (i) t->img = i, t->image = i->image;
}

void vk_dlaa_exec(VkExec *ex, const VkExecJob *j) {
    VkExecTarget t = j->scene;
    if (ex->chunked) {
        target_now(&t), target_now(&D.color), target_now(&D.depth);
        if (D.ids_ok) target_now(&D.ids);
    }
    if (j->kind == VK_JOB_DLSS_MASK) return mask_snapshot(ex, &t);
    if (!t.tex) return evaluate(ex, NULL);
    evaluate(ex, &t);
}

void vk_dlaa_pass(VkExec *ex) {
    VkExecJob j;
    if (!ex->chunked && vk_dlaa_plan(ex, &j)) vk_dlaa_exec(ex, &j);
}

// TOTK_DLSS, read at each present (the menu switches it live): the scene size of the mode (resolution.c) and the
// texture bias that goes with it while DLSS runs.
static void dlss_mode(void) {
    static char mode[32];
    static int bias_on = -1;
    if (!vk_dlaa_wanted()) {
        if (bias_on != 0) vk_sampler_dlss_bias(0.0f);
        bias_on = 0;
        return;
    }
    const char *m = cfg_value("TOTK_RSF_QUALITY");
    if (!strcmp(m, "native")) m = "dlaa";
    if (strcmp(m, mode)) {
        D.preset_dirty = 1;
        D.reset = 1;
        snprintf(mode, sizeof mode, "%s", m);
        res_dlss_mode(m);
        bias_on = -1;
    }
    int b = dl_active() && vk_dlaa_wanted();
    if (b != bias_on) bias_on = b, vk_sampler_dlss_bias(b ? res_dlss_lod_bias() : 0.0f);
}


// The preset, read again when a setting changed (the menu); a different one recreates the features at the next DLAA run.
static void preset_check(void) {
    unsigned v = cfg_version();
    if (v == D.cfg_seen) return;
    D.cfg_seen = v;
    int p = preset_value();
    if (p == D.preset) return;
    if (D.evals) {
        if (p) rt_log("dlaa: DLSS preset %c in every mode; the features are created again", 'A' + p - 1);
        else rt_log("dlaa: DLSS presets per mode; the features are created again");
    }
    D.preset = p, D.preset_dirty = 1, D.reset = 1;
}

void vk_dlaa_frame(void) {
    D.fg_on = N.available && cfg_int("TOTK_RSF_FG") != 0;
    vk_dlaa_swap_img = NULL, vk_dlaa_swap_view = VK_NULL_HANDLE;
    dlss_mode();
    preset_check();
    int on = N.available && temporal_wanted();
    if (on != D.on) {
        rt_log("dlaa: %s", on ? "on" : "off");
        D.on = on;
        // either way the next DLAA frame starts a new history from an unjittered frame
        D.reset = 1, D.cam_prev_ok = 0, D.jit_ok = D.jit_used = D.armed = D.done = D.gbuf = D.ended = D.up = 0;
        D.nodepth = VK_NULL_HANDLE;
        D.hold = 0;
        return;
    }
    if (!dl_active()) return;
    if (!D.done) D.frames_missed++;
    if (D.jit_used && !D.done) D.hold = 60, D.holds++;   // jittered but not resolved: that frame shook
    else if (D.hold) D.hold--;
    if (!D.done) D.jit_ok = 0;
    D.armed = D.done = D.jit_used = D.gbuf = D.ended = D.up = D.ids_ok = 0, D.nodepth = VK_NULL_HANDLE;
    D.cam_addr = 0;
}

// ---------------------------------------------------------------- the game's FXAA (TOTK_FXAA, live)
// TotK's FXAA is two full-screen passes after the bloom, just before the HUD: one copies the scene target into a
// scratch target, the other draws the anti-aliased result back over the scene (found by diffing frame dumps with the
// game's own switch, ModelSceneExtensionParam EnableFXAA, on and off). With fxaa = 0 the draws of that second pass's
// fragment shader are skipped, so the scene reaches the HUD as it was. Shaders are known by the FNV-1a hash of their
// SPIR-V, as vk_dump.c names them; a translator change gives new hashes and the switch then does nothing.
// With a scene smaller than the output (the game's own 5/6 render scale, or a DLSS mode) the same shader draws the
// scaled picture into the output-sized target, in a pass without the depth attachment the in-place FXAA has: there it
// is always drawn, whatever fxaa says (skipping it would leave the 3D picture out), unless DLSS already wrote that
// target this frame.
// The stock translator's hash, the one with tools/xlate patch 0010 (PCSS, PR #89) applied, the one with patches 0010
// and 0011 (the shadow filter spread) applied, and that one with the zero_locals job flag (Mesa, vk_caps.c).
// A scaling draw this list misses still scales DLSS's picture, not
// the jittered scene: after DLSS ran, passes larger than the light target sample DLSS's output (vk_dlaa_swap_img).
static const uint64_t FXAA_FS[] = {0xf450003ebeb3dc52ull, 0x0a12f4bc2a9a4fbeull, 0x5c43ae18948d52cbull,
                                   0xaad42238801d31d8ull};

// Chunks of one submission look shaders up side by side: an entry's hash is stored before its shader, and entries are
// added under the lock.
static uint64_t spirv_hash(const GfxShader *s) {
    static struct { const GfxShader *s; uint64_t h; } tab[8192];
    static PlatMutex lk = PLAT_MUTEX_INIT;
    uintptr_t k = ((uintptr_t)s >> 4) * 0x9E3779B97F4A7C15ull;
    for (uint32_t i = 0, j = (uint32_t)(k >> 51); i < 8192; i++, j = (j + 1) & 8191) {
        const GfxShader *e = __atomic_load_n(&tab[j].s, __ATOMIC_ACQUIRE);
        if (e == s) return tab[j].h;
        if (e) continue;
        plat_mutex_lock(&lk);
        e = tab[j].s;
        if (!e) {
            uint64_t words = 0, h = 0xcbf29ce484222325ull;
            const uint8_t *b = (const uint8_t *)gfx_shader_spirv(s, &words);
            for (uint64_t n = 0; b && n < words * 4; n++) h = (h ^ b[n]) * 0x100000001b3ull;
            tab[j].h = h;
            __atomic_store_n(&tab[j].s, s, __ATOMIC_RELEASE);
            plat_mutex_unlock(&lk);
            return h;
        }
        plat_mutex_unlock(&lk);
        if (e == s) return tab[j].h;
    }
    return 0;   // table full: no skip
}

void vk_dlaa_fg_after_draw(VkExec *ex, const GfxShader *fs) {
    if (!D.fg_on || !fs || !ex->source_frame || ex->chunked) return;
    struct Size *e = D.last;
    const VkExecTarget *t = &ex->color[0];
    if (!e || e->fg_record.frame_id != ex->source_frame || e->fg_copied == ex->source_frame ||
        ex->pass_ncolor != 1 || !t->image || t->width != e->gw || t->height != e->gh ||
        !scene_color(t->format) || t->samples > 1 || t->is3d || t->level || t->base_layer || ex->pass_layers != 1) return;
    uint64_t hash = spirv_hash(fs);
    int known = 0;
    for (size_t i = 0; i < sizeof FXAA_FS / sizeof FXAA_FS[0]; i++) known |= hash == FXAA_FS[i];
    if (!known) return;
    unsigned output[2];
    if (!vk_present_fg_output(e->gw, e->gh, output) || output[0] != e->fw || output[1] != e->fh) return;
    VkCtx *c = vk_ctx();
    VkFormatProperties src, dst;
    vkGetPhysicalDeviceFormatProperties(c->phys, t->format, &src);
    vkGetPhysicalDeviceFormatProperties(c->phys, VK_FORMAT_R8G8B8A8_SRGB, &dst);
    if (!(src.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !(dst.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) return;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!e->fg_srgb && !image2d(e->gw, e->gh, VK_FORMAT_R8G8B8A8_SRGB, usage,
        &e->fg_srgb, &e->fg_srgb_mem, &e->fg_srgb_view)) return;
    if (!e->fg_color && !image2d(e->gw, e->gh, VK_FORMAT_R8G8B8A8_UNORM, usage,
        &e->fg_color, &e->fg_color_mem, &e->fg_color_view)) return;
    // The known final scaling/FXAA draw is complete; overlay draws have not run.
    // End rendering and restore feedback-loop targets before reading this image.
    vk_exec_barrier(ex);
    VkCommandBuffer cb = ex->cb;
    rsf_layout(cb, t->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageLayout old = e->fg_copied ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    rsf_layout(cb, e->fg_srgb, old, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    rsf_layout(cb, e->fg_color, old, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffsets = {{0, 0, 0}, {(int)e->gw, (int)e->gh, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets = {{0, 0, 0}, {(int)e->gw, (int)e->gh, 1}}};
    // Vulkan encodes linear RGB when blitting to an sRGB format. Copying those
    // bytes into UNORM then preserves the encoding expected by the FG service.
    vkCmdBlitImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, e->fg_srgb,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    rsf_layout(cb, e->fg_srgb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageCopy copy = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {e->gw, e->gh, 1}};
    vkCmdCopyImage(cb, e->fg_srgb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, e->fg_color,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    rsf_layout(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    rsf_layout(cb, e->fg_srgb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    rsf_layout(cb, e->fg_color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    e->fg_copied = ex->source_frame;
    rsf_fgvk_frame frame = {.struct_size = sizeof frame, .abi_version = RSF_FGVK_ABI_VERSION, .record = &e->fg_record,
        .hudless = rsf_image(e->fg_color, VK_FORMAT_R8G8B8A8_UNORM, e->gw, e->gh, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)};
    rec_drain();
    int result = vk_present_fg_hudless(cb, &frame);
    rsf_layout(cb, e->fg_color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vk_exec_draw_new_cb(ex);
    vk_exec_pass_begin(ex);   // load the existing target for the next native draw
    static uint64_t copied, refused;
    if (!result) {
        if (++copied == 1 || !(copied % 300))
            rt_log("rsf: FG HUD-less %llu source %llu, %ux%u encoded, boundary %016llx",
                (unsigned long long)copied, (unsigned long long)ex->source_frame, e->gw, e->gh, (unsigned long long)hash);
    } else if (++refused == 1 || !(refused % 300))
        rt_log("rsf: FG HUD-less refused (%d) source %llu (%llu refusals)", result,
            (unsigned long long)ex->source_frame, (unsigned long long)refused);
}

int vk_dlaa_skip_draw(VkExec *ex, const GfxShader *fs) {
    static uint64_t frame_read = ~0ull;
    static int fxaa = 1;
    // fxaa = 0 reports once whether it found the game's FXAA draw: the first skip, or a minute of frames (3600) with
    // none, which is what a translator change that moves FXAA_FS looks like on every GPU, with or without DLSS
    static uint64_t fx_off_frames;
    static int fx_found, fx_reported;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f != frame_read) {   // once a frame: the menu switches it live
        frame_read = f, fxaa = cfg_bool("TOTK_FXAA");
        if (!fxaa && !fx_found && !fx_reported && ++fx_off_frames == 3600) {
            fx_reported = 1;
            rt_log("dlaa: fxaa = 0 found no draw of the game's FXAA in %llu frames (its fragment shader is not in "
                   "FXAA_FS): the FXAA stays on", (unsigned long long)fx_off_frames);
        }
    }
    if (fxaa && !D.up) return 0;   // nothing to skip: no shader lookup per draw at the defaults
    if (!fs) return 0;
    // 1.2.1 scales the light target to the output with an upscaler of its own (12 texelFetch taps around the source
    // pixel, fragment shader e6f7ae35b8479ef3 with this build's translator), not with the FXAA one, in a pass without
    // depth. It addresses its source in the scene's pixels (constant bank 3), so DLSS's output-sized picture swapped in
    // (tex_info) showed only its top-left part, the scene's share of the output (two thirds at DLSS quality), over
    // the whole screen (#1106). DLSS already wrote its picture into that pass's target
    // (evaluate), so on 1.2.1 a draw there that samples the light target is skipped instead; the HUD drawn later into
    // the same target samples other textures and stays. On 1.4.3 a tonemap can be the draw that scales (the comment
    // above struct D), and it keeps the swap.
    if (D.up && !RT_V143 && !ex->depth.image && vk_dlaa_swap_img && ex->pass_w > vk_dlaa_swap_img->width + 1 &&
        vk_exec_draw_samples(ex, fs, vk_dlaa_swap_img)) {
        static int logged;
        if (!logged) logged = 1, rt_log("dlaa: DLSS replaces the game's scaling draw (fragment shader %016llx)",
                                        (unsigned long long)spirv_hash(fs));
        return 1;
    }
    uint64_t h = spirv_hash(fs);
    int known = 0;
    for (size_t i = 0; i < sizeof FXAA_FS / sizeof FXAA_FS[0]; i++) known |= h == FXAA_FS[i];
    if (!known) return 0;
    if (!ex->depth.image) return D.up;   // the scaling pass, which DLSS replaced
    if (!fxaa && !fx_found) {
        fx_found = 1;
        rt_log("dlaa: fxaa = 0 skips the game's FXAA (fragment shader %016llx)", (unsigned long long)h);
    }
    return !fxaa;
}

