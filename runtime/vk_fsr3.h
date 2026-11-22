// AMD FSR 3.1 temporal upscaler (vk_fsr3.cpp): the FidelityFX SDK 1.1.4's upscaler (third_party/FidelityFX, MIT) on
// its Vulkan backend, the other engine of vk_dlaa.c's DLSS point. The same scene light, depth, motion vectors and
// jitter go to FSR 3.1 instead of NGX, on any GPU the back end runs on. Plain C interface: the SDK is C++.
#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    VkImage color;              // the scene's light (level 0, layer 0; R11G11B10F or RGBA16F, linear), w x h
    VkFormat color_format;
    VkImage depth;              // its D32F depth (level 0, layer 0), w x h, standard direction (0 near)
    VkImage mv;                 // RG16F motion vectors, w x h: last frame's window position minus this frame's, render
                                // pixels, without the jitter (vk_dlaa.c mv_pass, with the per-object pass)
    VkImage out;                // RGBA16F with storage, ow x oh: the upscaled scene
    uint32_t w, h, ow, oh;
    float jx, jy;               // the jitter the scene was drawn with, render pixels (x right, y down)
    float near_m, far_m, fov_y; // the camera's planes (m) and vertical field of view (radians)
    float dt_ms;                // time since the last upscaled frame
    int reset;                  // no usable history (a cut, a new size, the first frame)
    float sharpness;            // FSR 3.1's own sharpening (RCAS) on its output, 0..1; 0 leaves the pass out
    uint64_t fence;             // the submission's timeline value (VkExec.fence)
} Fsr3Frame;

// Records FSR 3.1 into cb (every image in VK_IMAGE_LAYOUT_GENERAL, left so; no render pass open; recorder drained by the
// caller). The context is made at the first call and made again when the output size changes; the scene may be any
// size up to the output (the modes, the dynamic resolution). 0 when it failed (logged; FSR 3.1 stays off for the run).
int fsr3_dispatch(VkCommandBuffer cb, const Fsr3Frame *f);
// vk_device.c destroy_all, before the device goes away; harmless when nothing was made.
void fsr3_release(void);
// vk_fg.cpp ffxGetPermutationBlobByIndex for FFX_EFFECT_FSR3UPSCALER: the blob of pass (FfxFsr3UpscalerPass) for the
// SDK's permutation options into out (an FfxShaderBlob). 0: no shader for that pass.
int vk_fsr3_blob(uint32_t pass, uint32_t options, void *out);

#ifdef __cplusplus
}
#endif
