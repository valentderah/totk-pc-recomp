// DLAA (vk_dlaa.c): NVIDIA DLSS at the scene's own size, behind TOTK_DLAA (totk.ini dlaa = 1).
#pragma once
#include "vk.h"

// The narrowest scene the DLSS pass and the motion passes recognise by its D32F depth: Ultra Performance at a 1080p
// output renders 640 wide, smaller windows less.
#define DLAA_SCENE_MIN_W 320u

int vk_dlaa_wanted(void);   // the setting is on (the device may still lack DLSS)
// vk_device.c: NGX's instance and device extensions join the lists (room for max names); vk_dlaa_init after the
// device exists brings NGX up and checks that DLSS is available.
void vk_dlaa_instance_exts(const char **ext, uint32_t *n, uint32_t max);
void vk_dlaa_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max);
void vk_dlaa_init(void);
// 1 when dlaa is on and NGX came up with DLSS (after vk_dlaa_init): a DLSS mode's smaller scene is scaled up by DLSS.
int vk_dlaa_available(void);
// vk_exec.c vk_exec_pass_begin, once the pass's targets are resolved and before its barrier: arms the frame on the
// scene's light target or, at the first quarter-width pass after it, runs DLAA on that target. Each present ends the
// frame (vk_dlaa_frame).
typedef struct VkExec VkExec;
typedef struct VkExecJob VkExecJob;
void vk_dlaa_pass(VkExec *ex);
// The same decision for a chunked replay's scan, in replay order: 1 with the record due before this pass (j), which
// vk_dlaa_exec makes on the chunk that holds the op (vk_exec.c run_job).
int vk_dlaa_plan(const VkExec *ex, VkExecJob *j);
void vk_dlaa_exec(VkExec *ex, const VkExecJob *j);
void vk_dlaa_frame(void);
// vk_exec_draw.c, each draw before its fixed state is recorded: notes the scene camera block (vk_dlaa_draw), and moves
// the viewport of the scene's draws by the frame's jitter (vk_dlaa_viewport, when the viewport is recorded).
void vk_dlaa_draw(VkExec *ex);
void vk_dlaa_scan_draw(const VkExec *ex);   // a chunked replay's scan, at each draw: the camera note of vk_dlaa_draw
void vk_dlaa_viewport(VkExec *ex, VkViewport *v);
// vk_mvsurvey.c (TOTK_MV_SURVEY): notes the uniform blocks of each scene draw, beside vk_dlaa_draw; each present
// compares them with the previous frames' and logs a summary now and then.
void vk_mvsurvey_draw(VkExec *ex);
void vk_mvsurvey_frame(void);
void vk_mvprev_draw(VkExec *ex);   // vk_mvprev.c: notes the scene draw's object and bone blocks
// Per-object motion (vk_exec_draw.c): the moving scene draws are listed again during the replay with
// their vertex shader's last-frame variant; vk_dlaa.c mv_pass records them over the camera motion into ex->cb (and
// leaves every binding of the replay to be set again; draws whose camera block is not cam are left out).
// vk_dlaa_prev_camera: last frame's camera block and this frame's jitter, 0 when there is no history.
int vk_mvobj_execute(VkExec *ex, uint64_t cam, VkImageView mv, VkImageView depth, VkFormat depth_fmt, uint32_t w,
                     uint32_t h);
int vk_dlaa_prev_camera(VkDescriptorBufferInfo *out, float jitter[2]);
// vk_mvcheck.c (TOTK_MV_CHECK, test): mv_pass hands it the motion image after the camera pass and, after the object
// pass, the motion image again and the scene light target it ran on; it measures how well the vectors fit the picture.
void vk_mvcheck_camera(VkCommandBuffer cb, VkImage mv, uint32_t w, uint32_t h, float jx, float jy);
void vk_mvcheck_final(VkCommandBuffer cb, uint64_t fence, VkImage mv, VkImage cimg, VkFormat cfmt, uint32_t level,
                      uint32_t layer, uint32_t w, uint32_t h, float jx, float jy);
const char *vk_mvobj_frame_masks(void);   // vk_exec_draw.c: the last motion list, per position and changed banks
void vk_mvcheck_output(VkCommandBuffer cb, VkImage out, uint32_t ow, uint32_t oh);   // after DLSS on a check frame
// 1 while the scene is followed and its motion vectors are made: DLSS runs, or the motion blur is on without it
// (vk_dlaa.c blur_only). vk_usnap.c then keeps a host copy of the uniform snapshot and vk_mvprev.c last frame's blocks.
int vk_dlaa_motion(void);
// vk_mblur.c (TOTK_MOTION_BLUR). vk_mblur_wanted: the setting is on and its passes can run on this device (read at each
// present). vk_mblur_apply blurs the picture *pic_view (ow x oh: DLSS's output, or the scene's light target without
// DLSS) along the motion vectors mv (w x h, scene pixels) with the scene depth; inv_w gives a pixel's distance (row 3
// of the inverse view-projection). 1: *pic and *pic_view now name the blurred picture, in the top left ow x oh of an
// image that may be larger unless exact is set (for a view sampled by normalised coordinates), valid until the next
// call; 0: the setting is off or the pass cannot run, and they are unchanged.
int vk_mblur_wanted(void);
int vk_mblur_apply(VkExec *ex, VkImage *pic, VkImageView *pic_view, uint32_t ow, uint32_t oh, VkImageView mv,
                   VkImageView depth, uint32_t w, uint32_t h, const double inv_w[4], int exact);
// vk_fsr3.c (TOTK_FSR3): AMD FSR 3.1 as the scene's temporal upscaler where DLSS does not run, through AMD's library
// next to the executable. vk_fsr3_wanted: the setting, read at start. vk_fsr3_available: wanted and the library
// loaded (at the first call). vk_fsr3_upscale records one frame
// into cb (the recorder drained): the images in GENERAL, first level and layer; the jitter and vectors as DLSS takes
// them; 0: failed, to be taken as off for the run.
typedef struct {
    VkImage color, depth, mv, mask, out;   // mask: FSR's reactive mask (R8), or none
    VkFormat color_fmt;                    // R11G11B10F or RGBA16F; depth is D32F, mv RG16F, out RGBA16F
    uint32_t w, h, ow, oh;                 // the scene's size and the picture's
    float jx, jy;                          // the jitter the scene's draws took, pixels
    int reset;                             // no history carries over
    float dt_ms, near_m, far_m, fov_y;     // frame time; the camera's planes and vertical field of view (radians)
    int inverted;                          // depth 1 is the near plane
    float sharpness;                       // 0..1, 0: no sharpening pass
} VkFsr3Frame;
int vk_fsr3_wanted(void);
int vk_fsr3_available(void);
int vk_fsr3_upscale(VkCommandBuffer cb, const VkFsr3Frame *f);
// vk_exec_draw.c, once a draw's fragment shader is known: 1 skips the draw (the game's FXAA while TOTK_FXAA is 0, its
// scaling draw when DLSS wrote the scaled picture).
int vk_dlaa_skip_draw(VkExec *ex, const GfxShader *fs);
