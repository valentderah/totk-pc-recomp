// AMD FSR upscaling (vk_ffx.c) for the DLAA path (vk_dlaa.c): with totk.ini dlaa = 1 and dlaa_backend = fsr, the
// scene that DLSS would get goes to AMD's FSR (FSR 4 where the GPU has it, FSR 3.1 elsewhere) through
// amd_fidelityfx_loader_vk.dll next to the executable. Windows only; elsewhere every call does nothing.
#pragma once
#include "vk.h"

int vk_ffx_wanted(void);   // dlaa and dlaa_backend = fsr at start (the device may still lack FSR)
// vk_device.c, while it builds the device: the extensions FSR's shaders use (cooperative matrix, 8-bit floats) join
// the list when the device has them; their feature structures are chained there.
void vk_ffx_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max);
void vk_ffx_init(void);      // after the device exists: loads the loader and picks the upscaler version
int vk_ffx_available(void);  // vk_ffx_init found an upscaler for this device

typedef struct {   // one image FSR reads or writes, in VK_IMAGE_LAYOUT_GENERAL, level 0 and layer 0
    VkImage image;
    VkFormat format;
    uint32_t width, height;
} VkFfxImage;

typedef struct {
    VkFfxImage color, depth, mv, reactive, out;   // reactive.image VK_NULL_HANDLE: none
    float jx, jy;               // this frame's jitter, render pixels (window x right, y down), as the scene was drawn
    float dt_ms;                // time since the previous dispatch
    float cam_near, cam_far;    // view-space distances of the depth range's ends
    float fov_y;                // vertical field of view, radians
    int reset;                  // a new history
} VkFfxDispatch;

// A context for one render and output size (vk_dlaa.c keeps one per scene size like DLSS's features). depth_inverted:
// depth 1 is near; depth_infinite: the far plane is at infinity. NULL on failure (logged).
void *vk_ffx_create(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, int depth_inverted, int depth_infinite);
// Records the upscaling into cb. 0 on failure (logged). FSR binds pipelines and descriptors of its own.
int vk_ffx_dispatch(void *ctx, VkCommandBuffer cb, const VkFfxDispatch *d);
void vk_ffx_release(void *ctx);
const char *vk_ffx_version(void);   // the upscaler version in use, for the log ("" before vk_ffx_init)
