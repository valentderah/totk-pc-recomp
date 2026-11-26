// AMD FSR 4 for the scene upscaler (vk_ffx.h). The FSR SDK 2.4 ships its Vulkan effects as signed DLLs behind a
// loader: amd_fidelityfx_loader_vk.dll exports the ffx-api entry points and finds the effect DLL
// (amd_fidelityfx_upscaler_vk.dll) next to itself. Both are loaded from the executable's folder at run time; no SDK
// code is compiled in, only the descriptor layouts of ffx_api_min.h.
//
// The upscaler DLL offers several versions (FSR 4.x, FSR 3.1.x) and decides by itself which ones a device can run:
// vk_ffx_init lists them and pins the newest 4.x. A device the DLL offers no FSR 4 for runs without the upscaler (the
// log and the menu say so): FSR 3.1 is scene_upscaler = fsr3, built from the SDK 1.1.4 sources (vk_fsr3.cpp), not a
// fallback of this DLL. Every context is created with the pinned version (ffxOverrideVersion).
//
// What FSR 4 gets is what FSR 3.1 gets (vk_dlaa.c fsr_evaluate): the R11G11B10F light target as linear HDR colour, the
// D32F scene depth (standard direction, the far plane finite), motion vectors at the render size in render pixels
// pointing from this frame's position to last frame's without the jitter (motionVectorScale 1), the jitter the scene
// was drawn with, the camera's planes and field of view. That is FSR's own convention: its guide derives the
// projection offset as (2 jx / w, -2 jy / h), which moves the picture by (jx, jy) window pixels with y down, the way
// vk_dlaa_viewport moves the viewport. The exposure is FSR's own (AUTO_EXPOSURE), as codexx-dev's pull request #1245
// ran it on an RX 9070 XT.
#include "vk_ffx.h"

#include "cfg.h"
#include "host.h"

#ifndef _WIN32
int vk_ffx_wanted(void) { return 0; }
void vk_ffx_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max) {
    (void)have, (void)nhave, (void)ext, (void)n, (void)max;
}
void vk_ffx_init(void) {}
int vk_ffx_tried(void) { return 0; }
int vk_ffx_available(void) { return 0; }
int vk_ffx_dispatch(VkCommandBuffer cb, const VkFfxDispatch *d) { return (void)cb, (void)d, 0; }
void vk_ffx_release(void) {}
const char *vk_ffx_version(void) { return ""; }
#else
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ffx_api_min.h"

#define FFX_LOADER L"amd_fidelityfx_loader_vk.dll"
#define FFX_MAX_VERSIONS 16
// The SDK's Vulkan backend writes a dispatch's descriptor sets and constants into rings this deep and frees the views
// of the dispatch that many back without knowing the GPU (FFX_MAX_QUEUED_FRAMES in the SDK's ffx_types.h, as
// vk_fsr3.cpp wait_ring reads it for SDK 1.1.4).
#define FFX_QUEUED 4

static struct {
    HMODULE dll;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxQuery query;
    PfnFfxDispatch dispatch;
    int tried, loaded, available, failed;
    VkDeviceContext dev;
    uint64_t version_id;
    char version[48];
    ffxContext ctx;               // the live context, made for ow x oh
    uint32_t ow, oh;
    uint64_t ring[FFX_QUEUED], n, waits;   // the fences of the last dispatches (wait_ring)
} F;

int vk_ffx_wanted(void) { return vk_upscaler_engine() == UPSCALER_FSR4; }

static int loader_load(void) {
    if (F.tried) return F.loaded;
    F.tried = 1;
    wchar_t path[MAX_PATH + 64];
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
    while (len && path[len - 1] != L'\\') len--;
    path[len] = 0;
    wcscat(path, FFX_LOADER);
    // LOAD_WITH_ALTERED_SEARCH_PATH: the loader's own imports and the effect DLL resolve from the executable's folder
    F.dll = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!F.dll) {
        rt_log("fsr4: cannot load amd_fidelityfx_loader_vk.dll next to totk.exe (error %lu): the scene upscaler is off",
               GetLastError());
        return 0;
    }
    F.create = (PfnFfxCreateContext)(void *)GetProcAddress(F.dll, "ffxCreateContext");
    F.destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(F.dll, "ffxDestroyContext");
    F.query = (PfnFfxQuery)(void *)GetProcAddress(F.dll, "ffxQuery");
    F.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(F.dll, "ffxDispatch");
    if (!F.create || !F.destroy || !F.query || !F.dispatch) {
        rt_log("fsr4: amd_fidelityfx_loader_vk.dll lacks the ffx-api entry points: the scene upscaler is off");
        return 0;
    }
    return F.loaded = 1;
}

static int has(const VkExtensionProperties *have, uint32_t nhave, const char *name) {
    for (uint32_t i = 0; i < nhave; i++)
        if (!strcmp(have[i].extensionName, name)) return 1;
    return 0;
}

static void add(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max,
                const char *name) {
    for (uint32_t i = 0; i < *n; i++)
        if (!strcmp(ext[i], name)) return;
    if (has(have, nhave, name) && *n < max) ext[(*n)++] = name;
}

// The extensions the FSR SDK's own sample enables for its effects (Cauldron2 device_vk.cpp): cooperative matrix and
// 8-bit floats for FSR 4's network where the device has them. The rest it uses (16-bit floats and storage, integer
// dot products, subgroup size control) is core Vulkan 1.3, which vk_device.c enables as far as the driver reports it.
void vk_ffx_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max) {
    if (!vk_ffx_wanted()) return;
    add(have, nhave, ext, n, max, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME);
#ifdef VK_EXT_SHADER_FLOAT8_EXTENSION_NAME
    add(have, nhave, ext, n, max, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
#endif
}

void vk_ffx_init(void) {
    if (!vk_ffx_wanted() || !loader_load()) return;
    const VkCtx *c = vk_ctx();
    F.dev = (VkDeviceContext){c->device, c->phys, vkGetDeviceProcAddr};
    uint64_t count = FFX_MAX_VERSIONS, ids[FFX_MAX_VERSIONS] = {0};
    const char *names[FFX_MAX_VERSIONS] = {0};
    struct ffxQueryDescGetVersions q = {.header = {FFX_API_QUERY_DESC_TYPE_GET_VERSIONS, NULL},
                                        .createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, .device = &F.dev,
                                        .outputCount = &count, .versionIds = ids, .versionNames = names};
    ffxReturnCode_t r = F.query(NULL, &q.header);
    if (r != FFX_API_RETURN_OK || !count) {
        rt_log("fsr4: no upscaler for this device (query %u, %llu versions; is amd_fidelityfx_upscaler_vk.dll next to "
               "totk.exe?): the scene upscaler is off", (unsigned)r, (unsigned long long)count);
        return;
    }
    if (count > FFX_MAX_VERSIONS) count = FFX_MAX_VERSIONS;
    char list[256] = "";
    // The newest 4.x the DLL offers for this device; the DLL leaves FSR 4 out for GPUs it does not run on
    int best = -1, best_minor = 0, best_patch = 0;
    for (uint64_t i = 0; i < count; i++) {
        const char *nm = names[i] ? names[i] : "?";
        size_t used = strlen(list);
        snprintf(list + used, sizeof list - used, "%s%s", i ? ", " : "", nm);
        int ma = 0, mi = 0, pa = 0;
        if (sscanf(nm, "%d.%d.%d", &ma, &mi, &pa) < 1 || ma != 4) continue;
        if (best < 0 || mi > best_minor || (mi == best_minor && pa > best_patch))
            best = (int)i, best_minor = mi, best_patch = pa;
    }
    if (best < 0) {
        rt_log("fsr4: the FSR libraries offer %s for this device, no FSR 4: the scene upscaler is off", list);
        return;
    }
    F.version_id = ids[best];
    snprintf(F.version, sizeof F.version, "FSR %s", names[best]);
    F.available = 1;
    rt_log("fsr4: upscaler versions for this device: %s; using %s", list, F.version);
}

int vk_ffx_tried(void) { return F.tried; }

int vk_ffx_available(void) { return vk_ffx_wanted() && F.available; }

const char *vk_ffx_version(void) { return F.available ? F.version : ""; }

// Messages from the upscaler (errors and warnings), in UTF-16; into the log as UTF-8.
static void on_message(uint32_t type, const wchar_t *msg) {
    char s[512];
    if (!msg || !WideCharToMultiByte(CP_UTF8, 0, msg, -1, s, (int)sizeof s, NULL, NULL)) return;
    rt_log("fsr4: %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", s);
}

// The context for an ow x oh output, any scene up to that size (DYNAMIC_RESOLUTION: the modes and the game's tiers
// change the scene without a new context), as vk_fsr3.cpp create makes FSR 3.1's.
static int create(uint32_t ow, uint32_t oh) {
    struct ffxOverrideVersion ov = {.header = {FFX_API_DESC_TYPE_OVERRIDE_VERSION, NULL}, .versionId = F.version_id};
    struct ffxCreateContextDescUpscaleVersion ver = {
        .header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION, &ov.header}, .version = FFX_UPSCALER_VERSION};
    struct ffxCreateBackendVKDesc be = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, &ver.header},
                                        .deviceContext = F.dev};
    // Linear HDR scene light with the upscaler's own exposure; standard depth with a finite far plane, as FSR 3.1's
    uint32_t flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
                     FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    if (rt_knob("FSR4_DEBUG", "0")) flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;   // TOTK_FSR4_DEBUG=1: API checks
    struct ffxCreateContextDescUpscale cd = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &be.header},
                                             .flags = flags, .maxRenderSize = {ow, oh}, .maxUpscaleSize = {ow, oh},
                                             .fpMessage = on_message};
    ffxContext ctx = NULL;
    ffxReturnCode_t r = F.create(&ctx, &cd.header, NULL);
    if (r != FFX_API_RETURN_OK || !ctx) {
        rt_log("fsr4: context creation failed (%u) at %ux%u: FSR 4 off for this run", (unsigned)r, ow, oh);
        return 0;
    }
    struct ffxQueryGetProviderVersion pv = {.header = {FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION, NULL}};
    F.query(&ctx, &pv.header);
    rt_log("fsr4: context up to %ux%u -> %ux%u with %s (flags %#x)", ow, oh, ow, oh,
           pv.versionName ? pv.versionName : F.version, flags);
    F.ctx = ctx, F.ow = ow, F.oh = oh, F.n = 0;
    memset(F.ring, 0, sizeof F.ring);
    return 1;
}

static void destroy(void) {
    if (F.ctx) F.destroy(&F.ctx, NULL);
    F.ctx = NULL;
}

static uint32_t surface_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_D32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

// Every image of the scene upscaler stays in VK_IMAGE_LAYOUT_GENERAL: GENERIC_READ and UNORDERED_ACCESS are the
// states the Vulkan backend maps to that layout (ffx_vk.cpp getVKImageLayoutFromResourceState), and it puts each
// resource back into the state it was given at the end of the dispatch.
static struct FfxApiResource resource(const VkFfxImage *im, uint32_t usage, uint32_t state) {
    struct FfxApiResource r;
    memset(&r, 0, sizeof r);
    if (!im->image) return r;
    r.resource = (void *)im->image;
    r.description = (struct FfxApiResourceDescription){.type = FFX_API_RESOURCE_TYPE_TEXTURE2D,
                                                       .format = surface_format(im->format), .width = im->width,
                                                       .height = im->height, .depth = 1, .mipCount = 1, .usage = usage};
    r.state = state;
    return r;
}

// With the game's two or three frames in flight the dispatch FFX_QUEUED - 1 back is long done; should it not be, its
// submission is waited for here (counted), as vk_fsr3.cpp wait_ring does for FSR 3.1.
static void wait_ring(uint64_t fence) {
    if (F.n >= FFX_QUEUED - 1) {
        uint64_t back = F.ring[(F.n + 1) % FFX_QUEUED];
        if (back && vk_sync_fence_done() < back) {
            if (!F.waits++) rt_log("fsr4: waiting for the submission of a dispatch %d back (more frames in flight than "
                                   "the SDK's rings hold)", FFX_QUEUED - 1);
            vk_sync_fence_wait(back, 2000000000ull);
        }
    }
    F.ring[F.n % FFX_QUEUED] = fence;
}

int vk_ffx_dispatch(VkCommandBuffer cb, const VkFfxDispatch *d) {
    if (!F.available || F.failed) return 0;
    uint32_t ow = d->out.width, oh = d->out.height;
    if (!F.ctx || ow != F.ow || oh != F.oh) {
        if (F.ctx) {
            vkDeviceWaitIdle(vk_ctx()->device);   // what the old context recorded is queued or submitted
            destroy();
        }
        if (!create(ow, oh)) return F.failed = 1, 0;
    }
    if (d->color.width > F.ow || d->color.height > F.oh) {
        rt_log("fsr4: a %ux%u scene is larger than the %ux%u output: FSR 4 off for this run", d->color.width,
               d->color.height, F.ow, F.oh);
        return F.failed = 1, 0;
    }
    wait_ring(d->fence);
    struct ffxDispatchDescUpscale u;
    memset(&u, 0, sizeof u);
    u.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    u.commandList = (void *)cb;
    u.color = resource(&d->color, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_GENERIC_READ);
    u.depth = resource(&d->depth, FFX_API_RESOURCE_USAGE_DEPTHTARGET, FFX_API_RESOURCE_STATE_GENERIC_READ);
    u.motionVectors = resource(&d->mv, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_GENERIC_READ);
    u.output = resource(&d->out, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    // (no reactive or transparency mask: FSR 3.1 gets none either)
    u.jitterOffset = (struct FfxApiFloatCoords2D){d->jx, d->jy};
    u.motionVectorScale = (struct FfxApiFloatCoords2D){1.0f, 1.0f};   // render pixels already
    u.renderSize = (struct FfxApiDimensions2D){d->color.width, d->color.height};
    u.upscaleSize = (struct FfxApiDimensions2D){ow, oh};
    u.enableSharpening = d->sharpness > 0;
    u.sharpness = d->sharpness > 1.0f ? 1.0f : d->sharpness;
    u.frameTimeDelta = d->dt_ms;
    u.preExposure = 1.0f;
    u.reset = d->reset != 0;
    u.cameraNear = d->cam_near;
    u.cameraFar = d->cam_far;
    u.cameraFovAngleVertical = d->fov_y;
    u.viewSpaceToMetersFactor = 1.0f;   // TotK's world units are metres
    ffxReturnCode_t r = F.dispatch(&F.ctx, &u.header);
    F.n++;
    if (r != FFX_API_RETURN_OK) {
        rt_log("fsr4: dispatch failed (%u) at %ux%u -> %ux%u: FSR 4 off for this run", (unsigned)r, d->color.width,
               d->color.height, ow, oh);
        return F.failed = 1, 0;
    }
    return 1;
}

void vk_ffx_release(void) {
    const VkCtx *c = vk_ctx();
    if (!F.ctx || !c || !c->device) return;
    vkDeviceWaitIdle(c->device);
    if (F.waits) rt_log("fsr4: %llu dispatches waited for an earlier one's submission", (unsigned long long)F.waits);
    destroy();
}
#endif
