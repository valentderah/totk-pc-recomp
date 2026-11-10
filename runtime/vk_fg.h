// Frame generation (TOTK_FRAME_GEN, totk.ini frame_gen): vk_fg.c. See there for what it does and where it runs.
#pragma once
#include "vk.h"
#include "vk_exec.h"

// The setting is on and the library is there (vk_ffx.h); read at each present. Not with HDR output.
int vk_fg_wanted(void);

// ---- the replay (vk_dlaa.c), once a frame, where the game is about to draw its HUD
typedef struct {
    const VkExecTarget *target;          // the game's float target the HUD goes into: the frame's picture so far
    int have_mv;                         // the frame has the scene's motion (else: only the picture is kept)
    VkImage depth, mv;                   // the scene's depth (D32F) and motion vectors (RG16F), w x h, in GENERAL: as
    uint32_t w, h;                       // FSR 3 takes them (vk_dlaa.h VkFsr3Frame)
    float jx, jy;                        // the jitter the scene's draws took, pixels
    float near_m, far_m, fov_y;          // the camera's planes and vertical field of view (radians)
    int inverted;                        // depth 1 is the near plane
    int reset;                           // the scene's motion does not continue the frame before
} VkFgScene;
// Copies the picture and generates the one halfway from the last frame's to it, into ex->cb (the recorder drained, no
// pass open).
void vk_fg_scene(VkExec *ex, const VkFgScene *s);

// ---- the present
typedef struct {
    VkImage target;                      // the float target the pictures were taken from
    VkImageView before_hud, generated;   // its copy from before the HUD; the generated picture (RGBA16F, w x h)
    uint32_t w, h;
} VkFgFrame;
// The thread that queues the present, after the frame's replay: 1 with *f when this frame has a generated picture.
int vk_fg_take(VkFgFrame *f);
// The present thread, into the present's command buffer: the picture to show before this frame's, with this frame's
// HUD (fg_compose.comp); pic is the float target at the frame's end and win the window texture, both w x h and in
// GENERAL. An RGBA8 image in GENERAL holding what the present's blit makes of a window texture, valid until the next
// call; VK_NULL_HANDLE: not this time.
VkImage vk_fg_compose(VkCommandBuffer cb, unsigned slot, const VkFgFrame *f, VkImage pic, VkFormat pic_fmt, VkImage win,
                      VkFormat win_fmt);
// gfx_present.c, the present thread: vk_present_blit for a frame with a generated picture. 1: two blits wait for
// vk_present_flip, the generated picture's and then the frame's (src: the window texture, in GENERAL); 0: the generated
// picture could not be made and at most the frame's blit waits.
int vk_present_blit_fg(const VkFgFrame *f, VkImage pic, VkFormat pic_fmt, VkImage src, VkFormat fmt, int w, int h,
                       const int crop[4]);
void vk_fg_release(void);   // before the device goes away
