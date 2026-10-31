// AMD FSR 3.1 temporal upscaler (vk_fsr3.c), the other engine of vk_dlaa.c's DLAA / DLSS point: the same scene colour,
// depth, motion vectors and jitter go to FSR 3.1.5 instead of NGX, on any GPU the back end runs on.
#pragma once
#include <stdint.h>

#include "vk.h"

typedef struct Fsr3 Fsr3;

// One context per scene size w x h and output size ow x oh, kept by vk_dlaa.c with that size's work images: mv (RG16F
// motion vectors at the scene size, render pixels, this frame's position to last frame's) and out (RGBA16F at the
// output size), both in VK_IMAGE_LAYOUT_GENERAL. NULL when the context cannot be made; the reason is logged.
Fsr3 *fsr3_create(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, VkImage mv, VkImage out);
// After the GPU finished every frame recorded with it (vk_dlaa.c free_size waits for the device first).
void fsr3_destroy(Fsr3 *f);

typedef struct {
    VkImage color, depth;   // the scene's colour (mip 0, layer 0; any float colour format a blit reads) and its D32F
                            // depth, both in VK_IMAGE_LAYOUT_GENERAL
    float jx, jy;           // the jitter the scene was drawn with, render pixels
    int reset;              // start a new history
    uint64_t frame;         // the submission's timeline value (VkExec.fence); retired once vk_sync_fence_done reaches it
} Fsr3Frame;

// Records the upscale into cb, writing the context's out image (no render pass open; the recorder drained). 1 on success.
int fsr3_dispatch(Fsr3 *f, VkCommandBuffer cb, const Fsr3Frame *in);
