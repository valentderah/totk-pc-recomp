// Frame generation (vk_fg.cpp): AMD FSR 3's interpolation and optical flow on its Vulkan backend, or NVIDIA DLSS Frame
// Generation through the driver's NGX core. Plain C interface: the FidelityFX SDK is C++.
//
// The frame's inputs come from the replay (vk_dlaa.c follows the scene for it, with DLSS off): at the scene's end the
// camera motion vectors and the SDK's prepare step (vk_fg_prepare), before the HUD's first draw the picture without
// the HUD (vk_fg_hudless), at the end of the submission that copied the picture into the window texture the optical
// flow and the interpolation (vk_fg_generate). The default path records all of it in the replay's command buffer.
// dlssg_async copies the DLSS Frame Generation inputs into a rotating set of private images and evaluates them on a
// second graphics queue after the real frame is submitted, so the next real frame can draw at the same time.
#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// The frame generator frame_gen names (TOTK_FRAME_GEN, live: read again after a settings change): off, AMD FSR 3 or
// NVIDIA DLSS Frame Generation. The one place that reads the string; an unknown value is off.
typedef enum { VK_FG_OFF, VK_FG_FSR3, VK_FG_DLSSG } VkFgEngine;
#define VK_FG_MAX_GENERATED 3
VkFgEngine vk_fg_engine(void);
// Pictures generated between two pictures the game draws: FSR 3 is one; DLSS FG follows dlssg_frames and the maximum
// the GPU and feature report. 0 while frame generation is off.
int vk_fg_generated_count(void);
// The extra swapchain images the present needs, and the live signal that a rebuilt swapchain is required.
int vk_fg_swapchain_extra(void);
// vk_device.c, after the device exists: with frame_gen = fsr3 makes the shared backend, the optical flow and the
// frame interpolation contexts and logs one line; otherwise does nothing, and vk_fg_wanted makes them at the first
// frame after the menu turns FSR 3 on. DLSS FG's NGX feature is made lazily at its scene size. Turned off, contexts
// stay until exit (unused).
void vk_fg_init(void);
// 1 while the contexts exist and frame_gen names a generator.
int vk_fg_active(void);
// vk_device.c destroy_all, before the device goes away; harmless when nothing was made.
void vk_fg_release(void);

// vk_dlaa.c vk_dlaa_frame, once per present: 1 when frame generation follows the scene in the next frame. FSR 3 is off
// while the scene upscaler runs; DLSS FG works with DLSS, but neither generator runs with FSR or HDR, with replay
// workers. DLSS FG additionally needs async replay while sync_ahead is on. Also off while the game shows a menu without
// its world (vk_fg_menu); each reason is logged once.
int vk_fg_wanted(int dlss, int fsr);
// vk_dlaa.c, the game's thread at each present: 1 while the map or a loading or event fade is open (sampled there,
// read by vk_fg_wanted).
void vk_fg_menu(int open);

// What the scene end hands over: this frame's depth and camera motion, and the camera.
typedef struct {
    VkImage depth, mv;          // D32F scene depth (level 0, layer 0) and RG16F camera motion, w x h, GENERAL: last
                                // frame's window position minus this frame's, scene pixels (vk_dlaa.c mv_pass)
    VkImageView depth_view, mv_view;   // views of the same images for DLSS Frame Generation
    VkFormat depth_format;      // the scene depth's format
    uint32_t w, h;              // the scene's size
    uint32_t ow, oh;            // the output's (the window texture's)
    float near_m, far_m, fov_y; // the camera's planes (far 0: none) and vertical field of view (radians)
    int inverted, infinite;     // device depth 1 is the near plane; the far plane is at infinity
    float pos[3], up[3], right[3], fwd[3];   // the camera in world space
    int reset;                  // no history: no camera motion this frame, or a cut (vk_fg_camera)
} VkFgScene;
// The camera from the scene's uniform block rows 0-10 (view 3x4, view-projection, projection, row-major, as in
// vk_dlaa.c note_camera's block), the NVN depth mode (minus_w: clip z -w..w) and the viewport depth range dr: fills s's
// camera fields. Returns 1 at a cut against the last call (over 10 m or 35 degrees, as Savage Wind's frame
// generation tests it), else 0.
int vk_fg_camera(const float *rows, int minus_w, const float dr[2], VkFgScene *s);
// Records the SDK's prepare (dilated depth and motion) into cb, recorder drained here. Makes the contexts again when
// the sizes, the depth convention or the window texture's format changed. 1: done; 0: frame generation off this
// frame (logged).
int vk_fg_prepare(VkCommandBuffer cb, const VkFgScene *s);
// DLSS FG needs no interpolation context or prepare dispatch. vk_dlaa.c calls this at the scene's end after its motion
// vectors and camera are ready; the frame keeps those inputs until the window texture is drawn.
void vk_fg_dlssg_scene(const VkFgScene *s);
// The picture before the HUD: src (level, layer) copied, sRGB-encoded as the game's last draw encodes it into the
// window texture, into an RGBA8 image of the SDK's. No pass open, earlier writes visible (vk_exec_barrier).
int vk_fg_hudless(VkCommandBuffer cb, VkImage src, VkFormat fmt, uint32_t level, uint32_t layer, uint32_t w, uint32_t h);
// The optical flow and the interpolation into cb, after the frame's last draw into the window texture win (fmt, w x h,
// GENERAL, level 0); fence: the submission's timeline value, frame: the present this frame will be (TOTK_FG_SHOT).
// 1: a frame between the last one and this was made.
int vk_fg_generate(VkCommandBuffer cb, uint64_t fence, uint64_t frame, VkImage win, VkImageView win_view,
                   VkFormat fmt, uint32_t w, uint32_t h);
// vk_backend.c submit_sync, immediately after the submission that armed a DLSSG picture: records and submits the
// evaluation on the second graphics queue, waiting for the real frame's timeline value. Returns its timeline value.
uint64_t vk_fg_async_submit(uint64_t ready);
// Wait for a DLSSG timeline value before reusing the image set that produced it.
void vk_fg_async_wait(uint64_t value);
// vk_backend.c submit_sync before vk_sync_submit: waits outside the submission lock before a recorded frame may reuse
// the image set its previous generated pictures still read or present.
void vk_fg_wait_reuse(void);
// The generated picture's queue value and rotating slot. slot is -1 when presented is not a DLSSG frame.
uint64_t vk_fg_present_state(VkImage presented, int *slot);
// The present thread after submitting the generated and real pictures: their timeline value protects the slot from
// reuse until the generated pictures have also finished being read by the present.
void vk_fg_present_submitted(int slot, uint64_t value);
// GPU timestamps of the frame's steps (TOTK_FG_TIMES, default on with frame generation): FG_T_MV0 resets the frame's
// slot, so it comes first in the frame.
enum { FG_T_MV0, FG_T_MV1, FG_T_PREP0, FG_T_PREP1, FG_T_HL0, FG_T_HL1, FG_T_FI0, FG_T_FI1, FG_T_N };
void vk_fg_mark(VkCommandBuffer cb, int which);
void vk_fg_cpu(uint64_t ns);   // replay-thread time the frame generation recording took, added to the frame's
// vk_dlaa.c vk_dlaa_frame, each present before the frame state is cleared: the frame's counts and times.
void vk_fg_frame(void);
void vk_fg_exit_log(void);   // vk_backend.c gfx_exit_log: the run's counts and times, once
// vk_backend.c present_image (testing, TOTK_FG_TEST): 1 = the frame's first generated picture, 2 = its picture before
// the HUD in place of the window texture (*img, *fmt). 0: the present is left as it is.
int vk_fg_present_swap(VkImage *img, VkFormat *fmt);
// Nonzero while present_swap put a linear DLSS Frame Generation picture in place of the game's: it needs the same sRGB
// encoding as the generated pictures shown between real frames. FSR 3's output is already in display space.
int vk_fg_present_linear(void);
// vk_backend.c present_image, after vk_dlaa_frame: the pictures generated between the last frame and the one just
// ended, in display order, when presented is the window texture they were generated against (RGBA8 holding the window
// texture's bytes, GENERAL, its size). Returns their count; 0 means none (a test swap or another texture presented).
int vk_fg_present_gens(VkImage presented, VkImage out[VK_FG_MAX_GENERATED]);
// Format of the images returned by vk_fg_present_gens (RGBA or BGRA UNORM, matching the window texture).
VkFormat vk_fg_present_format(void);

#ifdef __cplusplus
}
#endif
