// AMD FSR 3 frame generation (frame_gen = fsr3): the FidelityFX SDK 1.1.4's frame interpolation and optical flow
// (third_party/FidelityFX, MIT) on its Vulkan backend, driven directly instead of through the SDK's frame
// interpolation swapchain (Windows only, and it would own the present).
//
// Ported from Savage Wind (https://github.com/Revivedx/SavageWindRecomp @ bf6146c14c37fee8781a601e5a544b93dff6d75e,
// MIT), runtime/nvn/vulkan/framegen.cpp: the blob lookup, the backend set-up, the contexts as it makes them (what the
// SDK's ffx-api provider, ffx_provider_framegeneration.cpp, does: a shared backend context for the resources the two
// effects exchange, the optical flow context and the frame interpolation context on a second backend interface) and
// the parameters of fg_dispatch's three calls; its exec.c vk_fg_generate for the camera, the cut test and the HUD-less
// copy's encoding.
//
// Per frame, all in the replay's command buffer (vk_dlaa.c finds the places, with DLSS off):
// 1. vk_fg_prepare at the scene's end: the SDK dilates the scene's depth and the camera motion vectors (made there by
//    vk_dlaa.c mv_pass, at the scene's size), and reconstructs the previous frame's depth from them.
// 2. vk_fg_hudless before the HUD's first draw: the game's float picture, blitted into an R8G8B8A8_SRGB image (the
//    blit encodes as the game's last draw encodes the picture into its sRGB window texture) and copied byte for byte
//    into an RGBA8 UNORM image, which the SDK reads. Its bytes then equal the window texture's wherever the HUD is not,
//    which is how the SDK tells the HUD apart (it compares the two) and keeps it out of the interpolation.
// 3. vk_fg_generate at the end of the submission that drew the window texture: the optical flow over the HUD-less
//    picture, then the interpolation between the last frame and this one, with this frame's window texture as the
//    picture with the HUD, into an RGBA8 image holding bytes as the window texture does (sRGB transfer function).
// The present (vk_backend.c present_pair) shows it before the frame, through vk_fg_present_gen (TOTK_FG_TEST shows it, or
// the picture before the HUD, in place of the frame instead, for checks only).
#include "vk_fg.h"
#include "vk_fsr3.h"

#include <malloc.h>
#include <math.h>
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
    if (effect == FFX_EFFECT_FSR3UPSCALER && vk_fsr3_blob(pass, options, out)) return FFX_OK;   // the upscaler's (vk_fsr3.cpp)
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

// What the contexts are made for; a change makes them again, but for the scene's size, which only has to fit
// (vk_fg_prepare)
typedef struct {
    uint32_t display_w, display_h;  // the window texture's (the interpolated frame's)
    uint32_t render_w, render_h;    // the largest scene (depth and motion vectors): the output's, or a larger scene's
    VkFormat color_format;          // the window texture's bytes (its UNORM twin)
    uint32_t inverted, infinite;
} FgConfig;

struct Img {
    VkImage img;
    VmaAllocation mem;
};

#define TSLOTS 8u           // frames of timestamps in flight: a slot is read when it comes round again
#define CPU_RING 65536u     // frames of replay-thread times kept for the median and p99
#define LOG_EVERY 600u      // generated frames between two summary lines

static struct {
    bool live, failed;
    FgConfig cfg;
    FfxInterface shared, fi_if;
    void *scratch_shared, *scratch_fi;
    uint32_t shared_id;
    FfxOpticalflowContext of;
    FfxFrameInterpolationContext fi;
    FfxResourceInternal res[R_COUNT];
    unsigned toggle;
    uint64_t frame_id, last_prep_ns, made;
    // settings read at start (vk_fg_init)
    int workers, sync_ahead, hdr, test, debug, times;
    uint64_t shot_at;               // TOTK_FG_SHOT: the first present shot, or (shot_w) the first after that world time
    int64_t shot_w;
    char shot_dir[260];
    unsigned said;                  // vk_fg_wanted's reasons logged (bits)
    volatile int menu;              // vk_fg_menu
    // this frame
    bool prepared, hl_ok, gen_ok, chain, reset;
    float near_m, far_m, fov_y, dt_ms;
    uint32_t rw, rh;                // this frame's scene size (the SDK's renderSize), at most cfg.render_w x render_h
    VkFormat win_fmt;               // the window texture's format as last seen (UNORM twin)
    int k;                          // the pair of hudless/out images this frame writes
    VkImage gen_win;                // the window texture this frame's picture was generated against (vk_fg_generate)
    VkImage show_out, show_hl, show_win;   // the frame just ended (vk_fg_frame), for vk_fg_present_swap / _gen
    // images
    uint32_t iw, ih;
    Img hl_srgb, hl[2], out[2];
    // timing
    VkQueryPool qp;
    uint32_t qslot, qmask[TSLOTS];
    double gpu[4];                  // ms summed: camera motion, prepare, hudless copy, optical flow + interpolation
    double gpu_run[4];
    uint64_t timed, timed_run;
    uint64_t cpu_frame;             // ns this frame
    uint32_t *cpu_us;               // per frame with frame generation work, microseconds
    uint64_t ncpu;
    uint64_t gens, resets, skipped, gens_logged, resized;   // resized: prepares at another scene size than the last
    uint64_t drain_ns, sdk_ns, split_frames;   // since the last summary: recorder drains, the SDK's own recording
    uint64_t drain_run, sdk_run, prep_ns, prep_run;   // prep: the prepare's share of sdk
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

// The SDK's objects (not this file's images): the device is idle or the contexts were never used.
static void sdk_destroy(void) {
    if (g.live) {
        for (int i = 0; i < R_COUNT; i++) g.shared.fpDestroyResource(&g.shared, g.res[i], g.shared_id);
        ffxFrameInterpolationContextDestroy(&g.fi);
        ffxOpticalflowContextDestroy(&g.of);
        g.shared.fpDestroyBackendContext(&g.shared, g.shared_id);
    }
    scratch_free(g.scratch_shared);
    scratch_free(g.scratch_fi);
    g.scratch_shared = g.scratch_fi = nullptr;
    memset(&g.of, 0, sizeof g.of);
    memset(&g.fi, 0, sizeof g.fi);
    memset(g.res, 0, sizeof g.res);
    g.live = false;
}

static void img_free(VkCtx *c, Img *m) {
    if (m->img) vmaDestroyImage(c->vma, m->img, m->mem);
    *m = Img{};
}

static void times_log(const char *what, const double *gpu, uint64_t timed);
static void cpu_log(const char *what, uint64_t from);

extern "C" void vk_fg_release(void) {
    VkCtx *c = vk_ctx();
    if (!c || !c->device) return;
    if (!g.live && !g.scratch_shared && !g.hl_srgb.img && !g.qp) return;
    vkDeviceWaitIdle(c->device);
    vk_fg_exit_log();
    if (g.live) rt_log("fg: frame interpolation contexts destroyed");
    sdk_destroy();
    img_free(c, &g.hl_srgb);
    for (int i = 0; i < 2; i++) img_free(c, &g.hl[i]), img_free(c, &g.out[i]);
    if (g.qp) vkDestroyQueryPool(c->device, g.qp, NULL);
    free(g.cpu_us);
    memset(&g, 0, sizeof g);
}

static bool create(VkCtx *c, const FgConfig *cf) {
    g.cfg = *cf;
    if (!make_backend(c, &g.shared, &g.scratch_shared, 1) || !make_backend(c, &g.fi_if, &g.scratch_fi, 2)) return false;
    TRY(g.shared.fpCreateBackendContext(&g.shared, FFX_EFFECT_SHAREDAPIBACKEND, nullptr, &g.shared_id));
    FfxOpticalflowContextDescription od = {};
    od.backendInterface = g.fi_if;
    od.resolution = {cf->display_w, cf->display_h};
    TRY(ffxOpticalflowContextCreate(&g.of, &od));
    FfxFrameInterpolationContextDescription fd = {};
    fd.backendInterface = g.fi_if;
    fd.flags = (cf->inverted ? FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED : 0) |
               (cf->infinite ? FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INFINITE : 0);
    fd.maxRenderSize = {cf->render_w, cf->render_h};
    fd.displaySize = {cf->display_w, cf->display_h};
    fd.backBufferFormat = ffxGetSurfaceFormatVK(cf->color_format);
    fd.previousInterpolationSourceFormat = ffxGetSurfaceFormatVK(VK_FORMAT_R8G8B8A8_UNORM);   // the HUD-less copy's
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

static int knob_int(const char *key) {
    const char *v = cfg_get(key);
    return v && *v ? atoi(v) : 0;
}

extern "C" VkFgEngine vk_fg_engine(void) {
    static volatile int e = VK_FG_OFF;
    static volatile unsigned seen;   // the cfg_version read, + 1 (0: never)
    unsigned v = cfg_version();
    if (__atomic_load_n(&seen, __ATOMIC_ACQUIRE) != v + 1) {
        const char *s = cfg_value("TOTK_FRAME_GEN");
        int n = !strcmp(s, "fsr3") ? VK_FG_FSR3 : VK_FG_OFF;
        static char said[16];
        if (strncmp(said, s, sizeof said - 1)) {
            snprintf(said, sizeof said, "%s", s);
            if (n == VK_FG_OFF && strcmp(s, "off")) rt_log("fg: frame_gen = %s is not off or fsr3: off", s);
            else if (seen) rt_log("fg: frame_gen = %s", s);
        }
        e = n;
        __atomic_store_n(&seen, v + 1, __ATOMIC_RELEASE);
    }
    return (VkFgEngine)e;
}

// The settings and the contexts, once: at start with frame_gen = fsr3, else at the first frame after the menu turns it
// on (vk_fg_wanted).
static bool started;
static void start(void) {
    started = true;
    VkCtx *c = vk_ctx();
    if (!c) return;
    // The first contexts at the output size from the settings; vk_fg_prepare makes them again for the real scene
    uint32_t w = 1920, h = 1080;
    sscanf(cfg_value("TOTK_OUTPUT_RES"), "%ux%u", &w, &h);
    if (w < 64 || h < 64 || w > 16384 || h > 16384) w = 1920, h = 1080;
    FgConfig cf = {w, h, w, h, VK_FORMAT_R8G8B8A8_UNORM, 0, 0};
    if (!create(c, &cf)) {
        rt_log("fg: frame interpolation unavailable (the contexts could not be made)");
        g.live = false;  // (a half-made set is not destroyed through its context functions: only its memory is freed)
        sdk_destroy();
        g.failed = true;
        return;
    }
    g.win_fmt = VK_FORMAT_R8G8B8A8_UNORM;
    g.workers = cfg_int("TOTK_REPLAY_WORKERS");
    g.sync_ahead = cfg_bool("TOTK_SYNC_AHEAD");
    g.hdr = vk_hdr_wanted();
    g.test = knob_int("TOTK_FG_TEST");
    g.debug = knob_int("TOTK_FG_DEBUG");
    g.times = !cfg_get("TOTK_FG_TIMES") || knob_int("TOTK_FG_TIMES");
    const char *sh = cfg_get("TOTK_FG_SHOT");
    if (sh && (sh[0] == 'w' || sh[0] == 'W')) g.shot_w = (int64_t)(atof(sh + 1) * 1e6);
    else g.shot_at = (uint64_t)knob_int("TOTK_FG_SHOT");
    const char *d = cfg_get("TOTK_FG_SHOT_DIR");
    snprintf(g.shot_dir, sizeof g.shot_dir, "%s", d && *d ? d : ".");
    g.cpu_us = (uint32_t *)calloc(CPU_RING, sizeof *g.cpu_us);
    if (g.times) {
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP,
                                    TSLOTS * FG_T_N, 0};
        if (vkCreateQueryPool(c->device, &qi, NULL, &g.qp) != VK_SUCCESS) g.qp = VK_NULL_HANDLE;
    }
    rt_log("fg: FSR 3 frame interpolation contexts created at %ux%u (FidelityFX SDK 1.1.4)%s%s", w, h,
           g.test == 1 ? "; TOTK_FG_TEST=1: the generated frame is shown in place of the game's"
           : g.test == 2 ? "; TOTK_FG_TEST=2: the picture before the HUD is shown in place of the game's" : "",
           g.debug ? "; TOTK_FG_DEBUG: the SDK's debug view" : "");
}

extern "C" void vk_fg_init(void) {
    if (vk_fg_engine() == VK_FG_FSR3) start();
}

extern "C" int vk_fg_active(void) { return g.live && vk_fg_engine() != VK_FG_OFF; }

extern "C" void vk_fg_menu(int open) { __atomic_store_n(&g.menu, open, __ATOMIC_RELAXED); }

extern "C" int vk_fg_wanted(int dlss) {
    if (vk_fg_engine() != VK_FG_FSR3) return 0;
    if (!started) start();   // turned on in the menu: the contexts now; the first prepare makes them for the scene
    if (!g.live || g.failed) return 0;
    // the inputs are this frame's, recorded in replay order: one frame in flight more or chunks recording side by
    // side are not handled yet
    int why = dlss ? 1 : g.workers > 0 ? 2 : g.sync_ahead ? 3 : g.hdr ? 4 : 0;
    if (why) {
        if (!(g.said & 1u << why)) {
            g.said |= 1u << why;
            static const char *const r[] = {"", "DLSS runs (dlaa = 1)", "the replay records in chunks (replay_workers > 0)",
                                            "sync_ahead = 1", "HDR output (hdr = 1)"};
            rt_log("fg: frame generation off while %s", r[why]);
        }
        return 0;
    }
    return !__atomic_load_n(&g.menu, __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------- the camera
static struct {
    bool have, logged;
    float pos[3], fwd[3];
    int inverted, infinite;
} cam;

extern "C" int vk_fg_camera(const float *r, int minus_w, const float dr[2], VkFgScene *s) {
    // rows 0-2: the view, 3x4 by rows (right, up, back; the translation in .w); rows 7-10: the projection
    const float *v0 = r, *v1 = r + 4, *v2 = r + 8, *py = r + 8 * 4, *pz = r + 9 * 4, *pw = r + 10 * 4;
    for (int k = 0; k < 3; k++) {
        s->right[k] = v0[k], s->up[k] = v1[k], s->fwd[k] = -v2[k];
        s->pos[k] = -(v0[k] * v0[3] + v1[k] * v1[3] + v2[k] * v2[3]);
    }
    s->fov_y = 2.0f * atanf(1.0f / (py[1] > 1e-3f ? py[1] : 1.0f));
    // A point on the view axis at distance t: its clip z and w, the depth before the viewport's range (d01) and the
    // device depth the depth image holds
    auto d01 = [&](double t) {
        double zc = -pz[2] * t + pz[3], wc = -pw[2] * t + pw[3];
        double z = wc != 0 ? zc / wc : 0;
        return minus_w ? z * 0.5 + 0.5 : z;
    };
    auto dev = [&](double t) { return dr[0] + (dr[1] - dr[0]) * d01(t); };
    int inv01 = d01(1.0) > d01(100.0);
    s->inverted = dev(1.0) > dev(100.0);
    // the planes: where d01 reaches its near and far ends, in clip z: t = (tz * pw.w - pz.w) / (tz * pw.z - pz.z)
    auto plane = [&](double end) {
        double tz = minus_w ? end * 2 - 1 : end, den = tz * pw[2] - pz[2];
        return fabs(den) > 1e-12 ? (tz * pw[3] - pz[3]) / den : -1.0;
    };
    double tn = plane(inv01 ? 1.0 : 0.0), tf = plane(inv01 ? 0.0 : 1.0);
    s->infinite = !(tf > 0 && tf < 1e7);
    s->near_m = tn > 0 ? (float)tn : 0.1f;
    s->far_m = s->infinite ? 1e7f : (float)tf;
    if (!cam.logged || s->inverted != cam.inverted || s->infinite != cam.infinite) {
        cam.logged = true, cam.inverted = s->inverted, cam.infinite = s->infinite;
        rt_log("fg: camera from the scene block: near %.4g m, far %s%.6g m, vertical field of view %.1f degrees; depth %s "
               "(NVN depth mode %s, viewport depth range %g..%g; device depth at 1 m %.6f, 100 m %.6f, 10 km %.6f); "
               "projection z row %g %g %g %g, w row %g %g %g %g; view axes |right| %.3f |up| %.3f |back| %.3f; camera "
               "at %.1f %.1f %.1f", s->near_m, s->infinite ? "infinite, " : "", tf, s->fov_y * 57.29578f,
               s->inverted ? "inverted (1 near)" : "standard (0 near)", minus_w ? "-w..w" : "0..w", dr[0], dr[1],
               dev(1.0), dev(100.0), dev(10000.0), pz[0], pz[1], pz[2], pz[3], pw[0], pw[1], pw[2], pw[3],
               sqrtf(v0[0] * v0[0] + v0[1] * v0[1] + v0[2] * v0[2]), sqrtf(v1[0] * v1[0] + v1[1] * v1[1] + v1[2] * v1[2]),
               sqrtf(v2[0] * v2[0] + v2[1] * v2[1] + v2[2] * v2[2]), s->pos[0], s->pos[1], s->pos[2]);
        if (!minus_w || fabsf(dr[0]) > 1e-6f || fabsf(dr[1] - 1.0f) > 1e-6f)
            rt_log("fg: the camera motion pass (shaders/dlaa_mv.comp) takes depth as z_ndc * 0.5 + 0.5 over 0..1: this "
                   "depth mode or range does not fit it, its vectors are off for anything the camera moves past");
    }
    // a cut: no history (a jump of over 10 m, or the view turned over ~35 degrees in a frame)
    float dp = 0, dot = 0;
    for (int k = 0; k < 3; k++)
        dp += (s->pos[k] - cam.pos[k]) * (s->pos[k] - cam.pos[k]), dot += s->fwd[k] * cam.fwd[k];
    int cut = !cam.have || dp > 100.0f || dot < 0.82f;
    cam.have = true;
    memcpy(cam.pos, s->pos, sizeof cam.pos), memcpy(cam.fwd, s->fwd, sizeof cam.fwd);
    return cut;
}

// ---------------------------------------------------------------- timing
static void times_log(const char *what, const double *gpu, uint64_t timed) {
    double n = timed ? (double)timed : 1.0;
    rt_log("fg times %s: gpu camera motion %.3f ms, prepare %.3f ms, HUD-less copy %.3f ms, optical flow + "
           "interpolation %.3f ms (%llu frames timed)", what, gpu[0] / n, gpu[1] / n, gpu[2] / n, gpu[3] / n,
           (unsigned long long)timed);
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void cpu_log(const char *what, uint64_t from) {
    uint64_t n = g.ncpu - from;
    if (!n || !g.cpu_us) return;
    if (n > CPU_RING) n = CPU_RING, from = g.ncpu - n;
    uint32_t *v = (uint32_t *)malloc(n * sizeof *v);
    if (!v) return;
    for (uint64_t i = 0; i < n; i++) v[i] = g.cpu_us[(from + i) % CPU_RING];
    qsort(v, n, sizeof *v, cmp_u32);
    rt_log("fg cpu %s: replay-thread recording %.3f ms median, %.3f ms p99, %.3f ms max a frame (%llu frames)", what,
           v[n / 2] / 1e3, v[n * 99 / 100] / 1e3, v[n - 1] / 1e3, (unsigned long long)n);
    free(v);
}

extern "C" void vk_fg_mark(VkCommandBuffer cb, int which) {
    if (!g.qp) return;
    if (which == FG_T_MV0) {   // the frame's slot: its last frame's results are read, then it is reset
        g.qslot = (g.qslot + 1) % TSLOTS;
        if (g.qmask[g.qslot] == (1u << FG_T_N) - 1) {
            uint64_t q[FG_T_N];
            if (vkGetQueryPoolResults(vk_ctx()->device, g.qp, g.qslot * FG_T_N, FG_T_N, sizeof q, q, sizeof q[0],
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                double ms = vk_ctx()->props.limits.timestampPeriod / 1e6;
                double d[4] = {(double)(q[FG_T_MV1] - q[FG_T_MV0]) * ms, (double)(q[FG_T_PREP1] - q[FG_T_PREP0]) * ms,
                               (double)(q[FG_T_HL1] - q[FG_T_HL0]) * ms, (double)(q[FG_T_FI1] - q[FG_T_FI0]) * ms};
                for (int i = 0; i < 4; i++) g.gpu[i] += d[i], g.gpu_run[i] += d[i];
                g.timed++, g.timed_run++;
            }
        }
        g.qmask[g.qslot] = 0;
        vkCmdResetQueryPool(cb, g.qp, g.qslot * FG_T_N, FG_T_N);
    }
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g.qp, g.qslot * FG_T_N + (uint32_t)which);
    g.qmask[g.qslot] |= 1u << which;
}

extern "C" void vk_fg_cpu(uint64_t ns) { g.cpu_frame += ns; }

// ---------------------------------------------------------------- images
static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cb, &di);
}

static bool img_make(VkCtx *c, VkCommandBuffer cb, Img *m, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags u) {
    VkImageCreateInfo ii = {};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, ii.imageType = VK_IMAGE_TYPE_2D, ii.format = f;
    ii.extent = {w, h, 1}, ii.mipLevels = 1, ii.arrayLayers = 1, ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL, ii.usage = u, ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(c->vma, &ii, &ai, &m->img, &m->mem, NULL) != VK_SUCCESS) return m->img = VK_NULL_HANDLE, false;
    VkImageMemoryBarrier2 b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = m->img, b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di = {};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
    return true;
}

// The HUD-less copies and the generated frames at w x h, two of each (the present of the frame before may still read
// the other pair). Another size: the old ones go once every present and submission before is done.
static bool images_for(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (g.hl_srgb.img && g.iw == w && g.ih == h) return true;
    VkCtx *c = vk_ctx();
    if (g.hl_srgb.img) {
        vk_present_drain();
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        g.show_out = g.show_hl = VK_NULL_HANDLE;
    }
    img_free(c, &g.hl_srgb);
    for (int i = 0; i < 2; i++) img_free(c, &g.hl[i]), img_free(c, &g.out[i]);
    const VkImageUsageFlags t = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    bool ok = img_make(c, cb, &g.hl_srgb, w, h, VK_FORMAT_R8G8B8A8_SRGB, t);
    for (int i = 0; i < 2 && ok; i++)
        ok = img_make(c, cb, &g.hl[i], w, h, VK_FORMAT_R8G8B8A8_UNORM, t | VK_IMAGE_USAGE_SAMPLED_BIT) &&
             img_make(c, cb, &g.out[i], w, h, VK_FORMAT_R8G8B8A8_UNORM,
                      t | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    if (!ok) {
        rt_log("fg: no memory for the %ux%u pictures: frame generation off for this run", w, h);
        g.failed = true;
        return false;
    }
    g.iw = w, g.ih = h;
    return true;
}

static FfxResource res(VkImage img, VkFormat f, uint32_t w, uint32_t h, bool write, const wchar_t *name) {
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = f;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (write ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    // (every image of the runtime is in GENERAL: the backend takes both states as GENERAL and leaves them so)
    return ffxGetResourceVK((void *)img, ffxGetImageResourceDescriptionVK(img, ci, write ? FFX_RESOURCE_USAGE_UAV
                            : FFX_RESOURCE_USAGE_READ_ONLY), name, write ? FFX_RESOURCE_STATE_UNORDERED_ACCESS
                            : FFX_RESOURCE_STATE_GENERIC_READ);
}

static FfxResource shared(int i) { return g.shared.fpGetResource(&g.shared, g.res[i]); }

static VkFormat unorm_of(VkFormat f) {  // (the bytes as written: an sRGB image is interpolated as stored)
    return f == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : f == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : f;
}

// ---------------------------------------------------------------- the frame
extern "C" int vk_fg_prepare(VkCommandBuffer cb, const VkFgScene *s) {
    g.prepared = false;
    if (g.failed || !g.scratch_shared) return 0;
    // The game's dynamic resolution moves the scene between sizes (1600x896, 1432x808 and 1280x720 under a 1920x1080
    // output, seen within seconds in the town) and each remake waits for the device to idle (89 ms measured). The
    // contexts are made for a largest scene, the output's size (or a larger scene's, once one comes), and each frame
    // passes its own size as the SDK's renderSize: the SDK sizes its own images by maxRenderSize, dispatches over
    // renderSize and divides the motion vector scale by it, as its dynamic resolution path intends.
    FgConfig want = {s->ow, s->oh, s->w > s->ow ? s->w : s->ow, s->h > s->oh ? s->h : s->oh, g.win_fmt,
                     (uint32_t)s->inverted, (uint32_t)s->infinite};
    if (g.live) {   // a scene the contexts hold keeps them; a larger one makes them for the largest of both
        if (g.cfg.render_w > want.render_w) want.render_w = g.cfg.render_w;
        if (g.cfg.render_h > want.render_h) want.render_h = g.cfg.render_h;
    }
    if (!g.live || memcmp(&g.cfg, &want, sizeof want)) {
        // what the old contexts recorded is queued or submitted: every submission done before they go
        VkCtx *c = vk_ctx();
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        sdk_destroy();
        if (!create(c, &want)) {
            rt_log("fg: the contexts could not be made again: frame generation off for this run");
            sdk_destroy();
            g.failed = true;
            return 0;
        }
        g.chain = false;
        rt_log("fg: contexts %llu made for a %ux%u output from scenes up to %ux%u, %s depth%s, window texture format %d",
               (unsigned long long)++g.made, want.display_w, want.display_h, want.render_w, want.render_h,
               want.inverted ? "inverted" : "standard", want.infinite ? " without a far plane" : "", (int)want.color_format);
    }
    uint64_t now = kern_now_ns();
    double dt = g.last_prep_ns ? (double)(now - g.last_prep_ns) / 1e6 : 0.0;
    g.last_prep_ns = now;
    g.dt_ms = dt > 0.0 && dt < 100.0 ? (float)dt : 16.67f;
    g.near_m = s->near_m, g.far_m = s->far_m, g.fov_y = s->fov_y;
    // a frame at another scene size than the last: last frame's dilated depth and vectors are of the other size, and
    // the interpolation starts over from this one
    bool resized = s->w != g.rw || s->h != g.rh;
    g.rw = s->w, g.rh = s->h;
    g.toggle ^= 1;
    FfxFrameInterpolationPrepareDescription pd = {};
    pd.commandList = ffxGetCommandListVK(cb);
    pd.renderSize = {s->w, s->h};
    // the vectors are last frame's window position minus this frame's in scene pixels, y down: the SDK's convention
    // at a scale of one (its constant divides by the render size); no jitter without an upscaler
    pd.motionVectorScale = {1.0f, 1.0f};
    pd.frameTimeDelta = g.dt_ms;
    pd.cameraNear = s->near_m;
    pd.cameraFar = s->far_m;
    pd.viewSpaceToMetersFactor = 1;  // (TotK's units are metres)
    pd.cameraFovAngleVertical = s->fov_y;
    pd.depth = res(s->depth, VK_FORMAT_D32_SFLOAT, s->w, s->h, false, L"FG_Depth");
    pd.motionVectors = res(s->mv, VK_FORMAT_R16G16_SFLOAT, s->w, s->h, false, L"FG_MotionVectors");
    pd.frameID = ++g.frame_id;
    pd.dilatedDepth = shared(R_DILATED_DEPTH + g.toggle);
    pd.dilatedMotionVectors = shared(R_DILATED_MV + g.toggle);
    pd.reconstructedPrevDepth = shared(R_PREV_DEPTH + g.toggle);
    memcpy(pd.cameraPosition, s->pos, sizeof pd.cameraPosition);
    memcpy(pd.cameraUp, s->up, sizeof pd.cameraUp);
    memcpy(pd.cameraRight, s->right, sizeof pd.cameraRight);
    memcpy(pd.cameraForward, s->fwd, sizeof pd.cameraForward);
    vk_fg_mark(cb, FG_T_PREP0);
    uint64_t t0 = kern_now_ns();
    rec_drain();   // the SDK records into cb itself: the recorder must have made every call queued before
    uint64_t t1 = kern_now_ns();
    FfxErrorCode e = ffxFrameInterpolationPrepare(&g.fi, &pd);
    uint64_t t2 = kern_now_ns();
    g.drain_ns += t1 - t0, g.sdk_ns += t2 - t1, g.prep_ns += t2 - t1;
    vk_fg_mark(cb, FG_T_PREP1);
    if (e != FFX_OK) {
        rt_log("fg: the SDK's prepare failed (%#x): frame generation off for this run", (unsigned)e);
        g.failed = true;
        return 0;
    }
    g.reset = s->reset || !g.chain || resized;
    g.resized += resized && g.chain;
    g.prepared = true;
    return 1;
}

extern "C" int vk_fg_hudless(VkCommandBuffer cb, VkImage src, VkFormat fmt, uint32_t level, uint32_t layer, uint32_t w,
                             uint32_t h) {
    (void)fmt;
    if (!g.prepared || g.failed || !images_for(cb, w, h)) return 0;
    vk_fg_mark(cb, FG_T_HL0);
    VkImageBlit b = {};
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1};
    b.srcOffsets[1] = {(int32_t)w, (int32_t)h, 1};
    b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.dstOffsets[1] = {(int32_t)w, (int32_t)h, 1};
    vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, g.hl_srgb.img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
    full_barrier(cb);
    VkImageCopy cp = {};
    cp.srcSubresource = cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.extent = {w, h, 1};
    vkCmdCopyImage(cb, g.hl_srgb.img, VK_IMAGE_LAYOUT_GENERAL, g.hl[g.k].img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp);
    full_barrier(cb);
    vk_fg_mark(cb, FG_T_HL1);
    g.hl_ok = true;
    return 1;
}

// ---- shots (TOTK_FG_SHOT=<frame>): the window texture, the generated and the HUD-less picture of three frames from
// there, as PNGs in TOTK_FG_SHOT_DIR (fg_<frame>_real.png, _gen.png, _hudless.png)
struct Shot {
    VkBuffer buf;
    VmaAllocation mem;
    void *map;
    uint32_t w, h;
    int bgra;
    char path[300];
};

static void shot_write(void *arg) {
    Shot *s = (Shot *)arg;
    VkCtx *c = vk_ctx();
    vmaInvalidateAllocation(c->vma, s->mem, 0, VK_WHOLE_SIZE);
    uint8_t *p = (uint8_t *)s->map;
    if (s->bgra)
        for (size_t i = 0; i < (size_t)s->w * s->h; i++) {
            uint8_t t = p[4 * i];
            p[4 * i] = p[4 * i + 2], p[4 * i + 2] = t;
        }
    for (size_t i = 0; i < (size_t)s->w * s->h; i++) p[4 * i + 3] = 255;
    rt_write_png(s->path, p, s->w, s->h);
    rt_log("fg: shot %s", s->path);
    vmaDestroyBuffer(c->vma, s->buf, s->mem);
    free(s);
}

static void shot(VkCommandBuffer cb, uint64_t fence, VkImage img, VkFormat f, uint32_t w, uint32_t h, uint64_t frame,
                 const char *kind) {
    VkCtx *c = vk_ctx();
    Shot *s = (Shot *)calloc(1, sizeof *s);
    if (!s) return;
    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, bi.size = (VkDeviceSize)w * h * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &s->buf, &s->mem, &info) != VK_SUCCESS) return free(s);
    s->map = info.pMappedData, s->w = w, s->h = h;
    s->bgra = f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
    snprintf(s->path, sizeof s->path, "%s/fg_%llu_%s.png", g.shot_dir, (unsigned long long)frame, kind);
    VkBufferImageCopy r = {};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, s->buf, 1, &r);
    vk_completion_add(fence, shot_write, s);
}

extern "C" int vk_fg_generate(VkCommandBuffer cb, uint64_t fence, uint64_t frame, VkImage win, VkFormat fmt, uint32_t w,
                              uint32_t h) {
    if (!g.prepared || !g.hl_ok || g.failed) return 0;
    VkFormat wf = unorm_of(fmt);
    if (wf != VK_FORMAT_R8G8B8A8_UNORM && wf != VK_FORMAT_B8G8R8A8_UNORM) {
        static int said;
        if (!said) said = 1, rt_log("fg: the window texture's format %d is not 8-bit RGBA: no frame generated", (int)fmt);
        return 0;
    }
    if (wf != g.cfg.color_format || w != g.cfg.display_w || h != g.cfg.display_h || w != g.iw || h != g.ih) {
        g.win_fmt = wf;   // the next prepare makes the contexts for it
        static int said;
        if (said++ < 4)
            rt_log("fg: window texture %ux%u format %d against contexts for %ux%u format %d, HUD-less picture %ux%u: "
                   "no frame generated this time", w, h, (int)fmt, g.cfg.display_w, g.cfg.display_h,
                   (int)g.cfg.color_format, g.iw, g.ih);
        return 0;
    }
    VkImage hl = g.hl[g.k].img, out = g.out[g.k].img;
    vk_fg_mark(cb, FG_T_FI0);
    uint64_t t0 = kern_now_ns();
    rec_drain();   // the SDK records into cb itself
    uint64_t t1 = kern_now_ns();
    FfxCommandList cl = ffxGetCommandListVK(cb);
    FfxResource hud_less = res(hl, VK_FORMAT_R8G8B8A8_UNORM, w, h, false, L"FG_HudlessColor");
    FfxOpticalflowDispatchDescription od = {};
    od.commandList = cl;
    od.color = hud_less;
    od.reset = g.reset;
    od.backbufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    od.opticalFlowVector = shared(R_OF_VECTOR);
    od.opticalFlowSCD = shared(R_OF_SCD);
    FfxErrorCode e = ffxOpticalflowContextDispatch(&g.of, &od);
    if (e == FFX_OK) {
        FfxFrameInterpolationDispatchDescription dd = {};
        dd.flags = g.debug ? FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_VIEW : 0;
        dd.commandList = cl;
        dd.displaySize = {w, h};
        dd.renderSize = {g.rw, g.rh};
        dd.currentBackBuffer = res(win, wf, w, h, false, L"FG_Color");
        dd.currentBackBuffer_HUDLess = hud_less;
        dd.output = res(out, VK_FORMAT_R8G8B8A8_UNORM, w, h, true, L"FG_Output");
        dd.interpolationRect = {0, 0, (int)w, (int)h};
        dd.opticalFlowVector = shared(R_OF_VECTOR);
        dd.opticalFlowSceneChangeDetection = shared(R_OF_SCD);
        dd.opticalFlowBufferSize = {w / 8, h / 8};  // (unused by the shaders: the SDK's provider leaves it too)
        dd.opticalFlowScale = {1.0f / w, 1.0f / h};
        dd.opticalFlowBlockSize = 8;
        dd.cameraNear = g.near_m;
        dd.cameraFar = g.far_m;
        dd.cameraFovAngleVertical = g.fov_y;
        dd.viewSpaceToMetersFactor = 1;
        dd.frameTimeDelta = g.dt_ms;
        dd.reset = g.reset;
        dd.backBufferTransferFunction = FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        dd.minMaxLuminance[0] = 0;
        dd.minMaxLuminance[1] = 1;
        dd.frameID = g.frame_id;
        dd.dilatedDepth = shared(R_DILATED_DEPTH + g.toggle);
        dd.dilatedMotionVectors = shared(R_DILATED_MV + g.toggle);
        dd.reconstructedPrevDepth = shared(R_PREV_DEPTH + g.toggle);
        e = ffxFrameInterpolationDispatch(&g.fi, &dd);
    }
    g.drain_ns += t1 - t0, g.sdk_ns += kern_now_ns() - t1;
    full_barrier(cb);
    vk_fg_mark(cb, FG_T_FI1);
    if (e != FFX_OK) {
        rt_log("fg: the SDK's optical flow or interpolation failed (%#x): frame generation off for this run", (unsigned)e);
        g.failed = true;
        return 0;
    }
    g.gen_ok = true;
    g.gen_win = win;
    g.gens++, g.resets += g.reset;
    if (g.gens == 1)
        rt_log("fg: first frame generated: %ux%u from a %ux%u scene (near %g m, far %g m, vertical field of view %.1f "
               "degrees), window texture format %d", w, h, g.rw, g.rh, g.near_m, g.far_m,
               g.fov_y * 57.29578f, (int)fmt);
    if (g.shot_w > 0 && !g.shot_at && frame_step_world_us() >= g.shot_w) g.shot_at = frame;
    if (g.shot_at && frame >= g.shot_at && frame < g.shot_at + 3) {
        shot(cb, fence, win, fmt, w, h, frame, "real");
        shot(cb, fence, out, VK_FORMAT_R8G8B8A8_UNORM, w, h, frame, g.reset ? "gen-reset" : "gen");
        shot(cb, fence, hl, VK_FORMAT_R8G8B8A8_UNORM, w, h, frame, "hudless");
        full_barrier(cb);
    }
    return 1;
}

extern "C" void vk_fg_frame(void) {
    if (g.prepared || g.cpu_frame) {
        if (g.cpu_us) g.cpu_us[g.ncpu % CPU_RING] = (uint32_t)(g.cpu_frame / 1000);
        g.ncpu++;
        if (!g.gen_ok) g.skipped++;
    }
    g.show_out = g.gen_ok ? g.out[g.k].img : VK_NULL_HANDLE;
    g.show_win = g.gen_ok ? g.gen_win : VK_NULL_HANDLE;
    g.show_hl = g.hl_ok ? g.hl[g.k].img : VK_NULL_HANDLE;
    if (g.hl_ok) g.k ^= 1;
    g.chain = g.gen_ok;   // a frame without a generated picture breaks the chain: the next one starts over
    g.prepared = g.hl_ok = g.gen_ok = false;
    g.cpu_frame = 0;
    if (g.gens >= g.gens_logged + LOG_EVERY) {
        static uint64_t from;
        rt_log("fg: %llu frames generated (%llu without history, %llu of them at a new scene size), %llu frames prepared "
               "without one", (unsigned long long)g.gens, (unsigned long long)g.resets, (unsigned long long)g.resized,
               (unsigned long long)g.skipped);
        if (g.timed) times_log("last frames", g.gpu, g.timed);
        cpu_log("last frames", from);
        uint64_t n = g.ncpu - from ? g.ncpu - from : 1;
        rt_log("fg cpu last frames: of it recorder drains %.3f ms, the SDK's recording %.3f ms (prepare %.3f ms) a frame "
               "(means)", g.drain_ns / 1e6 / n, g.sdk_ns / 1e6 / n, g.prep_ns / 1e6 / n);
        g.drain_run += g.drain_ns, g.sdk_run += g.sdk_ns, g.prep_run += g.prep_ns, g.drain_ns = g.sdk_ns = g.prep_ns = 0;
        from = g.ncpu;
        memset(g.gpu, 0, sizeof g.gpu), g.timed = 0;
        g.gens_logged = g.gens;
    }
}

extern "C" void vk_fg_exit_log(void) {
    static int done;
    if (done || !g.ncpu) return;
    done = 1;
    rt_log("fg: over the run %llu frames generated (%llu without history, %llu of them at a new scene size), %llu "
           "frames prepared without one", (unsigned long long)g.gens, (unsigned long long)g.resets,
           (unsigned long long)g.resized, (unsigned long long)g.skipped);
    if (g.timed_run) times_log("over the run", g.gpu_run, g.timed_run);
    cpu_log("over the run", 0);
    rt_log("fg cpu over the run: of it recorder drains %.3f ms, the SDK's recording %.3f ms (prepare %.3f ms) a frame "
           "(means)", (g.drain_run + g.drain_ns) / 1e6 / g.ncpu, (g.sdk_run + g.sdk_ns) / 1e6 / g.ncpu,
           (g.prep_run + g.prep_ns) / 1e6 / g.ncpu);
}

extern "C" VkImage vk_fg_present_gen(VkImage presented) {
    if (g.test || !g.show_out || presented != g.show_win) return VK_NULL_HANDLE;
    return g.show_out;
}

extern "C" int vk_fg_present_swap(VkImage *img, VkFormat *fmt) {
    VkImage s = g.test == 1 ? g.show_out : g.test == 2 ? g.show_hl : VK_NULL_HANDLE;
    if (!s) return 0;
    *img = s, *fmt = VK_FORMAT_R8G8B8A8_UNORM;
    return 1;
}
