// DLAA: NVIDIA DLSS at the scene's own size (TOTK_DLAA, totk.ini dlaa). NGX is loaded at run time from the driver's
// NGX core: on Windows _nvngx.dll, found through HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore; on Linux
// libnvidia-ngx.so.1, which the NVIDIA driver installs on the library path. NGX loads the DLSS model from the folder of
// the executable: nvngx_dlss.dll on Windows, libnvidia-ngx-dlss.so.<version> on Linux. No NVIDIA SDK code or header is
// compiled in: the few NGX types used are declared here from the SDK's documented ABI, the way the Metroid Dread
// recomp's dlss.c reaches the core (read for behaviour only).
//
// The core's parameter object is a C++ class whose virtual functions are called through its vtable: the Set overloads
// in slots 0-7 and the Get overloads in 8-15. MSVC (the Windows core) lays out each group of overloads in reverse
// declaration order, the Itanium C++ ABI (the Linux core, built with GCC) in declaration order. vtable_ok checks the
// layout with a set-then-get round trip before anything relies on it.
//
// Where DLAA runs in the frame, the jitter and the motion vectors: see the sections below.
//
// dlaa_backend = fsr (TOTK_DLAA_BACKEND, read at start) hands the same frame to AMD's FSR instead (vk_ffx.c): the
// arming, the jitter, the motion vectors, the masks and the copy of the result below are shared, and only the call that
// upscales differs (evaluate). NGX is then never loaded.
#ifdef _WIN32
#include <windows.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_exec.h"
#include "vk_ffx.h"
#include "vk_rec.h"
#include "dlaa_mv_spv.h"

typedef int NgxResult;   // NVSDK_NGX_Result: 1 success, 0xBAD00000 | n failures
#define NGX_OK(r) (((unsigned)(r) & 0xFFF00000u) != 0xBAD00000u)
#define NGX_VERSION_API 0x15
#define NGX_FEATURE_SUPERSAMPLING 1
#define NGX_PERFQ_DLAA 5   // NVSDK_NGX_PerfQuality_Value: 0 performance, 1 balanced, 2 quality, 3 ultra performance
typedef struct NgxParam NgxParam;
typedef struct NgxHandle NgxHandle;
typedef struct { void *slot[17]; } NgxParamVtbl;
struct NgxParam { const NgxParamVtbl *vt; };
typedef struct {   // NVSDK_NGX_ImageViewInfo_VK
    VkImageView view;
    VkImage image;
    VkImageSubresourceRange range;
    VkFormat format;
    unsigned width, height;
} NgxImageView;
typedef struct {   // NVSDK_NGX_Resource_VK
    union { NgxImageView img; struct { VkBuffer buffer; unsigned bytes; } buf; } r;
    int type;      // 0 image view, 1 buffer
    _Bool rw;
} NgxResourceVK;

typedef NgxResult (*PFN_Init)(unsigned long long app, const wchar_t *path, VkInstance, VkPhysicalDevice, VkDevice,
                              PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, int version, const void *info);
typedef NgxResult (*PFN_ReqExt)(unsigned *ni, const char ***ie, unsigned *nd, const char ***de);
typedef NgxResult (*PFN_Params)(NgxParam **);
typedef NgxResult (*PFN_DestroyParams)(NgxParam *);
typedef NgxResult (*PFN_Create)(VkCommandBuffer, int feature, NgxParam *, NgxHandle **);
typedef NgxResult (*PFN_Evaluate)(VkCommandBuffer, const NgxHandle *, const NgxParam *, void *callback);
typedef NgxResult (*PFN_Release)(NgxHandle *);

static struct {
    void *core;   // the NGX core library (an HMODULE on Windows)
    PFN_Init init;
    PFN_ReqExt req_ext;
    PFN_Params caps, alloc_params;
    PFN_DestroyParams destroy_params;
    PFN_Create create;
    PFN_Evaluate evaluate;
    PFN_Release release;
    int tried, loaded, available;
    int wanted;   // -1 not read yet
} N = {.wanted = -1};

typedef void (*SetI)(NgxParam *, const char *, int);
typedef void (*SetU)(NgxParam *, const char *, unsigned);
typedef void (*SetF)(NgxParam *, const char *, float);
typedef void (*SetP)(NgxParam *, const char *, void *);
typedef NgxResult (*GetI)(const NgxParam *, const char *, int *);
typedef NgxResult (*GetU)(const NgxParam *, const char *, unsigned *);
typedef NgxResult (*GetF)(const NgxParam *, const char *, float *);
typedef NgxResult (*GetP)(const NgxParam *, const char *, void **);
// The SDK declares Set(u64), Set(float), Set(double), Set(u32), Set(int), Set(ID3D11Resource *), Set(ID3D12Resource *),
// Set(void *), then the Get overloads in the same order, then Reset.
#ifdef _WIN32
enum { S_P = 0, S_D12, S_D11, S_I, S_U32, S_D, S_F, S_U64, G_P, G_D12, G_D11, G_I, G_U32, G_D, G_F, G_U64 };
#else
enum { S_U64 = 0, S_F, S_D, S_U32, S_I, S_D11, S_D12, S_P, G_U64, G_F, G_D, G_U32, G_I, G_D11, G_D12, G_P };
#endif
static void p_seti(NgxParam *p, const char *n, int v) { ((SetI)p->vt->slot[S_I])(p, n, v); }
static void p_setu(NgxParam *p, const char *n, unsigned v) { ((SetU)p->vt->slot[S_U32])(p, n, v); }
static void p_setf(NgxParam *p, const char *n, float v) { ((SetF)p->vt->slot[S_F])(p, n, v); }
static void p_setp(NgxParam *p, const char *n, void *v) { ((SetP)p->vt->slot[S_P])(p, n, v); }
static int p_geti(const NgxParam *p, const char *n, int *v) { return NGX_OK(((GetI)p->vt->slot[G_I])(p, n, v)); }
static int p_getu(const NgxParam *p, const char *n, unsigned *v) { return NGX_OK(((GetU)p->vt->slot[G_U32])(p, n, v)); }
static int p_getf(const NgxParam *p, const char *n, float *v) { return NGX_OK(((GetF)p->vt->slot[G_F])(p, n, v)); }
static int p_getp(const NgxParam *p, const char *n, void **v) { return NGX_OK(((GetP)p->vt->slot[G_P])(p, n, v)); }

#ifdef _WIN32
#define DLSS_MODEL "nvngx_dlss.dll"
#else
#define DLSS_MODEL "libnvidia-ngx-dlss.so.<version>"
#endif

// NGX is brought up only when dlaa is on at start: its device extensions can only be asked for at device creation, and
// loading the driver's NGX core on every NVIDIA machine would cost start time and change the device with DLAA off. So
// dlaa applies after a restart; switched off, nothing else of DLAA runs.
int vk_dlaa_wanted(void) {
    static int wanted = -1;
    if (wanted < 0) wanted = cfg_bool("TOTK_DLAA");
    return wanted;
}

// FSR instead of DLSS (TOTK_DLAA_BACKEND=fsr), fixed for the run like dlaa: the backend's extensions are chosen at
// device creation. On Linux FSR is not available, and this choice leaves DLAA off rather than falling back to DLSS.
static int fsr_backend(void) {
    static int fsr = -1;
    if (fsr < 0) fsr = !strcmp(cfg_value("TOTK_DLAA_BACKEND"), "fsr");
    return fsr;
}

static int ngx_wanted(void) { return vk_dlaa_wanted() && !fsr_backend(); }

static int core_load(void) {
    if (N.tried) return N.loaded;
    N.tried = 1;
#ifdef _WIN32
    char dir[MAX_PATH] = {0}, path[MAX_PATH + 32];
    DWORD n = sizeof dir;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", "FullPath", RRF_RT_REG_SZ,
                     NULL, dir, &n) != ERROR_SUCCESS) {
        rt_log("dlaa: the driver has no NGX core (registry NGXCore missing): DLAA off");
        return 0;
    }
    snprintf(path, sizeof path, "%s\\_nvngx.dll", dir);
    N.core = LoadLibraryA(path);
    if (!N.core) {
        rt_log("dlaa: cannot load %s (error %lu): DLAA off", path, GetLastError());
        return 0;
    }
#define FN(f, name) N.f = (void *)GetProcAddress((HMODULE)N.core, name)
#else
    // The driver's NGX core; the dynamic loader finds it where the driver put it (/usr/lib, /usr/lib/x86_64-linux-gnu).
    const char *path = "libnvidia-ngx.so.1";
    N.core = plat_lib_open(path);
    if (!N.core) {
        rt_log("dlaa: cannot load %s (not an NVIDIA driver, or one without NGX): DLAA off", path);
        return 0;
    }
#define FN(f, name) N.f = plat_lib_sym(N.core, name)
#endif
    FN(init, "NVSDK_NGX_VULKAN_Init_Ext2");
    FN(req_ext, "NVSDK_NGX_VULKAN_RequiredExtensions");
    FN(caps, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
    FN(alloc_params, "NVSDK_NGX_VULKAN_AllocateParameters");
    FN(destroy_params, "NVSDK_NGX_VULKAN_DestroyParameters");
    FN(create, "NVSDK_NGX_VULKAN_CreateFeature");
    FN(evaluate, "NVSDK_NGX_VULKAN_EvaluateFeature");
    FN(release, "NVSDK_NGX_VULKAN_ReleaseFeature");
#undef FN
    if (!N.init || !N.req_ext || !N.caps || !N.alloc_params || !N.create || !N.evaluate || !N.release) {
        rt_log("dlaa: the NGX core %s lacks Vulkan entry points: DLAA off", path);
        return 0;
    }
    N.loaded = 1;
    rt_log("dlaa: NGX core %s", path);
    return 1;
}

static int listed(const char *const *list, uint32_t n, const char *s) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(list[i], s)) return 1;
    return 0;
}

void vk_dlaa_instance_exts(const char **ext, uint32_t *n, uint32_t max) {
    unsigned ni = 0, nd = 0;
    const char **ie = NULL, **de = NULL;
    if (!ngx_wanted() || !core_load() || !NGX_OK(N.req_ext(&ni, &ie, &nd, &de))) return;
    for (unsigned i = 0; i < ni; i++)
        if (!listed(ext, *n, ie[i]) && *n < max) ext[(*n)++] = ie[i];
}

void vk_dlaa_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n,
                         uint32_t max) {
    unsigned ni = 0, nd = 0;
    const char **ie = NULL, **de = NULL;
    if (fsr_backend()) {
        vk_ffx_device_exts(have, nhave, ext, n, max);
        return;
    }
    if (!ngx_wanted() || !core_load() || !NGX_OK(N.req_ext(&ni, &ie, &nd, &de))) return;
    for (unsigned i = 0; i < nd; i++) {
        int ok = 0;
        for (uint32_t k = 0; k < nhave && !ok; k++) ok = !strcmp(have[k].extensionName, de[i]);
        if (!ok) rt_log("dlaa: the device lacks %s, which NGX asks for", de[i]);
        else if (!listed(ext, *n, de[i]) && *n < max) ext[(*n)++] = de[i];
    }
}

static int vtable_ok(void) {
    NgxParam *p = NULL;
    if (!NGX_OK(N.alloc_params(&p)) || !p) return 0;
    int i = 0;
    unsigned u = 0;
    float f = 0;
    void *v = NULL;
    p_seti(p, "TotK.Test.I", -12345);
    p_setu(p, "TotK.Test.U", 4000000000u);
    p_setf(p, "TotK.Test.F", 1.25f);
    p_setp(p, "TotK.Test.P", (void *)p);
    int ok = p_geti(p, "TotK.Test.I", &i) && i == -12345 && p_getu(p, "TotK.Test.U", &u) && u == 4000000000u &&
             p_getf(p, "TotK.Test.F", &f) && f == 1.25f && p_getp(p, "TotK.Test.P", &v) && v == (void *)p;
    if (N.destroy_params) N.destroy_params(p);
    if (!ok) rt_log("dlaa: NGX parameter layout check failed (i=%d u=%u f=%g p=%p): DLAA off", i, u, f, v);
    return ok;
}

#ifndef _WIN32
// UTF-8 to UTF-32 wchar_t, without the C library's locale (the runtime never sets one, so mbstowcs would stop at the
// first non-ASCII byte). Returns 0 on malformed input or when out has no room.
static int utf8_to_wide(const char *in, wchar_t *out, size_t cap) {
    const unsigned char *u = (const unsigned char *)in;
    size_t n = 0;
    while (*u) {
        unsigned c = *u++, more = c < 0x80 ? 0 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 4;
        if (more == 4) return 0;
        if (more) c &= 0x3F >> more;
        for (unsigned k = 0; k < more; k++, u++) {
            if ((*u & 0xC0) != 0x80) return 0;
            c = c << 6 | (*u & 0x3F);
        }
        if (n + 1 >= cap) return 0;
        out[n++] = (wchar_t)c;
    }
    out[n] = 0;
    return 1;
}
#endif

void vk_dlaa_init(void) {
    if (vk_dlaa_wanted() && fsr_backend()) {
        vk_ffx_init();
        return;
    }
    if (!ngx_wanted() || !core_load()) return;
#ifdef _WIN32
    wchar_t wdir[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, wdir, MAX_PATH);
    while (len && wdir[len - 1] != L'\\') len--;
    wdir[len] = 0;
#else
    // The executable's folder, where NGX looks for libnvidia-ngx-dlss.so and writes its logs, as a wchar_t string:
    // 32-bit code points here, as the Linux core expects.
    char dir[PLAT_PATH_MAX];
    wchar_t wdir[PLAT_PATH_MAX];
    char *cut = plat_exe_path(dir, sizeof dir) ? plat_last_sep(dir) : NULL;
    if (!cut || !utf8_to_wide((cut[1] = 0, dir), wdir, PLAT_PATH_MAX)) {
        rt_log("dlaa: cannot name the executable's folder for NGX: DLAA off");
        return;
    }
#endif
    // 231313132: the application id NVIDIA's DLSS samples use for builds without an NVIDIA-issued id
    const VkCtx *c = vk_ctx();
    NgxResult r = N.init(231313132ull, wdir, c->instance, c->phys, c->device, vkGetInstanceProcAddr,
                         vkGetDeviceProcAddr, NGX_VERSION_API, NULL);
    if (!NGX_OK(r)) {
        rt_log("dlaa: NGX init failed (%08x): DLAA off", (unsigned)r);
        return;
    }
    if (!vtable_ok()) return;
    NgxParam *caps = NULL;
    int avail = 0, driver = 0;
    if (NGX_OK(N.caps(&caps)) && caps) {
        p_geti(caps, "SuperSampling.Available", &avail);
        p_geti(caps, "SuperSampling.NeedsUpdatedDriver", &driver);
    }
    N.available = avail != 0;
    rt_log("dlaa: NGX up; DLSS %s%s", avail ? "available" : "NOT available (is " DLSS_MODEL " next to the executable?)",
           driver ? "; the driver needs an update" : "");
}

// The backend of this run came up: DLSS through NGX, or FSR (vk_ffx.c).
static int be_available(void) { return fsr_backend() ? vk_ffx_available() : N.available; }

int vk_dlaa_available(void) { return vk_dlaa_wanted() && be_available(); }

static int dl_active(void);
static uint64_t dl_evals(void);

// The temporal upscaler at work, for the performance overlay (ui/menu.cpp perf_picture): "FSR 4.1.2" or "DLSS" once
// it has run, NULL while DLAA is off, failed or has not run yet.
const char *vk_dlaa_name(void) {
    if (!dl_active() || !dl_evals()) return NULL;
    return fsr_backend() ? vk_ffx_version() : "DLSS";
}

// ---------------------------------------------------------------- where DLAA runs in TotK's frame
// Measured (TOTK_DUMP, frame 2100 at Lookout Landing, 2560x1440 scene): the G-buffer passes draw into five targets with
// the scene depth (d32f at the scene size), one of them the R11G11B10F light target; lighting and the transparents
// then draw into that light target alone, the last time with the scene depth attached; the first pass narrower than a
// quarter of the scene starts the bloom chain, which reads it. After the bloom come the tonemap and post passes, then
// the HUD is drawn into the same target and a last pass copies it to the 8-bit output.
// So: after the G-buffer, a pass at the scene size with exactly one colour target (R11G11B10F or RGBA16F) and the D32F
// depth arms the frame with that pair; the next pass at most a quarter as wide ends the scene (no jitter after it) and,
// when the scene is as wide as the output, runs DLAA on the armed target first. Bloom, tonemap, post and HUD then all
// work on the anti-aliased picture.
// Upscaling (a scene smaller than the output: TOTK_DLSS quality/balanced/performance, or the game's own 5/6): measured
// at a 960x536 scene, 1920x1080 output, the bloom and its composite write back into the light target at the scene size,
// then one full-screen draw (the FXAA shader below, which also scales) reads it into an output-sized R11G11B10F target
// with no depth, and the HUD is drawn over that target at the output size. DLSS runs at the begin of that pass: it
// reads the light target and its result is blitted into the pass's target, whose scaling draw is then skipped. The
// HUD, drawn afterwards into the same target, is never jittered nor passed through DLSS.
// The scaling draw is not always that FXAA shader: at a 1280x720 scene and 1920x1080 output (1.4.3, dlss = quality)
// the game's tonemap (guest fs 7e114c4330dc212a) reads the light target and writes the output-sized target itself,
// over DLSS's picture, so the jittered scene reached the screen and the whole picture shook. So from DLSS's evaluation
// to the frame's end, draws in passes larger than the light target sample DLSS's output in its place
// (vk_dlaa_swap_img, applied by vk_exec_draw.c tex_info): whichever shader scales, it scales DLSS's picture.
static struct {
    VkExecTarget color, depth;   // the armed pair, valid in the frame it was armed
    int armed, done, gbuf;       // this frame: armed on the light target, DLAA ran, the G-buffer was drawn
    int ended, up;               // this frame: the scene is drawn (jitter over), DLSS wrote the scaled output
    struct Size {                // a DLSS feature and its work images per scene size (dynamic resolution flips it)
        uint32_t w, h, ow, oh;   // render size, output size
        VkImage out, mv;
        VmaAllocation out_mem, mv_mem;
        VkImageView out_view, mv_view;
        VkImage mask, chars[2];  // the bias mask DLSS reads, the character class mask of this and last frame (R8)
        VmaAllocation mask_mem, chars_mem[2];
        VkImageView mask_view, chars_view[2];
        int chars_cur, chars_ok; // chars[chars_cur] is written next; chars_ok: the other holds last frame's
        NgxHandle *feat;
        void *ffx;               // the FSR context instead (fsr_backend)
        uint64_t used;           // evals count when it last ran
    } sizes[5], *last;           // last: the one the previous DLAA frame ran with
    VkImage last_color;          // the target DLAA ran on last frame: another one means a new history
    NgxParam *params;
    int failed, reset;
    uint64_t evals, frames_missed;
    // jitter (vk_dlaa_viewport)
    VkImage jdepth;              // the scene depth of the last frame DLAA ran on: passes drawing with it are jittered
    float jx, jy;                // the offset the next scene draws take, render pixels (window x right, y down)
    float ux, uy;                // the offset this frame's scene draws took (DLSS and the motion vectors get these)
    uint32_t jframe;
    int jit_ok, jit_used, hold;  // jitter allowed this frame / a draw used it / frames left without it
    uint64_t holds;
    // camera (vk_dlaa_draw) and motion vectors (mv_pass)
    uint64_t cam_addr;           // this frame's scene camera block (c4 of a scene draw's vertex stage), 0: none seen
    VkBuffer camb;               // two CAM_SLOT slots: this frame's block and last frame's
    VmaAllocation camb_mem;
    VkBuffer mvm;                // MVM_SLOTS pairs of blocks for dlaa_mv.comp: the identity and the reprojection (mv_pass)
    VmaAllocation mvm_mem;
    uint8_t *mvm_map;
    uint32_t mvm_slot;
    double cam_prev[16];         // last frame's view-projection (rows 3-6 of its block), for the reprojection
    int cam_slot, cam_prev_ok;   // the slot this frame writes / the other slot holds last frame's camera
    VkDescriptorSetLayout mv_dsl;
    VkPipelineLayout mv_layout;
    VkPipeline mv_pipe;
    VkSampler mv_samp;
    int mv_ok;                   // -1 not built, 0 failed, 1 ready
    uint64_t mv_frames, mv_none, masked;   // masked: frames with the characters' bias mask
    uint64_t why[3];             // history resets: scene size changed, no camera, scene target changed
    int on;                      // the dlaa setting, read at each present (the menu switches it live)
    int preset, preset_dirty;    // the DLSS model features are created with (preset_value); a change pending
    VkExecTarget ids;            // this frame's G-buffer target 0 (object classes in green), valid with ids_ok
    int ids_ok;
    unsigned cfg_seen;           // cfg_version when preset was read
    uint64_t last_eval_ns;       // plat_now_ns of the previous evaluation (FrameTimeDeltaInMsec)
} D = {.mv_ok = -1, .cfg_seen = ~0u};

const VkTexImage *vk_dlaa_swap_img;
VkImageView vk_dlaa_swap_view;

static int dl_active(void) { return be_available() && !D.failed && D.on; }
static uint64_t dl_evals(void) { return D.evals; }

static void dl_fail(const char *what, NgxResult r) {
    rt_log("dlaa: %s failed (%08x): DLAA off for this run", what, (unsigned)r);
    D.failed = 1;
}

static int image2d(uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage, VkImage *img, VmaAllocation *mem,
                   VkImageView *view) {
    const VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, img, mem, NULL) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(c->device, &vi, NULL, view) == VK_SUCCESS;
}

static void free_size(struct Size *e) {
    const VkCtx *c = vk_ctx();
    vkDeviceWaitIdle(c->device);   // a third scene size: the evicted images may still be in flight
    if (e->out_view) vkDestroyImageView(c->device, e->out_view, NULL);
    if (e->mv_view) vkDestroyImageView(c->device, e->mv_view, NULL);
    if (e->out) vmaDestroyImage(c->vma, e->out, e->out_mem);
    if (e->mv) vmaDestroyImage(c->vma, e->mv, e->mv_mem);
    if (e->mask_view) vkDestroyImageView(c->device, e->mask_view, NULL);
    if (e->mask) vmaDestroyImage(c->vma, e->mask, e->mask_mem);
    for (int i = 0; i < 2; i++) {
        if (e->chars_view[i]) vkDestroyImageView(c->device, e->chars_view[i], NULL);
        if (e->chars[i]) vmaDestroyImage(c->vma, e->chars[i], e->chars_mem[i]);
    }
    if (e->feat) N.release(e->feat);
    if (e->ffx) vk_ffx_release(e->ffx);
    memset(e, 0, sizeof *e);
}

// The entry for a scene and output size: its output (RGBA16F at the output size, written by DLSS, then blitted into
// the target) and motion vectors (RG16F at the scene size, render pixels), both kept in GENERAL like every image of the
// back end. TotK's dynamic resolution switches between a few sizes; each keeps its feature, the least recently used
// goes for a new one.
static struct Size *size_for(VkCommandBuffer cb, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) {
    struct Size *e = NULL, *lru = &D.sizes[0];
    for (int i = 0; i < 5; i++) {
        if (D.sizes[i].w == w && D.sizes[i].h == h && D.sizes[i].ow == ow && D.sizes[i].oh == oh) return &D.sizes[i];
        if (!D.sizes[i].out && !e) e = &D.sizes[i];
        if (D.sizes[i].used < lru->used) lru = &D.sizes[i];
    }
    if (!e) {
        if (D.last == lru) D.last = NULL;
        free_size(e = lru);
    }
    VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageUsageFlags u8 = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!image2d(ow, oh, VK_FORMAT_R16G16B16A16_SFLOAT, u, &e->out, &e->out_mem, &e->out_view) ||
        !image2d(w, h, VK_FORMAT_R16G16_SFLOAT, u | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &e->mv, &e->mv_mem, &e->mv_view) ||
        !image2d(w, h, VK_FORMAT_R8_UNORM, u8, &e->mask, &e->mask_mem, &e->mask_view) ||
        !image2d(w, h, VK_FORMAT_R8_UNORM, u8, &e->chars[0], &e->chars_mem[0], &e->chars_view[0]) ||
        !image2d(w, h, VK_FORMAT_R8_UNORM, u8, &e->chars[1], &e->chars_mem[1], &e->chars_view[1]))
        return NULL;
    VkImageMemoryBarrier2 ib[5];
    VkImage im[5] = {e->out, e->mv, e->mask, e->chars[0], e->chars[1]};
    for (int i = 0; i < 5; i++)
        ib[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im[i],
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 5,
                           .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(cb, &di);
    e->w = w, e->h = h, e->ow = ow, e->oh = oh;
    return e;
}

static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static NgxResourceVK res_image(VkImageView v, VkImage img, VkFormat f, uint32_t w, uint32_t h, VkImageAspectFlags a,
                               int rw) {
    NgxResourceVK r;
    memset(&r, 0, sizeof r);
    r.r.img = (NgxImageView){v, img, {a, 0, 1, 0, 1}, f, w, h};
    r.type = 0;
    r.rw = rw;
    return r;
}

// ---------------------------------------------------------------- timing (TOTK_DLAA_TIMES=1)
// GPU timestamps around the camera motion dispatch, the per-object motion pass and DLSS's evaluation, and the CPU
// time of NGX's call and of draining the recorder before it, summed and logged every TIMES_SUMMARY frames:
//   dlaa times: gpu camera motion a ms, object motion b ms, DLSS c ms (frames timed); cpu: recorder drain d ms, NGX
//   evaluate e ms, motion pass recording f ms
// Each frame takes its own slot of TIMES_SLOTS; a slot's results are read when it comes round again (by then the GPU is
// long done with it; not ready: that frame is left out).
enum { T_MV0, T_MV1, T_OBJ, T_DL0, T_DL1, T_N };
#define TIMES_SLOTS 8u
#define TIMES_SUMMARY 600
static struct {
    int on;                      // -1 not read
    VkQueryPool pool;
    uint32_t slot;
    int used[TIMES_SLOTS];       // the slot holds a frame's timestamps (all T_N written)
    double gpu[3];               // ms summed: camera motion, object motion, DLSS
    uint64_t frames, cpu_drain, cpu_ngx, cpu_mv;   // ns summed
} T = {.on = -1};

static int times_on(void) {
    if (T.on < 0) {
        T.on = rt_knob("DLAA_TIMES", "0");
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = TIMES_SLOTS * T_N};
        if (T.on && vkCreateQueryPool(vk_ctx()->device, &qi, NULL, &T.pool) != VK_SUCCESS) T.on = 0;
    }
    return T.on;
}

static void times_mark(VkCommandBuffer cb, int which) {
    if (times_on()) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, T.pool, T.slot * T_N + (uint32_t)which);
}

// At the start of a frame's timing (mv_pass): the slot's last results are read and it is reset.
static void times_begin(VkCommandBuffer cb) {
    if (!times_on()) return;
    T.slot = (T.slot + 1) % TIMES_SLOTS;
    if (T.used[T.slot]) {
        uint64_t q[T_N];
        if (vkGetQueryPoolResults(vk_ctx()->device, T.pool, T.slot * T_N, T_N, sizeof q, q, sizeof q[0],
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            double ns = vk_ctx()->props.limits.timestampPeriod / 1e6;   // ticks to ms
            T.gpu[0] += (double)(q[T_MV1] - q[T_MV0]) * ns, T.gpu[1] += (double)(q[T_OBJ] - q[T_MV1]) * ns;
            T.gpu[2] += (double)(q[T_DL1] - q[T_DL0]) * ns;
            if (++T.frames % TIMES_SUMMARY == 0) {
                rt_log("dlaa times: gpu camera motion %.3f ms, object motion %.3f ms, DLSS %.3f ms (%llu frames timed); "
                       "cpu: recorder drain %.3f ms, NGX evaluate %.3f ms, motion pass recording %.3f ms",
                       T.gpu[0] / TIMES_SUMMARY, T.gpu[1] / TIMES_SUMMARY, T.gpu[2] / TIMES_SUMMARY,
                       (unsigned long long)T.frames, T.cpu_drain / 1e6 / TIMES_SUMMARY, T.cpu_ngx / 1e6 / TIMES_SUMMARY,
                       T.cpu_mv / 1e6 / TIMES_SUMMARY);
                T.gpu[0] = T.gpu[1] = T.gpu[2] = 0, T.cpu_drain = T.cpu_ngx = T.cpu_mv = 0;
            }
        }
    }
    T.used[T.slot] = 0;
    vkCmdResetQueryPool(cb, T.pool, T.slot * T_N, T_N);
}

// ---------------------------------------------------------------- motion vectors
// The camera's motion first (runtime/shaders/dlaa_mv.comp): this frame's scene camera block is copied out of the
// replay's uniform snapshot (vk_usnap.c, as of the submit call) into one slot of camb, last frame's stays in the other,
// and the compute pass reprojects every pixel's depth through the two (inv4 and mv_pass below). Then the per-object
// pass (vk_exec_draw.c, vk_mvobj_execute) writes the motion of whatever moves on its own over it.
#define CAM_BYTES 256u   // vec4 rows 0-15: view, view-projection (3-6), projection, inverse view
#define CAM_BLOCK 0x940u   // the whole block, kept for the per-object motion pass's vertex shaders (vk_exec_draw.c)
#define CAM_SLOT 0xa00u    // camb slot stride
#define MVM_SLOTS 16u      // frames the GPU may still be reading the reprojection blocks of

static int mv_init(void) {
    if (D.mv_ok >= 0) return D.mv_ok;
    D.mv_ok = 0;
    const VkCtx *c = vk_ctx();
    VkDescriptorSetLayoutBinding b[8] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 8, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof dlaa_mv_spv,
                                   .pCode = dlaa_mv_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 2 * CAM_SLOT,
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &D.mv_dsl) != VK_SUCCESS) goto fail;
    VkBufferCreateInfo mbi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = MVM_SLOTS * 2 * CAM_BYTES,
                              .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VmaAllocationCreateInfo mai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                   .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                            VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo minfo;
    if (vmaCreateBuffer(c->vma, &mbi, &mai, &D.mvm, &D.mvm_mem, &minfo) != VK_SUCCESS) goto fail;
    D.mvm_map = minfo.pMappedData;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &D.mv_dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &D.mv_layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &D.mv_samp) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS ||
        vmaCreateBuffer(c->vma, &bi, &ai, &D.camb, &D.camb_mem, NULL) != VK_SUCCESS)
        goto fail;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = D.mv_layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &D.mv_pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) goto fail;
    return D.mv_ok = 1;
fail:
    rt_log("dlaa: the motion-vector pass could not be set up: DLAA runs with zero motion");
    return 0;
}

// The reprojection from this frame's clip space to last frame's, last * inverse(this), in double precision: the shader
// took each pixel back to world space through a float inverse of the view-projection, and with TotK's world
// coordinates in the thousands and its far plane the result was noise of a fraction of a pixel and more on distant
// surfaces, which DLSS followed into a shimmer like heat haze (roofs and trees, most of all in the upscaling modes).
// The shader is unchanged: its "this frame" block gets the identity and its "last frame" block this matrix, so it
// applies just the one matrix to each pixel. A still camera gives the identity, and zero motion exactly.
static int inv4(const double m[16], double o[16]) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 8; k++) a[r][k] = k < 4 ? m[r * 4 + k] : (k - 4 == r);
    for (int col = 0; col < 4; col++) {
        int piv = col;
        for (int r = col + 1; r < 4; r++)
            if (fabs(a[r][col]) > fabs(a[piv][col])) piv = r;
        if (fabs(a[piv][col]) < 1e-300) return 0;
        for (int k = 0; k < 8; k++) { double t = a[col][k]; a[col][k] = a[piv][k]; a[piv][k] = t; }
        double d = a[col][col];
        for (int k = 0; k < 8; k++) a[col][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != col) {
                double f = a[r][col];
                for (int k = 0; k < 8; k++) a[r][k] -= f * a[col][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) o[r * 4 + k] = a[r][k + 4];
    return 1;
}

// The pass, recorded through the recorder like the replay's own commands. 0: no camera this frame (the vectors stay
// zero and DLSS starts a new history). bias > 0 with this frame's G-buffer classes (D.ids) also writes e->mask, the
// bias mask (dlaa_mv.comp); *masked says whether it did.
static int mv_pass(VkExec *ex, struct Size *e, VkImageView zv, float jx, float jy, float bias, int *masked) {
    *masked = 0;
    VkCommandBuffer cb = ex->cb;
    uint32_t w = e->w, h = e->h;
    VkDescriptorBufferInfo cam;
    if (!mv_init() || !D.cam_addr || !vk_usnap_find(D.cam_addr, CAM_BLOCK, 4, &cam)) {
        D.mv_none++;
        D.cam_prev_ok = 0;
        e->chars_ok = 0;
        return 0;
    }
    // this frame's view-projection as the snapshot holds it, for the reprojection; kept as last frame's for the next
    const float *cr = (const float *)vk_usnap_bytes(D.cam_addr + 48, 64);
    double cur_vp[16], m[16], ci[16];
    int have_m = 0;
    if (cr) {
        for (int i = 0; i < 16; i++) cur_vp[i] = cr[i];
        if (D.cam_prev_ok && inv4(cur_vp, ci)) {
            for (int r = 0; r < 4; r++)
                for (int k = 0; k < 4; k++) {
                    double v = 0;
                    for (int j = 0; j < 4; j++) v += D.cam_prev[r * 4 + j] * ci[j * 4 + k];
                    m[r * 4 + k] = v;
                }
            have_m = 1;
        }
    }
    int s = D.cam_slot, had_prev = D.cam_prev_ok;
    VkBufferCopy bc = {cam.offset, (VkDeviceSize)s * CAM_SLOT, CAM_BLOCK};
    vkCmdCopyBuffer(cb, cam.buffer, D.camb, 1, &bc);
    full_barrier(cb);
    D.cam_slot ^= 1, D.cam_prev_ok = cr != NULL;
    if (cr) memcpy(D.cam_prev, cur_vp, sizeof cur_vp);
    if (!had_prev || !have_m) {   // nothing to reproject into: zero motion, new history
        e->chars_ok = 0;
        return 0;
    }
    VkDescriptorImageInfo zi = {D.mv_samp, zv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo oi = {VK_NULL_HANDLE, e->mv_view, VK_IMAGE_LAYOUT_GENERAL};
    uint32_t ms = D.mvm_slot++ % MVM_SLOTS;
    float *blk = (float *)(D.mvm_map + (size_t)ms * 2 * CAM_BYTES);   // rows 3-6 of each block are read
    memset(blk, 0, 2 * CAM_BYTES);
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            blk[12 + r * 4 + k] = r == k ? 1.0f : 0.0f, blk[64 + 12 + r * 4 + k] = (float)m[r * 4 + k];
    vmaFlushAllocation(vk_ctx()->vma, D.mvm_mem, (VkDeviceSize)ms * 2 * CAM_BYTES, 2 * CAM_BYTES);
    VkDescriptorBufferInfo cur = {D.mvm, (VkDeviceSize)ms * 2 * CAM_BYTES, CAM_BYTES};
    VkDescriptorBufferInfo prev = {D.mvm, (VkDeviceSize)ms * 2 * CAM_BYTES + CAM_BYTES, CAM_BYTES};
    // The class mask needs the G-buffer at the scene size; otherwise bindings 4-7 get the mask images as stand-ins
    // (the shader returns before reading them with bias 0).
    const VkExecTarget *ids = &D.ids;
    int use = bias > 0 && D.ids_ok && ids->image && ids->width == w && ids->height == h && !ids->is3d;
    VkImageView iv = use ? vk_exec_target_view(ids->img, ids->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
                                               ids->level, ids->base_layer, 1, 0)
                         : e->mask_view;
    int cur_c = e->chars_cur;
    VkDescriptorImageInfo ii = {D.mv_samp, iv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo mki = {VK_NULL_HANDLE, e->mask_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo cpi = {VK_NULL_HANDLE, e->chars_view[cur_c ^ 1], VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo coi = {VK_NULL_HANDLE, e->chars_view[cur_c], VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[8] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &zi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &cur},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &prev},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 4, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 5, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &mki},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 6, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &cpi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 7, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &coi}};
    times_begin(cb);
    times_mark(cb, T_MV0);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, D.mv_pipe);
    rec_push(vk_pipeline_push_fn(), cb, VK_PIPELINE_BIND_POINT_COMPUTE, D.mv_layout, 0, 8, wr);
    float pc[6] = {(float)w, (float)h, jx, jy, use ? bias : 0.0f, use && e->chars_ok ? 1.0f : 0.0f};
    vkCmdPushConstants(cb, D.mv_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    full_barrier(cb);
    times_mark(cb, T_MV1);
    vk_mvcheck_camera(cb, e->mv, w, h, jx, jy);   // TOTK_MV_CHECK (test)
    // the characters' own motion over the camera's (vk_exec_draw.c, per-object motion)
    uint64_t c0 = T.on > 0 ? kern_now_ns() : 0;
    if (vk_mvobj_execute(ex, D.cam_addr, e->mv_view, zv, D.depth.format, w, h)) full_barrier(cb);
    if (T.on > 0) T.cpu_mv += kern_now_ns() - c0;
    times_mark(cb, T_OBJ);
    if (T.on > 0) T.used[T.slot] = 1;   // the DLSS marks follow in evaluate
    vk_mvcheck_final(cb, ex->fence, e->mv, D.color.image, D.color.format, D.color.level, D.color.base_layer, w, h, jx, jy);
    D.mv_frames++;
    if (use) e->chars_cur ^= 1, e->chars_ok = 1, D.masked++;
    else e->chars_ok = 0;
    *masked = use;
    return 1;
}

// Last frame's camera block for the per-object motion pass, recorded before this frame's mv_pass: that pass writes
// this frame's into slot cam_slot and leaves the other slot, last frame's, as it is. 0: none (no history).
int vk_dlaa_prev_camera(VkDescriptorBufferInfo *out, float jitter[2]) {
    if (!dl_active() || !D.cam_prev_ok || !D.camb) return 0;
    *out = (VkDescriptorBufferInfo){D.camb, (VkDeviceSize)(D.cam_slot ^ 1) * CAM_SLOT, CAM_BLOCK};
    jitter[0] = D.jit_ok ? D.jx : 0.0f, jitter[1] = D.jit_ok ? D.jy : 0.0f;
    return 1;
}

// ---------------------------------------------------------------- reactive mask
// Particles, smoke, sparks and other transparents have no motion vectors of their own: the motion image holds the
// surface's behind them, and DLSS carried their old colour along that surface (trails behind sparks, smoke smeared
// with the camera). DLSS takes a mask of where to trust this frame's colour over the history (NGX
// "DLSS.Input.Bias.Current.Color.Mask"). It is made from the picture itself, at no cost on the CPU: the light target is
// copied when the transparents start (the pass that arms DLAA, vk_dlaa_pass) and compared with what DLSS gets
// (runtime/dlaa_mask_spv.h, tools/make_dlaa_mask.py): the share of a pixel's colour that changed, from MASK_THR on,
// times MASK_GAIN. Off by default (TOTK_DLSS_REACTIVE=1 turns it on, TOTK_DLAA_MASKDEBUG=1 shows the mask instead of
// the picture): the pass that arms also draws the sky and blended fog layers over the solid scene, and the mask
// covered the sky, sunlit trees and roofs and a wide fog region besides the particles (seen with MASKDEBUG at 19:45 in
// the first save), where DLSS would give up its anti-aliasing. It needs the particle draws alone (drawn again into the
// mask, like the motion draws) before it can be on. In the upscaling modes the light target DLSS reads also holds the
// bloom by then; the copy would have to be compared at the end of the scene instead.
#include "dlaa_mask_spv.h"
#define MASK_THR 0.1f
#define MASK_GAIN 2.5f
#define SNAPS 3

static struct {
    struct Snap {                // per light target size and format: its copy at arming, and the mask
        uint32_t w, h;
        VkFormat f;
        VkImage before, mask;
        VmaAllocation before_mem, mask_mem;
        VkImageView before_view, mask_view;
        uint64_t used;
    } s[SNAPS];
    struct Snap *cur;            // this frame's copy, NULL: none
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipe;
    int ok, knob;                // ok: -1 not built, 0 failed, 1 ready; knob: -1 not read
    uint64_t made, missed;       // frames with a mask; frames DLSS ran without the copy
} R = {.ok = -1, .knob = -1};

static int mask_init(void) {
    if (R.knob < 0) R.knob = rt_knob("DLSS_REACTIVE", "0");
    if (!R.knob) return 0;
    if (R.ok >= 0) return R.ok;
    R.ok = 0;
    const VkCtx *c = vk_ctx();
    VkDescriptorSetLayoutBinding b[3] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 3, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof dlaa_mask_spv,
                                   .pCode = dlaa_mask_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &R.dsl) != VK_SUCCESS) goto fail;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &R.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(c->device, &li, NULL, &R.layout) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS)
        goto fail;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = R.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &R.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) goto fail;
    return R.ok = 1;
fail:
    rt_log("dlaa: the reactive mask pass could not be set up: DLSS runs without it");
    return 0;
}

static void free_snap(struct Snap *s) {
    const VkCtx *c = vk_ctx();
    vkDeviceWaitIdle(c->device);   // a fourth size: the evicted images may still be in flight
    if (s->before_view) vkDestroyImageView(c->device, s->before_view, NULL);
    if (s->mask_view) vkDestroyImageView(c->device, s->mask_view, NULL);
    if (s->before) vmaDestroyImage(c->vma, s->before, s->before_mem);
    if (s->mask) vmaDestroyImage(c->vma, s->mask, s->mask_mem);
    memset(s, 0, sizeof *s);
}

static struct Snap *snap_for(VkCommandBuffer cb, uint32_t w, uint32_t h, VkFormat f) {
    static uint64_t tick;
    struct Snap *e = NULL, *lru = &R.s[0];
    for (int i = 0; i < SNAPS; i++) {
        if (R.s[i].w == w && R.s[i].h == h && R.s[i].f == f) return R.s[i].used = ++tick, &R.s[i];
        if (!R.s[i].before && !e) e = &R.s[i];
        if (R.s[i].used < lru->used) lru = &R.s[i];
    }
    if (!e) free_snap(e = lru);
    VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!image2d(w, h, f, u, &e->before, &e->before_mem, &e->before_view) ||
        !image2d(w, h, VK_FORMAT_R8_UNORM, u | VK_IMAGE_USAGE_STORAGE_BIT, &e->mask, &e->mask_mem, &e->mask_view)) {
        free_snap(e);
        return NULL;
    }
    VkImageMemoryBarrier2 ib[2];
    VkImage im[2] = {e->before, e->mask};
    for (int i = 0; i < 2; i++)
        ib[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im[i],
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2,
                           .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(cb, &di);
    e->w = w, e->h = h, e->f = f, e->used = ++tick;
    return e;
}

// At arming: the light target as it is before the transparents, outside any pass.
static void mask_snapshot(VkExec *ex) {
    R.cur = NULL;
    if (mask_init() <= 0) return;
    const VkExecTarget *t = &D.color;
    struct Snap *s = snap_for(ex->cb, t->width, t->height, t->format);
    if (!s) return;
    vk_exec_barrier(ex);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, t->level, t->base_layer, 1},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {t->width, t->height, 1}};
    vkCmdCopyImage(ex->cb, t->image, VK_IMAGE_LAYOUT_GENERAL, s->before, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    full_barrier(ex->cb);
    ex->recorded += 2;
    R.cur = s;
}

// Before DLSS: the mask from the copy and the light target (view cv). NULL: none this frame.
static struct Snap *mask_pass(VkCommandBuffer cb, VkImageView cv, uint32_t w, uint32_t h) {
    struct Snap *s = R.cur;
    R.cur = NULL;
    if (mask_init() <= 0) return NULL;
    if (!s || s->w != w || s->h != h || s->f != D.color.format) return R.missed++, NULL;
    VkDescriptorImageInfo bi = {D.mv_samp, s->before_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo ai = {D.mv_samp, cv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo mi = {VK_NULL_HANDLE, s->mask_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[3] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &bi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ai},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &mi}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.pipe);
    rec_push(vk_pipeline_push_fn(), cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 3, wr);
    float pc[4] = {(float)w, (float)h, MASK_THR, MASK_GAIN};
    vkCmdPushConstants(cb, R.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    full_barrier(cb);
    R.made++;
    return s;
}

// ---------------------------------------------------------------- jitter
// The scene's draws (every pass drawing with the scene depth, until the DLAA point) have their viewport moved by a
// sub-pixel Halton(2,3) offset, which shifts what they rasterise as a jittered projection would; full-screen passes
// that shade by window position are unaffected. Jitter only follows a frame DLSS ran on: a jittered frame that DLSS
// does not resolve shakes on screen, so a missed DLAA point stops the jitter for a second.
static float halton(uint32_t i, uint32_t b) {
    float f = 1, r = 0;
    for (; i; i /= b) f /= (float)b, r += f * (float)(i % b);
    return r;
}

void vk_dlaa_viewport(VkExec *ex, VkViewport *v) {
    if (!D.jit_ok || D.ended || !ex->depth.image || ex->depth.image != D.jdepth) return;
    v->x += D.jx, v->y += D.jy;
    D.ux = D.jx, D.uy = D.jy, D.jit_used = 1;
}

void vk_dlaa_draw(VkExec *ex) {
    if (!dl_active() || D.ended) return;
    static int survey = -1;   // TOTK_DLAA_SURVEY=1 (test): what is drawn into the armed light target
    if (survey < 0) survey = rt_knob("DLAA_SURVEY", "0");
    static uint64_t dfn[16];   // scene draws writing depth, by depth test (reversed depth: GREATER/GEQUAL)
    if (survey && ex->depth.image && ex->depth.image == D.jdepth && ex->pipe.ds.dt && ex->pipe.ds.dw)
        dfn[ex->pipe.ds.df & 15]++;
    if (survey && D.armed && ex->color[0].image == D.color.image) {
        static uint64_t n[2][2][2], arms, frames, last_frame;   // [blend][depth write][depth test]
        const VkPipeState *s = &ex->pipe;
        n[s->color.blend_en & 1][s->ds.dw][s->ds.dt]++;
        uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
        if (f != last_frame) last_frame = f, frames++;
        static uint64_t ps;
        if (ex->pass_serial != ps) ps = ex->pass_serial, arms++;
        if (frames % 300 == 0) {
            static uint64_t logged;
            if (logged != frames) {
                logged = frames;
                rt_log("dlaa survey: scene draws writing depth by depth test (NVN func 1-8: never less equal lequal "
                       "greater notequal gequal always): %llu %llu %llu %llu %llu %llu %llu %llu",
                       (unsigned long long)dfn[1], (unsigned long long)dfn[2], (unsigned long long)dfn[3],
                       (unsigned long long)dfn[4], (unsigned long long)dfn[5], (unsigned long long)dfn[6],
                       (unsigned long long)dfn[7], (unsigned long long)dfn[8]);
                rt_log("dlaa survey: %llu frames, %.1f passes a frame into the light target after arming; draws a frame "
                       "(blend/depth write/depth test): 000 %.1f 001 %.1f 010 %.1f 011 %.1f 100 %.1f 101 %.1f 110 %.1f 111 %.1f",
                       (unsigned long long)frames, (double)arms / frames, (double)n[0][0][0] / frames, (double)n[0][0][1] / frames,
                       (double)n[0][1][0] / frames, (double)n[0][1][1] / frames, (double)n[1][0][0] / frames,
                       (double)n[1][0][1] / frames, (double)n[1][1][0] / frames, (double)n[1][1][1] / frames);
            }
        }
    }
    const p_bind_buf *b = &ex->ubo[0][1];   // constant bank c4: the replay keeps user banks from c3 on
    const VkExecTarget *z = &ex->depth;
    if (b->size == 0x940 && b->addr && z->image && z->width >= DLAA_SCENE_MIN_W && z->format == VK_FORMAT_D32_SFLOAT &&
        (!D.jdepth || z->image == D.jdepth))
        D.cam_addr = b->addr;
}

// The jitter moves on right after DLAA ran, not at the present: the replay can record the next frame's scene before
// the present of this one is handled, and a step taken there landed in the middle of a frame (worse after an off/on
// switch, which shifted that order). Jitter needs a frame DLSS ran on; a frame without the DLAA point stops it.
// The sequence is as long as NVIDIA's DLSS guide asks for the upscaling ratio, 8 x (output / scene)^2 phases (18 in
// Quality, 24 in Balanced, 32 in Performance, 72 in Ultra Performance), and 16 at least, the length DLAA has used from the start.
static void next_jitter(uint32_t w, uint32_t ow) {
    static int nj = -1;
    if (nj < 0) nj = !rt_knob("DLAA_JITTER", "1");   // TOTK_DLAA_JITTER=0: no jitter (test)
    double r = w ? (double)ow / w : 1.0;
    uint32_t phases = (uint32_t)ceil(8.0 * r * r - 1e-6);
    if (phases < 16) phases = 16;
    if (phases > 72) phases = 72;
    uint32_t i = D.jframe++ % phases + 1;
    D.jx = halton(i, 2) - 0.5f, D.jy = halton(i, 3) - 0.5f;
    D.jit_ok = !D.hold && !nj;
}

// The DLSS model (TOTK_DLSS_PRESET, live): default takes NVIDIA's model per mode (evaluate: K for DLAA, Quality and
// Balanced, M for Performance, L for Ultra Performance), 0 here. A letter a..m takes that model in every mode: k pins
// K, j is the other transformer model, which NVIDIA describes as slightly less ghosting at the cost of extra flicker, e
// the older CNN model, cheaper on slower RTX cards. The value is an NVSDK_NGX_DLSS_Hint_Render_Preset (1 A ... 13 M),
// read when a feature is created; a change recreates the features (preset_check).
static int preset_value(void) {
    const char *v = cfg_get("TOTK_DLSS_PRESET");
    int c = v && v[0] && !v[1] ? v[0] | 0x20 : 0;
    return c >= 'a' && c <= 'm' ? c - 'a' + 1 : 0;
}

// How strongly DLSS favours the current frame on characters and where they just were (TOTK_DLSS_CHAR_MASK, percent,
// live; 0 off). Where a character's vectors are the camera's only (dlaa_objmv = 0, or a draw the per-object pass
// skips), one that moves on its own drags a faint copy of itself through DLSS's history; the bias mask makes DLSS take
// less of the history there, at the cost of some of its anti-aliasing on those pixels.
static float char_bias(void) {
    const char *v = cfg_get("TOTK_DLSS_CHAR_MASK");
    long n = v && *v ? strtol(v, NULL, 10) : 65;
    return n <= 0 ? 0.0f : n >= 100 ? 1.0f : (float)n / 100.0f;
}

// The quality setting DLSS is created with, from the scene's share of the output width.
static int perf_quality(uint32_t w, uint32_t ow) {
    if (w >= ow) return NGX_PERFQ_DLAA;
    double k = (double)w / ow;
    return k >= 0.66 ? 2 : k >= 0.57 ? 1 : k >= 0.49 ? 0 : 3;
}

// The camera FSR asks for (its depth handling and its disocclusion use them; DLSS does not): the near and far distances,
// the vertical field of view, and whether depth 1 is near and the far plane infinite. From the projection in rows 7-10
// of this frame's scene camera block (CAM_BYTES), rows of a row-major 4x4 with OpenGL clip depth as dlaa_mv.comp reads
// the view-projection: clip z = A z + B, clip w = s z with s = +-1 (row 10 is (0, 0, s, 0)), so the depth range's ends
// (ndc z = -1 and +1, depth 0 and 1) sit at z = B / (s ndc - A), and the field of view comes from row 8's
// 1 / tan(fov / 2). A frame whose block does not have that shape (or has no camera block) reuses the last camera that
// did: FSR's contexts are created with the depth flags, and the first run (1.4.3 at the title screen, RX 9070 XT) read
// 0.1 m to 1001 m, 45 degrees, depth not inverted, while later contexts of the dynamic resolution's other sizes got the
// defaults and inverted, infinite depth. Before any camera was read the defaults are 0.1 m to 1000 m, 50 degrees, depth
// not inverted. The first read and the first miss are logged, each once.
static void cam_params(float *cn, float *cf, float *fov, int *inv, int *inf) {
    static struct { int ok; float cn, cf, fov; int inv, inf; } last = {0, 0.1f, 1000.0f, 0.8727f, 0, 0};
    *cn = last.cn, *cf = last.cf, *fov = last.fov, *inv = last.inv, *inf = last.inf;
    static int logged_ok, logged_miss;
    const float *p = D.cam_addr ? (const float *)vk_usnap_bytes(D.cam_addr + 7 * 16, 64) : NULL;
    const char *why = "no camera block";
    if (p) {
        const float *r8 = p + 4, *r9 = p + 8, *r10 = p + 12;
        double s = r10[2], A = r9[2], B = r9[3], f = fabs((double)r8[1]);
        double z0 = 0, z1 = 0;
        why = "not a perspective projection";
        if (fabs(fabs(s) - 1.0) < 1e-3 && fabs(r10[0]) < 1e-4 && fabs(r10[1]) < 1e-4 && fabs(r10[3]) < 1e-4 &&
            f > 0.05 && f < 20.0 && fabs(B) > 1e-9) {
            double d0 = -s - A, d1 = s - A;   // denominators at depth 0 (ndc -1) and depth 1 (ndc +1)
            z0 = fabs(d0) > 1e-9 ? fabs(B / d0) : INFINITY;
            z1 = fabs(d1) > 1e-9 ? fabs(B / d1) : INFINITY;
            double nz = fmin(z0, z1), fz = fmax(z0, z1);
            if (nz > 1e-4 && nz < 100.0 && fz > nz) {
                *inv = z1 < z0;
                *inf = !isfinite(fz) || fz > 1e7;
                *cn = (float)nz, *cf = *inf ? 1e30f : (float)fz, *fov = (float)(2.0 * atan(1.0 / f));
                last.ok = 1, last.cn = *cn, last.cf = *cf, last.fov = *fov, last.inv = *inv, last.inf = *inf;
                why = NULL;
            } else {
                why = "depth range out of bounds";
            }
        }
    }
    if (!why && !logged_ok) {
        logged_ok = 1;
        char fartxt[32];   // not "far": windows.h defines that as a macro
        if (*inf) snprintf(fartxt, sizeof fartxt, "infinite");
        else snprintf(fartxt, sizeof fartxt, "%.0f m", *cf);
        rt_log("fsr: camera from c4: near %.3f m, far %s, vertical fov %.1f degrees, depth %s", *cn, fartxt,
               *fov * 57.29578, *inv ? "inverted" : "not inverted");
    } else if (why && !logged_miss) {
        logged_miss = 1;
        rt_log("fsr: camera block not read (%s): %s", why, last.ok ? "the last camera read is kept" : "defaults");
    }
}

// dst: the target DLSS's picture goes to, NULL for the light target itself (DLAA in place).
static void evaluate(VkExec *ex, const VkExecTarget *dst) {
    const VkExecTarget *t = &D.color, *z = &D.depth;
    if (!dst) dst = t;
    uint32_t w = t->width, h = t->height, ow = dst->width, oh = dst->height;
    VkCommandBuffer cb = ex->cb;
    vk_exec_barrier(ex);   // what drew the scene is done (no pass is open here)
    if (D.preset_dirty) {   // another model: features are created with it, so every size gets a new one
        for (int i = 0; i < 3; i++)
            if (D.sizes[i].out) free_size(&D.sizes[i]);
        D.last = NULL, D.preset_dirty = 0;
    }
    struct Size *e = size_for(cb, w, h, ow, oh);
    if (!e) return dl_fail("work image creation", 0);
    if (e != D.last) D.reset = 1, D.why[0]++, e->chars_ok = 0;   // another size's history is from another moment
    int fsr = fsr_backend();
    // FSR is handed whole images, not views: the light target and the depth must be their image's first level and layer
    if (fsr && (t->level || t->base_layer || z->level || z->base_layer))
        return dl_fail("FSR input (the scene targets are not level 0, layer 0 of their images)", 0);
    if (!fsr && !D.params && !NGX_OK(N.alloc_params(&D.params))) return dl_fail("parameter allocation", 0);
    NgxParam *p = D.params;
    VkImageView zv = vk_exec_target_view(z->img, z->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, z->level,
                                         z->base_layer, 1, 0);
    float jx = D.jit_used ? D.ux : 0.0f, jy = D.jit_used ? D.uy : 0.0f;
    static int no_mv = -1;
    if (no_mv < 0) no_mv = !rt_knob("DLAA_MV", "1");   // TOTK_DLAA_MV=0: zero motion (test)
    int masked = 0;
    if (no_mv || !mv_pass(ex, e, zv, jx, jy, char_bias(), &masked)) {
        VkClearColorValue zero = {{0, 0, 0, 0}};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, e->mv, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
        full_barrier(cb);
        if (!no_mv) D.reset = 1, D.why[1]++;   // no camera: the history cannot be moved along
    }
    VkImageView cv = vk_exec_target_view(t->img, t->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, t->level,
                                         t->base_layer, 1, 0);
    struct Snap *mk = D.mv_samp ? mask_pass(cb, cv, w, h) : NULL;
    int timed = T.on > 0 && T.used[T.slot];   // mv_pass began this frame's slot
    if (timed) times_mark(cb, T_DL0);
    uint64_t c0 = timed ? kern_now_ns() : 0;
    rec_drain();   // NGX and FSR record into the command buffer themselves: the recorder thread must have made every call so far
    if (timed) T.cpu_drain += kern_now_ns() - c0;
    // The time since the previous evaluation, which DLSS and FSR weigh their history by; 60 fps before the first and
    // after a gap (a pause, a load), where no history carries over anyway.
    uint64_t now = plat_now_ns();
    double dt = D.last_eval_ns ? (now - D.last_eval_ns) / 1e6 : 0;
    D.last_eval_ns = now;
    float dt_ms = dt > 0 && dt < 100 ? (float)dt : 16.67f;
    if (fsr) {
        float cn, cf, fov;
        int inv, inf;
        cam_params(&cn, &cf, &fov, &inv, &inf);
        if (!e->ffx) {
            e->ffx = vk_ffx_create(w, h, ow, oh, inv, inf);
            if (!e->ffx) return dl_fail("FSR context creation", 0);
            D.reset = 1;
        }
        if (D.last_color != t->image) D.reset = 1, D.why[2]++, D.last_color = t->image;
        // The same bias mask DLSS would get, as FSR's reactive mask: how much to favour this frame's colour
        VkFfxDispatch fd = {.color = {t->image, t->format, w, h}, .depth = {z->image, z->format, w, h},
                            .mv = {e->mv, VK_FORMAT_R16G16_SFLOAT, w, h},
                            .out = {e->out, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh},
                            .jx = jx, .jy = jy, .dt_ms = dt_ms, .cam_near = cn, .cam_far = cf, .fov_y = fov,
                            .reset = D.reset};
        if (mk) fd.reactive = (VkFfxImage){mk->mask, VK_FORMAT_R8_UNORM, w, h};
        else if (masked) fd.reactive = (VkFfxImage){e->mask, VK_FORMAT_R8_UNORM, w, h};
        c0 = timed ? kern_now_ns() : 0;
        int ok = vk_ffx_dispatch(e->ffx, cb, &fd);
        if (timed) T.cpu_ngx += kern_now_ns() - c0;
        if (!ok) return dl_fail("FSR dispatch", 0);
        goto upscaled;   // the copy of the result and what follows are the same as DLSS's
    }
    if (!e->feat) {
        p_setu(p, "CreationNodeMask", 1);
        p_setu(p, "VisibilityNodeMask", 1);
        p_setu(p, "Width", w);
        p_setu(p, "Height", h);
        p_setu(p, "OutWidth", ow);
        p_setu(p, "OutHeight", oh);
        p_seti(p, "PerfQualityValue", perf_quality(w, ow));
        // IsHDR | MVLowRes | AutoExposure: linear scene light, motion vectors at the render size, no exposure texture
        p_seti(p, "DLSS.Feature.Create.Flags", (1 << 0) | (1 << 1) | (1 << 6));
        p_seti(p, "DLSS.Enable.Output.Subrects", 0);
        // The model per mode (NVSDK_NGX_DLSS_Hint_Render_Preset: 1 A ... 11 K, 12 L, 13 M), as NVIDIA recommends for
        // DLSS 4.5 (310.x): K (transformer) for DLAA, Quality and Balanced, M for Performance, L for Ultra Performance.
        // M and L cost more on cards without FP8 (RTX 20/30). dlss_preset (D.preset, preset_check) takes one model for
        // every mode instead.
        int pk = D.preset ? D.preset : 11, pm = D.preset ? D.preset : 13, pl = D.preset ? D.preset : 12;
        p_setu(p, "DLSS.Hint.Render.Preset.DLAA", (unsigned)pk);
        p_setu(p, "DLSS.Hint.Render.Preset.UltraQuality", (unsigned)pk);
        p_setu(p, "DLSS.Hint.Render.Preset.Quality", (unsigned)pk);
        p_setu(p, "DLSS.Hint.Render.Preset.Balanced", (unsigned)pk);
        p_setu(p, "DLSS.Hint.Render.Preset.Performance", (unsigned)pm);
        p_setu(p, "DLSS.Hint.Render.Preset.UltraPerformance", (unsigned)pl);
        NgxResult r = N.create(cb, NGX_FEATURE_SUPERSAMPLING, p, &e->feat);
        if (!NGX_OK(r) || !e->feat) return dl_fail("DLSS feature creation", r);
        D.reset = 1;
        int q = perf_quality(w, ow), pr = q == 0 ? pm : q == 3 ? pl : pk;
        rt_log("dlaa: DLSS feature created at %ux%u -> %ux%u, quality value %d, preset %c (scene format %d, depth format "
               "%d)", w, h, ow, oh, q, 'A' + pr - 1, (int)t->format, (int)z->format);
    }
    if (D.last_color != t->image) D.reset = 1, D.why[2]++, D.last_color = t->image;
    NgxResourceVK rc = res_image(cv, t->image, t->format, w, h, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    NgxResourceVK ro = res_image(e->out_view, e->out, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    NgxResourceVK rz = res_image(zv, z->image, z->format, w, h, VK_IMAGE_ASPECT_DEPTH_BIT, 0);
    NgxResourceVK rm = res_image(e->mv_view, e->mv, VK_FORMAT_R16G16_SFLOAT, w, h, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    p_setp(p, "Color", &rc);
    p_setp(p, "Output", &ro);
    p_setp(p, "Depth", &rz);
    p_setp(p, "MotionVectors", &rm);
    p_setf(p, "Jitter.Offset.X", jx);
    p_setf(p, "Jitter.Offset.Y", jy);
    p_setf(p, "Sharpness", 0.0f);
    p_seti(p, "Reset", D.reset);
    // One bias mask: the reactive one (TOTK_DLSS_REACTIVE, a test) where it ran, else the characters' (mv_pass), else
    // none. The parameter object is reused, so it is cleared explicitly.
    NgxResourceVK rk = mk ? res_image(mk->mask_view, mk->mask, VK_FORMAT_R8_UNORM, w, h, VK_IMAGE_ASPECT_COLOR_BIT, 0)
                          : res_image(e->mask_view, e->mask, VK_FORMAT_R8_UNORM, w, h, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    p_setp(p, "DLSS.Input.Bias.Current.Color.Mask", mk || masked ? (void *)&rk : NULL);
    p_setu(p, "DLSS.Input.Bias.Current.Color.Subrect.Base.X", 0);
    p_setu(p, "DLSS.Input.Bias.Current.Color.Subrect.Base.Y", 0);
    p_setf(p, "MV.Scale.X", 1.0f);
    p_setf(p, "MV.Scale.Y", 1.0f);
    p_setu(p, "DLSS.Render.Subrect.Dimensions.Width", w);
    p_setu(p, "DLSS.Render.Subrect.Dimensions.Height", h);
    p_setf(p, "DLSS.Pre.Exposure", 1.0f);
    p_setf(p, "DLSS.Exposure.Scale", 1.0f);
    p_setf(p, "FrameTimeDeltaInMsec", dt_ms);
    c0 = timed ? kern_now_ns() : 0;
    NgxResult r = N.evaluate(cb, e->feat, p, NULL);
    if (timed) T.cpu_ngx += kern_now_ns() - c0;
    if (!NGX_OK(r)) return dl_fail("DLSS evaluation", r);
upscaled:
    if (timed) times_mark(cb, T_DL1);   // through the recorder, after NGX's or FSR's own commands
    D.reset = 0;
    D.last = e, e->used = D.evals + 1;
    full_barrier(cb);
    vk_mvcheck_output(cb, e->out, ow, oh);   // TOTK_MV_CHECK (test)
    // the result into the scene target (DLAA: the bloom, the post passes and the HUD read it and draw over it) or into
    // the scaling pass's target (the HUD draws over it)
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .srcOffsets = {{0, 0, 0}, {(int32_t)ow, (int32_t)oh, 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dst->level, dst->base_layer, 1},
                     .dstOffsets = {{0, 0, 0}, {(int32_t)ow, (int32_t)oh, 1}}};
    // TOTK_DLAA_MVDEBUG=1 (test): the motion vectors in place of the picture, x in red and y in green, render pixels
    // as scene light (the post passes still run over them)
    static int mvdebug = -1;
    if (mvdebug < 0) mvdebug = rt_knob("DLAA_MVDEBUG", "0");
    static int maskdebug = -1;
    if (maskdebug < 0) maskdebug = rt_knob("DLAA_MASKDEBUG", "0");
    if (mvdebug || (maskdebug && mk)) b.srcOffsets[1] = (VkOffset3D){(int32_t)w, (int32_t)h, 1};
    vkCmdBlitImage(cb, mvdebug ? e->mv : maskdebug && mk ? mk->mask : e->out, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                   VK_FILTER_NEAREST);
    full_barrier(cb);
    if (dst != t) vk_dlaa_swap_img = t->img, vk_dlaa_swap_view = e->out_view;   // the scaling draw reads DLSS's picture
    ex->recorded += 4;
    vk_exec_draw_new_cb();   // NGX bound pipelines and descriptor sets of its own: the next draw binds everything again
    D.jdepth = z->image;
    next_jitter(w, ow);
    if (++D.evals == 1 || D.evals == 300 || D.evals % 3600 == 0)   // 300: one early line with the frames counted
        rt_log("dlaa: %llu frames anti-aliased (scene %ux%u, output %ux%u; %llu frames without the DLAA point, %llu with camera "
               "motion, %llu with the characters' mask, %llu without a camera, %llu jitter holds; resets: %llu size, %llu "
               "camera, %llu target; reactive mask %llu, without its copy %llu)",
               (unsigned long long)D.evals, w, h, ow, oh, (unsigned long long)D.frames_missed,
               (unsigned long long)D.mv_frames, (unsigned long long)D.masked, (unsigned long long)D.mv_none,
               (unsigned long long)D.holds, (unsigned long long)D.why[0], (unsigned long long)D.why[1],
               (unsigned long long)D.why[2], (unsigned long long)R.made, (unsigned long long)R.missed);
}

static int scene_color(VkFormat f) {
    return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT;
}

// A scene DLAA may run on: DLAA_SCENE_MIN_W wide at least, and at least a quarter of the output's width, below which
// no mode scales (Ultra Performance renders a third, the game's dynamic resolution a little less). At a 3440x1440
// output a 387x288 G-buffer and light target (seen while loading, 1.4.3) armed the frame and FSR scaled it ninefold
// into the output.
static int scene_wide(uint32_t w) {
    unsigned o[2], s[2];
    res_sizes(o, s);
    return w >= DLAA_SCENE_MIN_W && (uint64_t)w * 4 >= o[0];
}

void vk_dlaa_pass(VkExec *ex) {
    const VkExecTarget *c = &ex->color[0], *z = &ex->depth;
    if (!dl_active() || D.done) return;
    // The G-buffer: several colour targets with a D32F depth of at least DLAA_SCENE_MIN_W wide. Only the light target drawn after it
    // arms the frame; at night the game draws into the light target alone with the scene depth before the G-buffer too,
    // and DLAA then ran at a small pass ahead of the whole scene, which reached the screen jittered.
    if (ex->pass_ncolor >= 3 && z->image && z->format == VK_FORMAT_D32_SFLOAT && scene_wide(z->width)) {
        D.gbuf = 1, D.armed = 0;
        // target 0 is the object-class buffer (RGBA8, the scene size): the characters' bias mask reads its green. A
        // smaller pass of the same shape (640x360, seen while loading) does not replace the scene's in the same frame.
        if (c->image && c->format == VK_FORMAT_R8G8B8A8_UNORM && c->width == z->width && c->height == z->height &&
            c->samples <= 1 && !c->is3d && (!D.ids_ok || c->width >= D.ids.width))
            D.ids = *c, D.ids_ok = 1;
        return;
    }
    // the scene depth is D32F; a D16 target of the scene size also serves one composite pass before the transparents.
    // A smaller pair never takes over an armed one: the camera rune draws a half-size RGBA16F target with a half-size
    // D32F depth after the lighting (852x480 under a 1704x960 scene), and DLSS ran on that and wrote its picture into
    // the scene's own light target, which left every object black.
    if (D.gbuf && ex->pass_ncolor == 1 && c->image && scene_color(c->format) && z->image && z->format == VK_FORMAT_D32_SFLOAT &&
        c->width == z->width && c->height == z->height && scene_wide(c->width) &&
        (!D.armed || (c->width >= D.color.width && c->height >= D.color.height)) &&
        c->samples <= 1 && !c->is3d) {
        int first = !D.armed;
        D.color = *c, D.depth = *z, D.armed = 1;
        if (first) mask_snapshot(ex);   // the lit scene, before the transparents (reactive mask)
        return;
    }
    if (!D.armed) return;
    if (!D.ended) {
        if (!ex->pass_w || ex->pass_w * 4 > D.color.width) return;
        D.ended = 1;
        unsigned o[2], s[2];
        res_sizes(o, s);
        if (D.color.width >= o[0]) D.done = 1, evaluate(ex, NULL);   // DLAA in place; else wait for the scaling pass
        return;
    }
    // the game's scaling pass: one colour target of the output size, no depth
    unsigned o[2], s[2];
    res_sizes(o, s);
    if (ex->pass_ncolor == 1 && c->image && !z->image && scene_color(c->format) && c->width > D.color.width &&
        c->width == o[0] && c->height == o[1] && c->samples <= 1 && !c->is3d) {
        D.done = D.up = 1;
        evaluate(ex, c);
    }
}

// TOTK_DLSS, read at each present (the menu switches it live): the scene size of the mode (resolution.c) and the
// texture bias that goes with it while DLSS runs.
static void dlss_mode(void) {
    static char mode[32];
    static int bias_on = -1;
    const char *m = cfg_value("TOTK_DLSS");
    if (strcmp(m, mode)) {
        snprintf(mode, sizeof mode, "%s", m);
        res_dlss_mode(m);
        bias_on = -1;
    }
    int b = dl_active();
    if (b != bias_on) bias_on = b, vk_sampler_dlss_bias(b ? res_dlss_lod_bias() : 0.0f);
}


// The preset, read again when a setting changed (the menu); a different one recreates the features at the next DLAA run.
static void preset_check(void) {
    if (fsr_backend()) return;   // DLSS models only: FSR's version is fixed at start (vk_ffx_init)
    unsigned v = cfg_version();
    if (v == D.cfg_seen) return;
    D.cfg_seen = v;
    int p = preset_value();
    if (p == D.preset) return;
    if (D.evals) {
        if (p) rt_log("dlaa: DLSS preset %c in every mode; the features are created again", 'A' + p - 1);
        else rt_log("dlaa: DLSS presets per mode; the features are created again");
    }
    D.preset = p, D.preset_dirty = 1, D.reset = 1;
}

void vk_dlaa_frame(void) {
    vk_dlaa_swap_img = NULL, vk_dlaa_swap_view = VK_NULL_HANDLE;
    dlss_mode();
    preset_check();
    int on = be_available() && cfg_bool("TOTK_DLAA");
    if (on != D.on) {
        rt_log("dlaa: %s%s%s", on ? "on" : "off", on && fsr_backend() ? ", with " : "", on && fsr_backend() ? vk_ffx_version() : "");
        D.on = on;
        // either way the next DLAA frame starts a new history from an unjittered frame
        D.reset = 1, D.cam_prev_ok = 0, D.jit_ok = D.jit_used = D.armed = D.done = D.gbuf = D.ended = D.up = 0;
        D.hold = 0;
        return;
    }
    if (!dl_active()) return;
    if (!D.done) D.frames_missed++;
    if (D.jit_used && !D.done) D.hold = 60, D.holds++;   // jittered but not resolved: that frame shook
    else if (D.hold) D.hold--;
    if (!D.done) D.jit_ok = 0;
    D.armed = D.done = D.jit_used = D.gbuf = D.ended = D.up = D.ids_ok = 0;
    D.cam_addr = 0;
}

// ---------------------------------------------------------------- the game's FXAA (TOTK_FXAA, live)
// TotK's FXAA is two full-screen passes after the bloom, just before the HUD: one copies the scene target into a
// scratch target, the other draws the anti-aliased result back over the scene (found by diffing frame dumps with the
// game's own switch, ModelSceneExtensionParam EnableFXAA, on and off). With fxaa = 0 the draws of that second pass's
// fragment shader are skipped, so the scene reaches the HUD as it was. Shaders are known by the FNV-1a hash of their
// SPIR-V, as vk_dump.c names them; a translator change gives new hashes and the switch then does nothing.
// With a scene smaller than the output (the game's own 5/6 render scale, or a DLSS mode) the same shader draws the
// scaled picture into the output-sized target, in a pass without the depth attachment the in-place FXAA has: there it
// is always drawn, whatever fxaa says (skipping it would leave the 3D picture out), unless DLSS already wrote that
// target this frame.
// The stock translator's hash, the one with tools/xlate patch 0010 (PCSS, PR #89) applied, the one with patches 0010
// and 0011 (the shadow filter spread) applied, and that one with the zero_locals job flag (Mesa, vk_caps.c).
// A scaling draw this list misses still scales DLSS's picture, not
// the jittered scene: after DLSS ran, passes larger than the light target sample DLSS's output (vk_dlaa_swap_img).
static const uint64_t FXAA_FS[] = {0xf450003ebeb3dc52ull, 0x0a12f4bc2a9a4fbeull, 0x5c43ae18948d52cbull,
                                   0xaad42238801d31d8ull};

static uint64_t spirv_hash(const GfxShader *s) {
    static struct { const GfxShader *s; uint64_t h; } tab[8192];
    uintptr_t k = ((uintptr_t)s >> 4) * 0x9E3779B97F4A7C15ull;
    for (uint32_t i = 0, j = (uint32_t)(k >> 51); i < 8192; i++, j = (j + 1) & 8191) {
        if (tab[j].s == s) return tab[j].h;
        if (!tab[j].s) {
            uint64_t words = 0, h = 0xcbf29ce484222325ull;
            const uint8_t *b = (const uint8_t *)gfx_shader_spirv(s, &words);
            for (uint64_t n = 0; b && n < words * 4; n++) h = (h ^ b[n]) * 0x100000001b3ull;
            tab[j].s = s, tab[j].h = h;
            return h;
        }
    }
    return 0;   // table full: no skip
}

int vk_dlaa_skip_draw(VkExec *ex, const GfxShader *fs) {
    static uint64_t frame_read = ~0ull;
    static int fxaa = 1;
    // fxaa = 0 reports once whether it found the game's FXAA draw: the first skip, or a minute of frames (3600) with
    // none, which is what a translator change that moves FXAA_FS looks like on every GPU, with or without DLSS
    static uint64_t fx_off_frames;
    static int fx_found, fx_reported;
    uint64_t f = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (f != frame_read) {   // once a frame: the menu switches it live
        frame_read = f, fxaa = cfg_bool("TOTK_FXAA");
        if (!fxaa && !fx_found && !fx_reported && ++fx_off_frames == 3600) {
            fx_reported = 1;
            rt_log("dlaa: fxaa = 0 found no draw of the game's FXAA in %llu frames (its fragment shader is not in "
                   "FXAA_FS): the FXAA stays on", (unsigned long long)fx_off_frames);
        }
    }
    if (fxaa && !D.up) return 0;   // nothing to skip: no shader lookup per draw at the defaults
    if (!fs) return 0;
    uint64_t h = spirv_hash(fs);
    int known = 0;
    for (size_t i = 0; i < sizeof FXAA_FS / sizeof FXAA_FS[0]; i++) known |= h == FXAA_FS[i];
    if (!known) return 0;
    if (!ex->depth.image) return D.up;   // the scaling pass, which DLSS replaced
    if (!fxaa && !fx_found) {
        fx_found = 1;
        rt_log("dlaa: fxaa = 0 skips the game's FXAA (fragment shader %016llx)", (unsigned long long)h);
    }
    return !fxaa;
}

