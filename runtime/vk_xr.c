// VR through OpenXR (SteamVR), with --vr / TOTK_VR=1. The OpenXR loader is loaded at run time (libopenxr_loader.so.1 on
// Linux, openxr_loader.dll next to totk.exe on Windows); headers in runtime/openxr/. XR_KHR_vulkan_enable.
// - vk_xr_instance_exts (create_instance): starts OpenXR (instance + the head-mounted system) and adds the Vulkan
//   instance extensions the runtime needs; vk_xr_physical (pick_device): the GPU the runtime renders on;
//   vk_xr_device_exts (create_device): its device extensions.
// - vk_xr_start (after vk_init): a thread that creates the session on the game's graphics queue and runs the frame loop:
//   each headset frame it locates both eyes (xrLocateViews, LOCAL space) and publishes them (publish_views), copies the
//   newest complete pair of eye pictures into one swapchain per eye and submits a projection layer, plus quad layers for
//   the HUD panel, the aim reticle and the world markers.
// - The game draws one frame per eye (stereo pairs, vr_camera.c): the camera hook gives the renderer the head pose of the
//   frame's eye (vk_xr_eye_view); the frame's finished picture goes to that eye (vk_xr_pair_scene), and a right eye after
//   a left one completes a pair. Each picture keeps the pose it was drawn with, so SteamVR reprojects each eye from there.
// - The game's projection covers the headset's view at the output's shape: patch page +60 (field-of-view factor) = 0 and
//   +84 (angle added, recomp/patches-v143.txt set fov) = the vertical angle.
// - vk_xr_capture (present thread, gfx_present.c): menus without a scene and the HUD panel; publishes the newest pair.
// Every submission to the graphics queue holds vk_queue_lock, the runtime's included (xrEndFrame and the swapchain calls
// may submit).
#include "vk.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"

#include "cfg.h"
#include "input_map.h"
#include "input.h"
#include "host.h"
#include "platform/platform.h"

static struct {
    int wanted, ok;
    void *lib;
    PFN_xrGetInstanceProcAddr gpa;
    XrInstance inst;
    XrSystemId sys;
    char iext[2048], dext[2048];
    // session
    XrSession session;
    XrSpace space;
    XrSwapchain sc[2];
    VkImage sc_img[2][8];
    uint32_t sc_n[2], sw, sh;
    XrSwapchain hsc;      // the HUD panel's (TOTK_VR_HUD): made with the eyes', the HUD layer's size
    VkImage hsc_img[8];
    uint32_t hsc_n, hw, hh;
    XrSwapchain rsc;      // the aim reticle's
    VkImage rsc_img[8];
    uint32_t rsc_n, rw, rh;
    XrSwapchain wsc;      // the world markers' (health bars, labels: vk_exec.c)
    VkImage wsc_img[8];
    uint32_t wsc_n, ww, wh;
    int running;
    _Atomic int active;   // frames are wanted (the session runs)
    int frame_ext;        // XR_VALVE_frame_controller_interaction enabled
    _Atomic int quit;     // vk_xr_shutdown: 1 asked, 2 exit requested from SteamVR
    _Atomic int done;     // the VR thread has ended (its session destroyed)
    int64_t quit_t0;
} X;

// ---- poses (VR thread locates, the camera hook reads, the present thread and the VR thread show)
// tan: left (negative), right, up, down (negative); g*: the game camera this eye was rendered with (yaw of its back axis
// about +y, position in game units, units per metre), so a later frame's camera turn can be shown by reprojection.
typedef struct { XrPosef pose; float tan[4]; int valid; float gyaw, gcam[3], gscale; int gvalid; int64_t tdisp; uint64_t tloc; int locked; } EyeView;
static struct { float yaw, cam[3]; int valid; } game_now;   // the game camera of the newest frame shown (captured)
static _Atomic unsigned eye_gen[2];   // captures per eye (cadence log)
static PlatMutex vlock = PLAT_MUTEX_INIT;
static EyeView located[2], rendered[2], shown[2];

int vk_xr_world(void) { return vk_xr_wanted(); }   // TOTK_VR=1 is the one switch: stereo pairs, head-tracked world view
int vk_xr_replay_stereo(void) { return vk_xr_world(); }

#define XRF(name) PFN_##name name = (PFN_##name)xr_fn(#name)
static PFN_xrVoidFunction xr_fn(const char *name) {
    PFN_xrVoidFunction f = NULL;
    if (X.gpa) X.gpa(X.inst, name, &f);
    if (!f) rt_log("vr: OpenXR function %s missing", name);
    return f;
}

int vk_xr_wanted(void) {
    static int read;
    if (!read) read = 1, X.wanted = cfg_bool("TOTK_VR");
    return X.wanted;
}

static int xr_init_once(void);
// OpenXR's start (loader, instance, the headset), once: cfg.c vr_defaults asks first, before anything is sized for VR;
// vk_xr_instance_exts reuses the answer.
int vk_xr_probe(void) {
    static int tried, ok;
    if (!tried) tried = 1, ok = xr_init_once();
    return ok;
}
static int xr_init(void) { return vk_xr_probe(); }
static int xr_init_once(void) {
#ifdef _WIN32
    // beside totk.exe (the installer puts it there), by its full path: plat_lib_open searches only System32 for a bare name
    char xp[1024] = "";
    if (plat_exe_path(xp, sizeof xp)) {
        char *sl = strrchr(xp, '\\'), *fs = strrchr(xp, '/');
        if (fs > sl) sl = fs;
        if (sl && (size_t)(sl + 1 - xp) + 18 < sizeof xp) strcpy(sl + 1, "openxr_loader.dll");
        else xp[0] = 0;
    }
    X.lib = xp[0] ? plat_lib_open(xp) : NULL;
    if (!X.lib) {
        char why[256] = "";
        plat_lib_error(why, sizeof why);
        rt_log("vr: %s could not be loaded (%s)", xp[0] ? xp : "openxr_loader.dll", why);
        return 0;
    }
    rt_log("vr: OpenXR loader %s", xp);
#else
    X.lib = plat_lib_open("libopenxr_loader.so.1");
    if (!X.lib) { rt_log("vr: libopenxr_loader.so.1 not found (install the openxr package)"); return 0; }
#endif
    X.gpa = (PFN_xrGetInstanceProcAddr)plat_lib_sym(X.lib, "xrGetInstanceProcAddr");
    if (!X.gpa) return 0;
    XRF(xrCreateInstance);
    if (!xrCreateInstance) return 0;
    const char *exts[2] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    uint32_t next = 1;
    // The Steam Frame controllers' own interaction profile (SteamVR offers it as an extension): with it the game reads
    // them 1:1 as a split gamepad (xi_* below); without it the Touch bindings are used, which SteamVR maps onto them.
    XRF(xrEnumerateInstanceExtensionProperties);
    static XrExtensionProperties ep[128];
    uint32_t nep = 0;
    if (xrEnumerateInstanceExtensionProperties) {
        for (uint32_t i = 0; i < 128; i++) ep[i] = (XrExtensionProperties){.type = XR_TYPE_EXTENSION_PROPERTIES};
        if (XR_FAILED(xrEnumerateInstanceExtensionProperties(NULL, 128, &nep, ep))) nep = 0;
    }
    for (uint32_t i = 0; i < nep; i++)
        if (strstr(ep[i].extensionName, "frame_controller")) {
            exts[next++] = ep[i].extensionName, X.frame_ext = 1;
            rt_log("vr: the runtime offers %s: the Frame controllers are read as a gamepad", ep[i].extensionName);
            break;
        }
    XrInstanceCreateInfo ci = {.type = XR_TYPE_INSTANCE_CREATE_INFO, .enabledExtensionCount = next, .enabledExtensionNames = exts};
    strcpy(ci.applicationInfo.applicationName, "TotK Recomp");
    strcpy(ci.applicationInfo.engineName, "TotK Recomp");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    XrResult r = xrCreateInstance(&ci, &X.inst);
    if (XR_FAILED(r)) { rt_log("vr: xrCreateInstance failed (%d); is SteamVR the OpenXR runtime and running?", (int)r); return 0; }
    XRF(xrGetSystem);
    XrSystemGetInfo gi = {.type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    r = xrGetSystem(X.inst, &gi, &X.sys);
    if (XR_FAILED(r)) { rt_log("vr: no headset (xrGetSystem %d); is the Frame connected to SteamVR?", (int)r); return 0; }
    XRF(xrGetVulkanInstanceExtensionsKHR);
    XRF(xrGetVulkanDeviceExtensionsKHR);
    uint32_t n = 0;
    if (XR_FAILED(xrGetVulkanInstanceExtensionsKHR(X.inst, X.sys, sizeof X.iext, &n, X.iext)) ||
        XR_FAILED(xrGetVulkanDeviceExtensionsKHR(X.inst, X.sys, sizeof X.dext, &n, X.dext))) {
        rt_log("vr: the runtime's Vulkan extension lists could not be read");
        return 0;
    }
    rt_log("vr: OpenXR ready (SteamVR); Vulkan instance extensions: %s; device extensions: %s", X.iext, X.dext);
    return X.ok = 1;
}

// Splits a space-separated list in place and adds each name not already in ext[].
static void add_list(char *list, const char **ext, uint32_t *n, uint32_t cap) {
    for (char *s = list; *s;) {
        while (*s == ' ') *s++ = 0;
        if (!*s) break;
        char *name = s;
        while (*s && *s != ' ') s++;
        if (*s) *s++ = 0;
        int have = 0;
        for (uint32_t i = 0; i < *n; i++) have |= !strcmp(ext[i], name);
        if (!have && *n < cap) ext[(*n)++] = name;
    }
}

void vk_xr_instance_exts(const char **ext, uint32_t *n, uint32_t cap) {
    if (!vk_xr_wanted()) return;
    if (!xr_init()) { rt_log("vr: TOTK_VR is set but OpenXR could not start; playing without VR"); return; }
    add_list(X.iext, ext, n, cap);
}

VkPhysicalDevice vk_xr_physical(VkInstance instance) {
    if (!X.ok) return VK_NULL_HANDLE;
    XRF(xrGetVulkanGraphicsDeviceKHR);
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    if (XR_FAILED(xrGetVulkanGraphicsDeviceKHR(X.inst, X.sys, instance, &pd))) rt_log("vr: the runtime named no GPU");
    return pd;
}

void vk_xr_device_exts(const char **ext, uint32_t *n, uint32_t cap) {
    if (X.ok) add_list(X.dext, ext, n, cap);
}

// ---- eye images (present thread writes, VR thread reads; both on the graphics queue, ordered by its lock)
// img[0..1]: each eye's newest frame (present thread); img[2..3]: the pair the headset shows, copied from 0..1 when a
// right-eye frame completes a left/right pair, so both eyes change on the same headset frame.
static struct {
    VkImage img[6];   // 4..5: the newest complete stereo pair, snapshotted when its right eye finished
    VmaAllocation alloc[6];
    uint32_t w, h;
    VkFormat fmt;
    _Atomic int have[6];
} E;
static _Atomic int snap_ready;   // img[4..5] hold a pair not yet published
static EyeView snap_shown[2];
static EyeView pair_shown[2];   // poses of img[2..3] as last recorded (CPU side, ahead of the GPU copy)
// Which pair the GPU really copied: each pair gets a number (pgen) written into gbuf[0] by the GPU right after its copy;
// the headset blit copies gbuf[0] into gbuf[1 + eye] after reading the pictures, so the VR thread looks up the pose of
// exactly the pictures it sent (pring); a pose recorded on the CPU at capture time would be paired with older pictures
// whenever the headset blit ran before the present's copy.
static VkBuffer gbuf;
static VmaAllocation galloc;
static volatile uint32_t *gw;
static EyeView pring[16][2];
static uint32_t pgen;

static void img_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss,
                        VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = ss, .srcAccessMask = sa,
                               .dstStageMask = ds, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

static int eyes_ensure(VkCommandBuffer cb, uint32_t w, uint32_t h, VkFormat fmt) {
    if (E.img[0] && E.w == w && E.h == h && E.fmt == fmt) return 1;
    VkCtx *c = vk_ctx();
    if (E.img[0]) {
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        for (int i = 0; i < 6; i++) vmaDestroyImage(c->vma, E.img[i], E.alloc[i]), E.img[i] = VK_NULL_HANDLE;
    }
    for (int i = 0; i < 6; i++) {
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ii, &ai, &E.img[i], &E.alloc[i], NULL) != VK_SUCCESS) {
            E.img[i] = VK_NULL_HANDLE;
            return 0;
        }
        img_barrier(cb, E.img[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        atomic_store(&E.have[i], 0);
    }
    if (!gbuf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo bai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                       .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                       .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(c->vma, &bi, &bai, &gbuf, &galloc, &info) == VK_SUCCESS) gw = info.pMappedData, gw[0] = gw[1] = gw[2] = 0;
        else gbuf = VK_NULL_HANDLE, rt_log("vr: no pair number buffer; poses follow the CPU record");
    }
    E.w = w, E.h = h, E.fmt = fmt;
    rt_log("vr: eye images %ux%u (format %d)", w, h, (int)fmt);
    return 1;
}

int vk_xr_active(void) { return atomic_load(&X.active); }
static float xr_ipd_m;   // the eye distance SteamVR reports (publish_views)
float vk_xr_ipd(void) { return vk_xr_active() ? xr_ipd_m : 0; }

// The newest head pose for the frame of `eye`, as position (metres, LOCAL space: x right, y up, z back from where the
// headset faced at the start; the point between the eyes) and rotation (rot[col * 3 + row], columns = the head's right,
// up and back axes), plus *dx = the eye's offset along the head's right axis (negative = left). Marks the eye's pose as
// the one that frame renders with. 0 without VR or before SteamVR gave a pose.
static XrPosef head_pose;
static float eye_dx[2];
int vk_xr_eye_view(int eye, float pos[3], float rot[9], float *dx) {
    if (!X.ok || !vk_xr_active()) return 0;
    plat_mutex_lock(&vlock);
    EyeView v = located[eye & 1];
    XrPosef h = head_pose;
    *dx = eye_dx[eye & 1];
    if (v.valid) rendered[eye & 1] = v, rendered[eye & 1].gvalid = 0;
    if (v.valid && located[1].valid) rendered[1] = located[1], rendered[1].gvalid = 0;
    plat_mutex_unlock(&vlock);
    if (!v.valid) return 0;
    v.pose = h;
    const XrQuaternionf q = v.pose.orientation;
    float x = q.x, y = q.y, z = q.z, w = q.w;
    rot[0] = 1 - 2 * (y * y + z * z), rot[1] = 2 * (x * y + w * z), rot[2] = 2 * (x * z - w * y);
    rot[3] = 2 * (x * y - w * z), rot[4] = 1 - 2 * (x * x + z * z), rot[5] = 2 * (y * z + w * x);
    rot[6] = 2 * (x * z + w * y), rot[7] = 2 * (y * z - w * x), rot[8] = 1 - 2 * (x * x + y * y);
    pos[0] = v.pose.position.x, pos[1] = v.pose.position.y, pos[2] = v.pose.position.z;
    return 1;
}

// The camera hook (vr_camera.c vr_apply): the game camera the frame of `eye` renders with, for its eye view's label.
void vk_xr_note_game(int eye, float yaw, const float cam[3], float scale) {
    plat_mutex_lock(&vlock);
    EyeView *r = &rendered[eye & 1];
    r->gyaw = yaw, r->gscale = scale, r->gvalid = 1;
    memcpy(r->gcam, cam, 12);
    EyeView *q = &rendered[1];
    q->gyaw = yaw, q->gscale = scale, q->gvalid = 1;
    memcpy(q->gcam, cam, 12);
    plat_mutex_unlock(&vlock);
}

// Each frame's head pose (vr_camera.c vr_head_for) gets a serial (a ring of the last 16); the frame's picture is
// labelled at its scene end with that pose and the game camera it was drawn with (vk_xr_replay_label), never with a
// newer pose or camera (the world would swim with the head).
static EyeView rp_ring[16][2], rp_scene[2];
static int rp_eye[16];   // stereo pairs (vr_camera.c vr_pairs): the eye the serial's frame draws; -1 both
void vk_xr_replay_set_eye(unsigned s, int eye) { plat_mutex_lock(&vlock); rp_eye[s % 16] = eye; plat_mutex_unlock(&vlock); }
static unsigned rp_serial;
static int rp_valid;
// Head lock: while the game draws from a camera of its own (an event / talk camera: vr_camera.c sees the render camera
// records holding a camera that is not our head copy), its pictures have no head in them; labelled with the head pose,
// SteamVR would show them glued to the face. Those frames are labelled with the pose the head had when the lock began,
// without the game-camera correction: the picture stays put in the world and updates live. TOTK_VR_HEADLOCK=0 turns it
// off.
static _Atomic int head_lock;
static EyeView lock_view[2];
// The head lock decided for frame `serial`: the frame's label is taken at its start (vk_xr_note_replay) with the lock
// state of the frame before, so the first frame after every switch (a cutscene cutting between the game's own camera,
// held still, and the head-tracked event camera) would be drawn one way and labelled the other, and SteamVR would turn
// that eye by all the head motion since the lock began. The label follows the lock decided for the frame.
// TOTK_VR_LOCKLABEL=0: not.
// The label each frame would have without the head lock (vk_xr_head_lock_frame puts it back when the lock is decided off)
static EyeView rp_free[16][2];
void vk_xr_head_lock(int on);
void vk_xr_head_lock_frame(unsigned serial, int on) {
    vk_xr_head_lock(on);
    static int ll = -1;
    if (ll < 0) { const char *e = cfg_get("TOTK_VR_LOCKLABEL"); ll = !(e && *e == '0'); }
    if (!ll || !serial) return;
    plat_mutex_lock(&vlock);
    if (rp_serial - serial < 14) {
        static unsigned long nfix;
        for (int e = 0; e < 2; e++) {
            EyeView *r = &rp_ring[serial % 16][e];
            if (on && !r->locked) {
                *r = rp_free[serial % 16][e];
                r->pose = lock_view[e].pose, memcpy(r->tan, lock_view[e].tan, sizeof r->tan), r->valid = lock_view[e].valid, r->gvalid = 0, r->locked = 1;
                nfix++;
            } else if (!on && r->locked) *r = rp_free[serial % 16][e], nfix++;
        }
        static unsigned long nl;
        if (nfix && nfix != nl && (nl < 20 || nfix % 100 == 0)) nl = nfix, rt_log("vr: frame labels set to the head lock decided for their frame (%lu so far)", nfix);
    }
    plat_mutex_unlock(&vlock);
}
void vk_xr_head_lock(int on) {
    plat_mutex_lock(&vlock);
    if (on && !atomic_load(&head_lock))
        for (int e = 0; e < 2; e++) lock_view[e] = rendered[e], lock_view[e].gvalid = 0, lock_view[e].locked = 1;
    plat_mutex_unlock(&vlock);
    if (atomic_exchange(&head_lock, on) != on) {
        static unsigned nl;
        if (nl < 60) nl++, rt_log("vr: %s", on ? "the game draws from a camera of its own (event, talk): the view is held still in the world" : "the game draws from the head camera again: the view follows the head");
    }
}
unsigned vk_xr_note_replay(float yaw, const float cam[3], float scale) {
    plat_mutex_lock(&vlock);
    unsigned s = ++rp_serial;
    rp_eye[s % 16] = -1;
    for (int e = 0; e < 2; e++) {
        rp_ring[s % 16][e] = rendered[e];
        rp_ring[s % 16][e].gscale = scale, rp_ring[s % 16][e].gvalid = 1, rp_ring[s % 16][e].gyaw = yaw;
        memcpy(rp_ring[s % 16][e].gcam, cam, 12);
        if (!rendered[e].valid) rp_ring[s % 16][e].valid = 0;
        rp_free[s % 16][e] = rp_ring[s % 16][e];
        if (atomic_load(&head_lock)) {   // held still: the pose the head had when the lock began
            EyeView *r = &rp_ring[s % 16][e];
            r->pose = lock_view[e].pose, memcpy(r->tan, lock_view[e].tan, sizeof r->tan), r->valid = lock_view[e].valid, r->gvalid = 0, r->locked = 1;
        }
    }
    plat_mutex_unlock(&vlock);
    return s;
}
// Stereo pairs: the game camera each frame was really drawn with (vr_camera.c rslot_frame; the right eye reuses its left
// frame's), recorded with its serial, for the frame's label (otherwise the headset loop would turn a picture by the
// difference to another frame's camera on every new pair).
static struct { float yaw, cam[3]; int ok; unsigned ser; } rp_game[16];
void vk_xr_replay_set_game(unsigned s, float yaw, const float cam[3]) {
    if (!s) return;
    plat_mutex_lock(&vlock);
    rp_game[s % 16].yaw = yaw, memcpy(rp_game[s % 16].cam, cam, 12), rp_game[s % 16].ok = 1, rp_game[s % 16].ser = s;
    plat_mutex_unlock(&vlock);
}
void vk_xr_replay_label(unsigned serial, float yaw, const float cam[3]) {
    plat_mutex_lock(&vlock);
    if (serial && rp_game[serial % 16].ok && rp_game[serial % 16].ser == serial) yaw = rp_game[serial % 16].yaw, cam = rp_game[serial % 16].cam;
    if (serial && rp_serial - serial < 16) {
        int only = rp_eye[serial % 16];   // stereo pairs: this frame labels its own eye only
        for (int e = 0; e < 2; e++) {
            if (only >= 0 && e != only) continue;
            rp_scene[e] = rp_ring[serial % 16][e];
            if (rp_scene[e].locked) continue;   // a held frame: no game-camera correction
            rp_scene[e].gyaw = yaw, rp_scene[e].gvalid = 1;
            memcpy(rp_scene[e].gcam, cam, 12);
        }
        rp_valid = rp_scene[0].valid && rp_scene[1].valid;
    }
    plat_mutex_unlock(&vlock);
}
int vr_camera_replay(void);   // vr_camera.c


// An eye image's pose as seen from the newest game camera: the game camera turned by d = yaw_r - yaw_now and moved
// since the image was drawn, so SteamVR's reprojection carries the turn at the headset's rate. In LOCAL space the
// game camera's yaw frame is Ry(yaw): pose' = Ry(d) * pose, position' = Ry(d) p + Ry(-yaw_now) (cam_r - cam_now) / scale.
static XrPosef seen_now(const EyeView *v) {
    XrPosef p = v->pose;
    if (!v->gvalid || !game_now.valid) return p;
    float d = v->gyaw - game_now.yaw;
    while (d > 3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    float c = cosf(d), s = sinf(d), hc = cosf(d * 0.5f), hs = sinf(d * 0.5f);
    XrQuaternionf q = p.orientation;   // (0, hs, 0, hc) * q
    p.orientation = (XrQuaternionf){hc * q.x + hs * q.z, hc * q.y + hs * q.w, hc * q.z - hs * q.x, hc * q.w - hs * q.y};
    float x = p.position.x, z = p.position.z;
    p.position.x = c * x + s * z, p.position.z = -s * x + c * z;
    float sc = v->gscale > 0.01f ? v->gscale : 1, wx = (v->gcam[0] - game_now.cam[0]) / sc, wy = (v->gcam[1] - game_now.cam[1]) / sc,
          wz = (v->gcam[2] - game_now.cam[2]) / sc, cn = cosf(game_now.yaw), sn = sinf(game_now.yaw);
    p.position.x += cn * wx - sn * wz, p.position.y += wy, p.position.z += sn * wx + cn * wz;
    return p;
}

// Publishes both eyes of a headset frame, with one symmetric view (the game renders one) that covers both eyes' views at
// the output's shape, and points the game's projection at it.
static void publish_views(const XrView v[2], int64_t tdisp) {
    float th = 0, tv = 0;
    for (int e = 0; e < 2; e++) {
        float l = tanf(-v[e].fov.angleLeft), r = tanf(v[e].fov.angleRight), u = tanf(v[e].fov.angleUp), d = tanf(-v[e].fov.angleDown);
        th = fmaxf(th, fmaxf(l, r)), tv = fmaxf(tv, fmaxf(u, d));
    }
    // TOTK_VR_MARGIN degrees beyond the widest edge (live): SteamVR turns a picture that arrives late to the head's newest
    // pose, and past the drawn edge it shows black (black at each eye's outer edge in fast head turns)
    double mg = cfg_double("TOTK_VR_MARGIN");
    if (mg > 0 && mg <= 20) {
        float rad = (float)(mg / 57.2958);
        th = tanf(fminf(atanf(th) + rad, 1.45f)), tv = tanf(fminf(atanf(tv) + rad, 1.45f));
    }
    unsigned out[2] = {E.w, E.h};
    if (!out[0]) res_wanted(out);
    float a = out[1] ? (float)out[0] / (float)out[1] : 1.0f;
    tv = fmaxf(tv, th / a), th = tv * a;
    // head = between the eyes, orientation = the eyes' mean (the same unless the displays are canted)
    XrQuaternionf qa = v[0].pose.orientation, qb = v[1].pose.orientation;
    float dt = qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w, sg = dt < 0 ? -1.0f : 1.0f;
    XrQuaternionf m = {qa.x + sg * qb.x, qa.y + sg * qb.y, qa.z + sg * qb.z, qa.w + sg * qb.w};
    float ml = sqrtf(m.x * m.x + m.y * m.y + m.z * m.z + m.w * m.w);
    if (ml > 1e-6f) m.x /= ml, m.y /= ml, m.z /= ml, m.w /= ml; else m = qa;
    XrVector3f c = {(v[0].pose.position.x + v[1].pose.position.x) / 2, (v[0].pose.position.y + v[1].pose.position.y) / 2,
                    (v[0].pose.position.z + v[1].pose.position.z) / 2};
    float rx = 1 - 2 * (m.y * m.y + m.z * m.z), ry = 2 * (m.x * m.y + m.w * m.z), rz = 2 * (m.x * m.z - m.w * m.y);   // head right axis
    xr_ipd_m = sqrtf(powf(v[1].pose.position.x - v[0].pose.position.x, 2) + powf(v[1].pose.position.y - v[0].pose.position.y, 2) +
                     powf(v[1].pose.position.z - v[0].pose.position.z, 2));
    static int told;
    if (!told++) rt_log("vr: eyes %.1f mm apart, displays canted %.2f degrees", 1000 * sqrtf(powf(v[1].pose.position.x - v[0].pose.position.x, 2) +
           powf(v[1].pose.position.y - v[0].pose.position.y, 2) + powf(v[1].pose.position.z - v[0].pose.position.z, 2)),
           2 * acosf(fminf(1.0f, fabsf(dt))) * 57.2958f);
    plat_mutex_lock(&vlock);
    head_pose = (XrPosef){.orientation = m, .position = c};
    for (int e = 0; e < 2; e++) {
        XrVector3f p = v[e].pose.position;
        eye_dx[e] = (p.x - c.x) * rx + (p.y - c.y) * ry + (p.z - c.z) * rz;
        // rendered with the head's rotation at the eye's position along the head's right axis
        XrPosef rp = {.orientation = m, .position = {c.x + eye_dx[e] * rx, c.y + eye_dx[e] * ry, c.z + eye_dx[e] * rz}};
        located[e] = (EyeView){.pose = rp, .tan = {-th, th, tv, -tv}, .valid = 1, .tdisp = tdisp, .tloc = kern_now_ns()};
    }
    plat_mutex_unlock(&vlock);
    static int set = -2;
    static float shown_fovy;
    if (set == -2) set = rt_patch_set("fov");
    uint8_t *page = rt_patch_page();
    float fovy = 2 * atanf(tv), zero = 0;
    if (set < 0 || !page || fabsf(fovy - shown_fovy) < 1e-4f) return;
    memcpy(page + 84, &fovy, 4);
    memcpy(page + 60, &zero, 4);
    if (shown_fovy) rt_log("vr: game view now %.1f x %.1f degrees", 2 * atanf(th) * 57.2958f, fovy * 57.2958f);
    if (!shown_fovy) {
        rt_patch_switch(set, 1);
        rt_log("vr: game view %.1f x %.1f degrees (shape %.3f) covers both eyes", 2 * atanf(th) * 57.2958f, fovy * 57.2958f, a);
    }
    shown_fovy = fovy;
}

static _Atomic int scene_fresh;   // a complete pair (a right eye after a left one) since the last present
// A scene was drawn since the last present, pair complete or not: a scene only becomes fresh when a right eye follows a
// left one, and while the eye order breaks (Ascend through rock, falls into the Depths, pickups) a few presents go by
// without a complete pair although the game draws its world; those are no menu (TOTK_VR_SCENEANY=0 counts them as one).
static _Atomic int scene_any;
// Menus are drawn without a scene (full-screen menus, title, loading): the last pair stays around the player and the
// presented frame goes on the HUD panel, opaque (vk_xr_capture).
static _Atomic int menu_now;   // the HUD panel shows the presented frame
// Stereo pairs (vr_camera.c vr_pairs): the frame's scene image goes to its eye; a right frame after a left one makes
// the pair. The eye: the frame's label serial (vk_xr_replay_set_eye).
int vk_xr_serial_eye(unsigned serial) {
    plat_mutex_lock(&vlock);
    int e = serial && rp_serial - serial < 16 ? rp_eye[serial % 16] : -1;
    plat_mutex_unlock(&vlock);
    return e;
}
static int eyes_ensure(VkCommandBuffer cb, uint32_t w, uint32_t h, VkFormat fmt);
static void pair_publish(VkCommandBuffer cb, int stereo, int base);
// When the game last drew its 3D scene for an eye (vk_xr_pair_scene). Pacing runs at twice the headset rate only while it
// does (two game frames, one per eye, a headset frame); without a scene (movies, title, loading) there is nothing to pair
// and the game presents once per headset frame. Running scene-less presents at twice the rate kept the GPU so busy that
// SteamVR's compositor missed its turn (NVIDIA: Xid 109 CTX SWITCH TIMEOUT in vrcompositor during the opening movies,
// the game then waited for ever). TOTK_VR_NOSCENE_PACE=0: twice the rate always.
static _Atomic uint64_t last_scene_ns;
int vk_xr_pair_rate(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_VR_NOSCENE_PACE"); on = !(e && *e == '0'); }
    if (!on) return 1;
    uint64_t t = atomic_load(&last_scene_ns);
    return t && kern_now_ns() - t < 250000000ull;
}
void vk_xr_pair_scene(VkCommandBuffer cb, VkImage img, uint32_t w, uint32_t h, VkFormat fmt, int eye) {
    static int have_left;
    if (!vk_xr_active() || !img || w < 2 || h < 2 || eye < 0) return;
    atomic_store(&last_scene_ns, kern_now_ns());
    if (!eyes_ensure(cb, w, h, fmt)) return;
    const VkPipelineStageFlags2 ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, TR = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    img_barrier(cb, img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, TR, VK_ACCESS_2_TRANSFER_READ_BIT);
    img_barrier(cb, E.img[eye], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {w, h, 1}};
    vkCmdCopyImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, E.img[eye], VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    img_barrier(cb, E.img[eye], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL, VK_ACCESS_2_MEMORY_READ_BIT);
    img_barrier(cb, img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, 0, ALL, 0);
    atomic_store(&E.have[eye], 1);
    atomic_store(&scene_any, 1);
    if (eye == 0) have_left = 1;
    else if (have_left) {   // a right eye completing the pair
        scene_fresh = 1, have_left = 0;
        // The pair goes to the headset here, in the same command buffer that finished its right eye: at the present
        // (another thread, later in the queue) the next pair's left eye could already be in img[0].
        {   // snapshot the complete pair (img[0..1] -> img[4..5]); the present publishes it
            for (int e = 0; e < 2; e++) {
                img_barrier(cb, E.img[e], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, TR, VK_ACCESS_2_TRANSFER_READ_BIT);
                img_barrier(cb, E.img[4 + e], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL,
                            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT);
                VkImageCopy rs = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .extent = {E.w, E.h, 1}};
                vkCmdCopyImage(cb, E.img[e], VK_IMAGE_LAYOUT_GENERAL, E.img[4 + e], VK_IMAGE_LAYOUT_GENERAL, 1, &rs);
                img_barrier(cb, E.img[4 + e], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL, VK_ACCESS_2_MEMORY_READ_BIT);
                img_barrier(cb, E.img[e], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, 0, ALL, 0);
            }
            plat_mutex_lock(&vlock);
            snap_shown[0] = rp_valid ? rp_scene[0] : rendered[0], snap_shown[1] = rp_valid ? rp_scene[1] : rendered[1];
            // One game camera per pair: the right eye is drawn from its left frame's game camera (vr_camera.c, also for
            // event cameras), but its label can carry a newer one; seen_now would then move the left eye by a whole frame
            // of camera motion (35-40 cm in a fast pan). The right eye's game-camera label is the left's.
            // TOTK_VR_PAIRGAME=0: not.
            {
                static int pg = -1;
                if (pg < 0) { const char *e = cfg_get("TOTK_VR_PAIRGAME"); pg = !(e && *e == '0'); }
                if (pg && snap_shown[0].gvalid && snap_shown[1].gvalid && !snap_shown[0].locked && !snap_shown[1].locked) {
                    static double dsum, dmax; static unsigned long dn;
                    float sc = snap_shown[0].gscale > 0.01f ? snap_shown[0].gscale : 1;
                    float dx = snap_shown[1].gcam[0] - snap_shown[0].gcam[0], dy = snap_shown[1].gcam[1] - snap_shown[0].gcam[1], dz = snap_shown[1].gcam[2] - snap_shown[0].gcam[2];
                    double d = sqrt(dx * dx + dy * dy + dz * dz) / sc;
                    dsum += d, dn++, dmax = d > dmax ? d : dmax;
                    if (dn % 1200 == 0) rt_log("vr: pair labels: the right eye's game camera was %.3f m from its left's on average (max %.3f) over 1200 pairs; both now carry the left's", dsum / dn, dmax), dsum = dmax = 0, dn = 0;
                    snap_shown[1].gyaw = snap_shown[0].gyaw, memcpy(snap_shown[1].gcam, snap_shown[0].gcam, sizeof snap_shown[1].gcam);
                }
            }
            plat_mutex_unlock(&vlock);
            atomic_store(&snap_ready, 1);
        }
    }
    static int told;
    if (!told++) rt_log("vr: stereo pairs: each eye from its own game frame, %ux%u (format %d)", w, h, (int)fmt);
}

// The HUD panel (vk_exec.c hud_replay): the HUD's draws replayed onto a transparent RGBA16F layer after the scene, colour
// premultiplied by coverage. Each present with a scene copies the frame's layer into HL.img (the panel's picture, read
// by the VR thread); a frame without HUD draws leaves no panel.
static struct { VkImage img; VmaAllocation alloc; uint32_t w, h; _Atomic int have; _Atomic unsigned gen; } HL;
static VkImage hud_src;
static uint32_t hud_w, hud_h;
static _Atomic int hud_pending;
static _Atomic int strip_seen;   // a quick-select strip was drawn on the HUD layer (vk_exec.c, layout class 5)
void vk_xr_strip_drawn(void) { atomic_store(&strip_seen, 1); }
// After a quick-select strip or the ability wheel closes the game stays paused for a few presents with no scene and no
// strip; that is no menu either (the frozen frame would cover the view on the HUD panel). Held from the strip until the
// scene comes back or a full-screen menu layout starts (marker 9, vk_exec.c). TOTK_VR_STRIPHOLD=0: not.
static _Atomic int strip_hold;
static _Atomic int64_t menu_layout_ns;   // a full-screen menu layout (marker 9) was drawn then
void vk_xr_menu_layout(void) { atomic_store(&strip_hold, 0); atomic_store(&menu_layout_ns, kern_now_ns()); }
void vk_xr_hud_drawn(VkImage hud, uint32_t w, uint32_t h) { hud_src = hud, hud_w = w, hud_h = h, atomic_store(&hud_pending, 1); }
// The HUD panel's picture made on demand (a menu before the first HUD frame: the title screen, the first load)
static int hl_ensure(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (HL.img) return 1;
    VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {w, h, 1}, .mipLevels = 1,
                            .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &HL.img, &HL.alloc, NULL) != VK_SUCCESS) { HL.img = VK_NULL_HANDLE; return 0; }
    img_barrier(cb, HL.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    HL.w = w, HL.h = h;
    rt_log("vr: HUD panel picture %ux%u (for a menu)", w, h);
    return 1;
}
static void hud_publish(VkCommandBuffer cb) {
    if (!atomic_exchange(&hud_pending, 0) || !hud_src) { atomic_store(&HL.have, 0); return; }
    VkCtx *c = vk_ctx();
    if (HL.img && (HL.w != hud_w || HL.h != hud_h)) {   // a new size: the VR thread's swapchain is made again too
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        vmaDestroyImage(c->vma, HL.img, HL.alloc), HL.img = VK_NULL_HANDLE;
    }
    const VkPipelineStageFlags2 ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, TR = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    if (!HL.img) {
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {hud_w, hud_h, 1}, .mipLevels = 1,
                                .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ii, &ai, &HL.img, &HL.alloc, NULL) != VK_SUCCESS) { HL.img = VK_NULL_HANDLE; return; }
        img_barrier(cb, HL.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0, TR,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        HL.w = hud_w, HL.h = hud_h;
        rt_log("vr: HUD panel picture %ux%u", hud_w, hud_h);
    }
    img_barrier(cb, hud_src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, TR,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    img_barrier(cb, HL.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {hud_w, hud_h, 1}};
    vkCmdCopyImage(cb, hud_src, VK_IMAGE_LAYOUT_GENERAL, HL.img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    img_barrier(cb, HL.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL,
                VK_ACCESS_2_MEMORY_READ_BIT);
    img_barrier(cb, hud_src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, 0, ALL, 0);
    atomic_store(&HL.have, 1);
    atomic_fetch_add(&HL.gen, 1);
}

int vr_game_axes(float out[9]);   // vr_camera.c
void vr_aim_offset(float o[3]);   // vr_camera.c
static float reticle_dist = 8;   // m along the aim axis: where the aim ray hits the scene (vk_contact.c aim tracker)
void vk_xr_set_reticle_dist(float d) {   // smoothed a little (one sample per frame, from the left eye's depth)
    if (!(d > 0.3f)) d = 0.3f;
    if (d > 300) d = 300;
    reticle_dist = reticle_dist + (d - reticle_dist) * 0.35f;
    static unsigned n;
    if (++n % 360 == 1) rt_log("vr: aim hit at %.2f m (reticle at %.2f m)", d, reticle_dist);
}
void vk_xr_game_tan(float *th, float *tv) {
    plat_mutex_lock(&vlock);
    *th = located[0].tan[1], *tv = located[0].tan[2];
    plat_mutex_unlock(&vlock);
}
// The aim reticle (vk_exec.c: layout Pointer_00's draws on their own layer): copied like the HUD panel's picture, shown
// as a layer covering the game camera's view, centred on its aim axis (the reticle points lie where the game aims).
static struct { VkImage img; VmaAllocation alloc; uint32_t w, h; _Atomic int have; _Atomic unsigned gen; } RTL;
static VkImage ret_src;
static uint32_t ret_w, ret_h;
static _Atomic int ret_pending;
void vk_xr_reticle_drawn(VkImage img, uint32_t w, uint32_t h) { ret_src = img, ret_w = w, ret_h = h, atomic_store(&ret_pending, 1); }
static void ret_publish(VkCommandBuffer cb) {
    if (!atomic_exchange(&ret_pending, 0) || !ret_src) { atomic_store(&RTL.have, 0); return; }
    VkCtx *c = vk_ctx();
    if (RTL.img && (RTL.w != ret_w || RTL.h != ret_h)) {   // a new size: the VR thread's swapchain is made again too
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        vmaDestroyImage(c->vma, RTL.img, RTL.alloc), RTL.img = VK_NULL_HANDLE;
    }
    const VkPipelineStageFlags2 ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, TR = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    if (!RTL.img) {
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {ret_w, ret_h, 1}, .mipLevels = 1,
                                .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ii, &ai, &RTL.img, &RTL.alloc, NULL) != VK_SUCCESS) { RTL.img = VK_NULL_HANDLE; return; }
        img_barrier(cb, RTL.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0, TR,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        RTL.w = ret_w, RTL.h = ret_h;
        rt_log("vr: aim reticle picture %ux%u", ret_w, ret_h);
    }
    img_barrier(cb, ret_src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, TR,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    img_barrier(cb, RTL.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {ret_w, ret_h, 1}};
    vkCmdCopyImage(cb, ret_src, VK_IMAGE_LAYOUT_GENERAL, RTL.img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    img_barrier(cb, RTL.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL,
                VK_ACCESS_2_MEMORY_READ_BIT);
    img_barrier(cb, ret_src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, 0, ALL, 0);
    atomic_store(&RTL.have, 1);
    atomic_fetch_add(&RTL.gen, 1);
}
// The world markers (vk_exec.c: health bars, lock-on, labels, speech balloons on their own layer): shown as a layer over
// the frame's own view (the pose its pair is labelled with), so each marker sits on what it marks.
static struct { VkImage img; VmaAllocation alloc; uint32_t w, h; _Atomic int have; _Atomic unsigned gen; } WTL;
static VkImage wm_src;
static uint32_t wm_w, wm_h;
static _Atomic int wm_pending;
void vk_xr_worldhud_drawn(VkImage img, uint32_t w, uint32_t h) { wm_src = img, wm_w = w, wm_h = h, atomic_store(&wm_pending, 1); }
// Each world marker at its own depth: the layer is shrunk to WMS_N^2 and read back a few
// frames later; its patches of coverage are the markers. Each gets the depth of the nearest thing in a column just below
// it (what it floats over: an enemy, an NPC), from the G-buffer's depth grid (vk_contact.c), smoothed per marker, and is
// shown as its own quad at that depth (TOTK_VR_MARKERS3D, live, default 1; 0 = the one layer at TOTK_VR_WORLDHUD_DIST).
#define WMS_N 128
#define WMS_RING 4
#define WM_MAX 8
float vk_contact_depth_min(float u0, float v0, float u1, float v1);
static struct { VkImage img; VmaAllocation al; VkBuffer buf[WMS_RING]; VmaAllocation bal[WMS_RING]; const uint16_t *map[WMS_RING]; int used[WMS_RING], slot; } WMS;
static struct { float r[WM_MAX][4], d[WM_MAX]; int n; } WM_list[2];
static _Atomic int WM_cur, WM_ok;
static float half_f(uint16_t h) {
    uint32_t sgn = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, f;
    if (!e) return m ? (sgn ? -1.f : 1.f) * m / 16777216.f : 0;
    if (e == 31) return 1e30f;
    f = sgn | ((e + 112) << 23) | (m << 13);
    float r;
    memcpy(&r, &f, 4);
    return r;
}
static void wm_components(const uint16_t *px) {
    static uint8_t on[WMS_N * WMS_N], lab[WMS_N * WMS_N];
    static int stack[WMS_N * WMS_N];
    for (int i = 0; i < WMS_N * WMS_N; i++) on[i] = half_f(px[i * 4 + 3]) > 0.03f;
    {   // close small gaps (a bar and its icon are one marker): grow by one cell
        static uint8_t g[WMS_N * WMS_N];
        for (int y = 0; y < WMS_N; y++)
            for (int x = 0; x < WMS_N; x++) {
                int v = 0;
                for (int dy = -1; dy <= 1 && !v; dy++)
                    for (int dx = -1; dx <= 1 && !v; dx++) {
                        int xx = x + dx, yy = y + dy;
                        v = xx >= 0 && yy >= 0 && xx < WMS_N && yy < WMS_N && on[yy * WMS_N + xx];
                    }
                g[y * WMS_N + x] = (uint8_t)v;
            }
        memcpy(on, g, sizeof on);
    }
    memset(lab, 0, sizeof lab);
    float r[64][4];
    int area[64], n = 0;
    for (int i = 0; i < WMS_N * WMS_N && n < 64; i++) {
        if (!on[i] || lab[i]) continue;
        int sp = 0, a = 0, x0 = WMS_N, y0 = WMS_N, x1 = -1, y1 = -1;
        stack[sp++] = i, lab[i] = 1;
        while (sp) {
            int c = stack[--sp], x = c % WMS_N, y = c / WMS_N;
            a++, x0 = x < x0 ? x : x0, x1 = x > x1 ? x : x1, y0 = y < y0 ? y : y0, y1 = y > y1 ? y : y1;
            const int nb[4] = {c - 1, c + 1, c - WMS_N, c + WMS_N};
            for (int k = 0; k < 4; k++) {
                int q = nb[k];
                if ((k == 0 && x == 0) || (k == 1 && x == WMS_N - 1) || q < 0 || q >= WMS_N * WMS_N || !on[q] || lab[q]) continue;
                lab[q] = 1, stack[sp++] = q;
            }
        }
        if (a < 4) continue;
        r[n][0] = (float)x0 / WMS_N, r[n][1] = (float)y0 / WMS_N, r[n][2] = (float)(x1 + 1) / WMS_N, r[n][3] = (float)(y1 + 1) / WMS_N, area[n] = a, n++;
    }
    for (int i = 0; i < n; i++)   // the biggest first
        for (int j = i + 1; j < n; j++)
            if (area[j] > area[i]) { int t = area[i]; area[i] = area[j], area[j] = t; float tr[4]; memcpy(tr, r[i], 16), memcpy(r[i], r[j], 16), memcpy(r[j], tr, 16); }
    if (n > WM_MAX) n = WM_MAX;
    int cur = atomic_load(&WM_cur), nx = !cur;
    const typeof(WM_list[0]) *old = &WM_list[cur];
    WM_list[nx].n = n;
    for (int i = 0; i < n; i++) {
        float *q = r[i], h = q[3] - q[1];
        float d = vk_contact_depth_min(q[0], q[3], q[2], fminf(1, q[3] + 1.5f * h + 0.03f));   // what it floats over
        if (!(d > 0)) d = vk_contact_depth_min(q[0], q[1], q[2], q[3]);
        if (!(d > 0)) d = 4;
        if (d < 0.5f) d = 0.5f;
        if (d > 150) d = 150;
        float cx = (q[0] + q[2]) * 0.5f, cy = (q[1] + q[3]) * 0.5f, best = 0.05f;   // smoothing: the same marker last time
        for (int j = 0; j < old->n; j++) {
            float ox = (old->r[j][0] + old->r[j][2]) * 0.5f, oy = (old->r[j][1] + old->r[j][3]) * 0.5f, e = fabsf(ox - cx) + fabsf(oy - cy);
            if (e < best) best = e, d = 0.75f * old->d[j] + 0.25f * d;
        }
        memcpy(WM_list[nx].r[i], q, 16), WM_list[nx].d[i] = d;
    }
    atomic_store(&WM_cur, nx), atomic_store(&WM_ok, 1);
    static unsigned long nl;
    if (n && nl++ % 1800 == 0) rt_log("vr: world markers: %d, first at %.2f..%.2f x %.2f..%.2f, %.1f m", n, r[0][0], r[0][2], r[0][1], r[0][3], WM_list[nx].d[0]);
}
static void wm_find(VkCommandBuffer cb) {
    if (!cfg_bool("TOTK_VR_MARKERS3D")) { atomic_store(&WM_ok, 0); return; }
    VkCtx *c = vk_ctx();
    const VkPipelineStageFlags2 TR = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    if (!WMS.img) {
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R16G16B16A16_SFLOAT,
                                .extent = {WMS_N, WMS_N, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ii, &ai, &WMS.img, &WMS.al, NULL) != VK_SUCCESS) { WMS.img = VK_NULL_HANDLE; return; }
        img_barrier(cb, WMS.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    int s = WMS.slot;
    WMS.slot = (WMS.slot + 1) % WMS_RING;
    if (WMS.used[s] && WMS.map[s]) WMS.used[s] = 0, wm_components(WMS.map[s]);   // WMS_RING publishes old: done
    if (!WMS.buf[s]) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = WMS_N * WMS_N * 8, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST, .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                      .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(c->vma, &bi, &ai, &WMS.buf[s], &WMS.bal[s], &info) != VK_SUCCESS) { WMS.buf[s] = VK_NULL_HANDLE; return; }
        WMS.map[s] = info.pMappedData;
    }
    img_barrier(cb, WTL.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT, TR, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)WTL.w, (int32_t)WTL.h, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {WMS_N, WMS_N, 1}}};
    vkCmdBlitImage(cb, WTL.img, VK_IMAGE_LAYOUT_GENERAL, WMS.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    img_barrier(cb, WMS.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT, TR, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy rc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {WMS_N, WMS_N, 1}};
    vkCmdCopyImageToBuffer(cb, WMS.img, VK_IMAGE_LAYOUT_GENERAL, WMS.buf[s], 1, &rc);
    img_barrier(cb, WMS.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, VK_ACCESS_2_TRANSFER_READ_BIT, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    WMS.used[s] = 1;
}
// How big a marker or reticle at distance d looks next to its flat size: like an object of fixed size, flat-sized at
// TOTK_VR_MARKER_REF m, kept within TOTK_VR_MARKER_MIN..MAX of the flat size (live).
static float marker_scale(float d) {
    double ref = cfg_double("TOTK_VR_MARKER_REF"), lo = cfg_double("TOTK_VR_MARKER_MIN"), hi = cfg_double("TOTK_VR_MARKER_MAX");
    if (!(ref > 0.1)) ref = 4;
    if (!(lo > 0)) lo = 0.05;
    if (!(hi >= lo)) hi = 2;
    double k = ref / (d > 0.05f ? d : 0.05f);
    return (float)(k < lo ? lo : k > hi ? hi : k);
}
static void wm_publish(VkCommandBuffer cb) {
    if (!atomic_exchange(&wm_pending, 0) || !wm_src) { atomic_store(&WTL.have, 0); return; }
    VkCtx *c = vk_ctx();
    if (WTL.img && (WTL.w != wm_w || WTL.h != wm_h)) {   // a new size: the VR thread's swapchain is made again too
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        vmaDestroyImage(c->vma, WTL.img, WTL.alloc), WTL.img = VK_NULL_HANDLE;
    }
    const VkPipelineStageFlags2 ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, TR = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    if (!WTL.img) {
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {wm_w, wm_h, 1}, .mipLevels = 1,
                                .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ii, &ai, &WTL.img, &WTL.alloc, NULL) != VK_SUCCESS) { WTL.img = VK_NULL_HANDLE; return; }
        img_barrier(cb, WTL.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE, 0, TR,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        WTL.w = wm_w, WTL.h = wm_h;
        rt_log("vr: world markers picture %ux%u", wm_w, wm_h);
    }
    img_barrier(cb, wm_src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, TR,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    img_barrier(cb, WTL.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {wm_w, wm_h, 1}};
    vkCmdCopyImage(cb, wm_src, VK_IMAGE_LAYOUT_GENERAL, WTL.img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    img_barrier(cb, WTL.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL,
                VK_ACCESS_2_MEMORY_READ_BIT);
    img_barrier(cb, wm_src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, 0, ALL, 0);
    wm_find(cb);
    atomic_store(&WTL.have, 1);
    atomic_fetch_add(&WTL.gen, 1);
}

// A few presents without a scene are no menu (picking up a fused weapon or fusing with Ultrahand, the game renders the
// new item's icon for a few frames without its world): the panel shows the presented frame only with a full-screen menu
// layout drawn in the last second (vk_xr_menu_layout), or after TOTK_VR_NOSCENE_MS (default 400) without a scene
// (title, loading, movies).
static int menu_evidence(int no_scene) {
    static int64_t since;
    static int ms = -1;
    if (ms < 0) { const char *e = cfg_get("TOTK_VR_NOSCENE_MS"); ms = e ? atoi(e) : 400; }
    int64_t now = kern_now_ns();
    if (no_scene <= 3 || !since) since = now;
    int64_t ml = atomic_load(&menu_layout_ns);
    int ok = ms <= 0 || (ml && now - ml < 1000000000ll) || now - since >= (int64_t)ms * 1000000;
    static unsigned long nh;
    if (!ok && no_scene == 3 && (++nh <= 20 || nh % 200 == 0)) rt_log("vr: presents without a scene but no menu (yet): the 3D view and the HUD stay (%lu so far)", nh);
    return ok;
}
void vk_xr_capture(VkCommandBuffer cb, VkImage src, VkFormat fmt, const int crop[4]) {
    if (!vk_xr_active() || crop[2] < 2 || crop[3] < 2) return;
    // The eye pictures come with the scenes (vk_xr_pair_scene); a present without a complete pair keeps the last one.
    static int no_scene;
    // Without a scene (full-screen menus, title, loading) the last 3D pair stays around the player and the presented frame
    // (the menu) goes on the HUD panel, opaque. Dialogue and the inventory keep drawing the scene: their HUD goes on the
    // panel as usual.
    static int sa = -1;
    if (sa < 0) { const char *e = cfg_get("TOTK_VR_SCENEANY"); sa = !(e && *e == '0'); }
    int drawn = atomic_exchange(&scene_any, 0) && sa;   // the game drew its world since the last present
    if (drawn && !scene_fresh) {   // no complete pair, but a scene: not a menu (the last pair stays)
        no_scene = 0;
        if (atomic_exchange(&menu_now, 0)) rt_log("vr: menu off the HUD panel (a scene without a complete pair)"), atomic_store(&HL.have, 0);
    }
    if (drawn || scene_fresh) atomic_store(&strip_hold, 0);
    if (!scene_fresh && !drawn && atomic_load(&strip_hold) && !atomic_load(&strip_seen)) return;   // a strip just closed: the 3D view and the last panel stay
    if (scene_fresh) {
        no_scene = 0;
        if (atomic_exchange(&menu_now, 0)) rt_log("vr: menu off the HUD panel"), atomic_store(&HL.have, 0);
    } else if (atomic_exchange(&strip_seen, 0) && atomic_load(&hud_pending)) {
        // a quick-select strip over the frozen game: the last 3D pair stays and the panel shows the HUD layer (the strips,
        // without the game's frozen picture behind them)
        no_scene = 0;
        { static int sh = -1; if (sh < 0) { const char *e = cfg_get("TOTK_VR_STRIPHOLD"); sh = !(e && *e == '0'); } if (sh) atomic_store(&strip_hold, 1); }
        if (atomic_exchange(&menu_now, 0)) rt_log("vr: menu off the HUD panel (a quick-select strip)");
        hud_publish(cb);
        return;
    } else if (++no_scene >= 3 && menu_evidence(no_scene) && hl_ensure(cb, (uint32_t)crop[2], (uint32_t)crop[3])) {   // the menu on the panel
        if (!atomic_exchange(&menu_now, 1)) rt_log("vr: menu on the HUD panel (no scene)");
        atomic_store(&wm_pending, 0), atomic_store(&WTL.have, 0), atomic_store(&ret_pending, 0), atomic_store(&RTL.have, 0);
        const VkPipelineStageFlags2 ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, TR = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        img_barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, TR, VK_ACCESS_2_TRANSFER_READ_BIT);
        img_barrier(cb, HL.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, TR,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .srcOffsets = {{crop[0], crop[1], 0}, {crop[0] + crop[2], crop[1] + crop[3], 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)HL.w, (int32_t)HL.h, 1}}};
        vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, HL.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        img_barrier(cb, HL.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL, VK_ACCESS_2_MEMORY_READ_BIT);
        img_barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, 0, ALL, 0);
        atomic_store(&hud_pending, 0), atomic_store(&HL.have, 1), atomic_fetch_add(&HL.gen, 1);
        return;
    } else return;
    scene_fresh = 0;
    hud_publish(cb);
    ret_publish(cb);
    wm_publish(cb);
    // the snapshot of the newest complete pair (vk_xr_pair_scene), never a mix of two pairs
    if (!atomic_exchange(&snap_ready, 0)) return;
    plat_mutex_lock(&vlock);
    shown[0] = snap_shown[0], shown[1] = snap_shown[1];
    plat_mutex_unlock(&vlock);
    pair_publish(cb, 1, 4);
}

static void pair_publish(VkCommandBuffer cb, int stereo, int base) {   // img[base..base+1] -> img[2..3] (what the headset shows) + their poses
    const VkPipelineStageFlags2 ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, TR = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    for (int e = 0; e < 2; e++) {
        img_barrier(cb, E.img[base + e], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, TR,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        img_barrier(cb, E.img[2 + e], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL,
                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                          .extent = {E.w, E.h, 1}};
        vkCmdCopyImage(cb, E.img[base + e], VK_IMAGE_LAYOUT_GENERAL, E.img[2 + e], VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        img_barrier(cb, E.img[2 + e], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL,
                    VK_ACCESS_2_MEMORY_READ_BIT);
        img_barrier(cb, E.img[base + e], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, 0, ALL, 0);
        atomic_store(&E.have[2 + e], 1);
        atomic_fetch_add(&eye_gen[e], 1);
    }
    plat_mutex_lock(&vlock);
    pair_shown[0] = shown[0], pair_shown[1] = shown[1];
    const EyeView *nw = &pair_shown[stereo ? 1 : 0];   // reprojection reference: the newest frame on screen
    if (nw->gvalid) game_now.yaw = nw->gyaw, game_now.valid = 1, memcpy(game_now.cam, nw->gcam, 12);
    uint32_t g = ++pgen;
    pring[g % 16][0] = shown[0], pring[g % 16][1] = shown[1];
    plat_mutex_unlock(&vlock);
    if (gbuf) {
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
        VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &d);
        vkCmdUpdateBuffer(cb, gbuf, 0, 4, &g);
    }
}

// ---- the session and its frame loop (VR thread)
static VkCommandPool pool;
static VkCommandBuffer cbs[5];   // the two eyes' blits, the HUD panel's, the aim reticle's, the world markers'
static VkFence fence;

static int make_swapchains(void) {
    XRF(xrEnumerateSwapchainFormats);
    XRF(xrCreateSwapchain);
    XRF(xrEnumerateSwapchainImages);
    XRF(xrEnumerateViewConfigurationViews);
    XrViewConfigurationView v[2] = {{.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}, {.type = XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t nv = 0;
    xrEnumerateViewConfigurationViews(X.inst, X.sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, v);
    // the screen's swapchains: the game frame's size once known, else the headset's recommended eye size
    X.sw = E.w ? E.w : v[0].recommendedImageRectWidth * 2, X.sh = E.h ? E.h : v[0].recommendedImageRectHeight;
    int64_t fm[64];
    uint32_t nf = 0;
    xrEnumerateSwapchainFormats(X.session, 64, &nf, fm);
    int64_t fmt = 0;
    const int64_t want[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
    for (size_t w = 0; w < 4 && !fmt; w++)
        for (uint32_t i = 0; i < nf; i++)
            if (fm[i] == want[w]) { fmt = fm[i]; break; }
    if (!fmt) { rt_log("vr: the runtime offers no 8-bit colour swapchain format"); return 0; }
    for (int e = 0; e < 2; e++) {
        XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                                    .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
                                    .format = fmt, .sampleCount = 1, .width = X.sw, .height = X.sh, .faceCount = 1,
                                    .arraySize = 1, .mipCount = 1};
        if (XR_FAILED(xrCreateSwapchain(X.session, &ci, &X.sc[e]))) { rt_log("vr: xrCreateSwapchain failed"); return 0; }
        XrSwapchainImageVulkanKHR im[8];
        for (int i = 0; i < 8; i++) im[i] = (XrSwapchainImageVulkanKHR){.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
        xrEnumerateSwapchainImages(X.sc[e], 8, &X.sc_n[e], (XrSwapchainImageBaseHeader *)im);
        for (uint32_t i = 0; i < X.sc_n[e]; i++) X.sc_img[e][i] = im[i].image;
    }
    rt_log("vr: two %ux%u swapchains (format %lld), %u images each", X.sw, X.sh, (long long)fmt, X.sc_n[0]);
    return 1;
}

static void q_yaw_pitch(XrQuaternionf q, float *yaw, float *pitch) {
    float bx = 2 * (q.x * q.z + q.w * q.y), by = 2 * (q.y * q.z - q.w * q.x), bz = 1 - 2 * (q.x * q.x + q.y * q.y);
    *yaw = atan2f(bx, bz) * 57.2958f, *pitch = asinf(fmaxf(-1.0f, fminf(1.0f, -by))) * 57.2958f;
}

// ---- the headset's frame clock for the game's pacer (nvn_api_obj.c nvnQueuePresentTexture, frame_rate.c): when the VR
// thread's xrWaitFrame last returned and the headset's frame period. With TOTK_VR_PACE (default on) the game presents
// just before the headset's frames, a pair (two game frames) per headset frame (per second headset frame above 110 Hz),
// and the game's step follows the headset.
static _Atomic uint64_t xr_wake_ns, xr_period_ns;
int vk_xr_frame_clock(uint64_t *wake, uint64_t *period, int *every) {
    if (!X.ok || !atomic_load(&X.active)) return 0;
    if (!cfg_bool("TOTK_VR_PACE")) return 0;
    uint64_t w = atomic_load(&xr_wake_ns), p = atomic_load(&xr_period_ns), now = kern_now_ns();
    if (!w || p < 4000000 || p > 40000000 || now - w > 200000000) return 0;   // no recent headset frame
    *wake = w, *period = p, *every = p < 9000000 ? 2 : 1;
    return 1;
}

// ---- the HUD panel (VR thread): its swapchain, its picture, and where it floats. The panel sits TOTK_VR_HUD_DISTANCE m
// The panel's height / width (the game lays its HUD out in 1920x1080 and stretches it over the square
// headset output, so a square panel squished it sideways; shown at 16:9 it has its own shape again). TOTK_VR_HUD_ASPECT
// (width / height, live, default 16:9; 0 = the picture's own shape).
static float hud_shape(void) {
    double a = cfg_double("TOTK_VR_HUD_ASPECT");
    if (a > 0.1) return (float)(1.0 / a);
    return X.hw ? (float)X.hh / (float)X.hw : 1;
}
// ahead of the head and TOTK_VR_HUD_SIZE m wide, and follows the head lazily: it stays put while the head turns less
// than TOTK_VR_HUD_FOLLOW degrees away from it, then glides back in front (time constant 0.1 s) until nearly centred.
static int hud_swapchain(void) {
    XRF(xrEnumerateSwapchainFormats);
    XRF(xrCreateSwapchain);
    XRF(xrEnumerateSwapchainImages);
    XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                                .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
                                .format = 0, .sampleCount = 1, .width = HL.w, .height = HL.h, .faceCount = 1, .arraySize = 1,
                                .mipCount = 1};
    int64_t fm[64];
    uint32_t nf = 0;
    xrEnumerateSwapchainFormats(X.session, 64, &nf, fm);
    const int64_t want[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB};
    for (size_t w = 0; w < 2 && !ci.format; w++)
        for (uint32_t i = 0; i < nf; i++)
            if (fm[i] == want[w]) { ci.format = fm[i]; break; }
    if (!ci.format) { rt_log("vr: no sRGB swapchain format with alpha for the HUD panel"); return 0; }
    if (XR_FAILED(xrCreateSwapchain(X.session, &ci, &X.hsc))) { rt_log("vr: the HUD panel's swapchain failed"); X.hsc = XR_NULL_HANDLE; return 0; }
    XrSwapchainImageVulkanKHR im[8];
    for (int i = 0; i < 8; i++) im[i] = (XrSwapchainImageVulkanKHR){.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
    xrEnumerateSwapchainImages(X.hsc, 8, &X.hsc_n, (XrSwapchainImageBaseHeader *)im);
    for (uint32_t i = 0; i < X.hsc_n; i++) X.hsc_img[i] = im[i].image;
    X.hw = HL.w, X.hh = HL.h;
    rt_log("vr: HUD panel swapchain %ux%u, %u images", X.hw, X.hh, X.hsc_n);
    return 1;
}
static void hud_upload(VkCtx *c) {   // the newest HUD picture into the panel's swapchain
    XRF(xrAcquireSwapchainImage);
    XRF(xrWaitSwapchainImage);
    XRF(xrReleaseSwapchainImage);
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo aq = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wt = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = 100000000};
    vk_queue_lock();
    xrAcquireSwapchainImage(X.hsc, &aq, &idx);
    vk_queue_unlock();
    xrWaitSwapchainImage(X.hsc, &wt);
    VkCommandBuffer cb = cbs[2];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &cbi);
    VkImage dst = X.hsc_img[idx];
    const VkPipelineStageFlags2 ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, TR = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    img_barrier(cb, HL.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, TR, VK_ACCESS_2_TRANSFER_READ_BIT);
    img_barrier(cb, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ALL, 0, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)HL.w, (int32_t)HL.h, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)X.hw, (int32_t)X.hh, 1}}};
    vkCmdBlitImage(cb, HL.img, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    img_barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL,
                VK_ACCESS_2_MEMORY_READ_BIT);
    img_barrier(cb, HL.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, 0, ALL, 0);
    vkEndCommandBuffer(cb);
    VkCommandBufferSubmitInfo cs = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 sub = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cs};
    vk_queue_lock();
    vkQueueSubmit2(c->gfx_queue, 1, &sub, fence);
    vk_queue_unlock();
    vkWaitForFences(c->device, 1, &fence, VK_TRUE, 1000000000ull);
    vkResetFences(c->device, 1, &fence);
    XrSwapchainImageReleaseInfo rl = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    vk_queue_lock();
    xrReleaseSwapchainImage(X.hsc, &rl);
    vk_queue_unlock();
}
static int ret_swapchain(void) {
    XRF(xrEnumerateSwapchainFormats);
    XRF(xrCreateSwapchain);
    XRF(xrEnumerateSwapchainImages);
    XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                                .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
                                .format = 0, .sampleCount = 1, .width = RTL.w, .height = RTL.h, .faceCount = 1, .arraySize = 1,
                                .mipCount = 1};
    int64_t fm[64];
    uint32_t nf = 0;
    xrEnumerateSwapchainFormats(X.session, 64, &nf, fm);
    const int64_t want[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB};
    for (size_t w = 0; w < 2 && !ci.format; w++)
        for (uint32_t i = 0; i < nf; i++)
            if (fm[i] == want[w]) { ci.format = fm[i]; break; }
    if (!ci.format) { rt_log("vr: no sRGB swapchain format with alpha for the aim reticle"); return 0; }
    if (XR_FAILED(xrCreateSwapchain(X.session, &ci, &X.rsc))) { rt_log("vr: the aim reticle's swapchain failed"); X.rsc = XR_NULL_HANDLE; return 0; }
    XrSwapchainImageVulkanKHR im[8];
    for (int i = 0; i < 8; i++) im[i] = (XrSwapchainImageVulkanKHR){.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
    xrEnumerateSwapchainImages(X.rsc, 8, &X.rsc_n, (XrSwapchainImageBaseHeader *)im);
    for (uint32_t i = 0; i < X.rsc_n; i++) X.rsc_img[i] = im[i].image;
    X.rw = RTL.w, X.rh = RTL.h;
    rt_log("vr: aim reticle swapchain %ux%u, %u images", X.rw, X.rh, X.rsc_n);
    return 1;
}
static void ret_upload(VkCtx *c) {   // the newest HUD picture into the panel's swapchain
    XRF(xrAcquireSwapchainImage);
    XRF(xrWaitSwapchainImage);
    XRF(xrReleaseSwapchainImage);
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo aq = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wt = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = 100000000};
    vk_queue_lock();
    xrAcquireSwapchainImage(X.rsc, &aq, &idx);
    vk_queue_unlock();
    xrWaitSwapchainImage(X.rsc, &wt);
    VkCommandBuffer cb = cbs[3];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &cbi);
    VkImage dst = X.rsc_img[idx];
    const VkPipelineStageFlags2 ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, TR = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    img_barrier(cb, RTL.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, TR, VK_ACCESS_2_TRANSFER_READ_BIT);
    img_barrier(cb, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ALL, 0, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)RTL.w, (int32_t)RTL.h, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)X.rw, (int32_t)X.rh, 1}}};
    vkCmdBlitImage(cb, RTL.img, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    img_barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL,
                VK_ACCESS_2_MEMORY_READ_BIT);
    img_barrier(cb, RTL.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, 0, ALL, 0);
    vkEndCommandBuffer(cb);
    VkCommandBufferSubmitInfo cs = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 sub = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cs};
    vk_queue_lock();
    vkQueueSubmit2(c->gfx_queue, 1, &sub, fence);
    vk_queue_unlock();
    vkWaitForFences(c->device, 1, &fence, VK_TRUE, 1000000000ull);
    vkResetFences(c->device, 1, &fence);
    XrSwapchainImageReleaseInfo rl = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    vk_queue_lock();
    xrReleaseSwapchainImage(X.rsc, &rl);
    vk_queue_unlock();
}
static int wm_swapchain(void) {
    XRF(xrEnumerateSwapchainFormats);
    XRF(xrCreateSwapchain);
    XRF(xrEnumerateSwapchainImages);
    XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                                .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
                                .format = 0, .sampleCount = 1, .width = WTL.w, .height = WTL.h, .faceCount = 1, .arraySize = 1,
                                .mipCount = 1};
    int64_t fm[64];
    uint32_t nf = 0;
    xrEnumerateSwapchainFormats(X.session, 64, &nf, fm);
    const int64_t want[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB};
    for (size_t w = 0; w < 2 && !ci.format; w++)
        for (uint32_t i = 0; i < nf; i++)
            if (fm[i] == want[w]) { ci.format = fm[i]; break; }
    if (!ci.format) { rt_log("vr: no sRGB swapchain format with alpha for the world markers"); return 0; }
    if (XR_FAILED(xrCreateSwapchain(X.session, &ci, &X.wsc))) { rt_log("vr: the world markers's swapchain failed"); X.wsc = XR_NULL_HANDLE; return 0; }
    XrSwapchainImageVulkanKHR im[8];
    for (int i = 0; i < 8; i++) im[i] = (XrSwapchainImageVulkanKHR){.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
    xrEnumerateSwapchainImages(X.wsc, 8, &X.wsc_n, (XrSwapchainImageBaseHeader *)im);
    for (uint32_t i = 0; i < X.wsc_n; i++) X.wsc_img[i] = im[i].image;
    X.ww = WTL.w, X.wh = WTL.h;
    rt_log("vr: world markers swapchain %ux%u, %u images", X.ww, X.wh, X.wsc_n);
    return 1;
}
static void wm_upload(VkCtx *c) {   // the newest HUD picture into the panel's swapchain
    XRF(xrAcquireSwapchainImage);
    XRF(xrWaitSwapchainImage);
    XRF(xrReleaseSwapchainImage);
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo aq = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wt = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = 100000000};
    vk_queue_lock();
    xrAcquireSwapchainImage(X.wsc, &aq, &idx);
    vk_queue_unlock();
    xrWaitSwapchainImage(X.wsc, &wt);
    VkCommandBuffer cb = cbs[4];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &cbi);
    VkImage dst = X.wsc_img[idx];
    const VkPipelineStageFlags2 ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, TR = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    img_barrier(cb, WTL.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL, VK_ACCESS_2_MEMORY_WRITE_BIT, TR, VK_ACCESS_2_TRANSFER_READ_BIT);
    img_barrier(cb, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ALL, 0, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)WTL.w, (int32_t)WTL.h, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)X.ww, (int32_t)X.wh, 1}}};
    vkCmdBlitImage(cb, WTL.img, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    img_barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, TR, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL,
                VK_ACCESS_2_MEMORY_READ_BIT);
    img_barrier(cb, WTL.img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, 0, ALL, 0);
    vkEndCommandBuffer(cb);
    VkCommandBufferSubmitInfo cs = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 sub = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cs};
    vk_queue_lock();
    vkQueueSubmit2(c->gfx_queue, 1, &sub, fence);
    vk_queue_unlock();
    vkWaitForFences(c->device, 1, &fence, VK_TRUE, 1000000000ull);
    vkResetFences(c->device, 1, &fence);
    XrSwapchainImageReleaseInfo rl = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    vk_queue_lock();
    xrReleaseSwapchainImage(X.wsc, &rl);
    vk_queue_unlock();
}
static XrPosef hud_pose(XrPosef head, int64_t now_ns) {
    static int init;
    static float pyaw, ppitch;
    static int gliding;
    static int64_t last;
    float yaw, pitch;
    q_yaw_pitch(head.orientation, &yaw, &pitch);   // degrees: the head's back axis
    float dt = init ? (float)((now_ns - last) / 1e9) : 0;
    last = now_ns;
    if (dt < 0 || dt > 0.5f) dt = 0.5f;
    if (!init) pyaw = yaw, ppitch = pitch, init = 1;
    float dy = remainderf(yaw - pyaw, 360.0f), dp = pitch - ppitch;
    float off = sqrtf(dy * dy + dp * dp);   // near enough to the angle for these sizes
    double follow = cfg_double("TOTK_VR_HUD_FOLLOW");
    if (!(follow >= 0)) follow = 25;
    if (off > follow) gliding = 1;
    int vr_viewfinder_active(void);   // vr_camera.c: the camera / scope is open: the panel sits straight ahead of the head
    if (vr_viewfinder_active()) pyaw = yaw, ppitch = pitch, gliding = 0;
    else if (gliding) {
        float k = 1 - expf(-dt / 0.1f);
        pyaw += dy * k, ppitch += dp * k;
        if (off < 1.5f) gliding = 0;
    }
    float d = (float)cfg_double("TOTK_VR_HUD_DISTANCE");
    if (!(d > 0.2f)) d = 2;
    float ya = pyaw * 0.0174533f, pa = ppitch * 0.0174533f;
    float back[3] = {sinf(ya) * cosf(pa), -sinf(pa), cosf(ya) * cosf(pa)};
    float cy = cosf(ya / 2), sy = sinf(ya / 2), cx = cosf(pa / 2), sx = sinf(pa / 2);
    XrPosef p = {.orientation = {cy * sx, sy * cx, -sy * sx, cy * cx},   // yaw about y, then pitch about x: +z = back
                 .position = {head.position.x - d * back[0], head.position.y - d * back[1], head.position.z - d * back[2]}};
    return p;
}

// ---- the VR controllers as the game's gamepad (TOTK_VR_PAD, default on)
// In a SteamVR session the controllers belong to the OpenXR app, not to SDL, so they are read here as actions and
// handed to input.c (vk_xr_pad), which merges them with the SDL gamepad by position like any pad (PAD_SOUTH is the
// bottom face button, TOTK_PAD_LAYOUT and the remapping rows apply). Steam Frame controllers (their own profile, as
// Nightfall's controller mode reads them): right A/B/X/Y, Menu = Start, bumper = R; left D-pad, View = Back, bumper = L;
// triggers ZL/ZR, sticks and their clicks; grips unused. Touch fallback for other controllers: right A/B, left X/Y,
// grips L/R, left menu Start. The right controller's aim pose drives the six-axis sensor (vk_xr_motion), and rumble goes
// to the controllers (vk_xr_rumble: low to the left, high to the right).
enum { XI_TL = PAD_COUNT, XI_TR, XI_N };
static struct {
    XrActionSet set;
    XrAction b[XI_N];      // PAD_* buttons; XI_TL/XI_TR triggers (float)
    XrAction stick[2], aim, haptic;
    XrSpace aim_space;
    XrPath hand[2];
    int ok;
} XI;
static _Atomic uint32_t xi_buttons;   // bit per PAD_* held
static _Atomic int32_t xi_axes[6];    // sticks lx, ly, rx, ry (SDL range, y up) and triggers (0..32767)
static _Atomic uint64_t xi_ns;        // kern_now_ns of the last sync with input, 0 none
static _Atomic uint64_t xi_mot_ns;    // last motion sample
static _Atomic unsigned xi_mot_seq;   // odd while being written
static double xi_gyro[3], xi_acc[3];

static XrPath xi_path(const char *s) {
    static PFN_xrStringToPath f;
    if (!f) f = (PFN_xrStringToPath)xr_fn("xrStringToPath");
    XrPath p = XR_NULL_PATH;
    if (f) f(X.inst, s, &p);
    return p;
}

static XrAction xi_action(const char *name, XrActionType type, int both_hands) {
    XRF(xrCreateAction);
    XrActionCreateInfo ci = {.type = XR_TYPE_ACTION_CREATE_INFO, .actionType = type,
                             .countSubactionPaths = both_hands ? 2 : 0, .subactionPaths = both_hands ? XI.hand : NULL};
    snprintf(ci.actionName, sizeof ci.actionName, "%s", name);
    snprintf(ci.localizedActionName, sizeof ci.localizedActionName, "%s", name);
    for (char *c = ci.localizedActionName; *c; c++) if (*c == '_') *c = ' ';
    XrAction a = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateAction(XI.set, &ci, &a))) rt_log("vr: controller action %s could not be made", name);
    return a;
}

typedef struct { int act; const char *path; } XiBind;   // act: PAD_*, XI_TL/TR, or -1/-2 sticks, -3 aim, -4/-5 haptic
static int xi_suggest(const char *profile, const XiBind *bl, int n) {
    XRF(xrSuggestInteractionProfileBindings);
    XrActionSuggestedBinding sb[48];
    int k = 0;
    for (int i = 0; i < n && k < 48; i++) {
        XrAction a = bl[i].act >= 0 ? XI.b[bl[i].act] : bl[i].act == -1 ? XI.stick[0] : bl[i].act == -2 ? XI.stick[1]
                   : bl[i].act == -3 ? XI.aim : XI.haptic;
        if (a) sb[k++] = (XrActionSuggestedBinding){a, xi_path(bl[i].path)};
    }
    XrInteractionProfileSuggestedBinding s = {.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
                                              .interactionProfile = xi_path(profile), .countSuggestedBindings = (uint32_t)k,
                                              .suggestedBindings = sb};
    XrResult r = xrSuggestInteractionProfileBindings(X.inst, &s);
    if (XR_FAILED(r)) rt_log("vr: controller bindings for %s refused (%d)", profile, (int)r);
    return XR_SUCCEEDED(r);
}

#define L_ "/user/hand/left/"
#define R_ "/user/hand/right/"
static const XiBind xi_frame[] = {
    {PAD_SOUTH, R_ "input/a/click"}, {PAD_EAST, R_ "input/b/click"}, {PAD_WEST, R_ "input/x/click"},
    {PAD_NORTH, R_ "input/y/click"}, {PAD_DPAD_UP, L_ "input/dpad_up/click"}, {PAD_DPAD_DOWN, L_ "input/dpad_down/click"},
    {PAD_DPAD_LEFT, L_ "input/dpad_left/click"}, {PAD_DPAD_RIGHT, L_ "input/dpad_right/click"},
    {PAD_L, L_ "input/bumper/click"}, {PAD_R, R_ "input/bumper/click"}, {PAD_START, R_ "input/menu/click"},
    {PAD_BACK, L_ "input/view/click"}, {PAD_LSTICK, L_ "input/thumbstick/click"}, {PAD_RSTICK, R_ "input/thumbstick/click"},
    {XI_TL, L_ "input/trigger/value"}, {XI_TR, R_ "input/trigger/value"}, {-1, L_ "input/thumbstick"},
    {-2, R_ "input/thumbstick"}, {-3, R_ "input/aim/pose"}, {-4, L_ "output/haptic"}, {-4, R_ "output/haptic"},
};
static const XiBind xi_touch[] = {
    {PAD_SOUTH, R_ "input/a/click"}, {PAD_EAST, R_ "input/b/click"}, {PAD_WEST, L_ "input/x/click"},
    {PAD_NORTH, L_ "input/y/click"}, {PAD_L, L_ "input/squeeze/value"}, {PAD_R, R_ "input/squeeze/value"},
    {PAD_START, L_ "input/menu/click"}, {PAD_LSTICK, L_ "input/thumbstick/click"}, {PAD_RSTICK, R_ "input/thumbstick/click"},
    {XI_TL, L_ "input/trigger/value"}, {XI_TR, R_ "input/trigger/value"}, {-1, L_ "input/thumbstick"},
    {-2, R_ "input/thumbstick"}, {-3, R_ "input/aim/pose"}, {-4, L_ "output/haptic"}, {-4, R_ "output/haptic"},
};

// After the session is made: actions, bindings, attach. Failures leave the controllers unread (logged).
static void xi_setup(void) {
    if (!cfg_bool("TOTK_VR_PAD")) { rt_log("vr: controllers not read (vr_pad = 0)"); return; }
    XRF(xrCreateActionSet); XRF(xrAttachSessionActionSets); XRF(xrCreateActionSpace);
    if (!xrCreateActionSet || !xrAttachSessionActionSets) return;
    XrActionSetCreateInfo si = {.type = XR_TYPE_ACTION_SET_CREATE_INFO, .actionSetName = "gamepad",
                                .localizedActionSetName = "Gamepad", .priority = 0};
    if (XR_FAILED(xrCreateActionSet(X.inst, &si, &XI.set))) { rt_log("vr: no controller action set"); return; }
    XI.hand[0] = xi_path("/user/hand/left"), XI.hand[1] = xi_path("/user/hand/right");
    static const char *names[XI_N] = {
        [PAD_SOUTH] = "a", [PAD_EAST] = "b", [PAD_WEST] = "x", [PAD_NORTH] = "y", [PAD_LSTICK] = "left_stick_click",
        [PAD_RSTICK] = "right_stick_click", [PAD_L] = "left_bumper", [PAD_R] = "right_bumper", [PAD_START] = "menu",
        [PAD_BACK] = "view", [PAD_DPAD_LEFT] = "dpad_left", [PAD_DPAD_UP] = "dpad_up", [PAD_DPAD_RIGHT] = "dpad_right",
        [PAD_DPAD_DOWN] = "dpad_down", [XI_TL] = "left_trigger", [XI_TR] = "right_trigger"};
    for (int i = 0; i < XI_N; i++)
        if (names[i]) XI.b[i] = xi_action(names[i], i >= XI_TL ? XR_ACTION_TYPE_FLOAT_INPUT : XR_ACTION_TYPE_BOOLEAN_INPUT, 0);
    XI.stick[0] = xi_action("left_stick", XR_ACTION_TYPE_VECTOR2F_INPUT, 0);
    XI.stick[1] = xi_action("right_stick", XR_ACTION_TYPE_VECTOR2F_INPUT, 0);
    XI.aim = xi_action("motion", XR_ACTION_TYPE_POSE_INPUT, 0);
    XI.haptic = xi_action("rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT, 1);
    int any = 0;
    if (X.frame_ext) any |= xi_suggest("/interaction_profiles/valve/frame_controller_valve", xi_frame,
                                       (int)(sizeof xi_frame / sizeof *xi_frame));
    any |= xi_suggest("/interaction_profiles/oculus/touch_controller", xi_touch, (int)(sizeof xi_touch / sizeof *xi_touch));
    if (!any) return;
    XrSessionActionSetsAttachInfo ai = {.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = 1,
                                        .actionSets = &XI.set};
    XrResult r = xrAttachSessionActionSets(X.session, &ai);
    if (XR_FAILED(r)) { rt_log("vr: controller actions not attached (%d)", (int)r); return; }
    XrActionSpaceCreateInfo asi = {.type = XR_TYPE_ACTION_SPACE_CREATE_INFO, .action = XI.aim,
                                   .poseInActionSpace = {.orientation = {0, 0, 0, 1}}};
    if (XI.aim) xrCreateActionSpace(X.session, &asi, &XI.aim_space);
    XI.ok = 1;
    rt_log("vr: controllers read as the gamepad (%s bindings)", X.frame_ext ? "Steam Frame + Touch" : "Touch");
}

// Logs the profile SteamVR picked for a hand (after an interaction profile change event).
static void xi_profile_log(void) {
    if (!XI.ok) return;
    XRF(xrGetCurrentInteractionProfile); XRF(xrPathToString);
    for (int h = 0; h < 2; h++) {
        XrInteractionProfileState st = {.type = XR_TYPE_INTERACTION_PROFILE_STATE};
        char buf[256] = "none";
        uint32_t n = 0;
        if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(X.session, XI.hand[h], &st)) && st.interactionProfile)
            xrPathToString(X.inst, st.interactionProfile, sizeof buf, &n, buf);
        rt_log("vr: %s controller profile: %s", h ? "right" : "left", buf);
    }
}

// Once per headset frame (VR thread): sync, read every action, and the right controller's motion at that frame's time.
static void xi_sync(XrTime t) {
    if (!XI.ok) return;
    static PFN_xrSyncActions sync;
    static PFN_xrGetActionStateBoolean gb;
    static PFN_xrGetActionStateFloat gf;
    static PFN_xrGetActionStateVector2f gv;
    static PFN_xrLocateSpace loc;
    if (!sync) {
        sync = (PFN_xrSyncActions)xr_fn("xrSyncActions"), gb = (PFN_xrGetActionStateBoolean)xr_fn("xrGetActionStateBoolean");
        gf = (PFN_xrGetActionStateFloat)xr_fn("xrGetActionStateFloat");
        gv = (PFN_xrGetActionStateVector2f)xr_fn("xrGetActionStateVector2f"), loc = (PFN_xrLocateSpace)xr_fn("xrLocateSpace");
        if (!sync || !gb || !gf || !gv || !loc) { XI.ok = 0; return; }
    }
    XrActiveActionSet as = {XI.set, XR_NULL_PATH};
    XrActionsSyncInfo si = {.type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 1, .activeActionSets = &as};
    XrResult r = sync(X.session, &si);
    if (r != XR_SUCCESS) {   // not focused (SteamVR's dashboard open): nothing held
        atomic_store(&xi_ns, 0);
        return;
    }
    uint32_t held = 0;
    int32_t ax[6] = {0};
    int live = 0;
    for (int i = 0; i < XI_N; i++) {
        if (!XI.b[i]) continue;
        XrActionStateGetInfo gi = {.type = XR_TYPE_ACTION_STATE_GET_INFO, .action = XI.b[i]};
        if (i >= XI_TL) {
            XrActionStateFloat st = {.type = XR_TYPE_ACTION_STATE_FLOAT};
            if (XR_SUCCEEDED(gf(X.session, &gi, &st)) && st.isActive)
                ax[4 + i - XI_TL] = (int32_t)(fminf(fmaxf(st.currentState, 0), 1) * 32767), live = 1;
        } else {
            XrActionStateBoolean st = {.type = XR_TYPE_ACTION_STATE_BOOLEAN};
            if (XR_SUCCEEDED(gb(X.session, &gi, &st)) && st.isActive) {
                live = 1;
                if (st.currentState) held |= 1u << i;
            }
        }
    }
    for (int s = 0; s < 2; s++) {
        XrActionStateGetInfo gi = {.type = XR_TYPE_ACTION_STATE_GET_INFO, .action = XI.stick[s]};
        XrActionStateVector2f st = {.type = XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XI.stick[s] && XR_SUCCEEDED(gv(X.session, &gi, &st)) && st.isActive) {
            ax[2 * s] = (int32_t)(fminf(fmaxf(st.currentState.x, -1), 1) * 32767);
            ax[2 * s + 1] = (int32_t)(fminf(fmaxf(st.currentState.y, -1), 1) * 32767);
            live = 1;
        }
    }
    static int logged, changes;
    static uint32_t was;
    if (live && !logged) logged = 1, rt_log("vr: controller input arriving");
    if (held != was && changes < 40) changes++, rt_log("vr: controller buttons held %#x", held);   // the first few
    was = held;
    atomic_store(&xi_buttons, held);
    for (int i = 0; i < 6; i++) atomic_store(&xi_axes[i], ax[i]);
    atomic_store(&xi_ns, live ? kern_now_ns() : 0);
    // Motion: the aim pose's angular velocity and gravity in the controller's frame (OpenXR aim: x right, y up, -z
    // forward), then into the Switch controller's (x right, y forward, z up out of the face): (x, -z, y), as the
    // frame-gyro service did. Acceleration as the Switch reads it: gravity's direction in G.
    if (!XI.aim_space) return;
    XrSpaceVelocity vel = {.type = XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation sl = {.type = XR_TYPE_SPACE_LOCATION, .next = &vel};
    if (XR_FAILED(loc(XI.aim_space, X.space, t, &sl)) || !(sl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) ||
        !(vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT))
        return;
    XrQuaternionf q = sl.pose.orientation;
    // R (device -> world) columns; world -> device = R^T
    float x = q.x, y = q.y, z = q.z, w = q.w;
    float R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                     {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                     {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    float wv[3] = {vel.angularVelocity.x, vel.angularVelocity.y, vel.angularVelocity.z}, wd[3], gd[3];
    for (int i = 0; i < 3; i++) wd[i] = R[0][i] * wv[0] + R[1][i] * wv[1] + R[2][i] * wv[2], gd[i] = -R[1][i];
    int inv = cfg_bool("TOTK_VR_GYRO_INVERT");   // pitch the other way
    atomic_fetch_add(&xi_mot_seq, 1);
    xi_gyro[0] = inv ? -wd[0] : wd[0], xi_gyro[1] = -wd[2], xi_gyro[2] = wd[1];
    xi_acc[0] = gd[0], xi_acc[1] = -gd[2], xi_acc[2] = gd[1];
    atomic_fetch_add(&xi_mot_seq, 1);
    atomic_store(&xi_mot_ns, kern_now_ns());
}

int vk_xr_pad(uint8_t pd[], int32_t ax[6]) {
    uint64_t t = atomic_load(&xi_ns);
    if (!t || kern_now_ns() - t > 100000000ull) return 0;
    uint32_t held = atomic_load(&xi_buttons);
    for (int i = 1; i < PAD_COUNT; i++) if (held >> i & 1) pd[i] = 1;
    for (int i = 0; i < 6; i++) ax[i] = atomic_load(&xi_axes[i]);
    return 1;
}

int vk_xr_motion(double acc[3], double gyro[3]) {
    uint64_t t = atomic_load(&xi_mot_ns);
    if (!t || kern_now_ns() - t > 100000000ull) return 0;
    for (int tries = 0; tries < 4; tries++) {
        unsigned s = atomic_load(&xi_mot_seq);
        if (s & 1) continue;
        for (int i = 0; i < 3; i++) acc[i] = xi_acc[i], gyro[i] = xi_gyro[i];
        if (atomic_load(&xi_mot_seq) == s) return 1;
    }
    return 0;
}

void vk_xr_rumble(uint16_t low, uint16_t high, uint32_t ms) {
    if (!XI.ok || !XI.haptic || !atomic_load(&X.active)) return;
    static PFN_xrApplyHapticFeedback apply;
    static PFN_xrStopHapticFeedback stop;
    static int looked;
    if (!looked) looked = 1, apply = (PFN_xrApplyHapticFeedback)xr_fn("xrApplyHapticFeedback"), stop = (PFN_xrStopHapticFeedback)xr_fn("xrStopHapticFeedback");
    if (!apply || !stop) return;
    uint16_t v[2] = {low, high};
    for (int h = 0; h < 2; h++) {
        XrHapticActionInfo hi = {.type = XR_TYPE_HAPTIC_ACTION_INFO, .action = XI.haptic, .subactionPath = XI.hand[h]};
        if (!v[h] || !ms) { stop(X.session, &hi); continue; }
        XrHapticVibration vib = {.type = XR_TYPE_HAPTIC_VIBRATION, .duration = (XrDuration)ms * 1000000,
                                 .frequency = XR_FREQUENCY_UNSPECIFIED, .amplitude = v[h] / 65535.0f};
        apply(X.session, &hi, (const XrHapticBaseHeader *)&vib);
    }
}

static void xr_thread_run(void);
static void xr_thread(void *arg) {
    (void)arg;
    xr_thread_run();
    atomic_store(&X.active, 0);
    atomic_store(&X.done, 1);
}
static void xr_thread_run(void) {
    VkCtx *c = vk_ctx();
    XRF(xrGetVulkanGraphicsRequirementsKHR);
    XrGraphicsRequirementsVulkanKHR req = {.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    xrGetVulkanGraphicsRequirementsKHR(X.inst, X.sys, &req);
    XrGraphicsBindingVulkanKHR gb = {.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, .instance = c->instance,
                                     .physicalDevice = c->phys, .device = c->device, .queueFamilyIndex = c->gfx_family,
                                     .queueIndex = 0};
    XrSessionCreateInfo si = {.type = XR_TYPE_SESSION_CREATE_INFO, .next = &gb, .systemId = X.sys};
    XRF(xrCreateSession);
    vk_queue_lock();
    XrResult r = xrCreateSession(X.inst, &si, &X.session);
    vk_queue_unlock();
    if (XR_FAILED(r)) { rt_log("vr: xrCreateSession failed (%d)", (int)r); return; }
    XRF(xrCreateReferenceSpace);
    XrReferenceSpaceCreateInfo rs = {.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
                                     .poseInReferenceSpace = {.orientation = {0, 0, 0, 1}}};
    xrCreateReferenceSpace(X.session, &rs, &X.space);
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = c->gfx_family};
    vkCreateCommandPool(c->device, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 5};
    vkAllocateCommandBuffers(c->device, &ai, cbs);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(c->device, &fi, NULL, &fence);
    rt_log("vr: session created; waiting for SteamVR to start it");
    xi_setup();

    XRF(xrPollEvent); XRF(xrBeginSession); XRF(xrEndSession); XRF(xrWaitFrame); XRF(xrBeginFrame); XRF(xrEndFrame);
    XRF(xrAcquireSwapchainImage); XRF(xrWaitSwapchainImage); XRF(xrReleaseSwapchainImage); XRF(xrLocateViews);
    XRF(xrDestroySwapchain); XRF(xrRequestExitSession); XRF(xrDestroySession);
    uint64_t frames = 0;
    unsigned hud_gen = 0, ret_gen = 0, wm_gen = 0;
    int hud_failed = 0, ret_failed = 0, wm_failed = 0;
    float corr_sum = 0, corr_max = 0;
    unsigned corr_n = 0, race = 0;
    // shouldRender fallback: SteamVR can hold a session at SYNCHRONIZED with shouldRender 0 for good (a black headset,
    // seen with a Valve Index on NVIDIA), while layers submitted anyway show correctly. shouldRender is a hint:
    // submitting layers anyway is allowed. TOTK_VR_FORCE_RENDER: -1 (default) = render once the session has been running
    // for 3 s without ever reaching VISIBLE or getting shouldRender; 1 = always; 0 = never (the spec's hint only).
    XrSessionState xr_state = XR_SESSION_STATE_UNKNOWN;
    int64_t xr_run_t0 = 0;
    int xr_got_render = 0, xr_forced_logged = 0;
    for (;;) {
        // vk_xr_shutdown (the game is quitting): ask SteamVR to end the session and go on with frames until it has
        // stopped, then destroy it below; never more than 1.5 s
        int quit = atomic_load(&X.quit);
        if (quit == 1) {
            atomic_store(&X.quit, 2), atomic_store(&X.active, 0);
            if (!X.running || !xrRequestExitSession || XR_FAILED(xrRequestExitSession(X.session))) break;
            rt_log("vr: quitting: asked SteamVR to end the session");
        } else if (quit == 2 && (kern_now_ns() - X.quit_t0 > 1500000000ll || !X.running)) break;
        XrEventDataBuffer ev = {.type = XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(X.inst, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) xi_profile_log();
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                XrSessionState st = ((XrEventDataSessionStateChanged *)&ev)->state;
                rt_log("vr: session state %d", (int)st);
                xr_state = st;
                if (st == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi = {.type = XR_TYPE_SESSION_BEGIN_INFO,
                                             .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
                    if (XR_SUCCEEDED(xrBeginSession(X.session, &bi))) X.running = 1, atomic_store(&X.active, 1);
                } else if (st == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(X.session);
                    X.running = 0, atomic_store(&X.active, 0);
                } else if (st == XR_SESSION_STATE_EXITING || st == XR_SESSION_STATE_LOSS_PENDING) {
                    rt_log("vr: SteamVR ended the session");
                    atomic_store(&X.active, 0);
                    goto end;
                }
            }
            ev = (XrEventDataBuffer){.type = XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!X.running) {
            if (atomic_load(&X.quit)) break;
            plat_sleep_ms(20);
            continue;
        }
        if (!X.sc[0] && E.w) {   // swapchains once the game frame's size is known
            if (!make_swapchains()) goto end;
        }
        XrFrameState fs = {.type = XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo wi = {.type = XR_TYPE_FRAME_WAIT_INFO};
        if (XR_FAILED(xrWaitFrame(X.session, &wi, &fs))) continue;
        atomic_store(&xr_period_ns, (uint64_t)fs.predictedDisplayPeriod), atomic_store(&xr_wake_ns, kern_now_ns());
        xi_sync(fs.predictedDisplayTime);
        {
            // the views at the next headset frame: the frame's own head pose, which SteamVR reprojects from when it shows it
            XrViewLocateInfo li = {.type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                   .displayTime = fs.predictedDisplayTime, .space = X.space};
            XrViewState vs = {.type = XR_TYPE_VIEW_STATE};
            XrView v[2] = {{.type = XR_TYPE_VIEW}, {.type = XR_TYPE_VIEW}};
            uint32_t nv = 0;
            if (XR_SUCCEEDED(xrLocateViews(X.session, &li, &vs, 2, &nv, v)) && nv == 2 &&
                (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
                publish_views(v, fs.predictedDisplayTime);
        }
        XrFrameBeginInfo fbi = {.type = XR_TYPE_FRAME_BEGIN_INFO};
        vk_queue_lock();
        xrBeginFrame(X.session, &fbi);
        vk_queue_unlock();
        XrCompositionLayerProjectionView pv[2];
        XrCompositionLayerProjection pl;
        const XrCompositionLayerBaseHeader *layers[16];
        XrCompositionLayerQuad wm_layers[WM_MAX];
        XrCompositionLayerQuad rq_layer, wq_layer;
        XrPosef seen_left = {.orientation = {0, 0, 0, 1}};
        int seen_left_ok = 0;
        float seen_tan[2] = {1, 1};
        XrCompositionLayerQuad hq_layer;
        uint32_t nl = 0;
        int render = fs.shouldRender;
        {
            static int fr = -2;
            if (fr == -2) { const char *e = cfg_get("TOTK_VR_FORCE_RENDER"); fr = e ? atoi(e) : -1; }
            if (fs.shouldRender || xr_state >= XR_SESSION_STATE_VISIBLE) xr_got_render = 1;
            if (!xr_run_t0) xr_run_t0 = kern_now_ns();
            if (!render && (fr == 1 || (fr == -1 && !xr_got_render && kern_now_ns() - xr_run_t0 > 3000000000ll))) {
                render = 1;
                if (!xr_forced_logged) xr_forced_logged = 1, rt_log("vr: SteamVR has not asked for frames (session state %d, shouldRender 0): rendering anyway (TOTK_VR_FORCE_RENDER=0 to stop)", (int)xr_state);
            }
        }
        if (render && X.sc[0] && E.w == X.sw && E.h == X.sh && atomic_load(&E.have[2])) {
            // Both eyes are copied in one submission: a pair the game publishes meanwhile (vk_xr_capture, same queue)
            // lands before or after both, never between them.
            uint32_t idxs[2] = {0, 0};
            int srcEs[2];
            const VkPipelineStageFlags2 ALL = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, TR = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            for (int e = 0; e < 2; e++) {
                srcEs[e] = atomic_load(&E.have[2 + e]) ? e : 0;
                XrSwapchainImageAcquireInfo aq = {.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                XrSwapchainImageWaitInfo wt = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = 100000000};
                vk_queue_lock();
                xrAcquireSwapchainImage(X.sc[e], &aq, &idxs[e]);
                vk_queue_unlock();
                xrWaitSwapchainImage(X.sc[e], &wt);
            }
            {
                VkCommandBuffer cb = cbs[0];
                vkResetCommandBuffer(cb, 0);
                VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
                vkBeginCommandBuffer(cb, &cbi);
                for (int e = 0; e < 2; e++) {
                    VkImage srcImg = E.img[2 + srcEs[e]];   // the pair
                    VkImage dst = X.sc_img[e][idxs[e]];
                    img_barrier(cb, srcImg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, ALL,
                                VK_ACCESS_2_MEMORY_WRITE_BIT, TR, VK_ACCESS_2_TRANSFER_READ_BIT);
                    img_barrier(cb, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ALL, 0, TR,
                                VK_ACCESS_2_TRANSFER_WRITE_BIT);
                    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                      .srcOffsets = {{0, 0, 0}, {(int32_t)E.w, (int32_t)E.h, 1}},
                                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                      .dstOffsets = {{0, 0, 0}, {(int32_t)X.sw, (int32_t)X.sh, 1}}};
                    vkCmdBlitImage(cb, srcImg, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
                    img_barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, TR,
                                VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL, VK_ACCESS_2_MEMORY_READ_BIT);
                    img_barrier(cb, srcImg, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, TR, 0, ALL, 0);
                    if (gbuf) {   // the number of the pair whose pictures were just read
                        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = ALL,
                                               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = TR,
                                               .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
                        VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
                        vkCmdPipelineBarrier2(cb, &d);
                        VkBufferCopy bc = {.srcOffset = 0, .dstOffset = 4 + 4 * (VkDeviceSize)e, .size = 4};
                        vkCmdCopyBuffer(cb, gbuf, gbuf, 1, &bc);
                    }
                }
                vkEndCommandBuffer(cb);
                VkCommandBufferSubmitInfo cs = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
                VkSubmitInfo2 sub = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1,
                                     .pCommandBufferInfos = &cs};
                vk_queue_lock();
                vkQueueSubmit2(c->gfx_queue, 1, &sub, fence);
                vk_queue_unlock();
                vkWaitForFences(c->device, 1, &fence, VK_TRUE, 1000000000ull);
                vkResetFences(c->device, 1, &fence);
                for (int e = 0; e < 2; e++) {
                    XrSwapchainImageReleaseInfo rl = {.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    vk_queue_lock();
                    xrReleaseSwapchainImage(X.sc[e], &rl);
                    vk_queue_unlock();
                }
            }
            for (int e = 0; e < 2; e++) {
                int srcE = srcEs[e];
                plat_mutex_lock(&vlock);
                EyeView sv = pair_shown[srcE];
                uint32_t gg = gbuf ? gw[1 + e] : 0;
                if (gg) {   // the pose of the pictures actually sent
                    if (gg != pgen) race++;
                    sv = pring[gg % 16][srcE];
                    const EyeView *nw = &pring[gg % 16][1];
                    if (nw->gvalid) game_now.yaw = nw->gyaw, game_now.valid = 1, memcpy(game_now.cam, nw->gcam, 12);
                }
                XrPosef seen = seen_now(&sv);
                XrQuaternionf hq = head_pose.orientation;
                plat_mutex_unlock(&vlock);
                if (!sv.valid) continue;
                {   // SteamVR's correction for this eye: angle between the drawn pose and the head now
                    float dq = fabsf(seen.orientation.x * hq.x + seen.orientation.y * hq.y + seen.orientation.z * hq.z +
                                     seen.orientation.w * hq.w);
                    float ang = 2 * acosf(fminf(1.0f, dq)) * 57.2958f;
                    corr_sum += ang, corr_n++;
                    if (ang > corr_max) corr_max = ang;
                }
                if (e == 0) seen_left = seen, seen_left_ok = 1, seen_tan[0] = sv.tan[1], seen_tan[1] = sv.tan[2];   // (world markers)
                pv[e] = (XrCompositionLayerProjectionView){
                    .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, .pose = seen,
                    .fov = {atanf(sv.tan[0]), atanf(sv.tan[1]), atanf(sv.tan[2]), atanf(sv.tan[3])},
                    .subImage = {.swapchain = X.sc[e], .imageRect = {{0, 0}, {(int32_t)X.sw, (int32_t)X.sh}}}};
            }
            {
                plat_mutex_lock(&vlock);
                int ok = pair_shown[0].valid && pair_shown[1].valid;
                plat_mutex_unlock(&vlock);
                // A movie on the panel: the frozen world around it is left out (the panel shows in the dark, like a
                // cinema screen). TOTK_VR_MOVIE_DARK=0: the last pair stays around it.
                int movie_playing(void);   // ov_movie.c
                static int mdark = -1;
                if (mdark < 0) { const char *e = cfg_get("TOTK_VR_MOVIE_DARK"); mdark = !(e && *e == '0'); }
                int dark = mdark && atomic_load(&menu_now) && movie_playing();
                {
                    static int was;
                    if (dark != was) was = dark, rt_log("vr: %s", dark ? "a movie plays: shown on the panel, the world dark around it" : "movie over: the world is back");
                }
                if (ok && !dark) {
                    pl = (XrCompositionLayerProjection){.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = X.space,
                                                        .viewCount = 2, .views = pv};
                    layers[nl++] = (const XrCompositionLayerBaseHeader *)&pl;
                }
                if ((ok || atomic_load(&menu_now)) && atomic_load(&HL.have) && HL.img) {   // the HUD panel over the world (a menu: also before any 3D picture)
                    if (X.hsc && (X.hw != HL.w || X.hh != HL.h)) xrDestroySwapchain(X.hsc), X.hsc = XR_NULL_HANDLE, hud_gen = 0;
                    if (!X.hsc && !hud_failed) hud_failed = !hud_swapchain();
                    if (X.hsc) {
                        unsigned g = atomic_load(&HL.gen);
                        if (g != hud_gen) hud_upload(c), hud_gen = g;
                        plat_mutex_lock(&vlock);
                        XrPosef hp = head_pose;
                        plat_mutex_unlock(&vlock);
                        float size = (float)cfg_double("TOTK_VR_HUD_SIZE");
                        if (!(size > 0.1f)) size = 2;
                        hq_layer = (XrCompositionLayerQuad){.type = XR_TYPE_COMPOSITION_LAYER_QUAD,
                                                            .layerFlags = atomic_load(&menu_now) ? 0 : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                                                            .space = X.space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
                                                            .subImage = {.swapchain = X.hsc, .imageRect = {{0, 0}, {(int32_t)X.hw, (int32_t)X.hh}}},
                                                            .pose = hud_pose(hp, (int64_t)kern_now_ns()),
                                                            .size = {size, size * hud_shape()}};
                        layers[nl++] = (const XrCompositionLayerBaseHeader *)&hq_layer;
                    }
                }
                float ga[9];
                if (ok && atomic_load(&RTL.have) && RTL.img && vr_game_axes(ga)) {   // the aim reticle in the world
                    if (X.rsc && (X.rw != RTL.w || X.rh != RTL.h)) xrDestroySwapchain(X.rsc), X.rsc = XR_NULL_HANDLE, ret_gen = 0;
                    if (!X.rsc && !ret_failed) ret_failed = !ret_swapchain();
                    if (X.rsc) {
                        unsigned g = atomic_load(&RTL.gen);
                        if (g != ret_gen) ret_upload(c), ret_gen = g;
                        // the layer covers the game view (it is the HUD's picture of it) at the aim distance, centred on
                        // the game camera's aim axis and turned like the game camera: a reticle point lies in the direction
                        // the game aims through that point
                        // placed from the head along the aim axis it landed metres short of the target
                        // (the level lock keeps the VR camera ~5 m behind Link, the bow / Ultrahand camera is ~1 m
                        // behind him): the plane now starts at the game camera (seen from the VR camera; the world is
                        // anchored at the head, eye pictures are drawn from the VR camera and sent with the head's
                        // position), so each reticle point lies on the game's own aim ray through it at the hit distance
                        float sc = (float)cfg_double("TOTK_VR_SCALE");
                        if (!(sc > 0.01f)) sc = 1;
                        float d = reticle_dist / sc, off[3];
                        vr_aim_offset(off);
                        plat_mutex_lock(&vlock);
                        float th = located[0].tan[1], tv = located[0].tan[2];
                        XrVector3f eye = head_pose.position;
                        XrQuaternionf hq = head_pose.orientation;
                        plat_mutex_unlock(&vlock);
                        if (!(th > 0.1f)) th = tv = 1.2f;
                        // Head mode: Ultrahand / Recall pick their target along the head's view
                        // centre (the game derives its screen from the viewport record, which holds the head copy), so their
                        // reticle goes there too: straight ahead of the head at the depth the picture shows there. While ZR
                        // or R is held (bow, throw: aimed along the game camera) it stays on the game camera's aim axis.
                        // TOTK_VR_RETHEAD=0: always the game camera.
                        static int rh = -1;
                        if (rh < 0) { const char *e = cfg_get("TOTK_VR_RETHEAD"); rh = !(e && *e == '0'); }
                        uint64_t input_buttons_last(void);
                        int vr_head_aim_active(void);   // vr_camera.c: bow / throw aimed with the head (TOTK_VR_HEAD_AIM)
                        if (rh && (!(input_buttons_last() & ((1ull << 9) | (1ull << 7))) || vr_head_aim_active())) {   // B_ZR = 9, B_R = 7 (input_map.h)
                            float vk_contact_depth_min_aim(float u0, float v0, float u1, float v1);
                            float hd = vk_contact_depth_min_aim(0.47f, 0.47f, 0.53f, 0.53f);   // (without Link)
                            static float hds;
                            if (hd > 0.3f) hds = hds > 0 ? hds + 0.25f * (hd - hds) : hd;
                            float dh = (hds > 0 ? hds : reticle_dist) / sc;
                            float fx = -2 * (hq.x * hq.z + hq.w * hq.y), fy = -2 * (hq.y * hq.z - hq.w * hq.x), fz = -(1 - 2 * (hq.x * hq.x + hq.y * hq.y));
                            // the bow / throw: the game's aim ray starts at its camera (off from the eyes: the VR camera's
                            // height setting puts it up to metres above), in the head's direction: the reticle sits where that
                            // ray meets the scene (aim tracker, vk_contact.c), not along the eyes' own ray
                            float rx = fx * dh, ry = fy * dh, rz = fz * dh;
                            if (vr_head_aim_active()) {
                                float da = reticle_dist / sc;
                                rx = off[0] + fx * da, ry = off[1] + fy * da, rz = off[2] + fz * da;
                            }
                            float rl = sqrtf(rx * rx + ry * ry + rz * rz);
                            if (!(rl > 0.1f)) rl = dh;
                            static unsigned nlr;
                            if (nlr < 3) nlr++, rt_log("vr: aim reticle follows the head (Ultrahand / Recall), at %.2f m", dh);
                            rq_layer = (XrCompositionLayerQuad){.type = XR_TYPE_COMPOSITION_LAYER_QUAD,
                                                                .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                                                                .space = X.space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
                                                                .subImage = {.swapchain = X.rsc, .imageRect = {{0, 0}, {(int32_t)X.rw, (int32_t)X.rh}}},
                                                                .pose = {.orientation = hq, .position = {eye.x + rx, eye.y + ry, eye.z + rz}},
                                                                .size = {2 * rl * th * marker_scale(rl), 2 * rl * tv * marker_scale(rl)}};
                            layers[nl++] = (const XrCompositionLayerBaseHeader *)&rq_layer;
                            goto reticle_done;
                        }
                        float v[3] = {off[0] - ga[6] * d, off[1] - ga[7] * d, off[2] - ga[8] * d};
                        // rotation matrix with columns right, up, back -> quaternion
                        float m00 = ga[0], m10 = ga[1], m20 = ga[2], m01 = ga[3], m11 = ga[4], m21 = ga[5], m02 = ga[6], m12 = ga[7], m22 = ga[8];
                        float tr = m00 + m11 + m22, qw, qx, qy, qz;
                        if (tr > 0) { float k = sqrtf(tr + 1) * 2; qw = 0.25f * k, qx = (m21 - m12) / k, qy = (m02 - m20) / k, qz = (m10 - m01) / k; }
                        else if (m00 > m11 && m00 > m22) { float k = sqrtf(1 + m00 - m11 - m22) * 2; qw = (m21 - m12) / k, qx = 0.25f * k, qy = (m01 + m10) / k, qz = (m02 + m20) / k; }
                        else if (m11 > m22) { float k = sqrtf(1 + m11 - m00 - m22) * 2; qw = (m02 - m20) / k, qx = (m01 + m10) / k, qy = 0.25f * k, qz = (m12 + m21) / k; }
                        else { float k = sqrtf(1 + m22 - m00 - m11) * 2; qw = (m10 - m01) / k, qx = (m02 + m20) / k, qy = (m12 + m21) / k, qz = 0.25f * k; }
                        rq_layer = (XrCompositionLayerQuad){.type = XR_TYPE_COMPOSITION_LAYER_QUAD,
                                                            .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                                                            .space = X.space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
                                                            .subImage = {.swapchain = X.rsc, .imageRect = {{0, 0}, {(int32_t)X.rw, (int32_t)X.rh}}},
                                                            .pose = {.orientation = {qx, qy, qz, qw}, .position = {eye.x + v[0], eye.y + v[1], eye.z + v[2]}},
                                                            .size = {2 * d * th * marker_scale(d), 2 * d * tv * marker_scale(d)}};
                        layers[nl++] = (const XrCompositionLayerBaseHeader *)&rq_layer;
                    }
                }
reticle_done:
                if (ok && seen_left_ok && atomic_load(&WTL.have) && WTL.img) {   // world markers over the frame's own view
                    if (X.wsc && (X.ww != WTL.w || X.wh != WTL.h)) xrDestroySwapchain(X.wsc), X.wsc = XR_NULL_HANDLE, wm_gen = 0;
                    if (!X.wsc && !wm_failed) wm_failed = !wm_swapchain();
                    if (X.wsc) {
                        unsigned g = atomic_load(&WTL.gen);
                        if (g != wm_gen) wm_upload(c), wm_gen = g;
                        // the picture covers the view the left eye's picture was drawn with (same pose, same field of
                        // view), at a fixed depth (TOTK_VR_WORLDHUD_DIST, m): each marker on what it marks
                        double dd = cfg_double("TOTK_VR_WORLDHUD_DIST");
                        float d = dd > 0.3 && dd < 100 ? (float)dd : 4.0f;
                        XrQuaternionf q = seen_left.orientation;
                        float fx = -2 * (q.x * q.z + q.w * q.y), fy = -2 * (q.y * q.z - q.w * q.x), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
                        int nm = atomic_load(&WM_ok) && cfg_bool("TOTK_VR_MARKERS3D") ? WM_list[atomic_load(&WM_cur)].n : -1;
                        if (nm >= 0) {   // each marker its own quad at its own depth
                            const typeof(WM_list[0]) *L = &WM_list[atomic_load(&WM_cur)];
                            float rx = 1 - 2 * (q.y * q.y + q.z * q.z), ry = 2 * (q.x * q.y + q.w * q.z), rz = 2 * (q.x * q.z - q.w * q.y);
                            float ux = 2 * (q.x * q.y - q.w * q.z), uy = 1 - 2 * (q.x * q.x + q.z * q.z), uz = 2 * (q.y * q.z + q.w * q.x);
                            for (int i = 0; i < nm && i < WM_MAX; i++) {
                                const float *rr = L->r[i];
                                float md = L->d[i], k = marker_scale(md);
                                float cx = (rr[0] + rr[2]) * 0.5f, cy = (rr[1] + rr[3]) * 0.5f;
                                float tx = (2 * cx - 1) * seen_tan[0], ty = (1 - 2 * cy) * seen_tan[1];
                                int32_t px0 = (int32_t)(rr[0] * X.ww) - 2, py0 = (int32_t)(rr[1] * X.wh) - 2, px1 = (int32_t)(rr[2] * X.ww) + 2, py1 = (int32_t)(rr[3] * X.wh) + 2;
                                if (px0 < 0) px0 = 0;
                                if (py0 < 0) py0 = 0;
                                if (px1 > (int32_t)X.ww) px1 = (int32_t)X.ww;
                                if (py1 > (int32_t)X.wh) py1 = (int32_t)X.wh;
                                float sw = (float)(px1 - px0) / X.ww * 2 * seen_tan[0], sh = (float)(py1 - py0) / X.wh * 2 * seen_tan[1];
                                wm_layers[i] = (XrCompositionLayerQuad){.type = XR_TYPE_COMPOSITION_LAYER_QUAD,
                                                                        .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                                                                        .space = X.space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
                                                                        .subImage = {.swapchain = X.wsc, .imageRect = {{px0, py0}, {px1 - px0, py1 - py0}}},
                                                                        .pose = {.orientation = q, .position = {seen_left.position.x + (rx * tx + ux * ty + fx) * md,
                                                                                                               seen_left.position.y + (ry * tx + uy * ty + fy) * md,
                                                                                                               seen_left.position.z + (rz * tx + uz * ty + fz) * md}},
                                                                        .size = {sw * md * k, sh * md * k}};
                                if (nl < (int)(sizeof layers / sizeof layers[0])) layers[nl++] = (const XrCompositionLayerBaseHeader *)&wm_layers[i];
                            }
                        } else {
                        wq_layer = (XrCompositionLayerQuad){.type = XR_TYPE_COMPOSITION_LAYER_QUAD,
                                                            .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
                                                            .space = X.space, .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
                                                            .subImage = {.swapchain = X.wsc, .imageRect = {{0, 0}, {(int32_t)X.ww, (int32_t)X.wh}}},
                                                            .pose = {.orientation = q, .position = {seen_left.position.x + fx * d, seen_left.position.y + fy * d,
                                                                                                   seen_left.position.z + fz * d}},
                                                            .size = {2 * d * seen_tan[0], 2 * d * seen_tan[1]}};
                        layers[nl++] = (const XrCompositionLayerBaseHeader *)&wq_layer;
                        }
                    }
                }
            }
        }
        XrFrameEndInfo ei = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = fs.predictedDisplayTime,
                             .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE, .layerCount = nl, .layers = layers};
        vk_queue_lock();
        r = xrEndFrame(X.session, &ei);
        vk_queue_unlock();
        if (XR_FAILED(r) && frames < 5) rt_log("vr: xrEndFrame failed (%d)", (int)r);
        if (frames % 900 == 899 && corr_n) {
            rt_log("vr: correction SteamVR applies: mean %.2f deg, max %.2f deg; headset frames that got an older pair than the newest: %u of %u", corr_sum / corr_n, corr_max, race, corr_n);
            corr_sum = 0, corr_max = 0, corr_n = 0, race = 0;
        }
        {   // cadence: how often each eye had a new picture at a headset frame, and the longest run without one
            static unsigned seen[2], fresh[2], run[2], worst[2];
            for (int e = 0; e < 2; e++) {
                unsigned g = atomic_load(&eye_gen[e]);
                if (g != seen[e]) fresh[e]++, seen[e] = g, run[e] = 0;
                else if (++run[e] > worst[e]) worst[e] = run[e];
            }
            if (frames % 900 == 899) {
                rt_log("vr: cadence over 900 headset frames: new picture left %u, right %u; longest repeat left %u, right %u",
                       fresh[0], fresh[1], worst[0], worst[1]);
                fresh[0] = fresh[1] = worst[0] = worst[1] = 0;
            }
        }
        if (++frames == 1 || frames % 5400 == 0) rt_log("vr: %llu headset frames (%u layers)", (unsigned long long)frames, nl);
    }
end:
    if (X.running) xrEndSession(X.session), X.running = 0;   // (fails unless stopping; harmless)
    vk_queue_lock();
    xrDestroySession(X.session);
    vk_queue_unlock();
    X.session = XR_NULL_HANDLE;
    rt_log("vr: session destroyed");
}

// Before the process ends (gfx_window_closed, plat_exit runs no cleanup): end and destroy the session and the instance.
// Killing the VR thread inside an OpenXR call left a lock it shares with SteamVR held, and SteamVR's compositor aborted
// on it ("pthreads futex robust_list is corrupt"), taking SteamVR down.
void vk_xr_shutdown(void) {
    if (!X.ok || !X.inst) return;
    X.quit_t0 = kern_now_ns();
    atomic_store(&X.quit, 1);
    while (!atomic_load(&X.done) && kern_now_ns() - X.quit_t0 < 2000000000ll) plat_sleep_ms(5);
    if (!atomic_load(&X.done)) { rt_log("vr: the VR thread did not end in 2 s: quitting without the OpenXR shutdown"); return; }
    XRF(xrDestroyInstance);
    if (xrDestroyInstance) xrDestroyInstance(X.inst);
    X.inst = XR_NULL_HANDLE;
    rt_log("vr: OpenXR shut down");
}

void vk_xr_start(void) {
    if (!X.ok) return;
    if (!plat_thread_spawn(xr_thread, NULL, "vr", 0)) rt_log("vr: no VR thread");
}
