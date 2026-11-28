// AMD FSR 4 (vk_ffx.c), the fourth engine of vk_dlaa.c's scene upscaler (scene_upscaler = fsr4): the frame DLSS and
// FSR 3.1 get goes to FSR 4 through AMD's FSR SDK 2.4 signed libraries, amd_fidelityfx_loader_vk.dll and
// amd_fidelityfx_upscaler_vk.dll next to the executable. On Linux fsr4-vk's amd_fidelityfx_upscaler_vk.so stands in.
#pragma once
#include "vk.h"

// FSR 4 is the scene upscaler picked at start (vk_dlaa.c vk_upscaler_engine); the device may still lack it.
int vk_ffx_wanted(void);
// vk_device.c, while it builds the device: the extensions FSR 4's shaders use (cooperative matrix, 8-bit floats) join
// the list when the device has them; their feature structures are chained there.
void vk_ffx_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max);
void vk_ffx_init(void);      // vk_dlaa.c vk_dlaa_init, after the device exists: loads the loader and pins FSR 4
int vk_ffx_tried(void);      // vk_ffx_init ran (FSR 4 picked at start, on Windows)
int vk_ffx_available(void);  // vk_ffx_init found FSR 4 for this device
#ifndef _WIN32
// The path of fsr4-vk's amd_fidelityfx_upscaler_vk.so: TOTK_FSR4_LIB, else next to the executable. 0 when no file is there
int vk_ffx_so_path(char *out, size_t cap);
#endif

typedef struct {   // one image FSR reads or writes, in VK_IMAGE_LAYOUT_GENERAL, level 0 and layer 0
    VkImage image;
    VkFormat format;
    uint32_t width, height;
} VkFfxImage;

typedef struct {
    VkFfxImage color;           // the scene's light (R11G11B10F or RGBA16F, linear), the render size
    VkFfxImage depth;           // its D32F depth, standard direction (0 near), finite far plane
    VkFfxImage mv;              // RG16F motion vectors as vk_fsr3.h Fsr3Frame.mv (render pixels, without the jitter)
    VkFfxImage out;             // RGBA16F with storage, the output size
    float jx, jy;               // this frame's jitter, render pixels (window x right, y down), as the scene was drawn
    float dt_ms;                // time since the previous dispatch
    float cam_near, cam_far;    // the camera's planes (m)
    float fov_y;                // vertical field of view, radians
    float sharpness;            // FSR's own sharpening pass (RCAS) on its output, 0..1; 0: none
    int reset;                  // a new history
    uint64_t fence;             // the submission's timeline value (VkExec.fence)
} VkFfxDispatch;

// Records FSR 4 into cb (every image in GENERAL, left so; no render pass open; recorder drained by the caller). The
// context is made at the first call and again when the output size changes; the scene may be any size up to the
// output. 0 when it failed (logged; FSR 4 stays off for the run). FSR binds pipelines and descriptors of its own.
int vk_ffx_dispatch(VkCommandBuffer cb, const VkFfxDispatch *d);
void vk_ffx_release(void);          // vk_device.c destroy_all, before the device goes away; harmless when nothing was made
const char *vk_ffx_version(void);   // "FSR 4.1.2" once vk_ffx_init pinned it, "" before or without
