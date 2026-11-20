// AMD FSR 3 frame generation (vk_fg.cpp): the FidelityFX SDK 1.1.4's frame interpolation and optical flow
// (third_party/FidelityFX, MIT) on its Vulkan backend. Plain C interface: the SDK is C++.
//
// The frame's inputs come from the replay (vk_dlaa.c follows the scene for it, with DLSS off): at the scene's end the
// camera motion vectors and the SDK's prepare step (vk_fg_prepare), before the HUD's first draw the picture without
// the HUD (vk_fg_hudless), at the end of the submission that copied the picture into the window texture the optical
// flow and the interpolation (vk_fg_generate). All of it goes into the replay's command buffer.
#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// The frame generator frame_gen names (TOTK_FRAME_GEN, live: read again after a settings change): off, or AMD FSR 3 on
// the FidelityFX SDK. The one place that reads the string; an unknown value is off.
typedef enum { VK_FG_OFF, VK_FG_FSR3 } VkFgEngine;
VkFgEngine vk_fg_engine(void);
// vk_device.c, after the device exists: with frame_gen = fsr3 makes the shared backend, the optical flow and the
// frame interpolation contexts and logs one line; otherwise does nothing, and vk_fg_wanted makes them at the first
// frame after the menu turns frame generation on. Turned off, the contexts stay until exit (unused).
void vk_fg_init(void);
// 1 while the contexts exist and frame_gen names a generator.
int vk_fg_active(void);
// vk_device.c destroy_all, before the device goes away; harmless when nothing was made.
void vk_fg_release(void);

// vk_dlaa.c vk_dlaa_frame, once per present: 1 when frame generation follows the scene in the next frame. 0 while
// DLSS runs (dlss), with replay workers or sync_ahead = 1 (the inputs are per frame and recorded in replay order),
// with HDR output, or while the game shows a menu without its world (vk_fg_menu); each reason is logged once.
int vk_fg_wanted(int dlss);
// vk_dlaa.c, the game's thread at each present: 1 while the map or a loading or event fade is open (sampled there,
// read by vk_fg_wanted).
void vk_fg_menu(int open);

// What the scene end hands over: this frame's depth and camera motion, and the camera.
typedef struct {
    VkImage depth, mv;          // D32F scene depth (level 0, layer 0) and RG16F camera motion, w x h, GENERAL: last
                                // frame's window position minus this frame's, scene pixels (vk_dlaa.c mv_pass)
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
// The picture before the HUD: src (level, layer) copied, sRGB-encoded as the game's last draw encodes it into the
// window texture, into an RGBA8 image of the SDK's. No pass open, earlier writes visible (vk_exec_barrier).
int vk_fg_hudless(VkCommandBuffer cb, VkImage src, VkFormat fmt, uint32_t level, uint32_t layer, uint32_t w, uint32_t h);
// The optical flow and the interpolation into cb, after the frame's last draw into the window texture win (fmt, w x h,
// GENERAL, level 0); fence: the submission's timeline value, frame: the present this frame will be (TOTK_FG_SHOT).
// 1: a frame between the last one and this was made.
int vk_fg_generate(VkCommandBuffer cb, uint64_t fence, uint64_t frame, VkImage win, VkFormat fmt, uint32_t w,
                   uint32_t h);
// GPU timestamps of the frame's steps (TOTK_FG_TIMES, default on with frame generation): FG_T_MV0 resets the frame's
// slot, so it comes first in the frame.
enum { FG_T_MV0, FG_T_MV1, FG_T_PREP0, FG_T_PREP1, FG_T_HL0, FG_T_HL1, FG_T_FI0, FG_T_FI1, FG_T_N };
void vk_fg_mark(VkCommandBuffer cb, int which);
void vk_fg_cpu(uint64_t ns);   // replay-thread time the frame generation recording took, added to the frame's
// vk_dlaa.c vk_dlaa_frame, each present before the frame state is cleared: the frame's counts and times.
void vk_fg_frame(void);
void vk_fg_exit_log(void);   // vk_backend.c gfx_exit_log: the run's counts and times, once
// vk_backend.c present_image (testing, TOTK_FG_TEST): 1 = the frame's generated picture, 2 = its picture before the
// HUD in place of the window texture (*img, *fmt). 0: the present is left as it is.
int vk_fg_present_swap(VkImage *img, VkFormat *fmt);
// vk_backend.c present_image, after vk_dlaa_frame: the picture generated between the last frame and the one just
// ended, when presented is the window texture it was generated against (RGBA8 holding the window texture's bytes,
// GENERAL, its size), else VK_NULL_HANDLE (none made, a test swap, or another texture presented). It stays unwritten
// until the replay of the frame after next: its blit is on the queue before the next frame's submissions.
VkImage vk_fg_present_gen(VkImage presented);

#ifdef __cplusplus
}
#endif
