// AMD FSR 3 frame generation (frame_gen = 1): the FidelityFX SDK 1.1.4's frame interpolation and optical flow
// (third_party/FidelityFX, MIT) on its Vulkan backend, driven directly instead of through the SDK's frame
// interpolation swapchain (Windows only, and it would own the present).
//
// Ported from Savage Wind (https://github.com/Revivedx/SavageWindRecomp @ bf6146c14c37fee8781a601e5a544b93dff6d75e,
// MIT), runtime/nvn/vulkan/framegen.cpp: the blob lookup, the backend set-up and the contexts as it makes them, which is
// what the SDK's ffx-api provider (ffx_provider_framegeneration.cpp) does: a shared backend context for the resources
// the two effects exchange (optical flow vectors, scene change detection, dilated depth and motion vectors, the
// previous frame's reconstructed depth, two of each that alternate), the optical flow context and the frame
// interpolation context on a second backend interface. This stage only creates and destroys them; nothing is
// dispatched yet.
#include "vk_fg.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <ffx_shader_blobs.h>

extern "C" {
#include "cfg.h"
#include "host.h"
#include "vk.h"
}

// The shaders: tools/ffx_shaders.py's blobs (wave32, 32-bit floats; frame interpolation for low-resolution motion
// vectors without jitter, both depth directions) in place of the SDK's FidelityFX_SC permutation tables
#include "ffx_shaders_vk.h"

enum { FI_PERM_DEPTH_INVERTED = 1 << 2 };  // ffx_frameinterpolation_private.h FRAMEINTERPOLATION_SHADER_PERMUTATION_*

extern "C" FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effect, FfxPass pass, FfxBindStage, uint32_t options,
                                                     FfxShaderBlob *out) {
    if (effect == FFX_EFFECT_FRAMEINTERPOLATION && pass < FFX_FRAMEINTERPOLATION_PASS_COUNT) {
        memcpy(out, &ffx_fi_blobs[(options & FI_PERM_DEPTH_INVERTED) ? 1 : 0][pass], sizeof *out);
        return FFX_OK;
    }
    if (effect == FFX_EFFECT_OPTICALFLOW && pass < FFX_OPTICALFLOW_PASS_COUNT) {
        memcpy(out, &ffx_of_blobs[pass], sizeof *out);
        return FFX_OK;
    }
    rt_log("fg: no shader for effect %d pass %d", (int)effect, (int)pass);
    return FFX_ERROR_INVALID_ARGUMENT;
}

extern "C" FfxErrorCode ffxIsWave64(FfxEffect, uint32_t, bool &wave64) {
    wave64 = false;  // (only wave32 blobs: the backend then asks for no subgroup size)
    return FFX_OK;
}

// The backend's hook into its frame interpolation swapchain, which isn't built
FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const *) { return FFX_OK; }

enum { R_OF_VECTOR, R_OF_SCD, R_DILATED_DEPTH, R_DILATED_MV = R_DILATED_DEPTH + 2, R_PREV_DEPTH = R_DILATED_MV + 2,
       R_COUNT = R_PREV_DEPTH + 2 };

static struct {
    bool live;
    FfxInterface shared, fi_if;
    void *scratch_shared, *scratch_fi;
    uint32_t shared_id;
    FfxOpticalflowContext of;
    FfxFrameInterpolationContext fi;
    FfxResourceInternal res[R_COUNT];
} g;

#define TRY(x) do { FfxErrorCode e_ = (x); if (e_ != FFX_OK) { rt_log("fg: %s failed: %#x", #x, (unsigned)e_); return false; } } while (0)

// The backend calls a few extension functions unchecked: the debug labels (VK_EXT_debug_utils, an instance
// extension the runtime enables only with validation) and vkGetBufferMemoryRequirements2KHR (core since 1.1 without
// the suffix)
static VKAPI_ATTR void VKAPI_CALL no_label(VkCommandBuffer, const VkDebugUtilsLabelEXT *) {}
static VKAPI_ATTR void VKAPI_CALL no_label_end(VkCommandBuffer) {}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL device_proc(VkDevice d, const char *name) {
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(d, name);
    if (f) return f;
    if (!strcmp(name, "vkGetBufferMemoryRequirements2KHR")) return vkGetDeviceProcAddr(d, "vkGetBufferMemoryRequirements2");
    if (!strcmp(name, "vkCmdBeginDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)no_label;
    if (!strcmp(name, "vkCmdEndDebugUtilsLabelEXT")) return (PFN_vkVoidFunction)no_label_end;
    return nullptr;
}

// (the backend's context in it has members the compiler aligns past malloc's 16 bytes)
static void *scratch_alloc(size_t n) {
#ifdef _WIN32
    void *p = _aligned_malloc(n, 256);
#else
    void *p = aligned_alloc(256, n);
#endif
    if (p) memset(p, 0, n);
    return p;
}

static void scratch_free(void *p) {
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}

static bool make_backend(VkCtx *c, FfxInterface *i, void **scratch, size_t contexts) {
    size_t n = (ffxGetScratchMemorySizeVK(c->phys, contexts) + 255) & ~(size_t)255;
    *scratch = scratch_alloc(n);
    if (!*scratch) return false;
    VkDeviceContext dc = {c->device, c->phys, device_proc};
    TRY(ffxGetInterfaceVK(i, ffxGetDeviceVK(&dc), *scratch, n, contexts));
    return true;
}

extern "C" void vk_fg_release(void) {
    if (!g.live && !g.scratch_shared && !g.scratch_fi) return;
    VkCtx *c = vk_ctx();
    if (c && c->device) vkDeviceWaitIdle(c->device);
    if (g.live) {
        for (int i = 0; i < R_COUNT; i++) g.shared.fpDestroyResource(&g.shared, g.res[i], g.shared_id);
        ffxFrameInterpolationContextDestroy(&g.fi);
        ffxOpticalflowContextDestroy(&g.of);
        g.shared.fpDestroyBackendContext(&g.shared, g.shared_id);
        rt_log("fg: frame interpolation contexts destroyed");
    }
    scratch_free(g.scratch_shared);
    scratch_free(g.scratch_fi);
    memset(&g, 0, sizeof g);
}

static bool create(VkCtx *c, uint32_t w, uint32_t h) {
    if (!make_backend(c, &g.shared, &g.scratch_shared, 1) || !make_backend(c, &g.fi_if, &g.scratch_fi, 2)) return false;
    TRY(g.shared.fpCreateBackendContext(&g.shared, FFX_EFFECT_SHAREDAPIBACKEND, nullptr, &g.shared_id));
    FfxOpticalflowContextDescription od = {};
    od.backendInterface = g.fi_if;
    od.resolution = {w, h};
    TRY(ffxOpticalflowContextCreate(&g.of, &od));
    FfxFrameInterpolationContextDescription fd = {};
    fd.backendInterface = g.fi_if;
    fd.maxRenderSize = {w, h};
    fd.displaySize = {w, h};
    fd.backBufferFormat = ffxGetSurfaceFormatVK(VK_FORMAT_R8G8B8A8_UNORM);
    fd.previousInterpolationSourceFormat = ffxGetSurfaceFormatVK(VK_FORMAT_R8G8B8A8_UNORM);
    TRY(ffxFrameInterpolationContextCreate(&g.fi, &fd));
    FfxOpticalflowSharedResourceDescriptions ord = {};
    TRY(ffxOpticalflowGetSharedResourceDescriptions(&g.of, &ord));
    TRY(g.shared.fpCreateResource(&g.shared, &ord.opticalFlowVector, g.shared_id, &g.res[R_OF_VECTOR]));
    TRY(g.shared.fpCreateResource(&g.shared, &ord.opticalFlowSCD, g.shared_id, &g.res[R_OF_SCD]));
    FfxFrameInterpolationSharedResourceDescriptions frd = {};
    TRY(ffxFrameInterpolationGetSharedResourceDescriptions(&g.fi, &frd));
    for (int i = 0; i < 2; i++) {
        TRY(g.shared.fpCreateResource(&g.shared, &frd.dilatedDepth, g.shared_id, &g.res[R_DILATED_DEPTH + i]));
        TRY(g.shared.fpCreateResource(&g.shared, &frd.dilatedMotionVectors, g.shared_id, &g.res[R_DILATED_MV + i]));
        TRY(g.shared.fpCreateResource(&g.shared, &frd.reconstructedPrevNearestDepth, g.shared_id, &g.res[R_PREV_DEPTH + i]));
    }
    g.live = true;
    return true;
}

extern "C" void vk_fg_init(void) {
    if (!cfg_bool("TOTK_FRAME_GEN")) return;
    VkCtx *c = vk_ctx();
    if (!c) return;
    // The size is a placeholder until the dispatch stage sizes the contexts from the real output and scene.
    uint32_t w = 1920, h = 1080;
    sscanf(cfg_value("TOTK_OUTPUT_RES"), "%ux%u", &w, &h);
    if (w < 64 || h < 64 || w > 16384 || h > 16384) w = 1920, h = 1080;
    if (!create(c, w, h)) {
        rt_log("fg: frame interpolation unavailable (the contexts could not be made)");
        g.live = false;  // (a half-made set is not destroyed through its context functions: only its memory is freed)
        vk_fg_release();
        return;
    }
    rt_log("fg: FSR 3 frame interpolation contexts created at %ux%u (FidelityFX SDK 1.1.4)", w, h);
}

extern "C" int vk_fg_active(void) { return g.live; }
