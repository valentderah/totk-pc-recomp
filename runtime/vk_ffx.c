// AMD FSR upscaling for the DLAA path (vk_ffx.h). The FSR SDK 2.4 ships its Vulkan effects as signed DLLs behind a
// loader: amd_fidelityfx_loader_vk.dll exports the five ffx-api entry points and finds the effect DLL
// (amd_fidelityfx_upscaler_vk.dll) next to itself. Both are loaded from the executable's folder at run time; no SDK
// code is compiled in, only the descriptor layouts of ffx_api_min.h.
//
// The upscaler DLL offers several versions (FSR 4.x, FSR 3.1.x) and decides by itself which ones a device can run:
// vk_ffx_init lists them and pins the newest 4.x when there is one, else leaves the DLL's default (3.1 on GPUs without
// FSR 4). Every context is created with that version (ffxOverrideVersion).
//
// What FSR gets is what DLSS gets (vk_dlaa.c evaluate): the R11G11B10F light target as linear HDR colour with automatic
// exposure, the D32F scene depth, motion vectors at the render size in render pixels pointing from this frame's
// position to last frame's without the jitter (motionVectorScale 1), and the jitter the scene was drawn with. That is
// FSR's own convention as well: its guide derives the projection offset as (2 jx / w, -2 jy / h), which moves the
// picture by (jx, jy) window pixels with y down, the way vk_dlaa_viewport moves the viewport. TOTK_FSR_JITTER_SIGN=-1
// flips it to test the other convention.
#include "vk_ffx.h"

#include "cfg.h"
#include "host.h"

#ifndef _WIN32
int vk_ffx_wanted(void) { return 0; }
void vk_ffx_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max) {
    (void)have, (void)nhave, (void)ext, (void)n, (void)max;
}
void vk_ffx_init(void) {}
int vk_ffx_available(void) { return 0; }
void *vk_ffx_create(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, int depth_inverted, int depth_infinite) {
    (void)w, (void)h, (void)ow, (void)oh, (void)depth_inverted, (void)depth_infinite;
    return NULL;
}
int vk_ffx_dispatch(void *ctx, VkCommandBuffer cb, const VkFfxDispatch *d) { return (void)ctx, (void)cb, (void)d, 0; }
void vk_ffx_release(void *ctx) { (void)ctx; }
const char *vk_ffx_version(void) { return ""; }
#else
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ffx_api_min.h"

#define FFX_LOADER L"amd_fidelityfx_loader_vk.dll"
#define FFX_MAX_VERSIONS 16

static struct {
    HMODULE dll;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxQuery query;
    PfnFfxDispatch dispatch;
    int tried, loaded, available;
    VkDeviceContext dev;
    uint64_t version_id;   // 0: the DLL's default
    char version[48];
    float jitter_sign;
} F = {.jitter_sign = 1.0f};

int vk_ffx_wanted(void) {
    static int wanted = -1;
    if (wanted < 0) wanted = cfg_bool("TOTK_DLAA") && !strcmp(cfg_value("TOTK_DLAA_BACKEND"), "fsr");
    return wanted;
}

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
        rt_log("fsr: cannot load amd_fidelityfx_loader_vk.dll next to totk.exe (error %lu): FSR off", GetLastError());
        return 0;
    }
    F.create = (PfnFfxCreateContext)(void *)GetProcAddress(F.dll, "ffxCreateContext");
    F.destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(F.dll, "ffxDestroyContext");
    F.query = (PfnFfxQuery)(void *)GetProcAddress(F.dll, "ffxQuery");
    F.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(F.dll, "ffxDispatch");
    if (!F.create || !F.destroy || !F.query || !F.dispatch) {
        rt_log("fsr: amd_fidelityfx_loader_vk.dll lacks the ffx-api entry points: FSR off");
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
    const char *js = cfg_get("TOTK_FSR_JITTER_SIGN");
    if (js && !strcmp(js, "-1")) F.jitter_sign = -1.0f;
    uint64_t count = FFX_MAX_VERSIONS, ids[FFX_MAX_VERSIONS] = {0};
    const char *names[FFX_MAX_VERSIONS] = {0};
    struct ffxQueryDescGetVersions q = {.header = {FFX_API_QUERY_DESC_TYPE_GET_VERSIONS, NULL},
                                        .createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, .device = &F.dev,
                                        .outputCount = &count, .versionIds = ids, .versionNames = names};
    ffxReturnCode_t r = F.query(NULL, &q.header);
    if (r != FFX_API_RETURN_OK || !count) {
        rt_log("fsr: no upscaler for this device (query %u, %llu versions; is amd_fidelityfx_upscaler_vk.dll next to "
               "totk.exe?): FSR off", (unsigned)r, (unsigned long long)count);
        return;
    }
    if (count > FFX_MAX_VERSIONS) count = FFX_MAX_VERSIONS;
    char list[256] = "";
    int best = -1, best_major = 0, best_minor = 0, best_patch = 0;
    for (uint64_t i = 0; i < count; i++) {
        const char *nm = names[i] ? names[i] : "?";
        size_t used = strlen(list);
        snprintf(list + used, sizeof list - used, "%s%s", i ? ", " : "", nm);
        int ma = 0, mi = 0, pa = 0;
        if (sscanf(nm, "%d.%d.%d", &ma, &mi, &pa) < 1 || ma < 4) continue;
        if (best < 0 || ma > best_major || (ma == best_major && (mi > best_minor || (mi == best_minor && pa > best_patch))))
            best = (int)i, best_major = ma, best_minor = mi, best_patch = pa;
    }
    if (best >= 0) {
        F.version_id = ids[best];
        snprintf(F.version, sizeof F.version, "FSR %s", names[best]);
    } else {
        snprintf(F.version, sizeof F.version, "FSR %s", names[0] ? names[0] : "?");   // the DLL's default
    }
    F.available = 1;
    rt_log("fsr: upscaler versions for this device: %s; using %s%s", list, F.version,
           best >= 0 ? "" : " (FSR 4 is not offered for this GPU)");
}

int vk_ffx_available(void) { return vk_ffx_wanted() && F.available; }

const char *vk_ffx_version(void) { return F.version; }

// Messages from the upscaler (errors and warnings), in UTF-16; into the log as UTF-8.
static void on_message(uint32_t type, const wchar_t *msg) {
    char s[512];
    if (!msg || !WideCharToMultiByte(CP_UTF8, 0, msg, -1, s, (int)sizeof s, NULL, NULL)) return;
    rt_log("fsr: %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", s);
}

void *vk_ffx_create(uint32_t w, uint32_t h, uint32_t ow, uint32_t oh, int depth_inverted, int depth_infinite) {
    if (!F.available) return NULL;
    struct ffxOverrideVersion ov = {.header = {FFX_API_DESC_TYPE_OVERRIDE_VERSION, NULL}, .versionId = F.version_id};
    struct ffxCreateContextDescUpscaleVersion ver = {
        .header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION, F.version_id ? &ov.header : NULL},
        .version = FFX_UPSCALER_VERSION};
    struct ffxCreateBackendVKDesc be = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK, &ver.header},
                                        .deviceContext = F.dev};
    // Linear HDR scene light with the upscaler's own exposure, as DLSS gets it ("IsHDR | AutoExposure")
    uint32_t flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    if (depth_inverted) flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (depth_infinite) flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    if (rt_knob("FSR_DEBUG", "0")) flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;   // TOTK_FSR_DEBUG=1: API checks
    struct ffxCreateContextDescUpscale cd = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, &be.header},
                                             .flags = flags, .maxRenderSize = {w, h}, .maxUpscaleSize = {ow, oh},
                                             .fpMessage = on_message};
    ffxContext ctx = NULL;
    ffxReturnCode_t r = F.create(&ctx, &cd.header, NULL);
    if (r != FFX_API_RETURN_OK || !ctx) {
        rt_log("fsr: context creation failed (%u) at %ux%u -> %ux%u", (unsigned)r, w, h, ow, oh);
        return NULL;
    }
    struct ffxQueryGetProviderVersion pv = {.header = {FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION, NULL}};
    F.query(&ctx, &pv.header);
    rt_log("fsr: context created at %ux%u -> %ux%u with %s (flags %#x)", w, h, ow, oh,
           pv.versionName ? pv.versionName : F.version, flags);
    return ctx;
}

void vk_ffx_release(void *ctx) {
    if (!ctx) return;
    ffxContext c = ctx;
    F.destroy(&c, NULL);
}

static uint32_t surface_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_D32_SFLOAT: return FFX_API_SURFACE_FORMAT_R32_FLOAT;
    case VK_FORMAT_R8_UNORM: return FFX_API_SURFACE_FORMAT_R8_UNORM;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

// Every image of the DLAA path stays in VK_IMAGE_LAYOUT_GENERAL: GENERIC_READ and UNORDERED_ACCESS are the states the
// Vulkan backend maps to that layout (ffx_vk.cpp getVKImageLayoutFromResourceState), and it puts each resource back
// into the state it was given at the end of the dispatch.
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

int vk_ffx_dispatch(void *ctx, VkCommandBuffer cb, const VkFfxDispatch *d) {
    if (!ctx) return 0;
    ffxContext c = ctx;
    struct ffxDispatchDescUpscale u;
    memset(&u, 0, sizeof u);
    u.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    u.commandList = (void *)cb;
    u.color = resource(&d->color, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_GENERIC_READ);
    u.depth = resource(&d->depth, FFX_API_RESOURCE_USAGE_DEPTHTARGET, FFX_API_RESOURCE_STATE_GENERIC_READ);
    u.motionVectors = resource(&d->mv, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_GENERIC_READ);
    u.reactive = resource(&d->reactive, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_GENERIC_READ);
    u.output = resource(&d->out, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    u.jitterOffset = (struct FfxApiFloatCoords2D){d->jx * F.jitter_sign, d->jy * F.jitter_sign};
    u.motionVectorScale = (struct FfxApiFloatCoords2D){1.0f, 1.0f};
    u.renderSize = (struct FfxApiDimensions2D){d->color.width, d->color.height};
    u.upscaleSize = (struct FfxApiDimensions2D){d->out.width, d->out.height};
    u.frameTimeDelta = d->dt_ms;
    u.preExposure = 1.0f;
    u.reset = d->reset != 0;
    u.cameraNear = d->cam_near;
    u.cameraFar = d->cam_far;
    u.cameraFovAngleVertical = d->fov_y;
    u.viewSpaceToMetersFactor = 1.0f;   // TotK's world units are metres
    ffxReturnCode_t r = F.dispatch(&c, &u.header);
    if (r != FFX_API_RETURN_OK) {
        rt_log("fsr: dispatch failed (%u)", (unsigned)r);
        return 0;
    }
    return 1;
}
#endif
