// Hardware ray tracing (docs/raytracing.md). Phase 1: the plumbing and a test, behind TOTK_RT_TEST=1.
//
// Device: with the setting on, vk_device.c enables VK_KHR_acceleration_structure, VK_KHR_ray_query and
// VK_KHR_deferred_host_operations (and VK_KHR_ray_tracing_position_fetch when reported) and tells vk_rt_device_caps
// what the driver granted. With it off, nothing changes: the extensions stay off and none of this code runs.
//
// The test pass takes the contact shadows' place (vk_contact.c plans it after the lighting composite and hands it
// the scene colour, the linear depth and the camera block c4). Its scene is one test sphere of 1 m radius in world
// space: one BLAS, one TLAS instance, placed 10 m in front of the camera the first time the pass runs and again
// whenever the camera is 80 m from it. shaders/rt_test.comp rebuilds each pixel's world position from the linear
// depth and the inverse view (c4 rows 11-13), draws the sphere with a primary ray and darkens what it shades from
// the sun with a shadow ray. TOTK_RT_VIEW: 0 shadow + sphere, 1 world grid (1 m lines), 2 normals from depth,
// 3 shadow mask. TOTK_RT_STRENGTH (percent, 60): how much the shadow darkens; TOTK_RT_BRIGHT (1.0): sphere colour.
//
// Phase 2, geometry version 1 (TOTK_RT_GEOM=1, the default with the test on): the G-buffer draws (vk_exec_draw.c) bind
// a variant of their pipeline that also writes the vertex position to transform feedback (vk_pipeline_xfb), each into
// a range of the clip buffer reserved on the CPU (vk_rt_xfb_begin). Here, once a frame: shaders/rt_unproject.comp
// turns the clip-space corners into world space through the inverse of the camera block's view-projection (rows 3-6)
// and resets the clip buffer to NaN; one BLAS is built from the triangles (fast build, every frame) and the TLAS
// holds it (mask 1) and the test sphere (mask 2, TOTK_RT_SPHERE). TOTK_RT_VIEW 4: the captured scene by primary rays;
// 5: primary-ray distance against the depth buffer (green: match). Views 0 and 3 trace the sun against the scene.
//
// Phase 3, ambient occlusion (TOTK_RT_AO, on by default with the test): shaders/rt_ao.comp traces TOTK_RT_AO_RAYS (2)
// cosine-weighted rays of TOTK_RT_AO_RADIUS metres (3) per half-resolution pixel, blends them into a history kept
// per half-res pixel (reprojected with last frame's view-projection, reset where the depth disagrees), then a depth-aware
// 5x5 blur upsamples it and darkens the lit scene by TOTK_RT_AO_STRENGTH percent (100; the Kakariko inn test, 70 hardly showed). Pixels beyond TOTK_RT_AO_FAR
// metres (150) get none, and the captured scene then defaults to 200 m around the camera (TOTK_RT_RADIUS). Views with AO
// on: 0 the darkened scene, 6 the AO alone, 7 one frame's raw AO; 1-5 are the test views above (the AO is not run).
#include "vk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_hmap.h"
#include "platform/platform.h"
#include "rt_cache_spv.h"
#include "rt_unproject_spv.h"
#include "vk_exec.h"

// TOTK_RT_PROF=1 (debug): a GPU timestamp before and after every compute dispatch and acceleration structure build in
// this file, by source line; read back 8 frames later (prof_frame, from ts_begin) and logged every 300 frames as ms a
// frame per line (perf/<run>.log "vk: rt prof").
#define PROF_N 128   // timestamps a frame slot (64 dispatches)
static VkQueryPool prof_qp;
static int prof_on = -1, prof_slot;
static uint32_t prof_cnt[8];
static uint16_t prof_ln[8][PROF_N / 2];
static double prof_ms[8192];
static uint32_t prof_n, prof_over;
static void prof_stamp(VkCommandBuffer cb, int line, int after) {
    if (prof_on <= 0 || !prof_qp) return;
    uint32_t k = prof_cnt[prof_slot];
    if (k + (after ? 1u : 2u) > PROF_N) { if (after) prof_over++; return; }
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_qp, (uint32_t)prof_slot * PROF_N + k);
    if (after) prof_ln[prof_slot][k / 2] = (uint16_t)line, prof_cnt[prof_slot] = k + 1;
    else prof_cnt[prof_slot] = k + 1;
}
static void prof_frame(VkCtx *c, VkCommandBuffer cb, uint32_t slot) {
    if (prof_on < 0) { const char *e = cfg_get("TOTK_RT_PROF"); prof_on = e && *e == '1'; }
    if (prof_on <= 0) return;
    if (!prof_qp) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                    .queryCount = 8 * PROF_N};
        if (vkCreateQueryPool(c->device, &qi, NULL, &prof_qp) != VK_SUCCESS) { prof_on = 0; return; }
        vkCmdResetQueryPool(cb, prof_qp, 0, 8 * PROF_N);
        memset(prof_cnt, 0, sizeof prof_cnt);
        prof_slot = (int)slot;
        return;
    }
    uint32_t n = prof_cnt[slot] & ~1u;
    if (n) {
        uint64_t t[PROF_N * 2];
        if (vkGetQueryPoolResults(c->device, prof_qp, slot * PROF_N, n, sizeof t, t, 16,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) >= 0) {
            double k = c->props.limits.timestampPeriod / 1e6;
            for (uint32_t i = 0; i + 1 < n; i += 2)
                if (t[i * 2 + 1] && t[i * 2 + 3]) prof_ms[prof_ln[slot][i / 2] & 8191] += (double)(t[i * 2 + 2] - t[i * 2]) * k;
            prof_n++;
        }
    }
    vkCmdResetQueryPool(cb, prof_qp, slot * PROF_N, PROF_N);
    prof_cnt[slot] = 0, prof_slot = (int)slot;
    if (prof_n >= 300) {
        char b[2048];
        int o = snprintf(b, sizeof b, "vk: rt prof: ms a frame by line (over %u frames, %u stamps dropped):", prof_n, prof_over);
        double tot = 0;
        for (int i = 0; i < 8192; i++)
            if (prof_ms[i] / prof_n >= 0.005 && o < (int)sizeof b - 32) o += snprintf(b + o, sizeof b - o, " %d:%.3f", i, prof_ms[i] / prof_n), tot += prof_ms[i] / prof_n;
        snprintf(b + o, sizeof b - o, "; total %.2f", tot);
        rt_log("%s", b);
        memset(prof_ms, 0, sizeof prof_ms), prof_n = 0, prof_over = 0;
    }
}
#undef vkCmdDispatch   // vk_rec.h's recorder route (rc_vkCmdDispatch), kept, with the stamps around it
#define vkCmdDispatch(cb, x, y, z) (prof_stamp(cb, __LINE__, 0), rc_vkCmdDispatch(cb, x, y, z), prof_stamp(cb, __LINE__, 1))

#define SPH_RINGS 16
#define SPH_SEGS 32
#define SPH_NV ((SPH_RINGS + 1) * (SPH_SEGS + 1))
#define SPH_NI (SPH_RINGS * SPH_SEGS * 6)
#define CAP_TRIS (1u << 21)             // the captured scene's capacity (the village peaked at 977k)
#define CAP_VERTS (CAP_TRIS * 3)
#define SCENE_TRIS (1024u * 1024u)     // with the mesh cache: each capture BLAS's triangles (the frame's scene part;
                                        // 640k was reached on loading in Kakariko)
#define TOP_VERTS (CAP_VERTS / 8)       // the top of the clip buffer: captures only for the cache (compares, shadow passes),
                                        // kept out of the per-frame capture's BLAS so that its layout stays for the refit
#define CACHE_TRIS (1u << 21)           // the mesh cache's triangles, in the world buffer after the capture's
#define MESH_MAX 16384                  // identities tracked by the mesh cache (8192 filled up while walking; facing bits 16-31 hold slot + 1)
#define HASH_SIZE 65536
#define CELL R.cell                     // the cells' size (metres): TOTK_RT_CELL (64)
#define CELL_MAX 1024
#define MEMBER_MAX 2048                 // meshes in one cell (512 filled the Kakariko inn's 64 m cell)
#define CELLS_FRAME 8                   // cells built in one frame at most
#define CELL_TRIS 150000                // triangles of the cells built in one frame (one cell always)
#define CELL_WAIT 5                     // frames a cell that changed waits before it is built again (20 kept new meshes in the per-frame capture while walking)
#define INST_MAX (CELL_MAX + 2)         // TLAS instances: the capture, the sphere, the cells
#define GT_MAX 262144                   // gtab: the first world triangle of each cell BLAS geometry
#define PEND_MAX 512                    // meshes cached in one frame at most
#define CPOOL_BYTES (144ull << 20)      // the cells' BLASes (phase 8: highest ends 54-97 MiB at Kakariko, Lookout, Zora; was 192)
#define CSCRATCH_BYTES (48ull << 20)    // their builds' scratch in one frame
#define KEEP_FRAMES 600                 // a cached mesh off the screen stays in the scene this long
#define FREE_DELAY 12                   // frames before memory a frame in flight may use is reused
#define AMBIG_FRAMES 300                // an identity drawn twice in a frame is not cached for this long
#define RA_MAX 4096
#define RET_MAX 16384
#define VERIFY_FRAMES 60                // a ready mesh is captured again once in this many frames and compared with its copy
#define VFY_MAX 64                      // meshes compared in one frame at most
#define MFIX_MAX 256                    // cached meshes given a new material word in one frame at most
#define ANIM_FRAMES 600                 // an identity found moving stays known (not cached) this long after its last draw
#define ANIM_RETRY (4 * ANIM_FRAMES)    // and is tried in the cache again this long after it was found moving
#define VIS_MIN 8                       // cached captures: triangles the drawn test could judge before it counts
#define SHAPE_MAX 3                     // a ready mesh whose captures change shape this often is captured each frame
#define REFIT_MAX 30                    // the per-frame capture's BLAS is updated in place at most this many frames running
#define SCAM_MAX 15                     // shadow cascade cameras a frame (draw camera index 1-15; 0 the scene camera)
#define COMPACT_Q (8 * CELLS_FRAME)     // compacted-size queries: a ring of 8 frames of CELLS_FRAME

typedef struct { VkBuffer buf; VmaAllocation mem; void *map; VkDeviceAddress addr; } RtBuf;
typedef struct {
    uint64_t id;
    uint32_t state, ambig, seen, built, clip_vert, tris, woff, hidden;   // ambig: the frame + 1 it was drawn twice
    int32_t cell;
    int has_bounds;                      // 1 known, -1 none (no triangle), 0 not read yet
    float lo[3], hi[3];
    // vfy: the frame + 1 of its compare (its capture this frame: vclip, UINT32_MAX none); anim: found moving, not cached
    uint32_t vfy, vclip, anim;
    // src 1: from the sun's shadow passes (scam: the cascade camera's key the frame it was first drawn); sbox: lo/hi known
    // even when not cached; inview: that box is in the scene camera's view (the G-buffer has it: not cached from shadows)
    uint32_t src, scam, sbox, inview;
    uint32_t tw, mgen;                   // the material word its cached triangles hold (0 none), MT.gone_gen when last checked
    uint32_t ban;                        // the frame + 1 its copy came back without a triangle: not cached for AMBIG_FRAMES
    float ccam[3];                       // the camera when its copy was captured (the compare's error grows with distance)
    uint32_t nshape;                     // compares that found triangles appearing or disappearing (culled by distance)
    // its geometry key (vk_exec_geo_key, G-buffer meshes): while the sun's cascades draw that geometry (a depth-only
    // shader, another identity), the mesh stays in the scene though hidden behind something or off the screen
    uint64_t gk;
    uint32_t invis;                      // its geometry is drawn without a pixel surviving (VisEnt): not cached
    // step 6: its capture's varyings for a program (slots a corner, 0 none) and their copy in the cache's ext part
    // (MT.pext from XR_HALF x 2, vec4 units; xlen 0 none)
    uint32_t xn, xoff, xlen;
} RtMesh;
// Geometry the game draws without a pixel surviving, by geometry key (vk_exec_geo_key: the buffers, not the blocks, so
// the verdict outlives identities whose blocks change from frame to frame): the per-frame captures' drawn test
// (rt_unproject.comp: cumulative counts in bnd at MESH_MAX * 9, two a slot) read every 4 frames (pj, pf: the counts last
// read; pinit: read once) gives invis when a quarter of the triangles judged were never drawn (until under a twentieth
// of at least 64; judged: drawn or never drawn, not hidden behind something nearer)
// and the sun's cascades do not draw that geometry either (the game gives it no shadow). Such geometry is left out of the scene whole: the per-triangle test only sees the part
// on the screen, and the rest stayed. Found at Sitsum Shrine (Death Mountain): a hall around the shrine's platform
// (15418 + 5393 + 964 + 48 + 30 triangles) the game draws invisibly; its walls and roof off the screen shadowed the
// platform, more so the more of it was off the screen (looking down: all of it).
// sticky: once over half of at least 64 judged were never drawn, the geometry stays out while the cascades do not draw
// it: the Sitsum hall is the shrine's entrance seen through its door (drawn there, 96% of the judged in that view).
typedef struct { uint64_t k; uint32_t f, pj, pf, pinit, invis, verts, sticky; } VisEnt;
#define VIS_SLOTS 16384   // = MESH_MAX (the counters' room in bnd)
static VisEnt vis_t[VIS_SLOTS];
static int vis_log_n;
// the slot of geometry key k (under mesh_lock), made if new; one unused for 600 frames is taken over. -1: none
static int vis_slot(uint64_t k, uint32_t f, uint32_t verts) {
    if (!k) return -1;
    uint32_t h = (uint32_t)((k * 0x9e3779b97f4a7c15ull) >> 50) & (VIS_SLOTS - 1);
    int fr = -1;
    for (int p = 0; p < 32; p++, h = (h + 1) & (VIS_SLOTS - 1)) {
        if (vis_t[h].k == k) { vis_t[h].f = f, vis_t[h].verts = verts; return (int)h; }
        if (fr < 0 && (!vis_t[h].k || f - vis_t[h].f > 600u)) fr = (int)h;
    }
    if (fr < 0) return -1;
    memset(&vis_t[fr], 0, sizeof vis_t[fr]);
    vis_t[fr].k = k, vis_t[fr].f = f, vis_t[fr].verts = verts;
    return fr;
}
// The geometry keys the sun's cascades drew (shadow_begin, under mesh_lock), with the frame: an open table
#define GSET 32768
static uint64_t gset_k[GSET];
static uint32_t gset_f[GSET];
static void gset_put(uint64_t k, uint32_t f) {
    uint32_t h = (uint32_t)((k * 0x9e3779b97f4a7c15ull) >> 49);
    for (int p = 0; p < 16; p++, h = (h + 1) & (GSET - 1))
        if (gset_k[h] == k || !gset_k[h] || f - gset_f[h] > 8u) { gset_k[h] = k, gset_f[h] = f; return; }
}
static int gset_has(uint64_t k, uint32_t f) {
    if (!k) return 0;
    uint32_t h = (uint32_t)((k * 0x9e3779b97f4a7c15ull) >> 49);
    for (int p = 0; p < 16; p++, h = (h + 1) & (GSET - 1)) {
        if (!gset_k[h]) return 0;
        if (gset_k[h] == k) return f - gset_f[h] <= 2u;
    }
    return 0;
}
typedef struct {
    uint64_t key;                        // 0: free
    uint32_t nm, dirty, built, builds, goff, glen, tris;   // dirty: the frame + 1 its meshes first changed
    uint32_t cq, cq_builds;              // compaction: the query + 1 of its size (0 none), for the build it was asked for
    int32_t m[MEMBER_MAX];
    VkAccelerationStructureKHR as;
    VkDeviceAddress addr;
    VkDeviceSize boff, bsize;
    float lo[3], hi[3];
} RtCell;
typedef struct { uint64_t off, len; } RtSpan;
typedef struct { RtSpan r[RA_MAX]; int n; uint64_t cap, used, peak, hi, fail; } RtAlloc;   // peak/hi/fail: phase 8 sizing
typedef struct {
    uint32_t frame, woff, wlen;
    VkDeviceSize boff, blen;
    VkAccelerationStructureKHR as;
    uint32_t goff, glen;
    int32_t cell;
    uint32_t builds;
    uint32_t xoff, xlen;                 // step 6: the mesh's varyings in the cache's ext part (R.xa)
} RtRetired;
static PlatMutex mesh_lock = PLAT_MUTEX_INIT;   // the mesh cache: draws (vk_rt_xfb_begin) and the pass

#define QN 11  // timestamps a frame: 0-4 (ts_begin, unprojected, scene BLAS, TLAS, traced), 5-6 a second pass's trace,
                // 7-8 the next frame's first sun shadow pass (traced on this slot's scene), 9-10 the GI after the composite

static struct {
    int read, wanted, rq, pf;            // setting read; asked for; device granted ray queries, position fetch
    int tried, ok, logged, view;
    float strength, bright;
    // phase 3 AO: settings, pipeline, the two half-res histories (vec4 each: AO, depth, frames, raw), last frame's camera
    int ao, ao_rays, ao_inject;          // TOTK_RT_AO_INJECT: 1 replace the game's AO (default), 2 multiply it, 0 darken
                                         // the composite (phase 3); 3, 4 (debug): the game's AO set to 0, to 1
    float ao_radius, ao_strength, ao_far;
    VkDescriptorSetLayout ao_dsl;
    VkPipelineLayout ao_layout;
    VkPipeline ao_pipe, inj_pipe;
    VkExecTarget aot[16];                // per camera slot: the game's AO target (vk_contact.c), no image: none found
    uint64_t st_inj, st_noinj;
    RtBuf hist[2];
    VkDeviceSize hist_size;
    uint32_t ao_cur, ao_frame, prev_w, prev_h;
    int prev_ok;
    float prev_vp[16], prev_cam[3];
    float ov_ms[7];                      // the overlay's smoothed GPU times: unproject, BLAS, TLAS, trace, second trace,
                                         // first shadow pass, GI
    // phase 7 GI probe (TOTK_RT_GI=1, rt_gi.comp): one bounce at quarter resolution after the composite; two histories
    // of 2 vec4 per quarter-res pixel; the G-buffer's albedo per camera slot (vk_contact.c), no image: none found
    int gi, gi_ok;
    int qual;                            // rt_quality() the quality values were read for (read_quality)
    float gi_len, gi_strength, gi_far, gi_amb, gi_sat, gi_emit;
    VkPipeline gi_pipe;
    VkPipeline tone_pipe;                // rt_tone.comp: the night tone on the finished scene (vk_rt_tone_scene)
    int tone_late;                       // GI passes left that leave the tone to vk_rt_tone_scene (it ran lately)
    uint64_t st_tone_late;
    // the late material measure (rt_late.comp from vk_rt_tone_scene): mode 13's records (MT.cal mrec) of the GI pass
    // with this frame number and size wait for it; late_ok: GI passes left that leave the measure to it (it ran lately)
    VkPipeline late_pipe;
    int meas_pending, late_ok;
    uint32_t meas_fr, meas_w, meas_h;
    uint64_t st_late;
    RtBuf split;                         // view 26's right half, put in by the late hook (rt_late.comp mode 1)
    VkDeviceSize split_size;
    int split_pending;
    uint32_t split_w, split_h;
    RtBuf ghist[2], glights;            // glights: the light list (rt_gi.comp modes 5, 4): a count, then 32768 x 2 vec4
    float gi_thr;                        // TOTK_RT_GI_LIGHT: the lit brightness from which a pixel counts as a light
    VkDeviceSize ghist_size;
    VkExecTarget alb[16], mtl[16], mid[16];   // the G-buffer's albedo and material targets per camera slot
    uint32_t gi_frame, gprev_w, gprev_h;
    int gprev_ok, alb_logged, mtl_on;
    float gprev_vp[16], gprev_cam[3];
    uint64_t st_gi, st_gi_alb;
    // the GI's reflections (rt_gi.comp modes 8-10): TOTK_RT_REFL strength (0 off), roughness, ray length, F0, history
    // frames, a-trous steps; two half-res histories of 3 vec4 per pixel, made at the first frame's size
    float refl, refl_rough, refl_len, refl_f0;
    int refl_frames, refl_blur, rhist_ok, rhist_logged;
    RtBuf rhist[2];
    VkDeviceSize rhist_size;
    uint64_t st_refl;
    // the night tone (TOTK_RT_TONE, rt_gi.comp's apply): the frame's lighting level (mode 6, read back 3 frames late into
    // glevel, smoothed), the scene times mix(1, tone_g, tone x night) where night goes 1 -> 0 from tone_night to tone_day
    RtBuf glevel;
    float sky_level, sky_frac, sky_night, sky_day;   // the sky's smoothed brightness; TOTK_RT_TONE_SKY_NIGHT/_DAY
    int sky_n;
    RtBuf gcache;                        // the GI's world-space light cache (rt_gi.comp CN slots: keys, then colours)
    int gcache_on, gcache_clear;         // TOTK_RT_GI_CACHE (default 1); cleared once before first use
    float tone, tone_g[3], tone_night, tone_day, gi_level, st_level, tone_eff, night_f;
    int glevel_n, st_level_n;
    // phase 6 sun shadows (TOTK_RT_SHADOWS): settings, pipeline, the half-res shadow image the lighting reads and its
    // two histories (2 vec4 per half-res pixel, rt_shadow.comp), last frame's view-projection
    int shadows, simg_ready, simg_init;
    int lights, null_desc, tlas_built;   // TOTK_RT_LIGHTS (the light shaders trace their shadows); null AS descriptors; a TLAS built
    int test, ao_ok, sh_ok, sr_env;      // the test on; AO and shadow pipelines built (live switching); radius from the env
    int sh_rate;                         // TOTK_RT_SHADOW_RATE: 1 every ray every frame, 2 checkerboard (rt_shadow.comp)
    int sh_pct;                          // TOTK_RT_SHADOW_STRENGTH: 0 the game's shadow .. 100 ours
    int sh_nogame, sh_lit;                       // TOTK_RT_SHADOW_ONLY=1: no shadow map at all where rays are traced (G = 0)
    float sh_far, sh_len, sh_tan, sh_wide;
    VkPipeline sh_pipe;
    // the G-buffer's glow: the scene colour copied before the lighting composite (emit_copy); e_ok: copied for the GI
    // pass to come (the GI's lights, rt_gi.comp is_light)
    // the veil (rt_late.comp mode 2): the scene copied after the GI's apply (post), and lum(finished) / lum(post) at
    // the late hook (veil, R8); post_ok: copied this frame, veil_ok: measured
    VkImage pimg, vimg, aimg;
    VmaAllocation pimg_mem, vimg_mem, aimg_mem;
    VkImageView pview, vview, aview;   // aimg: what the GI's apply added (rt_gi.comp binding 6), for the late measure
    uint32_t aw, ah;
    int a_ok;   // aimg holds this frame's GI (the late measure's mode 0 takes it out, then clears this)
    uint32_t vw, vh;
    int post_ok, veil_ok;
    VkImage eimg;
    VmaAllocation eimg_mem;
    VkImageView eview;
    uint32_t ew, eh;
    int e_ok;
    VkImage simg;
    VmaAllocation simg_mem;
    VkImageView sview;
    // its quarter-res copy (a linear blit after each resolve and retrace) for the game's 480x270 lighting pass, which
    // lights a class of pixels the half-res pass then reads (frame dump pass 91); without it those took the game's shadow
    VkImage qimg;
    VmaAllocation qimg_mem;
    VkImageView qview;
    uint32_t qw, qh;
    uint32_t sw, sh, sh_frame, sh_gframe;   // sh_frame counts passes, sh_gframe first passes (one per game frame)
    uint64_t st_retrace, st_cut, st_late_dropped, st_late_pend;
    uint32_t cap_peak, xr_peak;          // phase 8: the per-frame capture's most triangles, the varyings ring's most vec4
    int early_built;                     // TOTK_RT_SHADOW_EARLY: this frame's first shadow pass built the scene   // second-pass retraces (mode 5); first passes skipped at a camera cut
    uint32_t sdump_every, sdump_state, sdump_n;   // TOTK_RT_SHADOW_DUMP=<n> (debug): every n-th pass to logs/rt_shadow_<pass>.ppm
    RtBuf sdump;
    VkImage sold; VmaAllocation sold_mem; VkImageView sold_view; RtBuf sold_hist[2]; uint32_t sold_at;   // after a resize
    VkImage sold_q; VmaAllocation sold_q_mem; VkImageView sold_qview;
    RtBuf shist[2];
    int sprev_ok;
    float sprev_vp[16], sprev_cam[3];
    // the sun's world direction from the last cascade camera (its depth row; shadow_begin), the gap to the one the
    // lighting's view-space row gives (shadow_exec, logged), TOTK_RT_SUN_SRC: 1 trace toward the cascade's
    float sun_w[3], sun_x[3], sun_l[3], sun_f[3];   // sun_x: cross of the cascade's x and y rows; sun_l, sun_f: last L, camera forward
    int sun_w_ok, sun_src;
    double sun_gap_sum;
    float sun_gap_max;
    uint32_t sun_gap_n;
    uint64_t st_shadow;
    PFN_vkCmdPushDescriptorSetKHR push;
    PFN_vkCreateAccelerationStructureKHR create_as;
    PFN_vkDestroyAccelerationStructureKHR destroy_as;
    PFN_vkGetAccelerationStructureBuildSizesKHR sizes;
    PFN_vkCmdBuildAccelerationStructuresKHR build;
    PFN_vkGetAccelerationStructureDeviceAddressKHR as_addr;
    VkDescriptorSetLayout dsl, as_dsl;   // push set (images, probe); the TLAS's own set
    VkDescriptorPool pool;
    VkDescriptorSet as_set;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkSampler sampler;
    RtBuf vb, ib, inst, scratch, blas_buf, tlas_buf;
    VkAccelerationStructureKHR blas, tlas;
    VkDeviceSize scratch_align;
    int blas_built, placed, probe_placed, since_place;
    VkDeviceSize scene_buf_size;
    RtBuf probe;                         // rt_test.comp's probe: a ground point near the player
    float center[3];
    // camera block rows 0-47 of the last 16 planned passes; the job carries its slot (pc[15]), since the planner can
    // run a submission ahead of the one being executed
    float cam[16][48 * 4];
    uint32_t cam_n;
    // geometry version 1
    int geom, sphere;                    // TOTK_RT_GEOM, TOTK_RT_SPHERE
    int vis;                             // TOTK_RT_VIS (1): triangles the depth buffer shows were not drawn are left out
    float max_edge, max_dist;            // TOTK_RT_EDGE (100 m), TOTK_RT_RADIUS (2000 m): triangles left out
    PFN_vkCmdBindTransformFeedbackBuffersEXT xfb_bind;
    PFN_vkCmdBeginTransformFeedbackEXT xfb_begin;
    PFN_vkCmdEndTransformFeedbackEXT xfb_end;
    RtBuf clip, world, scene_buf, scene_buf2;   // captured clip-space corners, their world positions, the capture BLASes' storage
    // Materials (docs/raytracing.md, materials step 2, TOTK_RT_MAT=1): transform feedback buffer 1 records each vertex's albedo UV
    // varying (offset 0) and its array layer's varying when that is another one (offset 16), stride 32, at the clip
    // buffer's vertex offsets with its top part moved down to follow the frame's (rt_unproject.comp aclip_vert); the
    // unprojection turns them into wattr, two vec4 a triangle indexed like world's: (uv0, uv1), (uv2, layer | flags << 16
    // bits: flags 1 UV, 2 array layer; the albedo texture word, vk_rt_mat_tex). dmat: each captured draw's material
    // words (vk_exec_draw.c rt_mat_words: UV and layer, the layer constant, the texture word).
    RtBuf aclip, wattr;
    // Materials step 4: the GI's sun (rt_gi.comp mode 12, gsun): the frame's lit colour / albedo summed over the blocks
    // the sun reaches (with N.L) and over those it does not, read back 3 frames later (gsun_rb, 4 slots) into the sun
    // and sky colours a lit hit gets (moving averages), handed back in gsun with the sun's world direction.
    RtBuf gsun, gsun_rb, gsun_up;   // gsun_up: the parameters staged (4 slots; a large vkCmdUpdateBuffer faulted)
    float sun_col[3], sky_col[3];
    int sun_est, sky_est, mprobe;
    double sacc[17], shacc[128];   // the measurement's sums and histograms (rt_gi.comp sun_level) decayed by 0.98 a GI frame
    double hacc[16][13];          // the haze sums (rt_gi.comp mat_measure) decayed by 0.98 a GI frame
    float hz[16][8];              // their fit (rt_gi.comp hz): extinction per metre rgb, 1 when fitted; the haze's colour rgb, 0
    int hz_on;
    int mat;
    volatile int mat_on;
    int mat_cap, wattr_clear, mat_retire;
    uint32_t xfb_nbuf, xfb_stride;   // the device's transform feedback buffers and their largest stride (step 6: 3, 128)   // mat_retire: the cached meshes captured again (their UVs)            // the device can capture UVs; the triangles' UVs zeroed at the next pass
    uint32_t dmat[8192][5];   // [3] step 6's ext word (rt_unproject.comp wext), [4] its slots word
    // step 6: the draws' varyings (MT.pext): two halves of XR_HALF vec4 by build (xpar: the half captures go to now; a
    // build flips it, so the late draws after the early build do not overwrite what this frame's GI reads); R.xa the
    // cache's part
    volatile uint32_t xring_used;
    uint32_t xpar;
    uint64_t st_xfull, st_xdraws, st_xverts;
    VkAccelerationStructureKHR scene;
    VkDescriptorSetLayout up_dsl;
    VkPipelineLayout up_layout;
    VkPipeline up_pipe;
    int cap_ready, inst_dirty;
    uint32_t xfb_used;                   // vertices reserved this frame (atomic)
    uint32_t xfb_draws;                  // draws captured this frame (atomic)
    uint32_t xfb_top;                    // vertices reserved this frame at the top (TOP_VERTS, atomic)
    uint32_t xfb_calls;                  // G-buffer draws offered for capture since the last pass (atomic)
    uint64_t st_dup;                     // passes with none: a second pass in one frame, which reuses the scene
    uint64_t st_why[3], st_retire[8];    // capture BLAS builds: no BLAS for the layout, -, a rebuild due; retire reasons
    uint64_t st_cap[9];                  // captured G-buffer draws by why not from the cache (see vk_rt_xfb_begin)
    uint64_t st_frames, st_tris, st_draws, st_over;
    uint32_t st_max;
    // TOTK_RT_DUMPGEOM=<n> (debug): at the n-th scene build the clip and world buffers are copied to the host and written
    // with the camera block to rt_geom_<n>.bin in the logs folder 8 builds later
    int dump_at, dump_state, dump_frames;
    uint32_t dump_verts;
    RtBuf dump_clip, dump_world;
    float dump_cam[48 * 4], dump_ivp[16];
    uint64_t tags[8192][3], dump_tags[8192][3];
    uint32_t dface[8192];                // each captured draw's facing (vk_rt_xfb_begin's flags, camera index bits 8-11,
                                         // the cache's mesh + 1 bits 16-31: its drawn test counted)
    RtBuf dtab;                          // per frame (a ring of 8): the draws' first triangles and facing, sorted
    // GPU timestamps around the pass's parts, a ring of 8 frames: start, unprojected, scene BLAS, TLAS, traced
    VkQueryPool qp;
    uint32_t qframe;
    uint8_t qused[8], qdup[8], qfirst[8], qgi[8];  // qdup: a second pass in the frame wrote timestamps 5 and 6 around its trace
    double t_ms[7];
    uint32_t t_n;
    uint32_t dump_ntags;
    // geometry version 2: the mesh cache
    int cache;                           // TOTK_RT_CACHE (1)
    uint32_t new_budget;                 // TOTK_RT_CACHE_NEW: triangles cached a frame at most
    uint32_t keep;                       // TOTK_RT_CACHE_KEEP: frames a cached mesh off the screen stays (KEEP_FRAMES)
    float cell;
    uint32_t cframe;                     // passes run: the frame the draws being recorded belong to
    uint32_t new_tris, npend;
    int32_t pend[PEND_MAX];
    RtMesh M[MESH_MAX];
    uint32_t mfix[MFIX_MAX][4], nmfix;   // material refreshes queued this frame: woff, triangles, word, mesh
    uint64_t cs_mfix;
    RtCell C[CELL_MAX];
    int32_t hidx[HASH_SIZE];             // entry + 1; 0 empty, -1 deleted
    int32_t mfree[MESH_MAX];
    int nfree, tombs, nret, mhigh;
    RtAlloc wa, ba, ga;                  // the cache's triangles, the BLAS pool's bytes, gtab's entries
    RtAlloc xa;                          // step 6: the cache's part of MT.pext (vec4)
    RtRetired ret[RET_MAX];
    RtBuf cpool, cscratch, bnd, jobs, gtab;
    VkDescriptorSetLayout ca_dsl;
    VkPipelineLayout ca_layout;
    VkPipeline ca_pipe;
    uint64_t cs_hit_draws, cs_hit_tris, cs_new, cs_newtris, cs_ret, cs_ambig, cs_inst, cs_inst_tris, cs_cells, cs_celltris;
    uint32_t cs_ready, cs_readytris, cs_copied;
    // moving meshes: the ready meshes captured again this frame to be compared with their copies
    uint32_t nvfy;
    int32_t vfyl[VFY_MAX];
    uint64_t cs_vfy, cs_anim;
    uint32_t cs_animn;
    // refit: the per-frame capture's layout (its draws' first triangles and count), BLAS updates in place since a build
    // two capture BLASes (with the cache), each refit while the layout it was built for comes again: some draws come
    // every other frame (docs/raytracing.md), so two layouts can alternate. cur: the one in this frame's TLAS.
    VkAccelerationStructureKHR scene2;
    uint64_t scene_sig[2];
    uint32_t scene_age[2], scene_used[2], scene_cap, low_verts;   // used: the frame it was last in a TLAS
    int scene_ok[2], refit, cur;         // a build to update from; TOTK_RT_REFIT (1)
    uint64_t st_refit;
    // shadow-pass capture (TOTK_RT_SHADOW, 1): this frame's cascade cameras (view-projection rows) and their keys
    int shadow, shadow_test;             // TOTK_RT_SHADOW_TEST=1 (debug): the scene from shadow passes alone
    int ckeep;                           // TOTK_RT_CASCADE_KEEP (default 1): G-buffer meshes stay while the cascades draw them
    float ckeep_m;                       // TOTK_RT_CASCADE_KEEP_M: within this many metres of the camera (default 40)
    float shadow_radius;                 // TOTK_RT_SHADOW_RADIUS: metres around the camera (15 with AO, else the capture's)
    float scam[SCAM_MAX][16];
    uint64_t scam_key[SCAM_MAX];
    uint32_t nscam;
    RtBuf camb;                          // a ring of 8 frames: 16 inverse view-projections (4 rows) each, 0 unused
    uint64_t cs_snew, cs_snewtris, cs_sinview, cs_scams, cs_skept;   // cs_skept: G-buffer meshes kept by the cascades
    uint32_t cs_sready, cs_sreadytris;
    // compaction (TOTK_RT_COMPACT, 1): cells built with it allowed, their compacted size queried, then copied
    int compact;
    VkQueryPool cqp;
    uint32_t cq_next, cq_frame[COMPACT_Q];
    uint64_t cs_compacted, cs_saved;
    PFN_vkCmdWriteAccelerationStructuresPropertiesKHR as_props;
    PFN_vkCmdCopyAccelerationStructureKHR as_copy;
} R;

static void read_settings(void) {
    if (R.read) return;
    const char *dev = cfg_get("TOTK_RT_DEVICE");
    R.wanted = dev && *dev == '1';
    const char *e = cfg_get("TOTK_RT_EDGE");
    R.max_edge = e && *e ? (float)atof(e) : 100.0f;
    e = cfg_get("TOTK_RT_RADIUS");
    R.max_dist = e && *e ? (float)atof(e) : 200.0f;
    e = cfg_get("TOTK_RT_VIS");
    R.vis = !(e && !strcmp(e, "0"));
    e = cfg_get("TOTK_RT_GEOM");
    R.geom = !(e && !strcmp(e, "0"));
    e = cfg_get("TOTK_RT_CACHE");
    R.cache = R.geom && !(e && !strcmp(e, "0"));
    e = cfg_get("TOTK_RT_CACHE_NEW");
    R.new_budget = e && *e ? (uint32_t)atoi(e) : 80000;
    e = cfg_get("TOTK_RT_CACHE_KEEP");
    R.keep = e && *e ? (uint32_t)atoi(e) : KEEP_FRAMES;
    {
        const char *ck = cfg_get("TOTK_RT_CASCADE_KEEP");
        R.ckeep = !(ck && *ck == '0');
        const char *cm = cfg_get("TOTK_RT_CASCADE_KEEP_M");
        R.ckeep_m = cm && *cm ? (float)atof(cm) : 40.0f;
    }
    e = cfg_get("TOTK_RT_CELL");
    R.cell = e && *e ? (float)atof(e) : 64.0f;
    if (!(R.cell >= 4.0f)) R.cell = 64.0f;
    e = cfg_get("TOTK_RT_REFIT");
    R.refit = !(e && !strcmp(e, "0"));
    e = cfg_get("TOTK_RT_SHADOW");
    R.shadow = R.cache && !(e && !strcmp(e, "0"));
    e = cfg_get("TOTK_RT_SHADOW_RADIUS");
    R.sr_env = e && *e;
    R.shadow_radius = R.sr_env ? (float)atof(e) : 80.0f;
    e = cfg_get("TOTK_RT_COMPACT");
    R.compact = R.cache && !(e && !strcmp(e, "0"));
    R.read = 1;
}

// vk_device.c: the ray tracing extensions are enabled only when a ray tracing setting is on at start, so players who
// leave it off get the device, the render targets and the shaders as before; TOTK_RT_DEVICE=1 enables them anyway,
// so the menu can switch ray tracing on in play
int vk_rt_wanted(void) {
    read_settings();
    const char *e = cfg_get("TOTK_RT_DEVICE");
    return R.wanted || (e && *e == '1');
}

// Ray tracing can run on this device (the extensions are on): render targets it writes get storage usage up front
static int rt_hw_supported;
void vk_rt_device_supported(int supported) { rt_hw_supported = supported != 0; }
int vk_rt_supported(void) { return rt_hw_supported; }
int vk_rt_capable(void) { read_settings(); return R.rq; }

static int cache_init(VkCtx *c);
static int mt_init(VkCtx *c);
static void mem_note(int line, uint64_t bytes, int host) { (void)line, (void)bytes, (void)host; }
static void mem_log(const char *tag) { (void)tag; }

void vk_rt_device_null(int null_desc) { R.null_desc = null_desc; }
void vk_rt_device_caps(int rq, int pos_fetch) {
    R.rq = rq, R.pf = pos_fetch;
    rt_log("vk: ray tracing %s (position fetch %d, not needed)",
           rq ? "enabled" : "unavailable on this device (needs acceleration structures, ray queries, buffer device "
                            "addresses and transform feedback): the rt_* settings do nothing", pos_fetch);
}

int vk_rt_on(void) {
    read_settings();
    return R.wanted && R.rq;
}

int vk_rt_note_camera(const float *c4) {
    int k = (int)(R.cam_n++ % 16);
    memcpy(R.cam[k], c4, sizeof R.cam[k]);
    memset(&R.aot[k], 0, sizeof R.aot[k]);
    return k;
}

// vk_contact.c: the game's AO target for the pass of camera slot k (planned before the composite)
static int make_buf_at(VkCtx *c, RtBuf *b, VkDeviceSize size, VkBufferUsageFlags usage, int host, int line);
#define make_buf(c, b, size, usage, host) make_buf_at(c, b, size, usage, host, __LINE__)
static int make_buf_at(VkCtx *c, RtBuf *b, VkDeviceSize size, VkBufferUsageFlags usage, int host, int line) {
    mem_note(line, size, host);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    VmaAllocationCreateInfo ai = {.usage = host ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                  .flags = host ? (host == 2 ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                                                             : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) |
                                                      VMA_ALLOCATION_CREATE_MAPPED_BIT : 0};
    VmaAllocationInfo info = {0};
    if (vmaCreateBuffer(c->vma, &bi, &ai, &b->buf, &b->mem, &info) != VK_SUCCESS) return 0;
    b->map = info.pMappedData;
    if (host && !b->map) return 0;
    VkBufferDeviceAddressInfo di = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b->buf};
    b->addr = vkGetBufferDeviceAddress(c->device, &di);
    return 1;
}

static void free_buf(VkCtx *c, RtBuf *b) {
    if (b->buf) vmaDestroyBuffer(c->vma, b->buf, b->mem);
    memset(b, 0, sizeof *b);
}

// Not opaque: the shaders skip a candidate that is the back of a one-sided triangle (world[].w 1; 2: two-sided), as
// the game's culling never shows it, a mesh the camera is inside of included.
static VkAccelerationStructureGeometryKHR scene_geometry(void) {
    return (VkAccelerationStructureGeometryKHR){
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .flags = 0,
        .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                               .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, .vertexData.deviceAddress = R.world.addr,
                               .vertexStride = 16, .maxVertex = CAP_VERTS - 1, .indexType = VK_INDEX_TYPE_NONE_KHR}};
}
// The per-frame capture's BLAS: fast build, and updated in place (refit) while its layout stays (TOTK_RT_REFIT)
#define SCENE_FLAGS (VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR)

// The instances of ring slot k (8 frames of INST_MAX)
static VkAccelerationStructureGeometryKHR inst_geometry(uint32_t k) {
    return (VkAccelerationStructureGeometryKHR){
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
        .geometry.instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                               .data.deviceAddress = R.inst.addr + (VkDeviceAddress)k * INST_MAX * 64}};
}

// One acceleration structure of the given type, sized for its geometry, with its buffer.
static int make_as(VkCtx *c, VkAccelerationStructureTypeKHR type, const VkAccelerationStructureGeometryKHR *g,
                   uint32_t prims, VkBuildAccelerationStructureFlagsKHR flags, RtBuf *buf, VkAccelerationStructureKHR *as,
                   VkDeviceSize *scratch) {
    VkAccelerationStructureBuildGeometryInfoKHR bi = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR, .type = type, .flags = flags,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .geometryCount = 1, .pGeometries = g};
    VkAccelerationStructureBuildSizesInfoKHR sz = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    R.sizes(c->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &prims, &sz);
    if (!make_buf(c, buf, sz.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, 0)) return 0;
    if (buf == &R.scene_buf) R.scene_buf_size = sz.accelerationStructureSize;
    VkAccelerationStructureCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                               .buffer = buf->buf, .size = sz.accelerationStructureSize, .type = type};
    if (R.create_as(c->device, &ci, NULL, as) != VK_SUCCESS) return 0;
    if (sz.buildScratchSize > *scratch) *scratch = sz.buildScratchSize;
    if ((flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR) && sz.updateScratchSize > *scratch) *scratch = sz.updateScratchSize;
    return 1;
}

#define MT_MAX 8192   // slots a kind (2D, 2D array)
// A material: an albedo view drawn by a fragment shader (its tint measured per shader), or a shader alone (flat: no
// texture, view and image null, in the 2D table). Its slot's generation moves on when it is freed.
typedef struct { VkImage image; VkImageView view; const void *fs; uint16_t gen; uint8_t used; } MtSlot;
typedef struct { VkImageView view; const void *fs; } MtKey;
// The materials' measured colours (rt_gi.comp modes 13, 14; rt_gi.comp MCal): per slot (2D, then array) 32 uints of
// sums and tags (64 uints); per shader id 6 vec4 (sval); MT_REC pairs of records of the late measure (mrec, a 4x4 block
// each: 3840x2176); per slot 8 vec4 of values (mval)
#define MT_SHADERS 4096   // fragment shaders given an id (their materials' fit averaged, for materials not measured)
#define MT_REC 522240
#define MT_AC 524288   // the appearance cache's cells (rt_gi.comp mcache, 16 uints each)
#define MT_CAL_BYTES ((VkDeviceSize)2 * MT_MAX * (64 * 4 + 128) + MT_SHADERS * 96 + (VkDeviceSize)MT_REC * 48 + \
                      (VkDeviceSize)MT_AC * 64)
static struct {
    PlatMutex lock;
    int ok;
    MtSlot s[2][MT_MAX];
    nvn_hmap by_view[2];          // MtKey -> slot + 1
    uint32_t fifo[2][MT_MAX], fhead[2], fcount[2], fresh[2];   // freed slots (oldest first); slots never used
    uint32_t nused[2];
    uint64_t st_new, st_gone, st_full;
    uint32_t gone_gen;                   // bumped whenever an image is dropped from MT (for lazy mfix checks on cached meshes)
    VkDescriptorSetLayout dsl;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkSampler samp;
    RtBuf gen;                    // uint per slot: 2D, then array
    RtBuf cal;                    // the measured colours (MT_CAL_BYTES), cleared before first use (cal_clear)
    int cal_clear;
    uint64_t st_flat;
    nvn_hmap by_fs;               // fragment shader -> its id (1..MT_SHADERS-1; gen's second half per slot)
    uint32_t nsid;
    // Step 6 (docs/raytracing.md, materials): the fragment shaders' programs (gfx_prog.c) at hits, set 2 bindings 4-7: pcode the
    // programs (uvec4, appended once each), pmat a record per material slot (PM_WORDS: the program + 1, the material word
    // it was written for, 8 texture words, the uniforms), pext the draws' varyings per corner (two frame halves of
    // XR_HALF, then the cache's XC; a sink of 16 at the end), wext per triangle (indexed like wattr: rt_unproject.comp)
    RtBuf pcode, pmat, pext, wext;
    int prog_feat, prog_ok;
    uint32_t pcode_used;
    PlatMutex plock;
    nvn_hmap by_prog;             // fragment shader -> program offset + 1 (uvec4)
    uint32_t pnote[2 * MT_MAX];   // the frame a material's record was last written
    uint64_t st_prog_up, st_prog_full, st_notes;
} MT = {.lock = PLAT_MUTEX_INIT, .plock = PLAT_MUTEX_INIT};
#define PCODE_V4 262144u          // program words (uvec4): 4 MiB
#define PM_WORDS 64u
#define XR_HALF (1u << 20)        // vec4 a frame half: 16 MiB
#define XC (2u << 20)             // the cache's: 32 MiB
#define XSINK (2u * XR_HALF + XC) // 16 vec4 a draw with no room writes into (its size 16 bytes: the rest discarded)

#define AO_PUSH 256   // rt_gi.comp's block; the AO's and shadows' shaders use the first 192 bytes and the last 16 (nr)
// The AO pipeline: set 0 pushed (linear depth, scene colour, history in, history out), set 1 the TLAS; the histories
// sized for a scene of up to 3840x2160. 0: AO stays off.
static float cam_near(const float *u) {
    float n = u[14 * 4 + 0];
    return n > 0.0f && n < 10.0f ? n : 0.0f;
}

static void screen_signs(const float *u, const float *iv, float *sx, float *sy) {
    const float *vp = u + 3 * 4;
    float pvw[3] = {1.0f, 1.0f, -10.0f}, pw[3], cl[4];
    for (int i = 0; i < 3; i++) pw[i] = iv[i * 4] * pvw[0] + iv[i * 4 + 1] * pvw[1] + iv[i * 4 + 2] * pvw[2] + iv[i * 4 + 3];
    for (int i = 0; i < 4; i++) cl[i] = vp[i * 4] * pw[0] + vp[i * 4 + 1] * pw[1] + vp[i * 4 + 2] * pw[2] + vp[i * 4 + 3];
    *sx = cl[0] / cl[3] > 0 ? 1.0f : -1.0f, *sy = cl[1] / cl[3] > 0 ? -1.0f : 1.0f;
}

// Phase 8: per-pixel histories sized for the frame they serve (1080p at start, grown when a larger frame comes), not
// for 4K up front (the AO's took 63 MiB, the GI's 47, at 1080p a quarter of that). A grown pair's old buffers wait
// FREE_DELAY passes for the frames in flight; the history starts again (valid: cleared).
static struct { RtBuf b; uint32_t at; } grave[8];
static int ngrave;
static int hist_fit(RtBuf b[2], VkDeviceSize *size, VkDeviceSize need, int *valid) {
    if (need <= *size) return 1;
    VkCtx *c = vk_ctx();
    int k = 0;
    for (int i = 0; i < ngrave; i++)
        if (R.qframe - grave[i].at > FREE_DELAY) free_buf(c, &grave[i].b);
        else grave[k++] = grave[i];
    ngrave = k;
    if (ngrave > 6) return 0;   // (two resizes within FREE_DELAY passes: this pass goes without)
    for (int i = 0; i < 2; i++)
        if (b[i].buf) grave[ngrave].b = b[i], grave[ngrave++].at = R.qframe, memset(&b[i], 0, sizeof b[i]);
    *size = 0, *valid = 0;
    if (!make_buf(c, &b[0], need, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0) ||
        !make_buf(c, &b[1], need, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0)) {
        free_buf(c, &b[0]), free_buf(c, &b[1]);
        return 0;
    }
    *size = need;
    rt_log("vk: ray tracing history resized to %.1f MiB x 2 for a larger frame", need / 1048576.0);
    return 1;
}

// The AO's two dispatches (rt_ao.comp): trace into this frame's history, then blur, upsample and apply. iv: the inverse
// view rows, u: the camera block, cam: its position.
int vk_rt_overlay(float *build_ms, float *trace_ms) {
    if (!R.ok) return 0;
    *build_ms = R.ov_ms[0] + R.ov_ms[1] + R.ov_ms[2], *trace_ms = R.ov_ms[3] + R.ov_ms[4] + R.ov_ms[5] + R.ov_ms[6];
    return 1;
}

// Materials on or off (TOTK_RT_MAT, at start and in play from vk_rt_live). The UV capture's buffers are made the first
// time and kept; the triangles' UVs are zeroed at the next pass, so meshes cached while off read as no material (not
// stale words). The draws' transform feedback variants are kept per materials state (vk_pipeline.c): a switch makes the
// draws look theirs up again, building the other kind where missing (those draws are not captured meanwhile).
static void ra_init(RtAlloc *a, uint64_t cap);
static int init(VkCtx *c) {
    R.tried = 1;
    if (!vk_rt_on()) return 0;
#define LOAD(f, n) (R.f = (void *)vkGetDeviceProcAddr(c->device, n))
    if (!LOAD(push, "vkCmdPushDescriptorSetKHR") || !LOAD(create_as, "vkCreateAccelerationStructureKHR") ||
        !LOAD(destroy_as, "vkDestroyAccelerationStructureKHR") || !LOAD(sizes, "vkGetAccelerationStructureBuildSizesKHR") ||
        !LOAD(build, "vkCmdBuildAccelerationStructuresKHR") ||
        !LOAD(as_addr, "vkGetAccelerationStructureDeviceAddressKHR") ||
        (R.geom && (!LOAD(xfb_bind, "vkCmdBindTransformFeedbackBuffersEXT") || !LOAD(xfb_begin, "vkCmdBeginTransformFeedbackEXT") ||
                    !LOAD(xfb_end, "vkCmdEndTransformFeedbackEXT")))) {
        rt_log("vk: ray tracing test off: the device has no acceleration structure commands");
        return 0;
    }
    if (!LOAD(as_props, "vkCmdWriteAccelerationStructuresPropertiesKHR") || !LOAD(as_copy, "vkCmdCopyAccelerationStructureKHR"))
        R.compact = 0;
#undef LOAD
    VkPhysicalDeviceAccelerationStructurePropertiesKHR ap = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &ap};
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    R.scratch_align = ap.minAccelerationStructureScratchOffsetAlignment ? ap.minAccelerationStructureScratchOffsetAlignment : 256;

    // The pipeline: the linear depth (sampled), the scene colour (storage), the TLAS
    VkDescriptorSetLayoutBinding b[8] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 4, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 5, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 6, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 7, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 8, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 8, .pBindings = b};
    // The TLAS in a set of its own: a pushed one needs a pNext chain, which the recorder (vk_rec.c) has no packet for
    // and drains for every frame.
    VkDescriptorSetLayoutBinding ab = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                                       .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo adci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1,
                                            .pBindings = &ab};
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1};
    VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1,
                                      .pPoolSizes = &ps};
    VkDescriptorSetLayout sets[2];
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 112};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2,
                                     .pSetLayouts = sets, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &R.dsl) != VK_SUCCESS ||
        vkCreateDescriptorSetLayout(c->device, &adci, NULL, &R.as_dsl) != VK_SUCCESS ||
        vkCreateDescriptorPool(c->device, &pci, NULL, &R.pool) != VK_SUCCESS ||
        !mt_init(c) || (sets[0] = R.dsl, sets[1] = R.as_dsl, 0) ||
        vkCreatePipelineLayout(c->device, &li, NULL, &R.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &R.sampler) != VK_SUCCESS) {
        rt_log("vk: ray tracing off: its layouts or sampler could not be created");
        return 0;
    }
    VkResult r;
    {   // the unprojection: two storage buffers, pushed
        VkDescriptorSetLayoutBinding ub[10] = {   // 8, 9: step 6's ext and wext
            {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            {.binding = 4, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            {.binding = 5, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            {.binding = 6, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            {.binding = 7, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            {.binding = 8, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
            {.binding = 9, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
        VkDescriptorSetLayoutCreateInfo udci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                                .bindingCount = 10, .pBindings = ub};
        VkPushConstantRange upr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 160};
        VkPipelineLayoutCreateInfo uli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                          .pSetLayouts = &R.up_dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &upr};
        VkShaderModuleCreateInfo umi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                        .codeSize = sizeof rt_unproject_spv, .pCode = rt_unproject_spv};
        VkShaderModule um = VK_NULL_HANDLE;
        if (vkCreateDescriptorSetLayout(c->device, &udci, NULL, &R.up_dsl) != VK_SUCCESS ||
            vkCreatePipelineLayout(c->device, &uli, NULL, &R.up_layout) != VK_SUCCESS ||
            vkCreateShaderModule(c->device, &umi, NULL, &um) != VK_SUCCESS) {
            rt_log("vk: ray tracing test off: the unprojection pipeline could not be created");
            return 0;
        }
        VkComputePipelineCreateInfo uci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                           .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                     .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = um, .pName = "main"},
                                           .layout = R.up_layout};
        r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &uci, NULL, &R.up_pipe);
        vkDestroyShaderModule(c->device, um, NULL);
        if (r != VK_SUCCESS) { rt_log("vk: ray tracing test off: the unprojection pipeline could not be created (%d)", (int)r); return 0; }
    }

    // AO and the sun shadows trace the captured scene. Both are built whenever ray tracing is on, so the menu can switch
    // either in play (vk_rt_live); the device features and the shader patch need ray tracing on at start.
    if (R.cache && !cache_init(c)) {
        rt_log("vk: ray tracing mesh cache off: its pipeline or memory could not be created");
        R.cache = 0;
    }

    VkBufferUsageFlags in = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    VkBufferUsageFlags sb = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (!make_buf(c, &R.inst, (VkDeviceSize)8 * INST_MAX * sizeof(VkAccelerationStructureInstanceKHR), in, 1) ||
        !make_buf(c, &R.gtab, (VkDeviceSize)GT_MAX * 4, sb, 1) ||
        (R.geom && (!make_buf(c, &R.clip, (VkDeviceSize)CAP_VERTS * 16,
                              VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0) ||
                    !make_buf(c, &R.world, (VkDeviceSize)(CAP_TRIS + (R.cache ? CACHE_TRIS : 0)) * 48,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | in, 0))) ||
        !make_buf(c, &R.probe, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1) ||
        (R.geom && !make_buf(c, &R.dtab, 8 * 8192 * 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1)) ||
        (R.geom && !make_buf(c, &R.camb, 8 * 16 * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1))) {
        rt_log("vk: ray tracing off: no memory for its buffers");
        return 0;
    }

    VkDeviceSize scratch = 0;
    R.scene_cap = R.cache ? SCENE_TRIS : CAP_TRIS;   // the frame's scene part of the capture: at most this
    R.low_verts = R.cache ? SCENE_TRIS * 3 : CAP_VERTS - TOP_VERTS;
    {   // materials: the attribute capture (transform feedback buffer 1, stride 32) and the triangles' UVs
        VkPhysicalDeviceTransformFeedbackPropertiesEXT xp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 xp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &xp};
        if (R.geom) vkGetPhysicalDeviceProperties2(c->phys, &xp2);
        R.mat_cap = R.geom && xp.maxTransformFeedbackBuffers >= 2 && xp.maxTransformFeedbackBufferDataStride >= 32;
        R.xfb_nbuf = xp.maxTransformFeedbackBuffers, R.xfb_stride = xp.maxTransformFeedbackBufferDataStride;
        if (R.geom && !R.mat_cap)
            rt_log("vk: ray tracing materials unavailable: the device has %u transform feedback buffers", xp.maxTransformFeedbackBuffers);
        R.mat = 0;
    }
    VkAccelerationStructureGeometryKHR ig = inst_geometry(0), cg = scene_geometry();
    if ((R.geom && !make_as(c, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &cg, R.scene_cap, SCENE_FLAGS, &R.scene_buf,
                            &R.scene, &scratch)) ||
        (R.geom && R.cache && !make_as(c, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &cg, R.scene_cap, SCENE_FLAGS,
                                       &R.scene_buf2, &R.scene2, &scratch)) ||
        !make_as(c, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &ig, INST_MAX,
                 VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR, &R.tlas_buf, &R.tlas, &scratch) ||
        !make_buf(c, &R.scratch, scratch + R.scratch_align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0)) {
        rt_log("vk: ray tracing off: its acceleration structures could not be created");
        return 0;
    }
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = R.pool,
                                       .descriptorSetCount = 1, .pSetLayouts = &R.as_dsl};
    if (vkAllocateDescriptorSets(c->device, &dai, &R.as_set) != VK_SUCCESS) {
        rt_log("vk: ray tracing off: no descriptor set for the TLAS");
        return 0;
    }
    VkWriteDescriptorSetAccelerationStructureKHR wa = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                                                       .accelerationStructureCount = 1, .pAccelerationStructures = &R.tlas};
    VkWriteDescriptorSet aw = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &wa, .dstSet = R.as_set,
                               .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    vkUpdateDescriptorSets(c->device, 1, &aw, 0, NULL);
    rt_log("vk: ray tracing on: captured scene %s (up to %u triangles, BLAS %.0f MiB), scratch %.1f MiB",
           R.geom ? "on" : "off", CAP_TRIS, R.scene_buf.buf ? (double)R.scene_buf_size / 1048576.0 : 0.0,
           (double)scratch / 1048576.0);
    R.inst_dirty = 1;
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                .queryCount = 8 * QN};
    if (vkCreateQueryPool(c->device, &qi, NULL, &R.qp) != VK_SUCCESS) R.qp = VK_NULL_HANDLE;
    return R.ok = 1;
}

// Timestamp k (0-4) of this frame's slot
static void ts(VkCommandBuffer cb, int k) {
    if (R.qp) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, R.qp, (R.qframe % 8) * QN + (uint32_t)k);
}

// The slot's results from 8 frames ago, if ready; then the slot is reset for this frame
static void ts_begin(VkCommandBuffer cb) {
    if (!R.qp) return;
    uint32_t slot = R.qframe % 8;
    uint64_t t[QN];
    if (R.qused[slot] && vkGetQueryPoolResults(vk_ctx()->device, R.qp, slot * QN, 5, 5 * 8, t, 8, VK_QUERY_RESULT_64_BIT) ==
                             VK_SUCCESS) {
        double k = vk_ctx()->props.limits.timestampPeriod / 1e6;
        for (int i = 0; i < 4; i++) {
            double v = (double)(t[i + 1] - t[i]) * k;
            R.t_ms[i] += v;
            R.ov_ms[i] = R.ov_ms[i] * 0.95f + (float)v * 0.05f;
        }
        double v = 0;   // the second pass's trace (the AO when the sun shadows ran first)
        if (R.qdup[slot] && vkGetQueryPoolResults(vk_ctx()->device, R.qp, slot * QN + 5, 2, 2 * 8, t + 5, 8,
                                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            v = (double)(t[6] - t[5]) * k;
        R.t_ms[4] += v;
        R.ov_ms[4] = R.ov_ms[4] * 0.95f + (float)v * 0.05f;
        v = 0;   // the first sun shadow pass of the next frame (traced on this slot's scene)
        if (R.qfirst[slot] && vkGetQueryPoolResults(vk_ctx()->device, R.qp, slot * QN + 7, 2, 2 * 8, t + 7, 8,
                                                    VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            v = (double)(t[8] - t[7]) * k;
        R.t_ms[5] += v;
        R.ov_ms[5] = R.ov_ms[5] * 0.95f + (float)v * 0.05f;
        v = 0;   // the GI after the composite
        if (R.qgi[slot] && vkGetQueryPoolResults(vk_ctx()->device, R.qp, slot * QN + 9, 2, 2 * 8, t + 9, 8,
                                                 VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            v = (double)(t[10] - t[9]) * k;
        R.t_ms[6] += v;
        R.ov_ms[6] = R.ov_ms[6] * 0.95f + (float)v * 0.05f;
        R.t_n++;
    }
    vkCmdResetQueryPool(cb, R.qp, slot * QN, QN);
    prof_frame(vk_ctx(), cb, slot);
    R.qused[slot] = 1, R.qdup[slot] = R.qfirst[slot] = R.qgi[slot] = 0;
    ts(cb, 0);
}

static void mem_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &d);
}

// the draw table by first triangle (vk_rt_exec)
static int dtab_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

// A build of as from g into cb; the recorder thread (vk_rec.c) has no packet for it, so what it queued goes first.
// update: refit as in place (built before with the same flags, geometry and count).
static void build_as_mode(VkCommandBuffer cb, VkAccelerationStructureTypeKHR type, VkBuildAccelerationStructureFlagsKHR flags,
                          const VkAccelerationStructureGeometryKHR *g, uint32_t prims, VkAccelerationStructureKHR as, int update) {
    VkDeviceAddress scratch = (R.scratch.addr + R.scratch_align - 1) & ~(R.scratch_align - 1);
    VkAccelerationStructureBuildGeometryInfoKHR bi = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR, .type = type, .flags = flags,
        .mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        .srcAccelerationStructure = update ? as : VK_NULL_HANDLE, .dstAccelerationStructure = as, .geometryCount = 1,
        .pGeometries = g, .scratchData.deviceAddress = scratch};
    VkAccelerationStructureBuildRangeInfoKHR range = {.primitiveCount = prims};
    const VkAccelerationStructureBuildRangeInfoKHR *rp = &range;
    if (rec_owns(cb)) rec_drain();
    R.build(cb, 1, &bi, &rp);
}

static void build_as(VkCommandBuffer cb, VkAccelerationStructureTypeKHR type, VkBuildAccelerationStructureFlagsKHR flags,
                     const VkAccelerationStructureGeometryKHR *g, uint32_t prims, VkAccelerationStructureKHR as) {
    VkDeviceAddress scratch = (R.scratch.addr + R.scratch_align - 1) & ~(R.scratch_align - 1);
    VkAccelerationStructureBuildGeometryInfoKHR bi = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR, .type = type, .flags = flags,
        .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .dstAccelerationStructure = as, .geometryCount = 1,
        .pGeometries = g, .scratchData.deviceAddress = scratch};
    VkAccelerationStructureBuildRangeInfoKHR range = {.primitiveCount = prims};
    const VkAccelerationStructureBuildRangeInfoKHR *rp = &range;
    if (rec_owns(cb)) rec_drain();
    R.build(cb, 1, &bi, &rp);
}

// The inverse of the view-projection (four rows, clip = row . world) into out[0..15], as rows. 0: singular.
static int invert_vp(const float *m, float *out) {
    double a[4][8];
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 8; k++) a[i][k] = k < 4 ? m[i * 4 + k] : (k - 4 == i);
    for (int col = 0; col < 4; col++) {
        int piv = col;
        for (int i = col + 1; i < 4; i++)
            if (fabs(a[i][col]) > fabs(a[piv][col])) piv = i;
        if (fabs(a[piv][col]) < 1e-30) return 0;
        for (int k = 0; k < 8; k++) { double t = a[col][k]; a[col][k] = a[piv][k]; a[piv][k] = t; }
        double d = a[col][col];
        for (int k = 0; k < 8; k++) a[col][k] /= d;
        for (int i = 0; i < 4; i++) {
            if (i == col) continue;
            double f = a[i][col];
            for (int k = 0; k < 8; k++) a[i][k] -= f * a[col][k];
        }
    }
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++) out[i * 4 + k] = (float)a[i][k + 4];
    return 1;
}


// ---- geometry version 2: the mesh cache (TOTK_RT_CACHE, on by default with the capture; docs/raytracing.md)
// Most draws are the same draw frame after frame: the same mesh, shader and object blocks. vk_exec_draw.c gives each
// captured draw an identity (rt_mesh_id: the vertex shader, the draw's mesh and the contents of the uniform blocks its
// vertex stage reads, except the camera c4, the environment c9 and the scene c14, which change every frame without
// placing it). Its life here:
//   seen     drawn, captured each frame like any draw.
//   pending  drawn this frame and in one of the last two (some draws come every other frame), once in each: captured
//            into the cache (unprojected without the drawn test
//            and the distance limit, bit 3 of its facing); rt_cache.comp copies its triangles to its range of the
//            world buffer after the capture's and takes their bounds.
//   copied   in the cache, still captured each frame. Its bounds come back 8 frames later and put it in a cell of
//            CELL metres (its bounds' centre).
//   ready    in its cell's BLAS, built from the cached triangles of all the cell's meshes: no longer captured. The cells
//            are the TLAS's instances (custom index 2 + the cell's first entry in gtab, which gives each of the BLAS's
//            geometries its first world triangle).
// A cached mesh leaves its cell (and the cell is built again, at most every CELL_WAIT frames, CELL_TRIS triangles a
// frame) when it is not drawn for 3 frames while its bounds are in the view (hidden, swapped for another level of
// detail, gone), when it has been off the screen for TOTK_RT_CACHE_KEEP frames (600), or when its identity was drawn
// twice in one frame (two objects: not cached for AMBIG_FRAMES). Objects behind the camera thus stay in the scene. A
// mesh's memory is reused once its cell was built without it and FREE_DELAY frames passed; a cell's old BLAS and gtab
// entries wait FREE_DELAY frames. TOTK_RT_CACHE_NEW: triangles cached a frame at most (80000). A copy that comes back
// without a triangle bans its identity from the cache for AMBIG_FRAMES.
// The rest of version 2:
//   moving meshes  once in VERIFY_FRAMES a ready mesh is captured again (at the top of the clip buffer, out of the
//                  frame's scene) and rt_cache.comp compares it with its copy; one that moved more than 10 cm + 1% of
//                  its distance (wind, water, cloth, camera-facing cards) leaves the cache and is captured each frame.
//   refit          the frame's own capture (skinned characters, moving and new meshes) is refit in place while its
//                  layout (its draws' first triangles) is the one a BLAS was built for; two BLASes, since some draws
//                  come every other frame and two layouts alternate. Unprojected triangles left out are dead (no area,
//                  w 0) rather than NaN, so every triangle stays active as a refit needs (TOTK_RT_REFIT).
//   shadow passes  the sun's cascade passes (vk_exec_draw.c) offer their draws too, each with its cascade's camera:
//                  a mesh drawn there only is cached (unprojected through that camera, both sides shown), within
//                  TOTK_RT_SHADOW_RADIUS of the camera; one whose box comes into the view is left to the G-buffer, and
//                  a G-buffer draw takes an identity over (TOTK_RT_SHADOW; _SHADOW_TEST=1 builds the scene from them alone).
//   compaction     each cell BLAS is built with compaction allowed; its compacted size is read back 8 frames later and
//                  it is copied into a BLAS of that size (TOTK_RT_COMPACT).
// A second pass in one frame (no G-buffer draw since the last) reuses the scene and leaves the cache's frame alone.
enum { MS_FREE, MS_SEEN, MS_PENDING, MS_COPIED, MS_READY };

static uint32_t mesh_hash(uint64_t id) { return (uint32_t)(id * 0x9E3779B97F4A7C15ull >> 40) & (HASH_SIZE - 1); }

static void mesh_reset(void) {
    memset(R.hidx, 0, sizeof R.hidx), memset(R.M, 0, sizeof R.M), memset(R.C, 0, sizeof R.C);
    R.tombs = 0, R.nfree = 0, R.mhigh = 0;
    for (int i = MESH_MAX - 1; i >= 0; i--) R.mfree[R.nfree++] = i;
}

// The entry of id; make: a new one (state seen) when there is none (*made 1). NULL: none, or the table is full.
static RtMesh *mesh_find(uint64_t id, int make, int *made) {
    uint32_t h = mesh_hash(id), tomb = UINT32_MAX;
    *made = 0;
    for (uint32_t k = 0; k < HASH_SIZE; k++) {
        uint32_t s = (h + k) & (HASH_SIZE - 1);
        int32_t v = R.hidx[s];
        if (v < 0) {
            if (tomb == UINT32_MAX) tomb = s;
            continue;
        }
        if (v && R.M[v - 1].id == id) return &R.M[v - 1];
        if (v) continue;
        if (!make || !R.nfree) return NULL;
        int32_t m = R.mfree[--R.nfree];
        if (m + 1 > R.mhigh) R.mhigh = m + 1;
        if (tomb != UINT32_MAX) s = tomb, R.tombs--;
        R.hidx[s] = m + 1;
        RtMesh *e = &R.M[m];
        memset(e, 0, sizeof *e);
        e->id = id, e->state = MS_SEEN, e->cell = -1, *made = 1;
        return e;
    }
    return NULL;
}

static void mesh_delete(RtMesh *e) {
    int32_t want = (int32_t)(e - R.M) + 1;
    for (uint32_t k = 0, h = mesh_hash(e->id); k < HASH_SIZE; k++) {
        uint32_t s = (h + k) & (HASH_SIZE - 1);
        if (R.hidx[s] == want) { R.hidx[s] = -1, R.tombs++; break; }
        if (!R.hidx[s]) break;
    }
    memset(e, 0, sizeof *e);
    R.mfree[R.nfree++] = want - 1;
}

static void mesh_rehash(void) {
    memset(R.hidx, 0, sizeof R.hidx);
    R.tombs = 0;
    for (int i = 0; i < MESH_MAX; i++) {
        if (R.M[i].state == MS_FREE) continue;
        for (uint32_t k = 0, h = mesh_hash(R.M[i].id); k < HASH_SIZE; k++)
            if (!R.hidx[(h + k) & (HASH_SIZE - 1)]) { R.hidx[(h + k) & (HASH_SIZE - 1)] = i + 1; break; }
    }
}

// The pools: first fit over sorted free ranges (cache triangles; BLAS bytes in multiples of 256; gtab entries)
static void ra_init(RtAlloc *a, uint64_t cap) { a->n = 1, a->r[0] = (RtSpan){0, cap}, a->cap = cap, a->used = 0; }
static int ra_alloc(RtAlloc *a, uint64_t len, uint64_t *out) {
    for (int i = 0; i < a->n; i++)
        if (a->r[i].len >= len) {
            *out = a->r[i].off;
            a->r[i].off += len, a->r[i].len -= len, a->used += len;
            if (a->used > a->peak) a->peak = a->used;
            if (*out + len > a->hi) a->hi = *out + len;
            if (!a->r[i].len) memmove(&a->r[i], &a->r[i + 1], sizeof *a->r * (size_t)(a->n - i - 1)), a->n--;
            return 1;
        }
    a->fail++;
    return 0;
}
static void ra_free(RtAlloc *a, uint64_t off, uint64_t len) {
    if (!len) return;
    int i = 0;
    while (i < a->n && a->r[i].off < off) i++;
    int prev = i > 0 && a->r[i - 1].off + a->r[i - 1].len == off, next = i < a->n && off + len == a->r[i].off;
    if (prev && next) {
        a->r[i - 1].len += len + a->r[i].len;
        memmove(&a->r[i], &a->r[i + 1], sizeof *a->r * (size_t)(a->n - i - 1)), a->n--;
    } else if (prev) {
        a->r[i - 1].len += len;
    } else if (next) {
        a->r[i].off = off, a->r[i].len += len;
    } else if (a->n < RA_MAX) {
        memmove(&a->r[i + 1], &a->r[i], sizeof *a->r * (size_t)(a->n - i)), a->r[i] = (RtSpan){off, len}, a->n++;
    } else {
        return;   // too fragmented: the range is lost
    }
    a->used -= len;
}

// Memory that frames in flight may still use: freed FREE_DELAY frames later (a cell's old BLAS and gtab entries, a
// mesh's triangles in no BLAS), or once the cell it was in has been built without it (cell >= 0).
static void defer_free(uint32_t woff, uint32_t wlen, VkAccelerationStructureKHR as, VkDeviceSize boff, VkDeviceSize blen,
                       uint32_t goff, uint32_t glen, int32_t cell) {
    if (R.nret >= RET_MAX) return;   // lost
    R.ret[R.nret++] = (RtRetired){R.cframe, woff, wlen, boff, blen, as, goff, glen, cell, cell >= 0 ? R.C[cell].builds : 0};
}

// Phase 8: how much of each fixed capacity is used (now, the most so far, the highest end reached, the size, failures)
static void mem_log_use(void) {
    const struct { const char *name; const RtAlloc *a; } t[] = {
        {"cache triangles (world)", &R.wa}, {"cell BLAS pool (bytes)", &R.ba}, {"cell geometry table", &R.ga},
        {"cache varyings (vec4)", &R.xa}};
    for (int i = 0; i < 4; i++)
        rt_log("vk:   %s: used %llu, peak %llu, highest end %llu of %llu, %llu allocations failed", t[i].name,
               (unsigned long long)t[i].a->used, (unsigned long long)t[i].a->peak, (unsigned long long)t[i].a->hi,
               (unsigned long long)t[i].a->cap, (unsigned long long)t[i].a->fail);
    rt_log("vk:   per-frame capture: most %u triangles of %u; varyings ring: most %u vec4 of %u a half", R.cap_peak,
           (unsigned)CAP_TRIS, R.xr_peak, (unsigned)XR_HALF);
}

static void retired_free(VkCtx *c, int all) {
    int k = 0;
    for (int i = 0; i < R.nret; i++) {
        RtRetired *r = &R.ret[i];
        int due = r->cell >= 0 ? R.C[r->cell].builds > r->builds && R.cframe - R.C[r->cell].built >= FREE_DELAY
                               : R.cframe - r->frame >= FREE_DELAY;
        if (all || due) {
            if (r->as) R.destroy_as(c->device, r->as, NULL);
            ra_free(&R.wa, r->woff, r->wlen), ra_free(&R.ba, r->boff, r->blen), ra_free(&R.ga, r->goff, r->glen);
            ra_free(&R.xa, r->xoff, r->xlen);
        } else {
            R.ret[k++] = *r;
        }
    }
    R.nret = k;
}

// The mesh's cached copy leaves the scene: out of its cell (built again later), back to seen
static void mesh_retire(RtMesh *e) {
    if (e->state != MS_COPIED && e->state != MS_READY) return;
    if (e->cell >= 0) {
        RtCell *cl = &R.C[e->cell];
        for (uint32_t k = 0; k < cl->nm; k++)
            if (cl->m[k] == (int32_t)(e - R.M)) { cl->m[k] = cl->m[--cl->nm]; break; }
        if (!cl->dirty) cl->dirty = R.cframe + 1;
    }
    uint32_t nr0 = R.nret;
    defer_free(e->woff, e->tris, VK_NULL_HANDLE, 0, 0, 0, 0, e->state == MS_READY ? e->cell : -1);
    if (R.nret > nr0) R.ret[R.nret - 1].xoff = e->xoff, R.ret[R.nret - 1].xlen = e->xlen;   // step 6's varyings with it
    e->xlen = 0;
    e->state = MS_SEEN, e->cell = -1, e->has_bounds = 0, e->hidden = 0, e->vfy = 0;
    R.cs_ret++;
}

static float ord_float(uint32_t o) {
    uint32_t u = o & 0x80000000u ? o & 0x7fffffffu : ~o;
    float f;
    memcpy(&f, &u, 4);
    return f;
}

// Is the box entirely outside the view (vp: the view-projection rows)?
static int box_outside(const float *vp, const float *lo, const float *hi) {
    int out[5] = {1, 1, 1, 1, 1};   // x < -w, x > w, y < -w, y > w, behind
    for (int k = 0; k < 8; k++) {
        float p[3] = {k & 1 ? hi[0] : lo[0], k & 2 ? hi[1] : lo[1], k & 4 ? hi[2] : lo[2]}, cl[4];
        for (int i = 0; i < 4; i++) cl[i] = vp[i * 4] * p[0] + vp[i * 4 + 1] * p[1] + vp[i * 4 + 2] * p[2] + vp[i * 4 + 3];
        out[0] &= cl[0] < -cl[3], out[1] &= cl[0] > cl[3], out[2] &= cl[1] < -cl[3], out[3] &= cl[1] > cl[3];
        out[4] &= cl[3] <= 0.01f;
    }
    return out[0] | out[1] | out[2] | out[3] | out[4];
}

// The cell of a point (its key), found or made; -1: none free
static int32_t cell_for(const float *p) {
    int32_t ix = (int32_t)floorf(p[0] / CELL), iy = (int32_t)floorf(p[1] / CELL), iz = (int32_t)floorf(p[2] / CELL);
    uint64_t key = ((uint64_t)(uint32_t)(ix & 0x1fffff) << 42 | (uint64_t)(uint32_t)(iy & 0x1fffff) << 21 |
                    (uint64_t)(uint32_t)(iz & 0x1fffff)) + 1;
    int32_t freec = -1;
    for (int32_t i = 0; i < CELL_MAX; i++) {
        if (R.C[i].key == key) return i;
        if (!R.C[i].key && freec < 0) freec = i;
    }
    if (freec >= 0) {
        RtCell *cl = &R.C[freec];
        uint32_t builds = cl->builds;   // kept: retired meshes count a cell's builds
        memset(cl, 0, sizeof *cl);
        cl->key = key, cl->builds = builds + 1, cl->built = R.cframe;
    }
    return freec;
}

// A mesh from the shadow passes whose box is in the scene camera's view (the G-buffer has it) or farther than
// TOTK_RT_SHADOW_RADIUS from the camera: not cached from the shadows
static int shadow_skip(const RtMesh *e, const float *vp, const float *cam) {
    if (R.shadow_test) return 0;
    float d2 = 0;
    for (int k = 0; k < 3; k++) {
        float d = cam[k] < e->lo[k] ? e->lo[k] - cam[k] : cam[k] > e->hi[k] ? cam[k] - e->hi[k] : 0.0f;
        d2 += d * d;
    }
    return d2 > R.shadow_radius * R.shadow_radius || !box_outside(vp, e->lo, e->hi);
}

// A cached capture's drawn test (rt_unproject.comp, bnd after the deviations): an eighth of the triangles it could judge
// were drawn without a pixel surviving (a fade, a cut-out, a hidden object). The cached copy skips the per-triangle
// test, so such a mesh stays out of the cache.
static uint64_t st_vis[4];   // drawn test of cached captures read back: meshes judged, triangles judged, failed; meshes out
static int never_drawn_mesh(VkCtx *c, int i) {
    vmaInvalidateAllocation(c->vma, R.bnd.mem, (VkDeviceSize)MESH_MAX * 28 + (VkDeviceSize)i * 8, 8);
    const uint32_t *v = (const uint32_t *)R.bnd.map + MESH_MAX * 7 + i * 2;
    int out = v[0] >= VIS_MIN && v[1] >= 4u && v[1] * 8u > v[0];   // an eighth: steady meshes fail ~0.2%, the shrine gate 15-49%
    static int dbg = -1;   // TOTK_RT_VISLOG=<n> (debug): the first n meshes with a failed triangle
    if (dbg < 0) { const char *e = cfg_get("TOTK_RT_VISLOG"); dbg = e && *e ? atoi(e) : 0; }
    if (v[0]) st_vis[0]++, st_vis[1] += v[0], st_vis[2] += v[1], st_vis[3] += (uint64_t)out;
    if (dbg > 0 && v[1]) dbg--, rt_log("vk: ray tracing drawn test: mesh %d (%u triangles, src %u, state %u): %u judged, %u never drawn%s", i,
                                         R.M[i].tris, R.M[i].src, R.M[i].state, v[0], v[1], out ? ": out" : "");
    return out;
}

// The mesh's box within r of the camera (its nearest point): the cascades' keep is limited to where the sun's shadows
// are traced, since the geometry key also matches the same mesh placed farther away
static int box_near(const RtMesh *e, const float *cam, float r) {
    float d2 = 0.0f;
    for (int k = 0; k < 3; k++) {
        float v = cam[k] < e->lo[k] ? e->lo[k] - cam[k] : cam[k] > e->hi[k] ? cam[k] - e->hi[k] : 0.0f;
        d2 += v * v;
    }
    return d2 <= r * r;
}

// Once a frame, before the frame's builds (vp: the view-projection rows): what left the scene, the bounds read back,
// the cells of the meshes whose bounds came, the identities not drawn any more
static void cache_upkeep(VkCtx *c, const float *vp, const float *cam) {
    retired_free(c, 0);
    uint32_t f = R.cframe, ready = 0, rtris = 0, copied = 0, sready = 0, srtris = 0, animn = 0;
    if (!(f & 3u)) {   // the per-frame drawn test by geometry, read every 4 frames (VisEnt)
        vmaInvalidateAllocation(c->vma, R.bnd.mem, (VkDeviceSize)MESH_MAX * 36, (VkDeviceSize)MESH_MAX * 8);
        for (int i = 0; i < VIS_SLOTS; i++) {
            VisEnt *v = &vis_t[i];
            if (!v->k || f - v->f > 8u) continue;
            const uint32_t *b = (const uint32_t *)R.bnd.map + MESH_MAX * 9 + i * 2;
            uint32_t j = b[0], fl = b[1], dj = j - v->pj, df = fl - v->pf;
            if (!v->pinit) {
                v->pinit = 1, v->pj = j, v->pf = fl;   // counts of earlier geometry in this slot: from here on
            } else if (dj >= (v->invis ? 64u : 16u)) {   // enough judged since the last verdict (else they add up)
                uint32_t was = v->invis;
                int cas = gset_has(v->k, f);
                static int on = -1;   // TOTK_RT_INVIS=0 (debug): such geometry kept (as before)
                if (on < 0) { const char *e = cfg_get("TOTK_RT_INVIS"); on = !(e && *e == '0'); }
                // out above a quarter, back in below a twentieth (visible meshes fail ~0.2%; the Sitsum hall 85-90%,
                // but 10-12% where its floor and walls lie on the real ground and rock, which pass as drawn)
                if (dj >= 64u && df * 2u > dj) v->sticky = 1;
                v->invis = on && !cas && (v->sticky || (was ? df * 20u >= dj : df * 4u > dj));
                v->pj = j, v->pf = fl;
                static int kc_n = 0;   // geometry the screen never shows that the game shadows (rt_unproject.comp bit 6)
                if (cas && df * 4u > dj && kc_n < 40)
                    kc_n++, rt_log("vk: ray tracing scene: geometry of %u triangles never drawn on the screen (%u of %u) but "
                                   "in the sun's cascades: kept for its shadow", v->verts / 3, df, dj);
                if (!was != !v->invis && vis_log_n < 40)
                    vis_log_n++, rt_log("vk: ray tracing scene: geometry of %u triangles %s (drawn test: %u of %u never drawn%s)",
                                        v->verts / 3, v->invis ? "drawn without a pixel surviving: left out" : "seen again: back in",
                                        df, dj, cas ? ", in the sun's cascades" : "");
            }
        }
    }
    for (int i = 0; i < R.mhigh; i++) {
        RtMesh *e = &R.M[i];
        if (e->state == MS_FREE) continue;
        if (e->ambig && f - (e->ambig - 1) > AMBIG_FRAMES) e->ambig = 0;   // tried again
        if (e->ban && f - (e->ban - 1) > AMBIG_FRAMES) e->ban = 0;
        if (e->anim && f - (e->anim - 1) > ANIM_RETRY) e->anim = 0, e->nshape = 0;   // tried in the cache again
        if (e->anim) animn++;
        if (R.mat_retire && (e->state == MS_COPIED || e->state == MS_READY)) {
            mesh_retire(e);
            continue;
        }
        if (e->state == MS_COPIED || e->state == MS_READY) {
            int gone = 0;
            int why = 0;
            if (e->ambig) {
                gone = 1, R.cs_ambig++;
            } else if (!e->src && e->invis) {
                gone = 1, why = 7;   // drawn without a pixel surviving (invis): not from the cache either
            } else if (e->src && e->has_bounds == 1 && shadow_skip(e, vp, cam)) {
                gone = 1, e->inview = 1, R.cs_sinview++, why = 5;   // in the view (the G-buffer's copy) or too far
            } else if (e->seen == f || (e->src && e->seen + 1 == f)) {   // shadow passes come after the G-buffer
                e->hidden = 0;
            } else if (R.ckeep && !e->src && e->has_bounds == 1 && gset_has(e->gk, f) && box_near(e, cam, R.ckeep_m)) {
                e->hidden = 0;   // not drawn in the view (occluded, or off the screen) but still in the sun's cascades: kept
                R.cs_skept++;
            } else if (!e->src && e->has_bounds == 1 && !box_outside(vp, e->lo, e->hi)) {
                // Keep occluded in-frustum static meshes for 30 frames (0.5 s at 60 fps) instead of 3 frames so
                // transient occlusion culling behind mist/overhangs does not churn the mesh cache or pop cave reflections.
                gone = ++e->hidden >= 30, why = 1;   // in the view and not drawn: hidden, another level of detail, gone
            } else {
                gone = f - e->seen > (e->has_bounds == 1 ? R.keep : 3u), why = 2;
            }
            // the compare with its copy (rt_cache.comp): moved more than 10 cm and 1% of the distance to it, it is a
            // moving mesh (wind, water, cloth, a camera-facing card) and is captured each frame from now on
            if (!gone && e->vfy && e->vfy != UINT32_MAX && f - (e->vfy - 1) >= 8) {
                vmaInvalidateAllocation(c->vma, R.bnd.mem, (VkDeviceSize)MESH_MAX * 24 + (VkDeviceSize)i * 4, 4);
                uint32_t dv = ((const uint32_t *)R.bnd.map)[MESH_MAX * 6 + i];
                float dev, d2 = 0, c2 = 0;
                memcpy(&dev, &dv, 4);
                for (int k = 0; k < 3; k++) {
                    float m = (e->lo[k] + e->hi[k]) * 0.5f, a = m - cam[k], b = m - e->ccam[k];
                    d2 += a * a, c2 += b * b;
                }
                e->vfy = 0;
                // the unprojection's error grows with the distance: the larger of now and when the copy was taken
                float far2 = d2 > c2 ? d2 : c2;
                if (!e->src && never_drawn_mesh(c, i)) {
                    gone = 1, e->ban = f + 1, why = 7;   // fades out now: the per-frame capture's drawn test decides
                } else if (dv == 0x7f800000u) {   // triangles appeared or went (culled by distance): taken again
                    gone = 1, why = 6;
                    if (++e->nshape >= SHAPE_MAX) e->anim = f + 1, R.cs_anim++;
                } else if (dev > 0.10f + 0.01f * sqrtf(far2)) {
                    gone = 1, e->anim = f + 1, R.cs_anim++, why = 3;
                }
            }
            if (gone) {
                R.st_retire[why]++;
                if (e->src && e->has_bounds == 1) e->sbox = 1;
                mesh_retire(e);
                continue;
            }
            if (e->state == MS_COPIED && !e->has_bounds && f - e->built >= 8) {
                vmaInvalidateAllocation(c->vma, R.bnd.mem, (VkDeviceSize)i * 24, 24);
                const uint32_t *b = (const uint32_t *)R.bnd.map + i * 6;
                e->has_bounds = -1;
                if (b[0] <= b[3] && b[1] <= b[4] && b[2] <= b[5]) {
                    for (int k = 0; k < 3; k++) e->lo[k] = ord_float(b[k]), e->hi[k] = ord_float(b[k + 3]);
                    e->has_bounds = 1, e->sbox = e->src;
                    // from the shadows but in the view or too far: out before it is put in a cell (no cell build)
                    if (e->src && shadow_skip(e, vp, cam)) {
                        e->inview = 1, R.cs_sinview++, R.st_retire[5]++;
                        mesh_retire(e);
                        continue;
                    }
                    if (!e->src && never_drawn_mesh(c, i)) {   // drawn, but no pixel of it survived
                        R.st_retire[7]++, e->ban = f + 1;
                        mesh_retire(e);
                        continue;
                    }
                }
            }
            if (e->state == MS_COPIED && e->has_bounds == -1) {   // no triangle survived the unprojection
                R.st_retire[4]++, e->ban = f + 1;
                mesh_retire(e);
                continue;
            }
            if (e->state == MS_COPIED && e->has_bounds == 1 && e->cell < 0) {
                float m[3] = {(e->lo[0] + e->hi[0]) * 0.5f, (e->lo[1] + e->hi[1]) * 0.5f, (e->lo[2] + e->hi[2]) * 0.5f};
                int32_t ci = cell_for(m);
                if (ci >= 0 && R.C[ci].nm < MEMBER_MAX) {
                    RtCell *cl = &R.C[ci];
                    cl->m[cl->nm++] = i, e->cell = ci;
                    if (!cl->dirty) cl->dirty = f + 1;
                }
            }
            if (e->state == MS_READY) ready++, rtris += e->tris;
            else copied++;
            if (e->state == MS_READY && e->src) sready++, srtris += e->tris;
        } else if (e->state == MS_SEEN &&
                   f - e->seen > (e->ambig || e->ban || (e->src && e->sbox) ? AMBIG_FRAMES : e->anim ? ANIM_FRAMES
                                  : e->src ? 3u : 2u)) {
            mesh_delete(e);   // waits to be cached only while drawn every frame: identities that change go at once
        } else if (e->state == MS_SEEN && e->src && e->sbox) {
            e->inview = shadow_skip(e, vp, cam);   // not cached from the shadows while in view or too far
        }
    }
    R.cs_sready = sready, R.cs_sreadytris = srtris, R.cs_animn = animn;
    if (R.tombs > HASH_SIZE / 4) mesh_rehash();
    // empty cells: their BLAS goes, the slot after FREE_DELAY
    for (int i = 0; i < CELL_MAX; i++) {
        RtCell *cl = &R.C[i];
        if (!cl->key || cl->nm) continue;
        if (cl->as || cl->glen) {
            defer_free(0, 0, cl->as, cl->boff, cl->bsize, cl->goff, cl->glen, -1);
            cl->as = VK_NULL_HANDLE, cl->bsize = 0, cl->glen = 0, cl->builds++, cl->built = f, cl->dirty = 0;
        } else if (f - cl->built > FREE_DELAY + 2) {
            cl->key = 0;
        }
    }
    if (R.mat_retire) rt_log("vk: ray tracing materials: the cached meshes captured again for their UVs"), R.mat_retire = 0;
    R.cs_ready = ready, R.cs_readytris = rtris, R.cs_copied = copied;
}

// The meshes captured for the cache this frame (after the unprojection): their triangles to the cache and their bounds
// (rt_cache.comp). ok 0: the frame captured nothing; they go back to waiting.
static void vis_zero(VkCtx *c, int ei) {
    uint32_t *v = (uint32_t *)R.bnd.map + MESH_MAX * 7 + ei * 2;
    v[0] = v[1] = 0;
    vmaFlushAllocation(c->vma, R.bnd.mem, (VkDeviceSize)MESH_MAX * 28 + (VkDeviceSize)ei * 8, 8);
}

static void cache_copy(VkCtx *c, VkCommandBuffer cb, int ok, const float *cam) {
    uint32_t slot = R.qframe % 8, f = R.cframe, maxt = 0, nb = 0;
    const VkDeviceSize jslot = (VkDeviceSize)(PEND_MAX + VFY_MAX + MFIX_MAX) * 16;
    uint32_t (*jobs)[4] = (uint32_t(*)[4])((uint8_t *)R.jobs.map + (size_t)(slot * jslot));
    // step 6, per job: the cache's ext vec4 its mesh's varyings go to; 0xfffffffe none (pointers cleared), ~0 off
    uint32_t *xjobs = (uint32_t *)((uint8_t *)R.jobs.map + (size_t)8 * jslot) + (size_t)slot * (PEND_MAX + VFY_MAX + MFIX_MAX);
    for (uint32_t i = 0; i < (PEND_MAX + VFY_MAX + MFIX_MAX); i++) xjobs[i] = 0xffffffffu;
    uint32_t *bnd = R.bnd.map;
    // ready meshes captured again: compared with their copies (bit 31 of the job's slot), read back 8 frames later
    for (uint32_t i = 0; i < R.nvfy; i++) {
        RtMesh *e = &R.M[R.vfyl[i]];
        if (e->state != MS_READY || e->vclip == UINT32_MAX || !ok) { e->vfy = 0; continue; }
        int ei = (int)(e - R.M);
        jobs[nb][0] = e->vclip / 3, jobs[nb][1] = CAP_TRIS + e->woff, jobs[nb][2] = e->tris, jobs[nb][3] = (uint32_t)ei | 0x80000000u;
        bnd[MESH_MAX * 6 + ei] = 0;
        vmaFlushAllocation(c->vma, R.bnd.mem, (VkDeviceSize)MESH_MAX * 24 + (VkDeviceSize)ei * 4, 4);
        vis_zero(c, ei);
        e->vfy = f + 1;
        if (e->tris > maxt) maxt = e->tris;
        nb++, R.cs_vfy++;
    }
    R.nvfy = 0;
    for (uint32_t i = 0; i < R.npend; i++) {
        RtMesh *e = &R.M[R.pend[i]];
        if (e->state != MS_PENDING) continue;
        e->state = MS_SEEN;
        uint64_t wo;
        if (!ok || e->ambig || !e->tris || !ra_alloc(&R.wa, e->tris, &wo)) continue;
        e->woff = (uint32_t)wo, e->built = f, e->has_bounds = 0, e->hidden = 0, e->cell = -1, e->state = MS_COPIED;
        int ei = (int)(e - R.M);
        // step 6: its varyings copied into the cache's part of ext (none: its triangles' pointers cleared)
        uint64_t xo2 = 0;
        e->xlen = 0;
        if (e->xn && MT.prog_ok && ra_alloc(&R.xa, (uint64_t)e->tris * 3 * e->xn, &xo2))
            e->xoff = (uint32_t)xo2, e->xlen = e->tris * 3 * e->xn;
        xjobs[nb] = !MT.prog_ok ? 0xffffffffu : e->xlen ? 2u * XR_HALF + e->xoff : 0xfffffffeu;
        jobs[nb][0] = e->clip_vert / 3, jobs[nb][1] = CAP_TRIS + (uint32_t)wo, jobs[nb][2] = e->tris, jobs[nb][3] = (uint32_t)ei;
        for (int k = 0; k < 3; k++) bnd[ei * 6 + k] = 0xffffffffu, bnd[ei * 6 + 3 + k] = 0, e->ccam[k] = cam[k];
        vmaFlushAllocation(c->vma, R.bnd.mem, (VkDeviceSize)ei * 24, 24);
        vis_zero(c, ei);
        if (e->tris > maxt) maxt = e->tris;
        nb++, R.cs_new++, R.cs_newtris += e->tris;
        if (e->src) R.cs_snew++, R.cs_snewtris += e->tris;
    }
    R.npend = 0, R.new_tris = 0;
    // cached meshes whose draws came with another material word: the word rewritten in their triangles (rt_cache.comp)
    for (uint32_t i = 0; i < R.nmfix; i++) {
        const RtMesh *e = &R.M[R.mfix[i][3]];
        if (!ok || (e->state != MS_READY && e->state != MS_COPIED) || e->woff != R.mfix[i][0] || e->tw != R.mfix[i][2]) continue;
        jobs[nb][0] = R.mfix[i][2], jobs[nb][1] = CAP_TRIS + R.mfix[i][0], jobs[nb][2] = R.mfix[i][1], jobs[nb][3] = 0x40000000u;
        if (R.mfix[i][1] > maxt) maxt = R.mfix[i][1];
        nb++, R.cs_mfix++;
    }
    R.nmfix = 0;
    if (!nb) return;
    vmaFlushAllocation(c->vma, R.jobs.mem, slot * jslot, (VkDeviceSize)nb * 16);
    VkDeviceSize xjo = (VkDeviceSize)8 * jslot + (VkDeviceSize)slot * (PEND_MAX + VFY_MAX + MFIX_MAX) * 4;
    vmaFlushAllocation(c->vma, R.jobs.mem, xjo, (VkDeviceSize)(PEND_MAX + VFY_MAX + MFIX_MAX) * 4);
    VkDescriptorBufferInfo b0 = {R.world.buf, 0, VK_WHOLE_SIZE}, b1 = {R.jobs.buf, slot * jslot, jslot},
                           b2 = {R.bnd.buf, 0, VK_WHOLE_SIZE}, b3 = {R.mat ? R.wattr.buf : R.probe.buf, 0, VK_WHOLE_SIZE},
                           b4 = {MT.prog_ok ? MT.pext.buf : R.probe.buf, 0, VK_WHOLE_SIZE},
                           b5 = {MT.prog_ok ? MT.wext.buf : R.probe.buf, 0, VK_WHOLE_SIZE},
                           b6 = {R.jobs.buf, xjo, (VkDeviceSize)(PEND_MAX + VFY_MAX + MFIX_MAX) * 4};
    VkWriteDescriptorSet w[7] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 4, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b4},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 5, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b5},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 6, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b6},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b0},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b1},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b2},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b3}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ca_pipe);
    rec_push(R.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ca_layout, 0, 7, w);
    vkCmdDispatch(cb, (maxt + 255) / 256, nb, 1);
    mem_barrier(cb);
}

// The cells whose meshes changed: a new BLAS each from its meshes' cached triangles (one geometry a mesh), at most
// CELLS_FRAME cells and CELL_TRIS triangles a frame (one cell always), a cell built before not again for CELL_WAIT
// frames after it changed. Its meshes are ready from the next frame.
static void cells_build(VkCtx *c, VkCommandBuffer cb) {
    static VkAccelerationStructureGeometryKHR g[CELLS_FRAME][MEMBER_MAX];
    static uint32_t cnt[CELLS_FRAME][MEMBER_MAX];
    static VkAccelerationStructureBuildRangeInfoKHR rg[CELLS_FRAME][MEMBER_MAX];
    static VkAccelerationStructureBuildGeometryInfoKHR bi[CELLS_FRAME];
    static const VkAccelerationStructureBuildRangeInfoKHR *rp[CELLS_FRAME];
    int built[CELLS_FRAME];
    uint32_t f = R.cframe, nb = 0, used = 0, *gt = R.gtab.map;
    VkDeviceAddress sbase = (R.cscratch.addr + R.scratch_align - 1) & ~(R.scratch_align - 1);
    VkDeviceSize soff = 0;
    for (int i = 0; i < CELL_MAX && nb < CELLS_FRAME; i++) {
        RtCell *cl = &R.C[i];
        if (!cl->key || !cl->dirty || !cl->nm) continue;
        if (cl->as && f - (cl->dirty - 1) < CELL_WAIT) continue;
        uint32_t tris = 0;
        for (uint32_t k = 0; k < cl->nm; k++) tris += R.M[cl->m[k]].tris;
        if (nb && used + tris > CELL_TRIS) continue;
        for (uint32_t k = 0; k < cl->nm; k++) {
            const RtMesh *e = &R.M[cl->m[k]];
            g[nb][k] = (VkAccelerationStructureGeometryKHR){
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
                .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                                       .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
                                       .vertexData.deviceAddress = R.world.addr + (VkDeviceAddress)(CAP_TRIS + e->woff) * 48,
                                       .vertexStride = 16, .maxVertex = e->tris * 3 - 1, .indexType = VK_INDEX_TYPE_NONE_KHR}};
            cnt[nb][k] = e->tris, rg[nb][k] = (VkAccelerationStructureBuildRangeInfoKHR){.primitiveCount = e->tris};
        }
        bi[nb] = (VkAccelerationStructureBuildGeometryInfoKHR){
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                     (R.compact ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR : 0),
            .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, .geometryCount = cl->nm, .pGeometries = g[nb]};
        VkAccelerationStructureBuildSizesInfoKHR sz = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        R.sizes(c->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi[nb], cnt[nb], &sz);
        VkDeviceSize asz = (sz.accelerationStructureSize + 255) & ~(VkDeviceSize)255;
        VkDeviceSize ssz = (sz.buildScratchSize + R.scratch_align - 1) & ~(R.scratch_align - 1);
        uint64_t bo, go;
        if (soff + ssz > CSCRATCH_BYTES - R.scratch_align) continue;
        if (!ra_alloc(&R.ba, asz, &bo)) continue;
        if (!ra_alloc(&R.ga, cl->nm, &go)) { ra_free(&R.ba, bo, asz); continue; }
        VkAccelerationStructureCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                   .buffer = R.cpool.buf, .offset = bo, .size = sz.accelerationStructureSize,
                                                   .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR};
        VkAccelerationStructureKHR as;
        if (R.create_as(c->device, &ci, NULL, &as) != VK_SUCCESS) {
            ra_free(&R.ba, bo, asz), ra_free(&R.ga, go, cl->nm);
            continue;
        }
        for (uint32_t k = 0; k < cl->nm; k++) gt[go + k] = CAP_TRIS + R.M[cl->m[k]].woff;
        vmaFlushAllocation(c->vma, R.gtab.mem, go * 4, (VkDeviceSize)cl->nm * 4);
        if (cl->as || cl->glen) defer_free(0, 0, cl->as, cl->boff, cl->bsize, cl->goff, cl->glen, -1);
        VkAccelerationStructureDeviceAddressInfoKHR ai = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                                                          .accelerationStructure = as};
        bi[nb].dstAccelerationStructure = as, bi[nb].scratchData.deviceAddress = sbase + soff;
        for (uint32_t k = 0; k < cl->nm; k++) rg[nb][k].primitiveCount = cnt[nb][k];
        rp[nb] = rg[nb];
        soff += ssz;
        cl->as = as, cl->addr = R.as_addr(c->device, &ai), cl->boff = bo, cl->bsize = asz, cl->goff = (uint32_t)go;
        cl->glen = cl->nm, cl->built = f, cl->builds++, cl->dirty = 0, cl->tris = tris;
        for (int k = 0; k < 3; k++) cl->lo[k] = 1e30f, cl->hi[k] = -1e30f;
        for (uint32_t k = 0; k < cl->nm; k++) {
            RtMesh *e = &R.M[cl->m[k]];
            e->state = MS_READY;
            for (int a = 0; a < 3; a++) {
                if (e->lo[a] < cl->lo[a]) cl->lo[a] = e->lo[a];
                if (e->hi[a] > cl->hi[a]) cl->hi[a] = e->hi[a];
            }
        }
        cl->cq = 0;
        built[nb] = i;
        used += tris, nb++, R.cs_cells++, R.cs_celltris += tris;
    }
    if (!nb) return;
    if (rec_owns(cb)) rec_drain();
    R.build(cb, nb, bi, rp);
    if (!R.compact) return;
    // their compacted sizes, read back by cells_compact. The barrier and the resets are recorder packets (vk_rec.h) and
    // the property writes are not: the recorder is drained between them, or the resets would land after the writes.
    mem_barrier(cb);
    uint32_t q[CELLS_FRAME];
    for (uint32_t k = 0; k < nb; k++) {
        q[k] = R.cq_next++ % COMPACT_Q;
        uint32_t owner = R.cq_frame[q[k]];
        if (owner && R.C[owner - 1].cq == q[k] + 1) R.C[owner - 1].cq = 0;   // its slot taken before it was read
        vkCmdResetQueryPool(cb, R.cqp, q[k], 1);
    }
    if (rec_owns(cb)) rec_drain();
    for (uint32_t k = 0; k < nb; k++) {
        RtCell *cl = &R.C[built[k]];
        R.as_props(cb, 1, &cl->as, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, R.cqp, q[k]);
        cl->cq = q[k] + 1, cl->cq_builds = cl->builds, R.cq_frame[q[k]] = (uint32_t)built[k] + 1;
    }
}

// Cells built 8 or more frames ago whose compacted size came back: copied into a BLAS of that size (the old one freed
// FREE_DELAY frames later). Before cells_build, so a cell built this frame waits.
static void cells_compact(VkCtx *c, VkCommandBuffer cb) {
    if (!R.compact) return;
    uint32_t f = R.cframe, n = 0;
    for (int i = 0; i < CELL_MAX && n < CELLS_FRAME; i++) {
        RtCell *cl = &R.C[i];
        if (!cl->key || !cl->cq || !cl->as) continue;
        if (cl->builds != cl->cq_builds) { cl->cq = 0; continue; }
        if (f - cl->built < 8) continue;
        uint64_t size = 0;
        VkResult r = vkGetQueryPoolResults(c->device, R.cqp, cl->cq - 1, 1, sizeof size, &size, sizeof size,
                                           VK_QUERY_RESULT_64_BIT);
        if (r == VK_NOT_READY && f - cl->built < 120) continue;
        cl->cq = 0;
        if (r != VK_SUCCESS || !size) continue;
        VkDeviceSize asz = (size + 255) & ~(VkDeviceSize)255;
        if (asz >= cl->bsize) continue;
        uint64_t bo;
        if (!ra_alloc(&R.ba, asz, &bo)) continue;
        VkAccelerationStructureCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                   .buffer = R.cpool.buf, .offset = bo, .size = size,
                                                   .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR};
        VkAccelerationStructureKHR as;
        if (R.create_as(c->device, &ci, NULL, &as) != VK_SUCCESS) { ra_free(&R.ba, bo, asz); continue; }
        if (!n && rec_owns(cb)) rec_drain();
        VkCopyAccelerationStructureInfoKHR cp = {.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
                                                 .src = cl->as, .dst = as, .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR};
        R.as_copy(cb, &cp);
        defer_free(0, 0, cl->as, cl->boff, cl->bsize, 0, 0, -1);
        VkAccelerationStructureDeviceAddressInfoKHR ai = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                                                          .accelerationStructure = as};
        R.cs_compacted++, R.cs_saved += cl->bsize - asz;
        cl->as = as, cl->addr = R.as_addr(c->device, &ai), cl->boff = bo, cl->bsize = asz;
        n++;
    }
    if (n) mem_barrier(cb);
}

// The cells in this frame's scene, as instances from inst (within the capture radius). The count.
static uint32_t cache_instances(const float *cam, VkAccelerationStructureInstanceKHR *inst) {
    uint32_t n = 0;
    for (int i = 0; i < CELL_MAX; i++) {
        RtCell *cl = &R.C[i];
        if (!cl->key || !cl->as) continue;
        float d2 = 0;
        for (int k = 0; k < 3; k++) {
            float d = cam[k] < cl->lo[k] ? cl->lo[k] - cam[k] : cam[k] > cl->hi[k] ? cam[k] - cl->hi[k] : 0.0f;
            d2 += d * d;
        }
        if (d2 > R.max_dist * R.max_dist) continue;
        inst[n++] = (VkAccelerationStructureInstanceKHR){
            .transform = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}}, .instanceCustomIndex = 2 + cl->goff, .mask = 0x01,
            .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR, .accelerationStructureReference = cl->addr};
        R.cs_inst_tris += cl->tris;
    }
    R.cs_inst += n;
    return n;
}

// A pass that returns before its work still ends the frame for the cache: the draws to come are the next frame's,
// and the meshes waiting to be cached wait for another capture.
static void cache_skip(void) {
    if (!R.cache) return;
    plat_mutex_lock(&mesh_lock);
    for (uint32_t i = 0; i < R.npend; i++)
        if (R.M[R.pend[i]].state == MS_PENDING) R.M[R.pend[i]].state = MS_SEEN;
    for (uint32_t i = 0; i < R.nvfy; i++) R.M[R.vfyl[i]].vfy = 0;
    R.npend = 0, R.new_tris = 0, R.nvfy = 0, R.nscam = 0, R.cframe++;
    plat_mutex_unlock(&mesh_lock);
}

// The late draws a second shadow pass drops (TOTK_RT_SHADOW_EARLY): their captures' clip vertices are written over
// next frame, so the meshes they queued for the cache (pend) or for a compare with their copy (vfyl) go back to
// waiting, as at a skipped pass; else cache_copy copied next frame's triangles at their clip offsets into the cache
// under their identity. The frame does not end here.
static void cache_drop_late(void) {
    if (!R.cache) return;
    plat_mutex_lock(&mesh_lock);
    for (uint32_t i = 0; i < R.npend; i++)
        if (R.M[R.pend[i]].state == MS_PENDING) R.M[R.pend[i]].state = MS_SEEN;
    for (uint32_t i = 0; i < R.nvfy; i++) R.M[R.vfyl[i]].vfy = 0;
    R.st_late_pend += R.npend + R.nvfy;
    R.npend = 0, R.new_tris = 0, R.nvfy = 0, R.nscam = 0;
    plat_mutex_unlock(&mesh_lock);
}

static int cache_init(VkCtx *c) {
    VkDescriptorSetLayoutBinding b[7];   // 4-6: step 6's ext, wext, xjobs
    for (int i = 0; i < 7; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 7, .pBindings = b};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &R.ca_dsl};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof rt_cache_spv,
                                   .pCode = rt_cache_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &R.ca_dsl) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &li, NULL, &R.ca_layout) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS)
        return 0;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = R.ca_layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &R.ca_pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) return 0;
    if (!make_buf(c, &R.cpool, CPOOL_BYTES, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, 0) ||
        !make_buf(c, &R.cscratch, CSCRATCH_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0) ||
        !make_buf(c, &R.bnd, (VkDeviceSize)MESH_MAX * 44, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 2) ||   // bounds, deviations, drawn test: read back (2: cached)
        !make_buf(c, &R.jobs, (VkDeviceSize)8 * (PEND_MAX + VFY_MAX + MFIX_MAX) * 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1))   // + step 6's xjobs
        return 0;
    if (R.compact) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, .queryCount = COMPACT_Q};
        if (vkCreateQueryPool(c->device, &qi, NULL, &R.cqp) != VK_SUCCESS) R.cqp = VK_NULL_HANDLE, R.compact = 0;
    }
    mesh_reset();
    ra_init(&R.wa, CACHE_TRIS), ra_init(&R.ba, CPOOL_BYTES), ra_init(&R.ga, GT_MAX);
    rt_log("vk: ray tracing mesh cache on: up to %u triangles, BLAS pool %llu MiB, cells of %.0f m, %u new triangles a "
           "frame, off the screen up to %u frames; moving meshes checked every %d frames; refit %s; shadow-pass capture %s%s; "
           "compaction %s", CACHE_TRIS, (unsigned long long)(CPOOL_BYTES >> 20), (double)CELL, R.new_budget, R.keep,
           VERIFY_FRAMES, R.refit ? "on" : "off", R.shadow ? "on" : "off", R.shadow_test ? " (test: shadow passes alone)" : "",
           R.compact ? "on" : "off");
    return 1;
}

static void cache_release(VkCtx *c) {
    retired_free(c, 1);
    for (int i = 0; i < CELL_MAX; i++)
        if (R.C[i].as) R.destroy_as(c->device, R.C[i].as, NULL);
    if (R.cqp) vkDestroyQueryPool(c->device, R.cqp, NULL);
    if (R.ca_pipe) vkDestroyPipeline(c->device, R.ca_pipe, NULL);
    if (R.ca_layout) vkDestroyPipelineLayout(c->device, R.ca_layout, NULL);
    if (R.ca_dsl) vkDestroyDescriptorSetLayout(c->device, R.ca_dsl, NULL);
    RtBuf *bufs[] = {&R.cpool, &R.cscratch, &R.bnd, &R.jobs};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; i++) free_buf(c, bufs[i]);
}

static int mt_init(VkCtx *c) { (void)c; return 1; }
static void mt_release(VkCtx *c) { (void)c; }
int vk_rt_capturing(void) { return R.ok && R.geom && R.cap_ready && R.wanted; }
int vk_rt_shadow_capturing(void) { return R.ok && R.shadow && R.cap_ready && R.wanted; }

// The menu's ray tracing switches in play (vk_contact.c, at each G-buffer): rt_ao and rt_shadows read again every 15th
// frame. Off, a feature's passes stop and the game's own AO / shadow is used; with both off the capture stops too.
static int shadow_begin(uint32_t verts, uint64_t id, const void *scam, uint32_t *flags, RtMesh **pe) {
    float m[16];
    memcpy(m, scam, sizeof m);
    uint64_t ck = 0xcbf29ce484222325ull;
    for (int k = 0; k < 16; k++) {
        uint32_t w;
        memcpy(&w, &m[k], 4);
        ck = (ck ^ w) * 0x100000001b3ull;
    }
    plat_mutex_lock(&mesh_lock);
    uint32_t f = __atomic_load_n(&R.cframe, __ATOMIC_ACQUIRE), ci = 0;
    for (uint32_t k = 0; k < R.nscam && !ci; k++)
        if (R.scam_key[k] == ck) ci = k + 1;
    if (!ci && R.nscam < SCAM_MAX) memcpy(R.scam[R.nscam], m, sizeof m), R.scam_key[R.nscam] = ck, ci = ++R.nscam;
    {   // the cascade's depth row: the sun's axis in world space (toward the sun: the upper side)
        float dl = sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
        if (dl > 1e-9f) {
            float sg = m[9] < 0 ? -1.0f : 1.0f;
            R.sun_w[0] = sg * m[8] / dl, R.sun_w[1] = sg * m[9] / dl, R.sun_w[2] = sg * m[10] / dl, R.sun_w_ok = 1;
        }
        float cx = m[1] * m[6] - m[2] * m[5], cy = m[2] * m[4] - m[0] * m[6], cz = m[0] * m[5] - m[1] * m[4];
        float cl = sqrtf(cx * cx + cy * cy + cz * cz);
        if (cl > 1e-12f) {
            float sg = cy < 0 ? -1.0f : 1.0f;
            R.sun_x[0] = sg * cx / cl, R.sun_x[1] = sg * cy / cl, R.sun_x[2] = sg * cz / cl;
        }
    }
    int made = 0, take = 0;
    RtMesh *e = mesh_find(id, 1, &made);
    if (e && made) e->src = 1;
    gset_put(vk_exec_geo_key(), f);
    {   // TOTK_RT_GKLOG=1 (debug): the big cascade draws, every 300 frames
        static int gl = -1;
        if (gl < 0) { const char *g = cfg_get("TOTK_RT_GKLOG"); gl = g && *g == '1'; }
        if (gl && verts >= 6000 && f % 300 == 0) rt_log("gk: frame %u cascade %u draw verts %u", f, ci, verts);
    }
    if (e && e->src) {
        if (e->seen == f) {
            if (e->scam == (uint32_t)ck) e->ambig = f + 1;   // twice in one cascade: two objects with one identity
        } else {
            take = f - e->seen <= 2 && e->state == MS_SEEN && !e->ambig && !e->ban && !e->inview && ci && R.npend < PEND_MAX &&
                   R.new_tris + verts / 3 <= R.new_budget;
            e->seen = f, e->scam = (uint32_t)ck;
        }
    }
    if (take) e->state = MS_PENDING, R.pend[R.npend++] = (int32_t)(e - R.M), R.new_tris += verts / 3, e->tw = 0, *pe = e;
    plat_mutex_unlock(&mesh_lock);
    *flags |= 8u | 16u | ci << 8;
    return take;
}

int vk_rt_xfb_begin(VkCommandBuffer cb, uint32_t verts, uint64_t tag, uint32_t flags, uint64_t id, const void *scam,
                    void (*mat_fn)(void *, uint32_t[5]), void *mat_ctx) {
    if (!scam) __atomic_fetch_add(&R.xfb_calls, 1, __ATOMIC_RELAXED);
    if (!verts) return 0;
    RtMesh *pe = NULL, *ve = NULL, *mfix_e = NULL;
    uint32_t gg = __atomic_load_n(&MT.gone_gen, __ATOMIC_ACQUIRE);
    uint32_t gmesh = 0;   // a G-buffer draw's geometry (VisEnt slot + 1) for its drawn test's counts, bit 31: left out
    int gcas = 0;         // the sun's cascades draw its geometry too: the game gives it a shadow (rt_unproject.comp bit 6)
    if (scam) {
        if (!id || !R.shadow || !shadow_begin(verts, id, scam, &flags, &pe)) return 0;
    } else if (R.shadow_test) {
        return 0;   // the scene from the shadow passes alone
    } else if (id && R.cache) {
        plat_mutex_lock(&mesh_lock);
        uint32_t f = __atomic_load_n(&R.cframe, __ATOMIC_ACQUIRE);
        int made = 0;
        RtMesh *e = mesh_find(id, 1, &made);
        if (e) e->gk = vk_exec_geo_key();
        {
            int vs = vis_slot(vk_exec_geo_key(), f, verts);
            if (vs >= 0) gmesh = (uint32_t)vs + 1u | (vis_t[vs].invis ? 0x80000000u : 0u);
            if (e) e->invis = vs >= 0 && vis_t[vs].invis;
            gcas = gset_has(vk_exec_geo_key(), f);
        }
        {   // TOTK_RT_GKLOG=1 (debug): every 300 frames, the big G-buffer draws and whether the sun's cascades drew them
            static int gl = -1;
            if (gl < 0) { const char *g = cfg_get("TOTK_RT_GKLOG"); gl = g && *g == '1'; }
            if (gl && verts >= 6000 && f % 300 == 0)
                rt_log("gk: frame %u verts %u vs %04x pass %ux%u in cascades %d", f, verts, (unsigned)((tag >> 48) & 0xffff),
                       (unsigned)(tag & 0xffff), (unsigned)((tag >> 16) & 0xffff), gset_has(vk_exec_geo_key(), f));
        }
        if (e && e->src) {   // cached from the shadow passes, drawn in the G-buffer after all: the G-buffer's from now on
            mesh_retire(e);
            e->src = 0, e->sbox = 0, e->inview = 0, e->ambig = 0;
        }
        if (e && !made) {
            if (e->seen == f) {
                e->ambig = f + 1;   // drawn twice in one frame: two objects with one identity, not cached
            } else if (!e->ambig) {
                if ((e->state == MS_READY || e->state == MS_COPIED) && mat_fn && R.mat && (e->tw >> 31) && e->mgen != gg)
                    mfix_e = e;   // an image was dropped since this mesh's material was checked: refresh lazily outside mesh_lock
                if (e->state == MS_READY) {
                    e->seen = f, R.cs_hit_draws++, R.cs_hit_tris += e->tris;
                    // once in VERIFY_FRAMES (spread over the meshes) captured again, to be compared with its copy
                    uint32_t ei = (uint32_t)(e - R.M);
                    if (e->vfy || R.nvfy >= VFY_MAX || (f + ei * 7u) % VERIFY_FRAMES || verts / 3 != e->tris) {
                        plat_mutex_unlock(&mesh_lock);
                        if (mfix_e) {
                            uint32_t mw[5] = {0};
                            mat_fn(mat_ctx, mw);
                            plat_mutex_lock(&mesh_lock);
                            if (mfix_e->state == MS_READY || mfix_e->state == MS_COPIED) {
                                mfix_e->mgen = gg;
                                if ((mw[2] >> 31) && mw[2] != mfix_e->tw && R.nmfix < MFIX_MAX) {
                                    uint32_t *q = R.mfix[R.nmfix++];
                                    q[0] = mfix_e->woff, q[1] = mfix_e->tris, q[2] = mw[2], q[3] = (uint32_t)(mfix_e - R.M), mfix_e->tw = mw[2];
                                }
                            }
                            plat_mutex_unlock(&mesh_lock);
                        }
                        return 0;   // its cell stands in: not captured
                    }
                    e->vfy = UINT32_MAX, e->vclip = UINT32_MAX, R.vfyl[R.nvfy++] = (int32_t)ei, ve = e;
                } else if (e->state == MS_SEEN && f - e->seen <= 2 && !e->anim && !e->ban && !e->invis && R.npend < PEND_MAX &&
                           R.new_tris + verts / 3 <= R.new_budget) {
                    e->state = MS_PENDING, R.pend[R.npend++] = (int32_t)(e - R.M), R.new_tris += verts / 3;
                    pe = e;
                }
            }
        }
        // why not from the cache: 1 new, 2 drawn twice, 3 not drawn in the last two frames, 4 moving, 5 empty, 6 over the budget,
        // 7 cached now, 8 copied (waiting for its cell)
        if (!e || e->state != MS_READY) R.st_cap[!e ? 0 : made ? 1 : e->seen == f || e->ambig ? 2 : e->state == MS_PENDING ? 7 : e->state == MS_COPIED ? 8
                 : f - e->seen > 2 ? 3 : e->anim ? 4 : e->ban ? 5 : 6]++;
        if (e) e->seen = f;
        plat_mutex_unlock(&mesh_lock);
    }
    int top = ve || (pe && pe->src);   // not part of this frame's scene: compared, or for the cache from a shadow pass
    uint32_t off;
    if (top) {
        uint32_t t = __atomic_add_fetch(&R.xfb_top, verts, __ATOMIC_RELAXED);
        off = t <= TOP_VERTS ? CAP_VERTS - t : UINT32_MAX;
    } else {
        off = __atomic_fetch_add(&R.xfb_used, verts, __ATOMIC_RELAXED);
        if ((uint64_t)off + verts > R.low_verts) off = UINT32_MAX;
    }
    if (off == UINT32_MAX) {
        if (pe) plat_mutex_lock(&mesh_lock), pe->state = MS_SEEN, plat_mutex_unlock(&mesh_lock);
        return 0;   // a compare without a capture: cache_copy drops it (vclip UINT32_MAX)
    }
    uint32_t matw[5] = {0};
    const uint32_t *mat = NULL;
    if (mat_fn && R.mat) {
        mat_fn(mat_ctx, matw);
        mat = matw;
    }
    if (pe) pe->tw = mat ? mat[2] : 0, pe->mgen = gg;
    if (mfix_e && mat) {
        plat_mutex_lock(&mesh_lock);
        if (mfix_e->state == MS_READY || mfix_e->state == MS_COPIED) {
            mfix_e->mgen = gg;
            if ((mat[2] >> 31) && mat[2] != mfix_e->tw && R.nmfix < MFIX_MAX) {
                uint32_t *q = R.mfix[R.nmfix++];
                q[0] = mfix_e->woff, q[1] = mfix_e->tris, q[2] = mat[2], q[3] = (uint32_t)(mfix_e - R.M), mfix_e->tw = mat[2];
            }
        }
        plat_mutex_unlock(&mesh_lock);
    }
    // step 6: a draw with a program records its varyings (vk_exec_draw.c rt_mat_words: mat[3] slots, mat[4] the uv and
    // layer slots) into the half of the ext ring captures go to now; none left: into the sink (16 bytes, the rest dropped)
    uint32_t xw = 0, xslots = mat && R.mat && MT.prog_ok ? mat[3] : 0;
    VkDeviceSize xo = 0, xz = 0;
    if (xslots) {
        uint32_t need = verts * xslots, o2 = __atomic_fetch_add(&R.xring_used, need, __ATOMIC_RELAXED);
        if (o2 + need > R.xr_peak) R.xr_peak = o2 + need;   // (a racy maximum: statistics)
        if ((uint64_t)o2 + need <= XR_HALF) {
            uint32_t base = R.xpar * XR_HALF + o2;
            xw = base | (xslots - 1) << 27 | (base ? 0u : 0x78000000u), xo = (VkDeviceSize)base * 16, xz = (VkDeviceSize)need * 16;
            R.st_xdraws++, R.st_xverts += verts;
        } else {
            xo = (VkDeviceSize)XSINK * 16, xz = 16, R.st_xfull++;
        }
    }
    if (pe) pe->clip_vert = off, pe->tris = verts / 3, flags |= 8 | (uint32_t)(pe - R.M + 1) << 16, pe->xn = xw ? xslots : 0;
    if (ve) ve->vclip = off, flags |= 8 | (uint32_t)(ve - R.M + 1) << 16;
    // rt_unproject.comp: a draw not for the cache counts its drawn test by geometry (VisEnt slot + 1 in bits 16-31, bnd at
    // MESH_MAX * 9); bit 5: the geometry is drawn without a pixel surviving (invis), all of it left out
    if (!pe && !ve && gmesh) flags |= (gmesh & 0xffffu) << 16 | (gmesh >> 31 ? 32u : 0u);
    // bit 6: the sun's cascades draw this geometry, so its triangles stay though the screen shows none of their pixels
    // (a character faded near the camera, dithered: the game keeps its shadow; the sages' avatars lost theirs)
    static int kc = -1;   // TOTK_RT_KEEPCAS=0 (debug): the drawn test drops them, as before
    if (kc < 0) { const char *e = cfg_get("TOTK_RT_KEEPCAS"); kc = !(e && *e == '0'); }
    if (kc && gcas && !pe && !ve) flags |= 64u;
    uint32_t di = __atomic_fetch_add(&R.xfb_draws, 1, __ATOMIC_RELAXED);
    if (di < 8192) {
        R.tags[di][0] = off, R.tags[di][1] = verts, R.tags[di][2] = tag, R.dface[di] = flags;
        R.dmat[di][0] = mat && R.mat ? mat[0] : 0, R.dmat[di][1] = mat && R.mat ? mat[1] : 0, R.dmat[di][2] = mat && R.mat ? mat[2] : 0;
        R.dmat[di][3] = xw, R.dmat[di][4] = xw ? mat[4] : 0;
        if (xslots && !xw && (mat[4] & 0x100u)) R.dmat[di][0] = 0;   // its UV was to come with the varyings: flat
    }
    VkDeviceSize o = (VkDeviceSize)off * 16, sz = (VkDeviceSize)verts * 16;
    // the attributes: at the same vertex, the top part's moved down after the frame's (rt_unproject.comp aclip_vert)
    uint32_t av = off >= CAP_VERTS - TOP_VERTS ? off - (CAP_VERTS - TOP_VERTS) + R.low_verts : off;
    rec_xfb((void *)R.xfb_bind, (void *)R.xfb_begin, (void *)R.xfb_end, cb, R.clip.buf, o, sz,
            R.aclip.buf, (VkDeviceSize)av * 32, (VkDeviceSize)verts * 32, MT.prog_ok ? MT.pext.buf : VK_NULL_HANDLE,
            xslots ? xo : (VkDeviceSize)XSINK * 16, xslots ? xz : 16, 0);   // (a variant that writes buffer 2 always has it)   // bound once made (never freed): a draw
            // that took its materials variant just before materials switched off still writes into a real buffer
    return 1;
}

void vk_rt_xfb_end(VkCommandBuffer cb) {
    rec_xfb((void *)R.xfb_bind, (void *)R.xfb_begin, (void *)R.xfb_end, cb, VK_NULL_HANDLE, 0, 0, VK_NULL_HANDLE, 0, 0,
            VK_NULL_HANDLE, 0, 0, 1);
}

void vk_rt_exec(VkExec *ex, const VkExecJob *j) {
    VkCtx *c = vk_ctx();
    if (!R.tried) init(c);
    if (!R.ok) return;
    if (!R.wanted) { cache_skip(); return; }   // both switched off in the menu
    int gi_job = j->pc[12] > 1.5f;   // the GI after the composite (vk_contact.c)
    const float *u = R.cam[(int)j->pc[15] & 15];
    const float *iv = u + 11 * 4;   // rows 11-13: view space -> world
    float cam[3] = {iv[3], iv[7], iv[11]}, fwd[3] = {-iv[2], -iv[6], -iv[10]};
    float fl = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    if (!(fl > 1e-6f)) { cache_skip(); return; }   // no usable camera: nothing recorded
    VkImageView in = vk_exec_target_view(j->depth.img, VK_FORMAT_R32_SFLOAT, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
                                         0, 0, 1, 0);
    VkImageView out = vk_exec_target_view(j->scene.img, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_IMAGE_VIEW_TYPE_2D,
                                          VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0);
    if (!in || !out) { cache_skip(); return; }
    vk_exec_barrier(ex);   // as vk_contact_exec: a full barrier that also takes feedback-layout images to GENERAL
    VkCommandBuffer cb = ex->cb;
    if (R.wattr_clear && R.wattr.buf) {   // materials just switched on (mat_switch)
        vkCmdFillBuffer(cb, R.wattr.buf, 0, VK_WHOLE_SIZE, 0);
        if (MT.wext.buf) vkCmdFillBuffer(cb, MT.wext.buf, 0, VK_WHOLE_SIZE, 0);
        mem_barrier(cb);
        R.wattr_clear = 0;
    }
    if (j->pc[13] > 1.5f) return;   // a job for the glow's copy alone
    // A second pass in one frame (no G-buffer draw since the last, nothing captured): the scene stays as it is, and the
    // cache's frame does not advance (else every mesh looks undrawn in the frame between and none is cached). With the
    // sun shadows on this is every frame's AO pass: the shadow pass before the lighting built the scene. Its trace is
    // timed by timestamps 5 and 6 of the first pass's slot; the ring does not advance.
    int dup = R.geom && R.cap_ready && !__atomic_load_n(&R.xfb_calls, __ATOMIC_ACQUIRE) &&
              !__atomic_load_n(&R.xfb_draws, __ATOMIC_ACQUIRE) && !__atomic_load_n(&R.xfb_top, __ATOMIC_ACQUIRE);
    // TOTK_RT_SHADOW_EARLY (1, default): the first shadow pass of a frame builds the scene from this frame's draws (all
    // but a few small late ones, frame dump passes 118-127), so shadows of moving things are not a frame late; the second
    // pass drops the late captures and retraces against that scene. 0: the first traces last frame's scene and the
    // second builds with every draw (one frame late).
    static int early = -1;
    if (early < 0) { const char *e = cfg_get("TOTK_RT_SHADOW_EARLY"); early = !(e && *e == '0'); }
    if (j->pc[14] > 0.5f && j->pc[14] < 1.5f && R.geom && R.cap_ready && !early) dup = 1;
    if (early && j->pc[14] > 1.5f && R.geom && R.cap_ready && R.early_built) {
        __atomic_store_n(&R.xfb_calls, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&R.xfb_used, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&R.xfb_draws, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&R.xfb_top, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&R.xring_used, 0, __ATOMIC_RELEASE);   // (the late draws' varyings dropped too)
        R.st_late_dropped++;
        cache_drop_late();
        dup = 1;
    }
    R.early_built = early && j->pc[14] > 0.5f && j->pc[14] < 1.5f && !dup;
    int dup_ts = 0;
    if (dup) {
        R.st_dup++;
        uint32_t ps = (R.qframe + 7) % 8;
        int first = j->pc[14] > 0.5f && j->pc[14] < 1.5f;   // the first sun shadow pass: timestamps 7-8; GI 9-10
        uint8_t *qf = gi_job ? &R.qgi[ps] : first ? &R.qfirst[ps] : &R.qdup[ps];
        if (R.qp && R.qused[ps] && !*qf) {
            dup_ts = gi_job ? 9 : first ? 7 : 5;
            *qf = 1;
            vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, R.qp, ps * QN + (uint32_t)dup_ts);
        }
        goto trace_only;
    }
    ts_begin(cb);
    __atomic_store_n(&R.xfb_calls, 0, __ATOMIC_RELEASE);
    if (R.cache) plat_mutex_lock(&mesh_lock), cache_upkeep(c, u + 3 * 4, cam);
    uint32_t used = __atomic_exchange_n(&R.xfb_used, 0, __ATOMIC_ACQ_REL), ndraws = __atomic_exchange_n(&R.xfb_draws, 0, __ATOMIC_ACQ_REL);
    __atomic_store_n(&R.xring_used, 0, __ATOMIC_RELEASE), R.xpar ^= 1;   // step 6: this build's varyings stay in their half
    uint32_t topv = __atomic_exchange_n(&R.xfb_top, 0, __ATOMIC_ACQ_REL);
    if (used > R.low_verts) R.st_over++, used = R.low_verts;
    if (topv > TOP_VERTS) topv = TOP_VERTS;
    uint32_t tris = used / 3, ttris = topv / 3;   // the frame's scene; the captures at the top
    int refit = 0;   // the capture's BLAS updated in place: the same layout as last frame's build
    if (R.geom) {
        float up[40] = {0};
        uint32_t *um = (uint32_t *)&up[36];   // materials: the frame's vertices, on, the top part's first vertex
        um[0] = R.low_verts, um[1] = (uint32_t)R.mat, um[2] = CAP_VERTS - TOP_VERTS, um[3] = (uint32_t)(R.mat && MT.prog_ok);
        up[20] = cam[0], up[21] = cam[1], up[22] = cam[2], up[24] = R.max_edge, up[25] = R.max_dist, up[26] = cam_near(u);
        uint32_t *un = (uint32_t *)&up[16];
        uint32_t slot = R.qframe % 8, nd = ndraws < 8192 ? ndraws : 8192;
        if (!R.cap_ready) {
            un[0] = CAP_TRIS, un[1] = 1, tris = 0;   // the clip buffer to NaN before the first capture
        } else if (tris || ttris) {
            if (!invert_vp(u + 3 * 4, up)) tris = 0, ttris = 0;
            un[0] = tris, un[1] = 0, un[2] = nd, un[3] = ttris;
            // the draws by first triangle: (first triangle, facing, material words), for the unprojection's search
            // built and sorted in host memory, then written once: the mapped table may be write-combined, where every
            // read the sort makes is uncached (3 ms a frame while walking)
            static uint32_t dt[8192][8];
            for (uint32_t i = 0; i < nd; i++)
                dt[i][0] = (uint32_t)(R.tags[i][0] / 3), dt[i][1] = R.dface[i], dt[i][2] = R.dmat[i][0], dt[i][3] = R.dmat[i][1],
                dt[i][4] = R.dmat[i][2], dt[i][5] = R.dmat[i][3], dt[i][6] = R.dmat[i][4], dt[i][7] = 0;
            // sorted: the frame's captures and the cache-only ones at the top of the buffer interleave, so an insertion
            // sort moved most entries past every top one (quadratic in the draws, 2 ms a frame while walking)
            qsort(dt, nd, sizeof dt[0], dtab_cmp);
            memcpy((uint8_t *)R.dtab.map + (size_t)slot * 8192 * 32, dt, (size_t)nd * 32);
            vmaFlushAllocation(c->vma, R.dtab.mem, (VkDeviceSize)slot * 8192 * 32, (VkDeviceSize)nd * 32 + 32);
            // the capture's layout, for the refit: its draws' first triangles and its size
            uint64_t sig = 0xcbf29ce484222325ull ^ tris;
            for (uint32_t i = 0; i < nd; i++)
                if (dt[i][0] < tris) sig = (sig ^ dt[i][0]) * 0x100000001b3ull;
            // the BLAS built for this layout, refit; else a build into the one used longer ago
            int nb = R.cache ? 2 : 1, k = -1;
            for (int i = 0; i < nb && k < 0; i++)
                if (R.scene_ok[i] && R.scene_sig[i] == sig) k = i;
            refit = R.refit && k >= 0 && R.scene_age[k] < REFIT_MAX;
            if (!refit) {
                R.st_why[k < 0 ? 0 : 2]++;   // a layout with no BLAS built for it; a rebuild due
                if (k < 0) k = nb == 2 && R.scene_used[1] < R.scene_used[0] ? 1 : 0;
            }
            R.cur = k, R.scene_sig[k] = sig;
            // the shadow cascades' cameras this frame: their inverse view-projections (camera index 1-15)
            if (R.shadow) {
                float *cm = (float *)R.camb.map + (size_t)slot * 16 * 16;
                for (uint32_t k = 0; k < R.nscam; k++)
                    if (!invert_vp(R.scam[k], cm + (k + 1) * 16)) memset(cm + (k + 1) * 16, 0, 64);
                vmaFlushAllocation(c->vma, R.camb.mem, (VkDeviceSize)slot * 16 * 64, 16 * 64);
                R.cs_scams += R.nscam;
            }
            // the projection's sign: a triangle facing the camera, its clip-space (x, y, w) determinant
            float P[3][3], sv[3] = {0, 0, 0};
            for (int k = 0; k < 3; k++) {
                float vx = k == 1 ? 1.0f : 0.0f, vy = k == 2 ? 1.0f : 0.0f, vz = -10.0f, w4[4] = {0, 0, 0, 1}, cl[4];
                for (int i = 0; i < 3; i++) w4[i] = iv[i * 4] * vx + iv[i * 4 + 1] * vy + iv[i * 4 + 2] * vz + iv[i * 4 + 3];
                for (int i = 0; i < 4; i++) cl[i] = u[(3 + i) * 4] * w4[0] + u[(3 + i) * 4 + 1] * w4[1] + u[(3 + i) * 4 + 2] * w4[2] + u[(3 + i) * 4 + 3];
                P[k][0] = cl[0], P[k][1] = cl[1], P[k][2] = cl[3];
            }
            float det = P[0][0] * (P[1][1] * P[2][2] - P[1][2] * P[2][1]) - P[0][1] * (P[1][0] * P[2][2] - P[1][2] * P[2][0]) +
                        P[0][2] * (P[1][0] * P[2][1] - P[1][1] * P[2][0]);
            (void)sv;
            up[23] = det > 0 ? 1.0f : -1.0f;
            // the depth test against what was drawn: projection scales, far plane, on; screen signs and size
            screen_signs(u, iv, &up[32], &up[33]);
            up[28] = j->pc[0], up[29] = j->pc[1], up[30] = j->pc[2], up[31] = R.vis ? 1.0f : 0.0f;
            up[34] = (float)j->scene.width, up[35] = (float)j->scene.height;
        }
        R.dump_frames++;   // builds since the camera arrived
        int dumping = R.dump_at && R.dump_frames == R.dump_at && tris && !R.dump_state && fabsf(cam[0]) + fabsf(cam[2]) > 100.0f;
        if (dumping) {
            VkDeviceSize bytes = (VkDeviceSize)tris * 3 * 16;
            if (make_buf(c, &R.dump_clip, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1) &&
                make_buf(c, &R.dump_world, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1)) {
                VkBufferCopy bc = {0, 0, bytes};
                vkCmdCopyBuffer(cb, R.clip.buf, R.dump_clip.buf, 1, &bc);
                mem_barrier(cb);
                R.dump_verts = tris * 3, R.dump_state = 1;
                memcpy(R.dump_cam, u, sizeof R.dump_cam);
                memcpy(R.dump_ivp, up, sizeof R.dump_ivp);
                R.dump_ntags = ndraws < 8192 ? ndraws : 8192;
                memcpy(R.dump_tags, R.tags, sizeof R.tags[0] * R.dump_ntags);
            } else {
                dumping = 0;
            }
        }
        if (un[0] || un[3]) {
            VkDescriptorImageInfo dii = {R.sampler, in, VK_IMAGE_LAYOUT_GENERAL};
            VkDescriptorBufferInfo b0 = {R.clip.buf, 0, VK_WHOLE_SIZE}, b1 = {R.world.buf, 0, VK_WHOLE_SIZE},
                                   b2 = {R.dtab.buf, (VkDeviceSize)slot * 8192 * 32, 8192 * 32},
                                   b4 = {R.camb.buf, (VkDeviceSize)slot * 16 * 64, 16 * 64},
                                   b5 = {R.cache ? R.bnd.buf : R.camb.buf, 0, VK_WHOLE_SIZE},
                                   b6 = {R.mat ? R.aclip.buf : R.probe.buf, 0, VK_WHOLE_SIZE},
                                   b7 = {R.mat ? R.wattr.buf : R.probe.buf, 0, VK_WHOLE_SIZE},
                                   b8 = {R.mat && MT.prog_ok ? MT.pext.buf : R.probe.buf, 0, VK_WHOLE_SIZE},
                                   b9 = {R.mat && MT.prog_ok ? MT.wext.buf : R.probe.buf, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet uw[10] = {
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 8, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b8},
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 9, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b9},
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b0},
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b1},
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b2},
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii},
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 4, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b4},
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 5, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b5},
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 6, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b6},
                {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 7, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b7}};
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.up_pipe);
            rec_push(R.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.up_layout, 0, 10, uw);
            vkCmdPushConstants(cb, R.up_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof up, up);
            vkCmdDispatch(cb, (un[0] + un[3] + 255) / 256, 1, 1);
            mem_barrier(cb);
        }
        if (R.cache) cache_copy(c, cb, (un[0] || un[3]) && un[1] == 0, cam), cells_compact(c, cb), cells_build(c, cb);
        R.cap_ready = 1;
        ts(cb, 1);
        if (dumping) {
            VkBufferCopy bc = {0, 0, (VkDeviceSize)R.dump_verts * 16};
            vkCmdCopyBuffer(cb, R.world.buf, R.dump_world.buf, 1, &bc);
            mem_barrier(cb);
        } else if (R.dump_state && R.dump_state++ == 9) {
            char path[512];
            snprintf(path, sizeof path, "logs/rt_geom_%d.bin", R.dump_at);
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(&R.dump_verts, 4, 1, f);
                fwrite(R.dump_cam, sizeof R.dump_cam, 1, f);
                fwrite(R.dump_ivp, sizeof R.dump_ivp, 1, f);
                fwrite(R.dump_clip.map, 16, R.dump_verts, f);
                fwrite(R.dump_world.map, 16, R.dump_verts, f);
                fwrite(&R.dump_ntags, 4, 1, f);
                fwrite(R.dump_tags, 24, R.dump_ntags, f);
                fclose(f);
            }
            rt_log("vk: ray tracing scene: %u captured corners written to %s", R.dump_verts, path);
        }
        VkAccelerationStructureGeometryKHR g = scene_geometry();
        build_as_mode(cb, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, SCENE_FLAGS, &g, tris, R.cur ? R.scene2 : R.scene, refit);
        mem_barrier(cb);
        ts(cb, 2);
        R.scene_age[R.cur] = refit ? R.scene_age[R.cur] + 1 : 0, R.scene_ok[R.cur] = tris > 0, R.st_refit += (uint64_t)refit;
        R.scene_used[R.cur] = R.qframe + 1;
        if (!tris) R.scene_sig[R.cur] = 0;
        R.st_frames++, R.st_tris += tris, R.st_draws += ndraws;
        if (tris > R.st_max) R.st_max = tris;
        if (tris > R.cap_peak) R.cap_peak = tris;
        if (R.st_frames % 300 == 0) {
            int built = 0, failed = 0;
            vk_pipeline_xfb_stats(&built, &failed);
            double n = R.t_n ? R.t_n : 1;
            if (R.cache)
                rt_log("vk: ray tracing drawn test of cached captures: %llu meshes judged, %llu triangles judged, %llu never drawn, %llu meshes out",
                       (unsigned long long)st_vis[0], (unsigned long long)st_vis[1], (unsigned long long)st_vis[2],
                       (unsigned long long)st_vis[3]), memset(st_vis, 0, sizeof st_vis);
            if (R.ao)
                rt_log("vk: ray traced AO: %llu passes into the game's AO target, %llu darkening the composite",
                       (unsigned long long)R.st_inj, (unsigned long long)R.st_noinj), R.st_inj = R.st_noinj = 0;
            if (R.shadows && R.sun_gap_n)
                rt_log("vk: sun direction, the lighting's row vs the cascade camera's: %.2f degrees apart on average, %.2f at most (%s)",
                       R.sun_gap_sum / R.sun_gap_n, (double)R.sun_gap_max, R.sun_src ? "tracing the cascade's" : "tracing the lighting's"),
                    rt_log("vk: sun vectors (world): lighting %.3f %.3f %.3f, cascade depth row %.3f %.3f %.3f, cascade x*y %.3f %.3f %.3f, camera forward %.3f %.3f %.3f",
                           R.sun_l[0], R.sun_l[1], R.sun_l[2], R.sun_w[0], R.sun_w[1], R.sun_w[2], R.sun_x[0], R.sun_x[1], R.sun_x[2],
                           R.sun_f[0], R.sun_f[1], R.sun_f[2]),
                    R.sun_gap_sum = 0, R.sun_gap_max = 0, R.sun_gap_n = 0;
            if (R.shadows)
                rt_log("vk: ray traced sun shadows: %llu passes, %llu retraces after more G-buffer draws, %llu camera cuts; GPU "
                       "ms a frame: first pass %.2f, retrace %.2f, AO %.2f; late draws dropped %llu times, %llu cache "
                       "entries rolled back with them", (unsigned long long)R.st_shadow,
                       (unsigned long long)R.st_retrace, (unsigned long long)R.st_cut, R.t_ms[5] / n, R.t_ms[3] / n,
                       R.t_ms[4] / n, (unsigned long long)R.st_late_dropped, (unsigned long long)R.st_late_pend),
                    R.st_shadow = R.st_retrace = R.st_cut = 0, R.st_late_dropped = R.st_late_pend = 0;
            {   // phase 8: the ray tracing's memory, after the first stats window and again a few later (it grows)
                static int ml = 0;
                if (ml < 4 && (ml == 0 || (R.qframe % 1200) < 300)) ml++, mem_log(ml == 1 ? "early" : "later");
            }
            if (R.gi)
                rt_log("vk: ray traced GI: %llu passes (%llu with the G-buffer's albedo), %llu with reflections; lighting "
                       "level %.2f (smoothed %.2f, night tone %.2f, %llu after the fog); %llu late material measures; GPU ms a frame %.2f",
                       (unsigned long long)R.st_gi, (unsigned long long)R.st_gi_alb, (unsigned long long)R.st_refl,
                       R.st_level_n ? R.st_level / R.st_level_n : -1.0f, R.gi_level, R.tone_eff,
                       (unsigned long long)R.st_tone_late, (unsigned long long)R.st_late, R.t_ms[6] / n),
                    R.st_gi = R.st_gi_alb = R.st_refl = R.st_tone_late = R.st_late = 0, R.st_level = 0.0f, R.st_level_n = 0;
            rt_log("vk: ray tracing scene: %.0f triangles from %.0f draws a frame over the last %d (most %u, over capacity %llu "
                   "frames); transform feedback pipelines built %d, failed %d; GPU ms a frame: unproject %.2f, scene BLAS %.2f, "
                   "TLAS %.2f, trace %.2f", (double)R.st_tris / 300, (double)R.st_draws / 300, 300, R.st_max,
                   (unsigned long long)R.st_over, built, failed, R.t_ms[0] / n, R.t_ms[1] / n, R.t_ms[2] / n,
                   (R.t_ms[3] + R.t_ms[4] + R.t_ms[5] + R.t_ms[6]) / n);
            if (R.mat && MT.ok) {
                plat_mutex_lock(&MT.lock);
                rt_log("vk: ray tracing materials: albedo textures in the table %u 2D, %u array (of %d each); since the last: "
                       "%llu added (%llu flat), %llu dropped with their image, %llu not added (table full); %llu cached meshes given a new word",
                       MT.nused[0], MT.nused[1], MT_MAX, (unsigned long long)MT.st_new, (unsigned long long)MT.st_flat,
                       (unsigned long long)MT.st_gone, (unsigned long long)MT.st_full, (unsigned long long)R.cs_mfix);
                MT.st_new = MT.st_gone = MT.st_full = MT.st_flat = 0, R.cs_mfix = 0;
                plat_mutex_unlock(&MT.lock);
                if (MT.prog_ok)
                    rt_log("vk: ray tracing materials, the game's shaders at hits: %u program words (uvec4) of %u, %llu programs "
                           "added; since the last: %llu draws recorded their varyings (%.0f vertices a frame), %llu found the "
                           "ring full; %llu material records written; cache varyings %llu of %u vec4",
                           MT.pcode_used, PCODE_V4, (unsigned long long)MT.st_prog_up, (unsigned long long)R.st_xdraws,
                           R.st_xverts / 300.0, (unsigned long long)R.st_xfull, (unsigned long long)MT.st_notes,
                           (unsigned long long)R.xa.used, XC),
                        R.st_xdraws = R.st_xverts = R.st_xfull = 0, MT.st_notes = 0;

            }
            if (R.cache)
                rt_log("vk: ray tracing mesh cache: %u meshes ready (%u triangles), %u waiting for their cell; pools: BLAS "
                       "%.0f of %llu MiB, triangles %.0f%%, %d retired waiting; a frame: %.0f draws (%.0f triangles) from the "
                       "cache, %.1f cells in the scene (%.0f triangles), %.1f meshes cached (%.0f triangles), %.2f cells built "
                       "(%.0f triangles), %.1f meshes retired; %.2f identities drawn twice",
                       R.cs_ready, R.cs_readytris, R.cs_copied, (double)R.ba.used / 1048576.0,
                       (unsigned long long)(CPOOL_BYTES >> 20), 100.0 * (double)R.wa.used / CACHE_TRIS, R.nret,
                       (double)R.cs_hit_draws / 300, (double)R.cs_hit_tris / 300, (double)R.cs_inst / 300,
                       (double)R.cs_inst_tris / 300, (double)R.cs_new / 300, (double)R.cs_newtris / 300,
                       (double)R.cs_cells / 300, (double)R.cs_celltris / 300, (double)R.cs_ret / 300, (double)R.cs_ambig / 300);
            if (R.cache)
                rt_log("vk: ray tracing geometry v2: capture BLAS refit %.0f%% of frames (builds: new layout %llu, "
                   "due %llu; two BLASes of %u triangles each); retired: drawn twice %llu, hidden %llu, off the screen %llu, moving %llu, no triangle %llu, "
                   "shadow mesh in view or far %llu, shape changed %llu, never drawn %llu; second passes in a frame %llu; G-buffer draws captured: no entry %llu, new %llu, drawn twice %llu, "
                   "not drawn in the last two frames %llu, moving %llu, empty %llu, over budget %llu, cached now %llu, waiting for a cell %llu; "
                   "moving meshes: %.1f compared a frame, "
                       "%llu found moving, %u known; shadow passes: %.1f cascade cameras a frame, %.2f meshes cached a frame "
                       "(%.0f triangles), %u ready (%u triangles), %.2f a frame retired for coming into view; compaction: %llu "
                       "cells, %.1f MiB saved", 100.0 * (double)R.st_refit / 300, (unsigned long long)R.st_why[0],
                       (unsigned long long)R.st_why[2], R.scene_cap, (unsigned long long)R.st_retire[0],
                       (unsigned long long)R.st_retire[1], (unsigned long long)R.st_retire[2], (unsigned long long)R.st_retire[3],
                       (unsigned long long)R.st_retire[4], (unsigned long long)R.st_retire[5], (unsigned long long)R.st_retire[6],
                       (unsigned long long)R.st_retire[7], (unsigned long long)R.st_dup,
                       (unsigned long long)R.st_cap[0], (unsigned long long)R.st_cap[1], (unsigned long long)R.st_cap[2],
                       (unsigned long long)R.st_cap[3], (unsigned long long)R.st_cap[4], (unsigned long long)R.st_cap[5],
                       (unsigned long long)R.st_cap[6], (unsigned long long)R.st_cap[7], (unsigned long long)R.st_cap[8],
                       (double)R.cs_vfy / 300,
                       (unsigned long long)R.cs_anim, R.cs_animn, (double)R.cs_scams / 300, (double)R.cs_snew / 300,
                       (double)R.cs_snewtris / 300, R.cs_sready, R.cs_sreadytris, (double)R.cs_sinview / 300,
                       (unsigned long long)R.cs_compacted, (double)R.cs_saved / 1048576.0);
            R.cs_hit_draws = R.cs_hit_tris = R.cs_new = R.cs_newtris = R.cs_ret = R.cs_ambig = 0;
            R.cs_inst = R.cs_inst_tris = R.cs_cells = R.cs_celltris = 0;
            rt_log("vk: ray tracing mesh cache: G-buffer meshes not drawn in the view but kept while the sun's cascades draw them: %.1f a frame",
                   (double)R.cs_skept / 300);
            R.cs_vfy = R.cs_anim = R.cs_scams = R.cs_snew = R.cs_snewtris = R.cs_sinview = R.st_refit = R.cs_skept = 0;
            R.st_dup = 0, memset(R.st_why, 0, sizeof R.st_why), memset(R.st_retire, 0, sizeof R.st_retire), memset(R.st_cap, 0, sizeof R.st_cap);
            memset(R.t_ms, 0, sizeof R.t_ms), R.t_n = 0;
            R.st_tris = R.st_draws = 0, R.st_max = 0, R.st_over = 0;
        }
    }
    int tlas = R.geom;
    uint32_t islot = R.cache ? R.qframe % 8 : 0, ninst = 1;
    if (R.cache) R.inst_dirty = 1;   // the cells change
    if (R.inst_dirty) {   // the captured scene (identity, mask 1) followed by the cache's cells (customIndex >= 2)
        VkAccelerationStructureInstanceKHR inst = {
            .transform = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}},
            .instanceCustomIndex = 0, .mask = 0,
            .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR};
        if (R.geom) {
            VkAccelerationStructureDeviceAddressInfoKHR ai = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                                                              .accelerationStructure = R.cur ? R.scene2 : R.scene};
            inst.mask = 0x01, inst.accelerationStructureReference = R.as_addr(c->device, &ai);
        }
        VkAccelerationStructureInstanceKHR *dst = (VkAccelerationStructureInstanceKHR *)R.inst.map + (size_t)islot * INST_MAX;
        *dst = inst;
        if (R.cache) ninst += cache_instances(cam, dst + 1);
        vmaFlushAllocation(c->vma, R.inst.mem, (VkDeviceSize)islot * INST_MAX * sizeof *dst, (VkDeviceSize)ninst * sizeof *dst);
        R.inst_dirty = 0;
        tlas = 1;
    }
    if (R.cache) R.cframe++, R.nscam = 0, plat_mutex_unlock(&mesh_lock);
    if (tlas) {
        VkAccelerationStructureGeometryKHR g = inst_geometry(islot);
        build_as(cb, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
                 &g, ninst, R.tlas);
        mem_barrier(cb);
        R.tlas_built = 1;   // the light shaders may bind it from now on (vk_rt_light_tlas)
    }
    if (!R.geom) ts(cb, 1), ts(cb, 2);
trace_only:
    if (!dup) ts(cb, 3);
traced:
    mem_barrier(cb);
    if (dup_ts) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, R.qp, ((R.qframe + 7) % 8) * QN + (uint32_t)dup_ts + 1);
    if (!dup) ts(cb, 4), R.qframe++;
    ex->recorded += 6;
    vk_exec_draw_new_cb(ex);   // the game's compute pipeline and descriptors were replaced: bind them again
}

void vk_rt_release(void) {
    VkCtx *c = vk_ctx();
    if (!c || !R.tried) return;
    cache_release(c);
    mt_release(c);
    if (R.tlas) R.destroy_as(c->device, R.tlas, NULL);
    if (R.blas) R.destroy_as(c->device, R.blas, NULL);
    if (R.scene) R.destroy_as(c->device, R.scene, NULL);
    if (R.scene2) R.destroy_as(c->device, R.scene2, NULL);
    if (R.up_pipe) vkDestroyPipeline(c->device, R.up_pipe, NULL);
    if (R.qp) vkDestroyQueryPool(c->device, R.qp, NULL);
    if (prof_qp) vkDestroyQueryPool(c->device, prof_qp, NULL), prof_qp = VK_NULL_HANDLE;
    if (R.ao_pipe) vkDestroyPipeline(c->device, R.ao_pipe, NULL);
    if (R.inj_pipe) vkDestroyPipeline(c->device, R.inj_pipe, NULL);
    if (R.sh_pipe) vkDestroyPipeline(c->device, R.sh_pipe, NULL);
    if (R.gi_pipe) vkDestroyPipeline(c->device, R.gi_pipe, NULL);
    if (R.tone_pipe) vkDestroyPipeline(c->device, R.tone_pipe, NULL);
    if (R.late_pipe) vkDestroyPipeline(c->device, R.late_pipe, NULL);
    free_buf(c, &R.ghist[0]), free_buf(c, &R.ghist[1]), free_buf(c, &R.glights), free_buf(c, &R.gcache);
    free_buf(c, &R.rhist[0]), free_buf(c, &R.rhist[1]), free_buf(c, &R.glevel), free_buf(c, &R.split);
    if (R.sview) vkDestroyImageView(c->device, R.sview, NULL);
    if (R.simg) vmaDestroyImage(c->vma, R.simg, R.simg_mem);
    if (R.eview) vkDestroyImageView(c->device, R.eview, NULL);
    if (R.pview) vkDestroyImageView(c->device, R.pview, NULL);
    if (R.aview) vkDestroyImageView(c->device, R.aview, NULL);
    if (R.aimg) vmaDestroyImage(c->vma, R.aimg, R.aimg_mem);
    if (R.vview) vkDestroyImageView(c->device, R.vview, NULL);
    if (R.pimg) vmaDestroyImage(c->vma, R.pimg, R.pimg_mem);
    if (R.vimg) vmaDestroyImage(c->vma, R.vimg, R.vimg_mem);
    if (R.eimg) vmaDestroyImage(c->vma, R.eimg, R.eimg_mem);
    if (R.qview) vkDestroyImageView(c->device, R.qview, NULL);
    if (R.qimg) vmaDestroyImage(c->vma, R.qimg, R.qimg_mem);
    if (R.sold_q) vkDestroyImageView(c->device, R.sold_qview, NULL), vmaDestroyImage(c->vma, R.sold_q, R.sold_q_mem);
    if (R.sold) {
        vkDestroyImageView(c->device, R.sold_view, NULL), vmaDestroyImage(c->vma, R.sold, R.sold_mem);
        for (int i = 0; i < 2; i++) vmaDestroyBuffer(c->vma, R.sold_hist[i].buf, R.sold_hist[i].mem);
    }
    if (R.ao_layout) vkDestroyPipelineLayout(c->device, R.ao_layout, NULL);
    if (R.ao_dsl) vkDestroyDescriptorSetLayout(c->device, R.ao_dsl, NULL);
    if (R.up_layout) vkDestroyPipelineLayout(c->device, R.up_layout, NULL);
    if (R.up_dsl) vkDestroyDescriptorSetLayout(c->device, R.up_dsl, NULL);
    RtBuf *bufs[] = {&R.clip, &R.world, &R.aclip, &R.wattr, &R.gsun, &R.gsun_rb, &R.gsun_up, &R.scene_buf, &R.scene_buf2, &R.probe, &R.vb, &R.ib, &R.inst, &R.scratch, &R.blas_buf, &R.tlas_buf,
                     &R.dump_clip, &R.dump_world, &R.hist[0], &R.hist[1], &R.dtab, &R.gtab, &R.camb, &R.shist[0], &R.shist[1], &R.sdump};
    for (size_t i = 0; i < sizeof bufs / sizeof *bufs; i++) free_buf(c, bufs[i]);
    if (R.pipe) vkDestroyPipeline(c->device, R.pipe, NULL);
    if (R.sampler) vkDestroySampler(c->device, R.sampler, NULL);
    if (R.layout) vkDestroyPipelineLayout(c->device, R.layout, NULL);
    if (R.dsl) vkDestroyDescriptorSetLayout(c->device, R.dsl, NULL);
    if (R.as_dsl) vkDestroyDescriptorSetLayout(c->device, R.as_dsl, NULL);
    if (R.pool) vkDestroyDescriptorPool(c->device, R.pool, NULL);
    int rq = R.rq, pf = R.pf, nd = R.null_desc;   // the device's; the settings are read again (read_settings)
    memset(&R, 0, sizeof R);
    R.rq = rq, R.pf = pf, R.null_desc = nd;
}
