// AMD FidelityFX Super Resolution 3.1 as the scene's temporal upscaler and anti-aliasing (TOTK_FSR3, totk.ini fsr3):
// what DLSS does on NVIDIA RTX GPUs (vk_dlaa.c), on any GPU, from the same inputs. vk_dlaa.c follows the frame, makes
// the jitter and the motion vectors and decides where the upscaler runs; this file is the upscaler behind it.
//
// AMD's prebuilt, signed library is loaded at run time from the executable's folder: amd_fidelityfx_vk.dll of the
// FidelityFX SDK 1.1.4 (PrebuiltSignedDLL), the last SDK release with a Vulkan build. There is no build of it for
// Linux, so the setting does nothing there. Nothing of the SDK is compiled in; the structures of its C interface come
// from three of its headers, kept unmodified in runtime/third_party/ffx_api (MIT), and the Vulkan backend's
// description, which its C++-only header declares, is repeated below.
//
// The library records its passes into the replay's command buffer, as NGX does, so vk_dlaa.c drains the recorder
// before the call. It takes the game's images as they are: every image of the back end is kept in GENERAL, which is
// the layout the library's Vulkan backend maps the states COMMON and UNORDERED_ACCESS to (sdk/src/backends/vk/ffx_vk.cpp
// getVKImageLayoutFromResourceState), and it returns each image to the state it was given in
// (UnregisterResourcesVK). It chooses its shader variants by what the physical device supports (16-bit floats),
// which is what this device enables: vk_device.c turns on every core feature the driver reports.
//
// One context serves every scene size (FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION): it is made for the largest scene and
// output it was asked for and made again when one of them grows.
#include <math.h>
#include <string.h>
#include <wchar.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "third_party/ffx_api/ffx_upscale.h"

// ffx-api/include/ffx_api/vk/ffx_api_vk.h of the same SDK release (MIT, Copyright (C) 2024 Advanced Micro Devices,
// Inc.): the creation description of the Vulkan backend, chained behind an effect's.
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK 0x0000003u
struct ffxCreateBackendVKDesc {
    ffxCreateContextDescHeader header;
    VkDevice vkDevice;
    VkPhysicalDevice vkPhysicalDevice;
    PFN_vkGetDeviceProcAddr vkDeviceProcAddr;
};

#define LIB_NAME "amd_fidelityfx_vk.dll"

static struct {
    int wanted;                  // -1 not read
    int tried, loaded;
    void *lib;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxDispatch dispatch;
    ffxContext ctx;
    uint32_t mw, mh, mow, moh;   // the context's largest render and output size
    int inverted;                // the depth convention the context was made for
    uint64_t frames, made;
} F = {.wanted = -1};

// Read at start, like dlaa: the scene size of the upscaling modes is decided before the device exists (resolution.c).
int vk_fsr3_wanted(void) {
    if (F.wanted < 0) F.wanted = cfg_bool("TOTK_FSR3");
    return F.wanted;
}

static int lib_path(char *out, size_t cap) {
    char exe[PLAT_PATH_MAX];
    if (!plat_exe_path(exe, sizeof exe)) return 0;
    char *last = plat_last_sep(exe);
    if (!last) return 0;
    return snprintf(out, cap, "%.*s/" LIB_NAME, (int)(last - exe), exe) < (int)cap;
}

// The library's file is next to the executable (resolution.c fsr3_dll asks the same before the device exists).
static int lib_file(void) {
#ifdef _WIN32
    char path[PLAT_PATH_MAX + 32];
    return lib_path(path, sizeof path) && plat_file_exists(path);
#else
    return 0;   // AMD ships no Linux build of the library
#endif
}

int vk_fsr3_available(void) {
    if (!vk_fsr3_wanted()) return 0;
    if (F.tried) return F.loaded;
    F.tried = 1;
    char path[PLAT_PATH_MAX + 32];
    if (!lib_file() || !lib_path(path, sizeof path)) {
        rt_log("fsr3: " LIB_NAME " is not next to the executable (FidelityFX SDK 1.1.4, PrebuiltSignedDLL): FSR 3 off");
        return 0;
    }
    F.lib = plat_lib_open(path);
    if (!F.lib) {
        char why[256];
        plat_lib_error(why, sizeof why);
        rt_log("fsr3: cannot load %s (%s): FSR 3 off", path, why);
        return 0;
    }
    F.create = (PfnFfxCreateContext)plat_lib_sym(F.lib, "ffxCreateContext");
    F.destroy = (PfnFfxDestroyContext)plat_lib_sym(F.lib, "ffxDestroyContext");
    F.dispatch = (PfnFfxDispatch)plat_lib_sym(F.lib, "ffxDispatch");
    if (!F.create || !F.destroy || !F.dispatch) {
        rt_log("fsr3: %s lacks the ffx-api entry points: FSR 3 off", path);
        return 0;
    }
    rt_log("fsr3: %s loaded", path);
    return F.loaded = 1;
}

static void message(uint32_t type, const wchar_t *msg) {
    static int n;
    if (n++ < 32) rt_log("fsr3: %s: %ls", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", msg);
}

static uint32_t surface_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R8_UNORM: return FFX_API_SURFACE_FORMAT_R8_UNORM;
    case VK_FORMAT_D32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;   // as the SDK's ffxApiGetSurfaceFormatVK maps it
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

// An image of the back end as the library takes it: in GENERAL (state COMMON; UNORDERED_ACCESS for the one written).
static struct FfxApiResource resource(VkImage img, VkFormat f, uint32_t w, uint32_t h, int written) {
    struct FfxApiResource r;
    memset(&r, 0, sizeof r);
    if (!img) return r;
    r.resource = (void *)img;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = surface_format(f);
    r.description.width = w, r.description.height = h, r.description.depth = 1, r.description.mipCount = 1;
    r.description.usage = f == VK_FORMAT_D32_SFLOAT ? FFX_API_RESOURCE_USAGE_DEPTHTARGET
                          : written ? FFX_API_RESOURCE_USAGE_UAV : FFX_API_RESOURCE_USAGE_READ_ONLY;
    r.state = written ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMMON;
    return r;
}

static void destroy(void) {
    if (!F.ctx) return;
    vkDeviceWaitIdle(vk_ctx()->device);   // its images may still be read by the frames before this one
    F.destroy(&F.ctx, NULL);
    F.ctx = NULL;
}

// The context for this frame's sizes and depth convention; 0: the library refused.
static int context_for(const VkFsr3Frame *f) {
    if (F.ctx && f->w <= F.mw && f->h <= F.mh && f->ow <= F.mow && f->oh <= F.moh && f->inverted == F.inverted) return 1;
    destroy();
    const VkCtx *c = vk_ctx();
    uint32_t mow = f->ow > F.mow ? f->ow : F.mow, moh = f->oh > F.moh ? f->oh : F.moh;
    uint32_t mw = f->w > F.mw ? f->w : F.mw, mh = f->h > F.mh ? f->h : F.mh;
    if (mw > mow) mow = mw;   // a scene is never larger than the picture made from it
    if (mh > moh) moh = mh;
    struct ffxCreateBackendVKDesc backend = {.header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK},
                                             .vkDevice = c->device, .vkPhysicalDevice = c->phys,
                                             .vkDeviceProcAddr = vkGetDeviceProcAddr};
    // linear scene light, no exposure texture, scene sizes that change from frame to frame; the motion vectors are
    // at the scene's size and without the jitter, which are the library's defaults
    struct ffxCreateContextDescUpscale up = {
        .header = {.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, .pNext = &backend.header},
        .flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
                 FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION | (f->inverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0),
        .maxRenderSize = {mw, mh}, .maxUpscaleSize = {mow, moh}, .fpMessage = message};
    ffxReturnCode_t r = F.create(&F.ctx, &up.header, NULL);
    if (r != FFX_API_RETURN_OK || !F.ctx) {
        rt_log("fsr3: context creation for a %ux%u scene and a %ux%u picture failed (%u): FSR 3 off for this run", mw, mh,
               mow, moh, (unsigned)r);
        F.ctx = NULL;
        return 0;
    }
    F.mw = mw, F.mh = mh, F.mow = mow, F.moh = moh, F.inverted = f->inverted;
    rt_log("fsr3: context %llu for scenes up to %ux%u and pictures up to %ux%u%s", (unsigned long long)++F.made, mw, mh,
           mow, moh, f->inverted ? ", inverted depth" : "");
    return 1;
}

int vk_fsr3_upscale(VkCommandBuffer cb, const VkFsr3Frame *f) {
    if (!F.loaded) return 0;
    int fresh = !F.ctx || f->w > F.mw || f->h > F.mh || f->ow > F.mow || f->oh > F.moh || f->inverted != F.inverted;
    if (!context_for(f)) return 0;
    struct ffxDispatchDescUpscale d;
    memset(&d, 0, sizeof d);
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = (void *)cb;
    d.color = resource(f->color, f->color_fmt, f->w, f->h, 0);
    d.depth = resource(f->depth, VK_FORMAT_D32_SFLOAT, f->w, f->h, 0);
    d.motionVectors = resource(f->mv, VK_FORMAT_R16G16_SFLOAT, f->w, f->h, 0);
    d.reactive = resource(f->mask, VK_FORMAT_R8_UNORM, f->w, f->h, 0);
    d.output = resource(f->out, VK_FORMAT_R16G16B16A16_SFLOAT, f->ow, f->oh, 1);
    // The jitter as the scene's draws took it: the picture moved by this many pixels, x right and y down, which is
    // how the SDK's own jittered projection moves it. The vectors are last frame's window position minus this
    // frame's, in scene pixels: the library's convention, at a scale of one.
    d.jitterOffset.x = f->jx, d.jitterOffset.y = f->jy;
    d.motionVectorScale.x = d.motionVectorScale.y = 1.0f;
    d.renderSize.width = f->w, d.renderSize.height = f->h;
    d.upscaleSize.width = f->ow, d.upscaleSize.height = f->oh;
    d.enableSharpening = f->sharpness > 0.0f;
    d.sharpness = f->sharpness;
    d.frameTimeDelta = f->dt_ms;
    d.preExposure = 1.0f;
    d.reset = f->reset || fresh;
    d.cameraNear = f->near_m, d.cameraFar = f->far_m, d.cameraFovAngleVertical = f->fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    ffxReturnCode_t r = F.dispatch(&F.ctx, &d.header);
    if (r != FFX_API_RETURN_OK) {
        rt_log("fsr3: dispatch failed (%u): FSR 3 off for this run", (unsigned)r);
        return 0;
    }
    if (!F.frames++)
        rt_log("fsr3: upscaling %ux%u to %ux%u (near %g m, far %g m, vertical field of view %.1f degrees, sharpness %.2f)",
               f->w, f->h, f->ow, f->oh, f->near_m, f->far_m, f->fov_y * 57.29578f, f->sharpness);
    return 1;
}

void vk_fsr3_release(void) {
    if (F.ctx && vk_ctx()) destroy();
}
