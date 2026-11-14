// Hide the game's HUD (TOTK_HIDE_HUD, totk.ini hide_hud, live): hearts, stamina wheel, map, prompts and the rest of
// what the game lays over the world, for clean screenshots and video. Off by default.
//
// Where the HUD is in a frame (measured for frame generation in PR #1089 from a frame dump of 1.4.3, and for DLSS in
// vk_dlaa.c, "where DLAA runs in TotK's frame"): the G-buffer passes draw several colour targets with the scene's D32F
// depth; lighting and the transparents draw into the light target (R11G11B10F or RGBA16F) with that depth; the bloom
// chain starts with a pass at most a quarter of the scene's width; the bloom composite and FXAA draw into the light
// target with the scene depth attached; with a scene smaller than the output one full-screen draw scales it into an
// output-sized float target without depth. The HUD is then drawn into the light target (or the scaled one) in passes
// without depth, with the HUD's own small targets drawn in between, and one last draw copies the float target into
// the 8-bit window texture.
//
// So a draw is skipped as HUD when, after the bloom chain ended the scene, it draws into one float colour target at
// least the scene's size in a pass without depth, and its fragment shader samples nothing that could be the picture:
// no float or depth render target (the scaling draw, a tonemap and the bloom composite read those), no storage image,
// and no texture the replay cannot name. Every doubt keeps the draw: a HUD element that reads a float target stays
// on screen, the 3D picture never goes missing. A draw that samples no texture at all there is HUD too (the noise
// meter's disc, in a frame dump of 1.4.3 at a 1600x900 scene and 1920x1080 output), so a plain-colour overlay drawn
// at that point, such as a fade, is hidden with it. The final copy into the 8-bit window texture is never skipped, so
// the HDR path (vk_hdr.c) finds it as before.
// The game's menus, dialogue and text are drawn the same way as the HUD and are hidden with it. A loading screen or
// a movie has no scene, so nothing there is skipped.
#include "vk_hud.h"

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "vk_dlaa.h"
#include "vk_exec.h"

static struct {
    int on;                      // hide_hud as read at the last present
    int gbuf, armed;             // this frame: the G-buffer was drawn; the light target was drawn with the scene depth
    uint32_t gbuf_w;             // the widest G-buffer's width this frame
    uint32_t scene_w, scene_h;   // the armed light target's size
    int ended;                   // this frame: the bloom chain began, the scene is drawn
    uint64_t end_fence;          // a chunked replay: the submission and op the scan ended the scene at (ended_at)
    uint32_t end_pos;
    int hid;                     // this frame: a draw was skipped
} H;

static int scene_color(VkFormat f) {
    return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT;
}

static int float_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT:
        return 1;
    default:
        return 0;
    }
}

// The same scene follow as vk_dlaa.c vk_dlaa_plan, cut down to what tells where the scene ends: no DLSS records.
void vk_hud_pass(const VkExec *ex) {
    if (!__atomic_load_n(&H.on, __ATOMIC_RELAXED) || H.ended) return;
    const VkExecTarget *c = &ex->color[0], *z = &ex->depth;
    if (ex->pass_ncolor >= 3 && z->image && z->format == VK_FORMAT_D32_SFLOAT && z->width >= DLAA_SCENE_MIN_W) {
        H.gbuf_w = H.gbuf && H.gbuf_w > z->width ? H.gbuf_w : z->width;
        H.gbuf = 1, H.armed = 0;
        return;
    }
    // the light target with the scene depth, as wide as the frame's widest G-buffer (vk_dlaa_plan: the camera rune's
    // and the particles' half-size pairs never arm)
    if (H.gbuf && ex->pass_ncolor == 1 && c->image && scene_color(c->format) && z->image &&
        z->format == VK_FORMAT_D32_SFLOAT && c->width == z->width && c->height == z->height && c->width >= H.gbuf_w &&
        c->samples <= 1 && !c->is3d) {
        H.scene_w = c->width, H.scene_h = c->height, H.armed = 1;
        return;
    }
    // the bloom chain's first pass: at most a quarter of the scene's width, into a scene colour target
    if (H.armed && ex->pass_w && ex->pass_w * 4 <= H.scene_w && c->image && scene_color(c->format)) {
        H.end_fence = ex->fence, H.end_pos = ex->pos;
        __atomic_store_n(&H.ended, 1, __ATOMIC_RELEASE);
    }
}

// As vk_dlaa.c ended_at: a chunk recording side by side with the scan is past the end only from the op it ended at.
static int ended_at(const VkExec *ex) {
    if (!__atomic_load_n(&H.ended, __ATOMIC_ACQUIRE)) return 0;
    return !ex->chunked || ex->fence != H.end_fence || ex->pos >= H.end_pos;
}

// 1: t may hold the picture (a float or depth image the GPU renders into)
static int picture_like(const VkTexImage *t, const void *arg) {
    (void)arg;
    if (t->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return 1;
    return (t->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) && float_format(t->format);
}

int vk_hud_skip_draw(VkExec *ex, const GfxShader *fs) {
    if (!__atomic_load_n(&H.on, __ATOMIC_RELAXED) || !fs || !ended_at(ex)) return 0;
    const VkExecTarget *c = &ex->color[0];
    if (ex->pass_ncolor != 1 || ex->depth.image || !c->image || !scene_color(c->format) || c->level || c->is3d ||
        c->samples > 1 || c->width < H.scene_w || c->height < H.scene_h)
        return 0;
    const GfxShaderRes *res;
    if (gfx_shader_resources(fs, GFX_RES_IMG, &res)) return 0;
    // 0: every texture sampled is named and none may be the picture, or nothing is sampled (the noise meter's dark
    // disc is such a draw, 1.4.3)
    if (vk_exec_draw_samples_any(ex, fs, picture_like, NULL) != 0) return 0;
    if (!H.hid) {
        static int logged;
        H.hid = 1;
        if (!logged) logged = 1, rt_log("hud: hide_hud skips the HUD's draws (first into a %ux%u float target)",
                                        c->width, c->height);
    }
    return 1;
}

void vk_hud_frame(void) {
    // hide_hud on for a minute of frames (3600) with a scene in each and no draw skipped: the frame no longer looks
    // like the one measured, say so once
    static uint64_t missed;
    static int reported;
    if (H.on && H.ended && !H.hid && !reported && ++missed == 3600) {
        reported = 1;
        rt_log("hud: hide_hud found no HUD draw in %llu frames with a scene; the HUD stays",
               (unsigned long long)missed);
    }
    if (H.hid) missed = 0;
    H.gbuf = H.armed = H.hid = 0, H.gbuf_w = H.scene_w = H.scene_h = 0;
    __atomic_store_n(&H.ended, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&H.on, cfg_bool("TOTK_HIDE_HUD"), __ATOMIC_RELAXED);
}
