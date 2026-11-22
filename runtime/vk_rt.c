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
#include "rt_ao_spv.h"
#include "rt_aoinj_spv.h"
#include "rt_shadow_spv.h"
#include "rt_gi_spv.h"
#include "rt_tone_spv.h"
#include "rt_late_spv.h"
#include "rt_cache_spv.h"
#include "rt_test_spv.h"
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

// TOTK_RT_QUALITY (rt_quality, the menu's "Ray tracing quality"): low, medium, high (the default) or ultra; picks the
// defaults below, each setting still overriding its own. 0 low .. 3 ultra.
static int rt_quality(void) {
    const char *e = cfg_get("TOTK_RT_QUALITY");
    if (!e || !*e) return 2;
    return !strcmp(e, "low") ? 0 : !strcmp(e, "medium") ? 1 : !strcmp(e, "ultra") ? 3 : 2;
}
static float qdef(const char *e, float low, float med, float high, float ultra) {
    if (e && *e) return (float)atof(e);
    const float v[4] = {low, med, high, ultra};
    return v[rt_quality()];
}

// The values TOTK_RT_QUALITY picks (each own setting still overriding): read at start and again when the quality
// changes in play (vk_rt_live). All go to the shaders as push constants, so a change applies at the next frame.
static void read_quality(void) {
    const char *e = cfg_get("TOTK_RT_GI_LEN");
    R.gi_len = qdef(e, 15.0f, 20.0f, 30.0f, 40.0f);
    e = cfg_get("TOTK_RT_REFL_LEN");
    R.refl_len = qdef(e, 30.0f, 40.0f, 60.0f, 80.0f);
    e = cfg_get("TOTK_RT_SHADOW_FAR");   // the farthest pixel given ray-traced shadows (m); they fade over its last 30%
    R.sh_far = qdef(e, 60.0f, 90.0f, 120.0f, 160.0f);
    e = cfg_get("TOTK_RT_SHADOW_RATE");
    R.sh_rate = qdef(e, 2.0f, 2.0f, 1.0f, 1.0f) >= 2.0f ? 2 : 1;   // 1: no checkerboard (it doubled the lag of moving shadows)
    e = cfg_get("TOTK_RT_AO_RAYS");
    R.ao_rays = (int)qdef(e, 1.0f, 1.0f, 2.0f, 4.0f);
    if (R.ao_rays < 1) R.ao_rays = 1;
    if (R.ao_rays > 16) R.ao_rays = 16;
    R.qual = rt_quality();
}

// Any ray tracing asked for now: the test, AO, sun shadows or GI (the menu's switches, read again in play)
static int cfg_any_rt(int *ao, int *sh, int *gi) {
    const char *t = cfg_get("TOTK_RT_TEST"), *a = cfg_get("TOTK_RT_AO"), *s = cfg_get("TOTK_RT_SHADOWS"), *g = cfg_get("TOTK_RT_GI");
    int test = t && *t && strcmp(t, "0");
    *ao = a && *a ? strcmp(a, "0") != 0 : test;
    *sh = s && !strcmp(s, "1");
    *gi = g && !strcmp(g, "1");
    return test || (a && !strcmp(a, "1")) || *sh || *gi;
}

static void read_settings(void) {
    if (R.read) return;
    const char *e = cfg_get("TOTK_RT_TEST"), *a = cfg_get("TOTK_RT_AO"), *sh = cfg_get("TOTK_RT_SHADOWS");
    // TOTK_RT_AO=1 (totk.ini [display] rt_ao, the menu's "Ray-traced AO") turns ray tracing on for the AO alone;
    // TOTK_RT_SHADOWS=1 (rt_shadows, "Ray-traced sun shadows") for the sun shadows
    int test = e && *e && strcmp(e, "0");
    R.test = test;
    R.shadows = sh && !strcmp(sh, "1");
    e = cfg_get("TOTK_RT_GI");   // rt_gi, the menu's "Ray-traced global illumination": one-bounce GI after the composite
    R.gi = e && !strcmp(e, "1");
    e = cfg_get("TOTK_RT_LIGHTS");   // rt_lights: the game's local lights get ray-traced shadows (needs a scene)
    R.lights = e && !strcmp(e, "1");
    R.wanted = test || (a && !strcmp(a, "1")) || R.shadows || R.gi;
    read_quality();
    e = cfg_get("TOTK_RT_GI_CACHE");     // 0: off-screen GI hits bring nothing (no world light cache)
    R.gcache_on = !(e && !strcmp(e, "0"));
    e = cfg_get("TOTK_RT_GI_STRENGTH");
    R.gi_strength = e && *e ? (float)atof(e) / 100.0f : 3.0f;
    e = cfg_get("TOTK_RT_GI_FAR");
    R.gi_far = e && *e ? (float)atof(e) : 80.0f;
    e = cfg_get("TOTK_RT_GI_LIGHT");   // lit brightness from which a pixel counts as a light (aimed GI rays)
    R.gi_thr = e && *e ? (float)atof(e) : 0.3f;   // 0.3: the Zora crystals and lanterns (view 23)
    if (!(R.gi_thr > 0.01f)) R.gi_thr = 0.3f;
    e = cfg_get("TOTK_RT_GI_EMIT");   // how much stronger glowing surfaces light their surroundings (1: as seen)
    // 8 (x the GI strength): small glowing sources (lamps, crystals); large ones get their glow as it is (rt_gi.comp
    // glow_gain: boosted, the shrine doorway's glowing frame lit its surroundings 24 x as bright as itself)
    R.gi_emit = e && *e ? (float)atof(e) : 8.0f;
    e = cfg_get("TOTK_RT_GI_SAT");   // percent saturation of the bounce light (100 as traced)
    R.gi_sat = e && *e ? (float)atof(e) / 100.0f : 1.5f;   // a little boosted by default (the menu's look)
    e = cfg_get("TOTK_RT_GI_AMB");   // percent: how much the GI's sky visibility dims the game's ambient (needs the AO on)
    R.gi_amb = (float)(e && *e ? atoi(e) : 100) / 100.0f;
    if (R.gi_amb < 0) R.gi_amb = 0;
    if (R.gi_amb > 1) R.gi_amb = 1;
    e = cfg_get("TOTK_RT_REFL");   // percent strength of the GI's ray-traced reflections (0: none)
    R.refl = qdef(e, 0.0f, 100.0f, 100.0f, 100.0f) / 100.0f;
    if (!(R.refl >= 0.0f)) R.refl = 0.0f;
    e = cfg_get("TOTK_RT_REFL_ROUGH");   // percent: how far the reflection rays scatter around the mirror direction
    R.refl_rough = e && *e ? (float)atof(e) / 100.0f : 0.02f;
    e = cfg_get("TOTK_RT_REFL_F0");   // percent reflectance of floors facing them (Schlick's F0; walls keep 4, stone's)
    R.refl_f0 = e && *e ? (float)atof(e) / 100.0f : 0.30f;
    e = cfg_get("TOTK_RT_REFL_FRAMES");
    R.refl_frames = e && *e ? atoi(e) : 8;
    if (R.refl_frames < 1) R.refl_frames = 1;
    e = cfg_get("TOTK_RT_TONE");   // percent: the night tone (moonlit blue like the UltraCam bridge shots), with the GI
    R.tone = e && *e ? (float)atof(e) / 100.0f : 1.0f;
    if (!(R.tone >= 0.0f)) R.tone = 0.0f;
    R.tone_g[0] = 0.85f, R.tone_g[1] = 0.65f, R.tone_g[2] = 0.75f;   // fitted to the UltraCam RTGI shot
    e = cfg_get("TOTK_RT_TONE_GAIN");   // "r,g,b" percent at full night
    if (e && *e) {
        float g[3];
        if (sscanf(e, "%f,%f,%f", &g[0], &g[1], &g[2]) == 3) R.tone_g[0] = g[0] / 100.0f, R.tone_g[1] = g[1] / 100.0f, R.tone_g[2] = g[2] / 100.0f;
    }
    e = cfg_get("TOTK_RT_TONE_NIGHT");   // lighting level (lit colour / albedo, mean) at and below which the tone is full
    R.tone_night = e && *e ? (float)atof(e) : 1.5f;   // the Zora bridge at night: 0.8
    e = cfg_get("TOTK_RT_TONE_SKY_NIGHT");   // the sky's brightness at and below which it is night
    R.sky_night = e && *e ? (float)atof(e) : 0.06f;   // Zora bridge 3 AM: 0.03-0.05
    e = cfg_get("TOTK_RT_TONE_SKY_DAY");     // and at and above which it is day
    R.sky_day = e && *e ? (float)atof(e) : 0.15f;     // Kakariko 1 PM overcast: 0.19-0.64
    e = cfg_get("TOTK_RT_TONE_DAY");     // and at and above which there is none
    R.tone_day = e && *e ? (float)atof(e) : 4.0f;       // noon there: 5.9
    e = cfg_get("TOTK_RT_REFL_BLUR");   // a-trous steps after the temporal pass (0-2; roughness-gated)
    R.refl_blur = e && *e ? atoi(e) : 2;
    if (R.refl_blur < 0) R.refl_blur = 0;
    if (R.refl_blur > 2) R.refl_blur = 2;
    e = cfg_get("TOTK_RT_VIEW");
    R.view = e ? atoi(e) : 0;
    if (R.view < 0 || R.view > 33 || (R.view > 17 && R.view < 20)) R.view = 0;   // 20-25, 28: the GI, reflection and material views; 29, 30 step 6
    // 27 (debug): split screen, the game on the left, the G-buffer's albedo target (x 3) on the right
    // 26: split screen, the game on the left, the ray-traced scene lit from its textures on the right (materials step 4)
    // 15: the captured scene's albedo UVs by primary rays (materials step 2); 16: its albedo textures (step 3)
    R.ao = a && *a ? strcmp(a, "0") != 0 : test;   // with the test, on unless TOTK_RT_AO=0
    e = cfg_get("TOTK_RT_SHADOW_LEN");   // how far a shadow ray looks for a caster (m)
    R.sh_len = e && *e ? (float)atof(e) : 300.0f;
    e = cfg_get("TOTK_RT_SUN_DEG");      // the sun's apparent radius (degrees; the real one is 0.27): the penumbra
    float deg = e && *e ? (float)atof(e) : 0.5f;
    R.sh_tan = tanf((deg > 0.01f ? deg : 0.01f) * 3.14159265f / 180.0f);
    e = cfg_get("TOTK_RT_SHADOW_STRENGTH");
    R.sh_pct = e && *e ? atoi(e) : 100;
    if (R.sh_pct < 0) R.sh_pct = 0;
    if (R.sh_pct > 100) R.sh_pct = 100;
    { const char *e = cfg_get("TOTK_RT_SHADOW_LIT"); R.sh_lit = e && *e == '1'; }
    R.sh_nogame = cfg_bool("TOTK_RT_SHADOW_ONLY") != 0;   // the game's shadow map not kept for foliage and cut-outs either
    e = cfg_get("TOTK_RT_SHADOW_WIDE");  // the wide cone, in sun radii, that tells solid casters from the rest
    R.sh_wide = e && *e ? (float)atof(e) : 6.0f;
    e = cfg_get("TOTK_RT_AO_RADIUS");
    R.ao_radius = e && *e ? (float)atof(e) : 3.0f;
    if (!(R.ao_radius > 0.05f)) R.ao_radius = 3.0f;
    e = cfg_get("TOTK_RT_AO_STRENGTH");
    int as = e && *e ? atoi(e) : 100;
    R.ao_strength = (float)(as < 0 ? 0 : as > 200 ? 200 : as) / 100.0f;   // 0 the game's AO, 1 ours, 2 ours squared
    e = cfg_get("TOTK_RT_AO_INJECT");
    R.ao_inject = e && *e ? atoi(e) : 1;
    e = cfg_get("TOTK_RT_AO_FAR");
    R.ao_far = e && *e ? (float)atof(e) : 150.0f;
    e = cfg_get("TOTK_RT_STRENGTH");
    int s = e && *e ? atoi(e) : 60;
    R.strength = s == -1 ? -0.01f : (float)(s < 0 ? 0 : s > 100 ? 100 : s) / 100.0f;   // -1 (debug): view 3 colours the casters by place
    e = cfg_get("TOTK_RT_BRIGHT");
    R.bright = e && *e ? (float)atof(e) : 1.0f;
    e = cfg_get("TOTK_RT_SHADOW_DUMP");
    R.sdump_every = e && *e ? (uint32_t)atoi(e) : 0;
    e = cfg_get("TOTK_RT_DUMPGEOM");
    R.dump_at = e && *e ? atoi(e) : 0;
    e = cfg_get("TOTK_RT_EDGE");
    R.max_edge = e && *e ? (float)atof(e) : 100.0f;
    e = cfg_get("TOTK_RT_RADIUS");
    R.max_dist = e && *e ? (float)atof(e) : R.ao || R.shadows ? 200.0f : 2000.0f;   // AO needs only what is near the pixels it shades
    e = cfg_get("TOTK_RT_VIS");
    R.vis = !(e && !strcmp(e, "0"));
    e = cfg_get("TOTK_RT_SUN_SRC");
    R.sun_src = e && *e ? atoi(e) : 0;
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
    e = cfg_get("TOTK_RT_SHADOW_TEST");
    R.shadow_test = R.shadow && e && *e && strcmp(e, "0");
    e = cfg_get("TOTK_RT_SHADOW_RADIUS");
    // AO: 40 m cost 0.1 ms of trace more (Kakariko); sun shadows need casters off the screen farther out
    R.sr_env = e && *e;
    R.shadow_radius = R.sr_env ? (float)atof(e) : R.shadows ? 80.0f : R.ao ? 15.0f : R.max_dist;   // AO: 40 m cost 0.1 ms of trace more (Kakariko)
    e = cfg_get("TOTK_RT_COMPACT");
    R.compact = R.cache && !(e && !strcmp(e, "0"));
    e = cfg_get("TOTK_RT_SPHERE");
    R.sphere = e && *e ? strcmp(e, "0") != 0 : !R.geom;
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
int vk_rt_capable(void) { read_settings(); return R.rq; }

// The menu's switches read again in play (at most every 250 ms, from vk_rt_on): all off stops ray tracing (the capture
// and every pass, the game's own AO and shadows back); one on starts it, set up the first time (init, vk_rt_exec).
// Test: TOTK_RT_LIVE_SET="20:RT_AO=0,RT_GI=0;30:RT_QUALITY=low" sets those (TOTK_ prefixed, cfg_test_set: never
// saved) that many seconds after the first poll, as the menu would in play
static void live_test(uint64_t now) {
    static uint64_t t0;
    static const char *at;
    if (!t0) {
        t0 = now, at = cfg_get("TOTK_RT_LIVE_SET");
        if (at && *at) rt_log("vk: ray tracing live test: %s", at);
    }
    while (at && *at) {
        char *end;
        double s = strtod(at, &end);
        if (end == at || *end != ':' || now - t0 < (uint64_t)(s * 1000.0)) return;
        at = end + 1;
        while (*at && *at != ';') {
            char k[48] = "TOTK_", v[32];
            size_t kn = 5, vn = 0;
            while (*at && *at != '=' && *at != ';' && kn < sizeof k - 1) k[kn++] = *at++;
            k[kn] = 0;
            if (*at == '=') at++;
            while (*at && *at != ',' && *at != ';' && vn < sizeof v - 1) v[vn++] = *at++;
            v[vn] = 0;
            if (*at == ',') at++;
            cfg_test_set(k, v);
            rt_log("vk: ray tracing live test at %.1f s: %s=%s", (now - t0) / 1000.0, k, v);
        }
        if (*at == ';') at++;
    }
}

static void rt_poll(void) {
    static uint64_t next;   // draw threads call this too: one of them takes each 250 ms turn
    uint64_t now = plat_now_ms(), n = __atomic_load_n(&next, __ATOMIC_RELAXED);
    if (now < n || !__atomic_compare_exchange_n(&next, &n, now + 250, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return;
    live_test(now);
    int ao, sh, gi, w = cfg_any_rt(&ao, &sh, &gi);
    if (w == R.wanted) return;
    R.wanted = w;
    if (!w) {
        __atomic_store_n(&R.simg_ready, 0, __ATOMIC_RELEASE);
        R.ao = R.shadows = R.gi = 0, R.prev_ok = R.sprev_ok = R.gprev_ok = R.rhist_ok = 0;
    } else if (!R.tried) {
        R.ao = ao, R.shadows = sh, R.gi = gi;   // init keeps what it can run
    }   // else vk_rt_live switches each feature within 15 G-buffers
    rt_log("vk: ray tracing switched %s in play", w ? "on" : "off");
}

// gfx_shader.c patch_rt_shadow: the lighting shaders read the ray-traced sun shadow (asked as shaders load)
// any ray tracing on at start: the lighting shaders are patched, so the sun shadows can be switched on in play (with
// the image not ready they read a blank: the game's own shadow, exactly)
int vk_rt_shadow_patch_wanted(void) { read_settings(); return R.rq; }

// gfx_shader.c patch_rt_lights: the light shaders trace a ray query toward their light, against R.tlas bound by the
// replay. Needs null acceleration structure descriptors (the binding while the shadows are off or before the first
// scene) and push descriptor templates, which carry the handle (vk_exec_draw.c).
void vk_rt_device_null(int null_desc) { R.null_desc = null_desc; }
int vk_rt_light_patch_wanted(void) {
    read_settings();
    int ok = R.rq && R.null_desc && vk_pipeline_push_tpl_fn();
    static int logged;
    if (R.rq && !logged++)
        rt_log("vk: ray-traced light shadows %s", ok ? "available" : "unavailable (needs null descriptors and push descriptor templates)");
    return ok;
}
uint64_t vk_rt_light_tlas(void) {
    return R.ok && R.lights && R.tlas_built && (R.ao || R.shadows || R.gi) ? (uint64_t)(uintptr_t)R.tlas : 0;
}

// vk_contact.c: a shadow pass runs before the half-resolution lighting (shown normally or as views 9-11)
int vk_rt_shadows(void) { read_settings(); return R.wanted && R.rq && R.shadows && (R.view == 0 || (R.view >= 9 && R.view != 15 && R.view != 16 && R.view != 17)); }

// vk_exec_draw.c: the descriptor of the shadow image for a patched lighting shader; 0 until it has been written
// w, h: the pass the patched shader draws in. The image holds the main view's half-resolution pixels; a pass of another
// size (a reflection or other view drawn by the same lighting shader) gets the blank, i.e. the game's own shadow.
int vk_rt_shadow_desc(VkDescriptorImageInfo *o, uint32_t w, uint32_t h) {
    if (R.shadows && R.qview && __atomic_load_n(&R.simg_ready, __ATOMIC_ACQUIRE) && w == R.qw && h == R.qh) {
        *o = (VkDescriptorImageInfo){R.sampler, R.qview, VK_IMAGE_LAYOUT_GENERAL};
        return 1;
    }
    if (!R.shadows || !__atomic_load_n(&R.simg_ready, __ATOMIC_ACQUIRE) || w != R.sw || h != R.sh) {
        static volatile int32_t n_other;
        if (__atomic_load_n(&R.simg_ready, __ATOMIC_ACQUIRE) && __atomic_add_fetch(&n_other, 1, __ATOMIC_RELAXED) <= 3)
            rt_log("vk: ray traced sun shadows: a %ux%u pass drew with a patched lighting shader; the game's own shadow there "
                   "(the image is %ux%u)", w, h, R.sw, R.sh);
        return 0;
    }
    *o = (VkDescriptorImageInfo){R.sampler, R.sview, VK_IMAGE_LAYOUT_GENERAL};
    return 1;
}

// debug: per lighting pass that reads the shadow image, its camera block (fragment c4) against the newest main camera
void vk_rt_note_shadow_use(uint32_t w, uint32_t h, const float *blk) {
    static unsigned n, nq, nh, nqoff, nhoff, nnob;
    static float qgap, hgap;
    int quarter = w == R.qw && h == R.qh;
    n++;
    if (!blk) nnob++;
    else if (R.cam_n) {
        const float *m = R.cam[(R.cam_n - 1) % 16];
        float d = 0;
        for (int i = 12; i < 28; i++) d = fmaxf(d, fabsf(blk[i] - m[i]));
        if (quarter) { nq++; if (d > 1e-4f) nqoff++; if (d > qgap) qgap = d; }
        else { nh++; if (d > 1e-4f) nhoff++; if (d > hgap) hgap = d; }
    }
    if (n % 2000 == 0)
        rt_log("vk: shadow image reads: half-res %u (other camera %u, gap %.3f), quarter-res %ux%u %u (other camera %u, gap %.3f), no block %u",
               nh, nhoff, (double)hgap, R.qw, R.qh, nq, nqoff, (double)qgap, nnob), n = nq = nh = nqoff = nhoff = nnob = 0, qgap = hgap = 0;
}

void vk_rt_device_caps(int rq, int pos_fetch) {
    R.rq = rq, R.pf = pos_fetch;
    rt_log("vk: ray tracing %s (position fetch %d, not needed)",
           rq ? "enabled" : "unavailable on this device (needs acceleration structures, ray queries, buffer device "
                            "addresses and transform feedback): the rt_* settings do nothing", pos_fetch);
}

int vk_rt_on(void) {
    read_settings();
    if (R.rq) rt_poll();
    return R.wanted && R.rq;
}

int vk_rt_note_camera(const float *c4) {
    int k = (int)(R.cam_n++ % 16);
    memcpy(R.cam[k], c4, sizeof R.cam[k]);
    memset(&R.aot[k], 0, sizeof R.aot[k]);
    return k;
}

// vk_contact.c: the game's AO target for the pass of camera slot k (planned before the composite)
void vk_rt_note_aot(int k, const VkExecTarget *t) { R.aot[k & 15] = *t; }
void vk_rt_note_alb(int k, const VkExecTarget *t) { R.alb[k & 15] = *t; }
void vk_rt_note_mtl(int k, const VkExecTarget *mtl, const VkExecTarget *mid) { R.mtl[k & 15] = *mtl, R.mid[k & 15] = *mid; }

// vk_contact.c: a GI pass is due after the composite
// The night tone on the finished scene: vk_dlaa.c calls this where the scene ends (after the transparents, fog and
// haze, before the bloom), on the light target in GENERAL layout; the GI pass then leaves the tone out for a few frames.
// Only the R11G11B10F light target is handled; with another format (or without DLAA) the GI pass keeps the tone.
static void mem_barrier(VkCommandBuffer cb);
static void late_measure(VkCommandBuffer cb, VkImageView v, uint32_t w, uint32_t h, int mode);
static int veil_images(VkCtx *c, VkCommandBuffer cb, uint32_t w, uint32_t h);
static int added_image(VkCtx *c, VkCommandBuffer cb, uint32_t w, uint32_t h);
static void late_veil(VkCommandBuffer cb, VkImageView v, uint32_t w, uint32_t h);
int vk_rt_tone_scene(VkCommandBuffer cb, const VkTexImage *img, VkFormat fmt, uint32_t level, uint32_t layer, uint32_t w,
                     uint32_t h) {
    if (!R.gi || !R.gi_ok || fmt != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || !img) return 0;
    int tone = R.tone_pipe && R.tone_eff >= 0.002f;
    if (!tone && !R.meas_pending && !R.split_pending && !R.post_ok) return 0;
    VkImageView v = vk_exec_target_view(img, fmt, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1, 0);
    if (!v) return 0;
    if (R.post_ok || tone) vk_texture_storage_written(img);   // the veil and the tone write the scene colour
    if (R.post_ok) late_veil(cb, v, w, h);   // before the measure and the tone
    if (R.meas_pending) late_measure(cb, v, w, h, 0);   // before the tone, which goes over all of it
    if (R.split_pending) late_measure(cb, v, w, h, 1);   // then view 26's half
    if (!tone) return 0;
    VkDescriptorImageInfo oi = {VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi};
    struct { float tone[4]; int32_t size[4]; } pc = {{R.tone_g[0], R.tone_g[1], R.tone_g[2], R.tone_eff}, {(int32_t)w, (int32_t)h, 0, 0}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.tone_pipe);
    rec_push(R.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 0, 1, &wr);
    vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    mem_barrier(cb);
    R.tone_late = 3, R.st_tone_late++;
    return 1;
}

int vk_rt_gi(void) { read_settings(); return R.wanted && R.rq && R.gi && (!R.tried || R.gi_ok); }

// The AO goes into the game's AO target (the pass then runs before the composite): AO on, shown normally (view 0)
// (the sun shadow views 9-14 show the AO as normal)
int vk_rt_ao_inject(void) { read_settings(); return R.wanted && R.ao && (R.view == 0 || (R.view >= 9 && R.view != 15 && R.view != 16 && R.view != 17)) && R.ao_inject > 0; }

// host: 1 mapped for writing (may be write-combined: never read it on the CPU), 2 mapped and cached for reading back
// Phase 8: what the ray tracing allocates, by the source line that made it (vk_rt_mem_log): device-local and host
#define MEM_SITES 96
static struct { int line; uint32_t n; VkDeviceSize dev, host; } mem_site[MEM_SITES];
static void mem_note(int line, VkDeviceSize size, int host) {
    for (int i = 0; i < MEM_SITES; i++) {
        if (mem_site[i].line && mem_site[i].line != line) continue;
        mem_site[i].line = line, mem_site[i].n++;
        if (host) mem_site[i].host += size; else mem_site[i].dev += size;
        return;
    }
}
static void mem_log_use(void);
static void mem_log(const char *when) {
    VkDeviceSize dev = 0, host = 0;
    for (int i = 0; i < MEM_SITES && mem_site[i].line; i++) dev += mem_site[i].dev, host += mem_site[i].host;
    rt_log("vk: ray tracing memory (%s): %.1f MiB device-local, %.1f MiB host-visible (buffers and acceleration "
           "structures; images not counted)", when, dev / 1048576.0, host / 1048576.0);
    mem_log_use();
    for (int i = 0; i < MEM_SITES && mem_site[i].line; i++)
        if (mem_site[i].dev + mem_site[i].host >= (4u << 20))
            rt_log("vk:   vk_rt.c:%d  %.1f MiB%s in %u", mem_site[i].line, (mem_site[i].dev + mem_site[i].host) / 1048576.0,
                   mem_site[i].host ? " (host)" : "", mem_site[i].n);
}
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

static VkAccelerationStructureGeometryKHR sphere_geometry(void) {
    return (VkAccelerationStructureGeometryKHR){
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
        .flags = VK_GEOMETRY_OPAQUE_BIT_KHR,
        .geometry.triangles = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                               .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, .vertexData.deviceAddress = R.vb.addr,
                               .vertexStride = 12, .maxVertex = SPH_NV - 1, .indexType = VK_INDEX_TYPE_UINT32,
                               .indexData.deviceAddress = R.ib.addr}};
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
static int ao_layout_init(VkCtx *c) {
    if (R.ao_layout) return 1;
    if (c->props.limits.maxPushConstantsSize < AO_PUSH) {
        rt_log("vk: ray traced AO and shadows off: the device takes %u bytes of push constants, they need %u",
               c->props.limits.maxPushConstantsSize, AO_PUSH);
        return 0;
    }
    VkDescriptorSetLayoutBinding b[17] = {
        {.binding = 16, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},   // the veil (late_veil), for the GI
        {.binding = 15, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},   // the G-buffer's glow (emit_copy), the GI's lights
        {.binding = 14, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},   // the G-buffer's target 0 (shading class), for the GI
        {.binding = 13, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},   // the G-buffer's target 3 (smoothness in alpha), for the GI
        {.binding = 12, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},   // view 26's right half for the late hook (split)
        {.binding = 11, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},   // materials: the triangles' UVs and texture words (wattr)
        {.binding = 10, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},   // the GI's sun: its measurement and the sun and sky colours (gsun)
        {.binding = 9, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},   // the GI's light cache
        {.binding = 8, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},   // the GI's light list
        {.binding = 7, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},   // the GI's albedo
        {.binding = 6, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 4, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 5, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = (uint32_t)(sizeof b / sizeof *b), .pBindings = b};
    VkDescriptorSetLayout sets[3];
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, AO_PUSH};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = MT.dsl ? 3 : 2,
                                     .pSetLayouts = sets, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &R.ao_dsl) != VK_SUCCESS ||
        (sets[0] = R.ao_dsl, sets[1] = R.as_dsl, sets[2] = MT.dsl, 0) ||
        vkCreatePipelineLayout(c->device, &li, NULL, &R.ao_layout) != VK_SUCCESS) {
        R.ao_layout = VK_NULL_HANDLE;
        rt_log("vk: ray traced AO and shadows off: their pipeline layout could not be created");
        return 0;
    }
    return 1;
}

static int ao_init(VkCtx *c) {
    if (!ao_layout_init(c)) return 0;
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof rt_ao_spv,
                                   .pCode = rt_ao_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) {
        rt_log("vk: ray traced AO off: its pipeline could not be created");
        return 0;
    }
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = R.ao_layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &R.ao_pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) { rt_log("vk: ray traced AO off: its pipeline could not be created (%d)", (int)r); return 0; }
    VkShaderModuleCreateInfo imi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof rt_aoinj_spv,
                                    .pCode = rt_aoinj_spv};
    if (vkCreateShaderModule(c->device, &imi, NULL, &m) == VK_SUCCESS) {
        ci.stage.module = m;
        if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &R.inj_pipe) != VK_SUCCESS) R.inj_pipe = VK_NULL_HANDLE;
        vkDestroyShaderModule(c->device, m, NULL);
    }
    if (!R.inj_pipe) R.ao_inject = 0;
    R.hist_size = (VkDeviceSize)960 * 540 * 16;   // half-res 1080p; hist_fit grows it
    if (!make_buf(c, &R.hist[0], R.hist_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0) ||
        !make_buf(c, &R.hist[1], R.hist_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0)) {
        rt_log("vk: ray traced AO off: no memory for its history");
        return 0;
    }
    rt_log("vk: ray traced AO on: %d rays of %.2f m per half-res pixel, strength %.2f, up to %.0f m, view %d, %s", R.ao_rays,
           R.ao_radius, R.ao_strength, R.ao_far, R.view,
           R.ao_inject == 1 ? "replaces the game's AO" : R.ao_inject == 2 ? "multiplies the game's AO"
           : R.ao_inject >= 3 ? "debug: the game's AO set to a constant" : "darkens the composite");
    return 1;
}

static void mem_barrier(VkCommandBuffer cb);
static int cache_init(VkCtx *c);
static int mt_init(VkCtx *c);   // materials: the albedo texture table
static void mprobe_log(const uint32_t *p);
static void haze_fit(const uint32_t *f);
// gsun (rt_gi.comp binding 10): ssum[32], shist[128], mprobe[64] (uvec4), then the sun's direction and the colours
#define GSUN_PRM (4 * (32 + 128 + 384 + 256))
#define GSUN_PRM_SZ (64 + 512)   // sun, sky, probe box; the haze (rt_gi.comp hz)
#define HZ_D0 16.0
static double lumd(const double *c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }
static double lumd3f(const float *c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }
// the median of a 64-bin histogram's part above value lo (4 bins a stop from 2^-6); -1 when that part holds less than
// share of it
static double hist_median_above(const double *h, double lo, double share) {
    double b0 = lo > 0.0 ? (log2(lo) + 6.0) * 4.0 : 0.0, tot = 0.0, part = 0.0, c = 0.0;
    if (b0 < 0.0) b0 = 0.0;
    for (int i = 0; i < 64; i++) {
        double w = i + 1 <= b0 ? 0.0 : i >= b0 ? 1.0 : i + 1 - b0;   // the bin's share above lo
        tot += h[i], part += h[i] * w;
    }
    if (tot < 50.0 || part < share * tot) return -1.0;
    for (int i = 0; i < 64; i++) {
        double w = i + 1 <= b0 ? 0.0 : i >= b0 ? 1.0 : i + 1 - b0, x = h[i] * w;
        if (c + x >= part * 0.5) return exp2((i + 1 - w + (part * 0.5 - c) / (h[i] > 1e-9 ? h[i] : 1e-9)) / 4.0 - 6.0);
        c += x;
    }
    return -1.0;
}
// a 64-bin histogram's median (4 bins a stop from 2^-6), interpolated in its bin; -1 below 50 samples
static double hist_median(const double *h) {
    double tot = 0.0, c = 0.0;
    for (int i = 0; i < 64; i++) tot += h[i];
    if (tot < 50.0) return -1.0;
    for (int i = 0; i < 64; i++) {
        if (c + h[i] >= tot * 0.5) return exp2((i + (tot * 0.5 - c) / (h[i] > 1e-9 ? h[i] : 1e-9)) / 4.0 - 6.0);
        c += h[i];
    }
    return -1.0;
}
static void mt_cal_clear(VkCommandBuffer cb);   // the measured colours cleared once

// Which way the clip space's x and y run on the screen (uv = 0.5 + 0.5 * (sx, sy) * ndc): a view-space point up and to
// the right of the centre, through the camera block u (rows 3-6 the view-projection) and the inverse view rows iv.
// The camera block's near plane (c4[14].x, 0.1 m in play): the G-buffer's linear depth is (view distance - near) / far,
// so a depth times far alone put every surface that much nearer the camera than it is (shadow rays started off the
// surface by a distance that changed with the view, docs/raytracing.md). 0 when the block holds no sensible value.
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
static void ao_exec(VkCommandBuffer cb, const VkExecJob *j, const float *u, const float *iv, const float *cam, VkImageView in,
                    VkImageView out, const VkExecTarget *aot) {
    uint32_t w = j->scene.width, h = j->scene.height, hw = (w + 1) / 2, hh = (h + 1) / 2;
    if (!hist_fit(R.hist, &R.hist_size, (VkDeviceSize)hw * hh * 16, &R.prev_ok)) return;
    const float *vp = u + 3 * 4;   // rows 3-6: clip = row . world
    float sx, sy;
    screen_signs(u, iv, &sx, &sy);
    float dx = cam[0] - R.prev_cam[0], dy = cam[1] - R.prev_cam[1], dz = cam[2] - R.prev_cam[2];
    int ok = R.prev_ok && R.prev_w == w && R.prev_h == h && dx * dx + dy * dy + dz * dz < 100.0f;   // no cut, same size
    float pc[64] = {j->pc[0], j->pc[1], j->pc[2], 0};
    memcpy(pc + 4, iv, 12 * sizeof(float));
    memcpy(pc + 16, R.prev_vp, sizeof R.prev_vp);
    pc[60] = cam_near(u);
    pc[32] = R.ao_radius, pc[33] = R.ao_strength, pc[34] = (float)R.ao_rays, pc[35] = (float)(R.ao_frame++ & 0xffffff);
    pc[36] = (float)w, pc[37] = (float)h, pc[38] = (float)hw, pc[39] = (float)hh;
    pc[40] = ok ? 1.0f : 0.0f, pc[41] = sx, pc[42] = sy, pc[43] = (float)(R.view >= 9 ? 0 : R.view);
    pc[44] = R.ao_far, pc[45] = R.bright;   // TOTK_RT_BRIGHT: views 6 and 7
    VkDescriptorImageInfo ii = {R.sampler, in, VK_IMAGE_LAYOUT_GENERAL}, oi = {VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL};
    // hist[0]: the history (last frame's in, this frame's out); hist[1]: the trace's result before the temporal pass
    VkDescriptorBufferInfo hin = {R.hist[0].buf, 0, VK_WHOLE_SIZE}, hout = {R.hist[1].buf, 0, VK_WHOLE_SIZE},
                           wb = {R.world.buf, 0, VK_WHOLE_SIZE}, gb = {R.gtab.buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr[6] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &hin},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &hout},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 4, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &wb},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 5, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &gb}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_pipe);
    rec_push(R.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 0, 6, wr);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 1, 1, &R.as_set, 0, NULL);
    vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (hw + 7) / 8, (hh + 7) / 8, 1);
    mem_barrier(cb);
    pc[3] = 3.0f;   // temporal: hist[1] -> hist[0]
    hin.buffer = R.hist[1].buf, hout.buffer = R.hist[0].buf;
    rec_push(R.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 0, 6, wr);
    vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (hw + 7) / 8, (hh + 7) / 8, 1);
    mem_barrier(cb);
    VkImageView av = aot ? vk_exec_target_view(aot->img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D,
                                               VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0) : VK_NULL_HANDLE;
    if (av) {   // into the game's AO target, which the composite applies to the ambient light
        VkDescriptorImageInfo ai = {VK_NULL_HANDLE, av, VK_IMAGE_LAYOUT_GENERAL};
        // phase 7: the GI's history (last frame's) for the ambient rebalance; any buffer when there is none
        int gv = R.gi && R.gi_amb > 0.0f && R.gprev_ok && R.gprev_w == w && R.gprev_h == h;
        VkDescriptorBufferInfo gbi = {gv ? R.ghist[0].buf : R.hist[1].buf, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet gw = wr[2];
        gw.pBufferInfo = &gbi;
        VkWriteDescriptorSet iw[3] = {gw, wr[3], {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 6, .descriptorCount = 1,
                                              .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ai}};
        float ipc[44];
        memset(ipc, 0, sizeof ipc);
        ipc[0] = (float)hw, ipc[1] = (float)hh, ipc[2] = gv ? (float)((w + 3) / 4) : 0.0f, ipc[3] = gv ? (float)((h + 3) / 4) : 0.0f;
        ipc[4] = R.ao_strength, ipc[5] = R.ao_far, ipc[6] = (float)R.ao_inject, ipc[7] = R.view == 21 ? -R.gi_amb : R.gi_amb;
        // Reproject each pixel into last frame's GI sky-visibility buffer (ghist[0] was written under R.gprev_vp)
        ipc[8] = j->pc[0], ipc[9] = j->pc[1], ipc[10] = (float)w, ipc[11] = (float)h;
        ipc[12] = sx, ipc[13] = sy, ipc[14] = (gv && R.gprev_ok) ? 1.0f : 0.0f, ipc[15] = 0.0f;
        memcpy(ipc + 16, iv, 12 * sizeof(float));
        memcpy(ipc + 28, R.gprev_vp, 16 * sizeof(float));
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.inj_pipe);
        rec_push(R.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 0, 3, iw);
        vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof ipc, ipc);
        vkCmdDispatch(cb, (hw + 7) / 8, (hh + 7) / 8, 1);
        R.st_inj++;
    } else {
        pc[3] = 1.0f;   // apply: darken the composited scene
        vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
        R.st_noinj++;
    }
    memcpy(R.prev_vp, vp, sizeof R.prev_vp);
    memcpy(R.prev_cam, cam, sizeof R.prev_cam);
    R.prev_w = w, R.prev_h = h, R.prev_ok = 1;
}

// The sun shadow pipeline (rt_shadow.comp, the AO's layout). Its image and histories come with the first pass, sized
// for its half resolution. 0: shadows stay off.
static int shadow_init(VkCtx *c) {
    if (!ao_layout_init(c)) return 0;
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof rt_shadow_spv,
                                   .pCode = rt_shadow_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) {
        rt_log("vk: ray traced sun shadows off: their pipeline could not be created");
        return 0;
    }
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = R.ao_layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &R.sh_pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) {
        R.sh_pipe = VK_NULL_HANDLE;
        rt_log("vk: ray traced sun shadows off: their pipeline could not be created (%d)", (int)r);
        return 0;
    }
    rt_log("vk: ray traced sun shadows ready: sun radius %.3f degrees, wide cone x%.1f, up to %.0f m, rays of %.0f m, casters "
           "off the screen within %.0f m, view %d", atanf(R.sh_tan) * 180.0f / 3.14159265f, R.sh_wide, R.sh_far, R.sh_len,
           R.shadow_radius, R.view);
    return 1;
}

// The shadow image (RGBA8, w x h, storage and sampled) and the histories; 0 when either fails.
static int shadow_image(VkCtx *c, uint32_t w, uint32_t h) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &R.simg, &R.simg_mem, NULL) != VK_SUCCESS) return R.simg = VK_NULL_HANDLE, 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = R.simg,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &R.sview) != VK_SUCCESS) return R.sview = VK_NULL_HANDLE, 0;
    R.qw = (w + 1) / 2, R.qh = (h + 1) / 2;
    ii.extent = (VkExtent3D){R.qw, R.qh, 1};
    if (vmaCreateImage(c->vma, &ii, &ai, &R.qimg, &R.qimg_mem, NULL) != VK_SUCCESS) return R.qimg = VK_NULL_HANDLE, 0;
    vi.image = R.qimg;
    if (vkCreateImageView(c->device, &vi, NULL, &R.qview) != VK_SUCCESS) return R.qview = VK_NULL_HANDLE, 0;
    VkDeviceSize bytes = (VkDeviceSize)w * h * 32;
    if (!make_buf(c, &R.shist[0], bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0) ||
        !make_buf(c, &R.shist[1], bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0))
        return 0;
    R.sw = w, R.sh = h, R.simg_init = 1;
    rt_log("vk: ray traced sun shadows: a %ux%u shadow image for the lighting, %.1f MiB of history", w, h,
           2.0 * (double)bytes / 1048576.0);
    return 1;
}

// The quarter-res copy of the shadow image (linear 2x2 average), after a dispatch wrote the image
static void shadow_quarter(VkCommandBuffer cb) {
    if (!R.qimg) return;
    mem_barrier(cb);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)R.sw, (int32_t)R.sh, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)R.qw, (int32_t)R.qh, 1}}};
    vkCmdBlitImage(cb, R.simg, VK_IMAGE_LAYOUT_GENERAL, R.qimg, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    mem_barrier(cb);
}

// The shadow pass's three dispatches (rt_shadow.comp): trace, temporal, resolve into the shadow image. Runs before the
// game's half-resolution lighting, whose patched shaders read the image (gfx_shader.c patch_rt_shadow).
static void shadow_exec(VkCommandBuffer cb, const VkExecJob *j, const float *u, const float *iv, const float *cam, VkImageView in) {
    VkCtx *c = vk_ctx();
    uint32_t w = j->scene.width, h = j->scene.height, hw = (w + 1) / 2, hh = (h + 1) / 2;
    if (!R.sh_pipe) return;
    if (!R.simg && !shadow_image(c, hw, hh)) {
        rt_log("vk: ray traced sun shadows off: no memory for their image");
        R.sh_pipe = VK_NULL_HANDLE;
        return;
    }
    if (R.sold && R.sh_frame - R.sold_at > 32) {   // the image of the last size, once no frame in flight can use it
        vkDestroyImageView(c->device, R.sold_view, NULL), vmaDestroyImage(c->vma, R.sold, R.sold_mem);
        if (R.sold_q) vkDestroyImageView(c->device, R.sold_qview, NULL), vmaDestroyImage(c->vma, R.sold_q, R.sold_q_mem);
        for (int i = 0; i < 2; i++) vmaDestroyBuffer(c->vma, R.sold_hist[i].buf, R.sold_hist[i].mem);
        R.sold = VK_NULL_HANDLE, R.sold_q = VK_NULL_HANDLE;
    }
    if (hw != R.sw || hh != R.sh) {   // the scene changed size: a new image (the old one is freed 32 passes later)
        __atomic_store_n(&R.simg_ready, 0, __ATOMIC_RELEASE);
        if (R.sold) return;   // the one before that is still waiting
        rt_log("vk: ray traced sun shadows: the scene is now %ux%u, a new image (was %ux%u)", w, h, R.sw * 2, R.sh * 2);
        R.sold = R.simg, R.sold_mem = R.simg_mem, R.sold_view = R.sview, R.sold_at = R.sh_frame;
        R.sold_q = R.qimg, R.sold_q_mem = R.qimg_mem, R.sold_qview = R.qview, R.qview = VK_NULL_HANDLE;
        R.sold_hist[0] = R.shist[0], R.sold_hist[1] = R.shist[1];
        // R.sview is not cleared first: a draw reading it meanwhile still gets a live view (freed 32 passes later)
        memset(&R.shist, 0, sizeof R.shist);
        if (!shadow_image(c, hw, hh)) {
            rt_log("vk: ray traced sun shadows off: no memory for their image");
            R.sh_pipe = VK_NULL_HANDLE;
            return;
        }
        R.sprev_ok = 0;
    }
    if (R.simg_init) {   // to GENERAL, cleared to 0 (weight 0: the game's own shadow) before its first use
        VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = R.simg, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkImageMemoryBarrier2 bq[2] = {b, b};
        bq[1].image = R.qimg;
        VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = bq};
        vkCmdPipelineBarrier2(cb, &d);
        VkClearColorValue z = {{0, 0, 0, 0}};
        VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, R.simg, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr);
        vkCmdClearColorImage(cb, R.qimg, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rr);
        mem_barrier(cb);
        R.simg_init = 0, R.sprev_ok = 0;
    }
    const float *vp = u + 3 * 4;
    float sx, sy;
    screen_signs(u, iv, &sx, &sy);
    float dx = cam[0] - R.sprev_cam[0], dy = cam[1] - R.sprev_cam[1], dz = cam[2] - R.sprev_cam[2];
    int ok = R.sprev_ok && dx * dx + dy * dy + dz * dz < 100.0f;   // no cut
    if (!ok && j->pc[14] < 1.5f && R.sprev_ok) {
        // a camera cut: the first pass would trace last frame's scene, which is somewhere else. This frame's lighting
        // takes the game's own shadow (the retrace keeps the image not ready); the next frame starts a new history.
        __atomic_store_n(&R.simg_ready, 0, __ATOMIC_RELEASE);
        memcpy(R.sprev_vp, vp, sizeof R.sprev_vp);
        memcpy(R.sprev_cam, cam, sizeof R.sprev_cam);
        R.sprev_ok = 0, R.st_cut++;
        return;
    }
    // toward the light, world space: the view-space direction the planner took from the first cascade's depth row
    float L[3], ll = 0;
    for (int i = 0; i < 3; i++) L[i] = iv[i * 4] * j->pc[4] + iv[i * 4 + 1] * j->pc[5] + iv[i * 4 + 2] * j->pc[6], ll += L[i] * L[i];
    ll = sqrtf(ll);
    if (!(ll > 1e-6f)) return;
    if (R.sun_w_ok) {
        float d = (L[0] * R.sun_w[0] + L[1] * R.sun_w[1] + L[2] * R.sun_w[2]) / ll;
        float a = acosf(d > 1.0f ? 1.0f : d < -1.0f ? -1.0f : d) * 57.29578f;
        R.sun_gap_sum += a, R.sun_gap_n++;
        if (a > R.sun_gap_max) R.sun_gap_max = a;
        for (int i = 0; i < 3; i++) R.sun_l[i] = L[i] / ll, R.sun_f[i] = -iv[i * 4 + 2];
        if (R.sun_src == 1) L[0] = R.sun_w[0], L[1] = R.sun_w[1], L[2] = R.sun_w[2], ll = 1.0f;
        if (R.sun_src == 2) L[0] = R.sun_x[0], L[1] = R.sun_x[1], L[2] = R.sun_x[2], ll = 1.0f;
    }
    float pc[64] = {j->pc[0], j->pc[1], j->pc[2], 0};
    memcpy(pc + 4, iv, 12 * sizeof(float));
    memcpy(pc + 16, R.sprev_vp, sizeof R.sprev_vp);
    pc[60] = cam_near(u);
    pc[32] = L[0] / ll, pc[33] = L[1] / ll, pc[34] = L[2] / ll, pc[35] = (float)(R.sh_gframe & 0xffffff);
    R.sh_frame++;   // passes (the old-image timer and dumps)
    if (j->pc[14] < 1.5f) R.sh_gframe++;   // game frames: the checkerboard phase must change once per frame, not per pass
    pc[36] = (float)w, pc[37] = (float)h, pc[38] = (float)hw, pc[39] = (float)hh;
    pc[40] = ok ? 1.0f : 0.0f, pc[41] = sx, pc[42] = sy, pc[43] = (float)((R.view >= 15 ? 0 : R.view) + 16 * R.sh_rate + 64 * R.sh_pct + 8192 * R.sh_nogame + 16384 * R.sh_lit);
    pc[44] = R.sh_far, pc[45] = R.sh_len, pc[46] = R.sh_tan, pc[47] = R.sh_wide;
    VkDescriptorImageInfo ii = {R.sampler, in, VK_IMAGE_LAYOUT_GENERAL}, oi = {VK_NULL_HANDLE, R.sview, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo hin = {R.shist[0].buf, 0, VK_WHOLE_SIZE}, hout = {R.shist[1].buf, 0, VK_WHOLE_SIZE},
                           wb = {R.world.buf, 0, VK_WHOLE_SIZE}, gb = {R.gtab.buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr[6] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &hin},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &hout},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 4, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &wb},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 5, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &gb},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 6, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi}};
    uint32_t gx = (hw + 7) / 8, gy = (hh + 7) / 8;
    if (j->pc[14] > 1.5f) {   // the frame's second lighting pass: retrace the pixels drawn since the first (mode 5)
        pc[3] = 5.0f;
        hin.buffer = R.shist[1].buf, hout.buffer = R.shist[0].buf;   // shist[0]: the first pass's depths
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.sh_pipe);
        rec_push(R.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 0, 6, wr);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 1, 1, &R.as_set, 0, NULL);
        vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        vkCmdDispatch(cb, gx, gy, 1);
        shadow_quarter(cb);
        R.st_retrace++;
        return;
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.sh_pipe);
    rec_push(R.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 0, 6, wr);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 1, 1, &R.as_set, 0, NULL);
    vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, gx, gy, 1);   // trace: shist[0] (last frame's) -> shist[1]
    mem_barrier(cb);
    pc[3] = 3.0f;                   // temporal: shist[1] -> shist[0]
    hin.buffer = R.shist[1].buf, hout.buffer = R.shist[0].buf;
    rec_push(R.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 0, 6, wr);
    vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, gx, gy, 1);
    mem_barrier(cb);
    pc[3] = 4.0f;                   // resolve: shist[0] -> the shadow image
    vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, gx, gy, 1);
    shadow_quarter(cb);
    if (R.sdump_every) {   // debug: the shadow image (R = V, G = keep, B = weight) to a PPM, written 10 passes after its copy
        if (R.sdump_state && ++R.sdump_state == 10) {
            char path[512];
            snprintf(path, sizeof path, "logs/rt_shadow_%u.ppm", R.sdump_n);
            FILE *f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P6 %u %u 255 ", R.sw, R.sh);
                const uint8_t *m = R.sdump.map;
                for (uint32_t i = 0; i < R.sw * R.sh; i++) fwrite(m + i * 4, 1, 3, f);
                fclose(f);
            }
            R.sdump_state = 0;
        }
        if (!R.sdump_state && R.sh_frame % R.sdump_every == 0 &&
            (R.sdump.buf || make_buf(c, &R.sdump, (VkDeviceSize)R.sw * R.sh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1))) {
            mem_barrier(cb);
            VkBufferImageCopy rg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {R.sw, R.sh, 1}};
            vkCmdCopyImageToBuffer(cb, R.simg, VK_IMAGE_LAYOUT_GENERAL, R.sdump.buf, 1, &rg);
            R.sdump_state = 1, R.sdump_n = R.sh_frame;
        }
    }
    memcpy(R.sprev_vp, vp, sizeof R.sprev_vp);
    memcpy(R.sprev_cam, cam, sizeof R.sprev_cam);
    R.sprev_ok = 1, R.st_shadow++;
    __atomic_store_n(&R.simg_ready, 1, __ATOMIC_RELEASE);
}

// The phase 7 GI probe's pipeline (rt_gi.comp, the AO's layout) and its quarter-res histories, sized for 3840x2160.
static int gi_init(VkCtx *c) {
    if (!ao_layout_init(c)) return 0;
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof rt_gi_spv,
                                   .pCode = rt_gi_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) { rt_log("vk: ray traced GI off: no shader module"); return 0; }
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = R.ao_layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &R.gi_pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) { R.gi_pipe = VK_NULL_HANDLE; rt_log("vk: ray traced GI off: its pipeline could not be created (%d)", (int)r); return 0; }
    mi.codeSize = sizeof rt_tone_spv, mi.pCode = rt_tone_spv, m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(c->device, &mi, NULL, &m) == VK_SUCCESS) {
        ci.stage.module = m;
        if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &R.tone_pipe) != VK_SUCCESS) R.tone_pipe = VK_NULL_HANDLE;
        vkDestroyShaderModule(c->device, m, NULL);
    }
    mi.codeSize = sizeof rt_late_spv, mi.pCode = rt_late_spv, m = VK_NULL_HANDLE;
    if (MT.dsl && vkCreateShaderModule(c->device, &mi, NULL, &m) == VK_SUCCESS) {
        ci.stage.module = m;
        if (vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &R.late_pipe) != VK_SUCCESS) R.late_pipe = VK_NULL_HANDLE;
        vkDestroyShaderModule(c->device, m, NULL);
    }
    R.ghist_size = (VkDeviceSize)480 * 270 * 48;   // 3 vec4 per quarter-res pixel, 1080p; hist_fit grows it
    if (!make_buf(c, &R.ghist[0], R.ghist_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0) ||
        !make_buf(c, &R.ghist[1], R.ghist_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0) ||
        !make_buf(c, &R.glights, 32 + (VkDeviceSize)32768 * 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0) ||
        !make_buf(c, &R.gcache, (VkDeviceSize)524288 * 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0)) {
        rt_log("vk: ray traced GI off: no memory for its history");
        return 0;
    }
    rt_log("vk: ray traced GI (phase 7 probe) on: 1 ray of %.0f m per 4x4 block, strength %.2f, up to %.0f m, view %d",
           R.gi_len, R.gi_strength, R.gi_far, R.view);
    return 1;
}

// The late hook's passes (rt_late.comp) on the light target (the GI pass's size): mode 0 the material measure, the
// finished screen's colour at mode 13's records of this frame's GI pass, the haze taken out with the current fit; mode 1
// view 26's right half put in
static void late_measure(VkCommandBuffer cb, VkImageView v, uint32_t w, uint32_t h, int mode) {
    if (mode) R.split_pending = 0;
    else R.meas_pending = 0;
    if (!R.late_pipe || !MT.set || !MT.cal.buf || w != (mode ? R.split_w : R.meas_w) || h != (mode ? R.split_h : R.meas_h) ||
        (mode && !R.split.buf))
        return;
    VkDescriptorImageInfo oi = {VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo sb = {R.split.buf ? R.split.buf : MT.cal.buf, 0, VK_WHOLE_SIZE};
    static int aon = -1;   // TOTK_RT_LATE_SUB=0 (debug): the cache learns the finished picture, GI included, as before
    if (aon < 0) { const char *e = cfg_get("TOTK_RT_LATE_SUB"); aon = !(e && *e == '0'); }
    // what this frame's GI added, taken out of the finished picture (rt_gi.comp binding 6)
    int sub = aon && !mode && R.a_ok && R.aview && R.aw == w && R.ah == h;
    if (!mode) R.a_ok = 0;
    VkDescriptorImageInfo ai0 = {R.sampler, R.aview, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[3] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi},
                                  {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 12, .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &sb},
                                  {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ai0}};
    uint32_t qw = (w + 3) / 4, qh = (h + 3) / 4, x0 = (w + 1) / 2;
    struct { int32_t size[4]; float hs[4]; float hf[3]; uint32_t fr; int32_t md[4]; } pc = {
        {(int32_t)w, (int32_t)h, (int32_t)qw, (int32_t)qh},
        {R.hz[0][0], R.hz[0][1], R.hz[0][2], R.hz_on ? R.hz[0][3] : 0.0f}, {R.hz[0][4], R.hz[0][5], R.hz[0][6]}, R.meas_fr,
        {mode, (int32_t)x0, 0, sub}};
    static int ab = -1;
    if (ab < 0) { const char *e = cfg_get("TOTK_RT_AB"); ab = e && *e == '1'; }
    pc.md[2] = ab;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.late_pipe);
    rec_push(R.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 0, sub ? 3 : 2, wr);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 2, 1, &MT.set, 0, NULL);
    vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    if (mode) vkCmdDispatch(cb, (w - x0 + 7) / 8, (h + 7) / 8, 1);
    else vkCmdDispatch(cb, (qw + 7) / 8, (qh + 7) / 8, 1);
    mem_barrier(cb);
    if (!mode) R.late_ok = 3, R.st_late++;
}

// The GI's three dispatches (rt_gi.comp): trace, temporal, apply into the lit scene (after the composite).
static void gi_exec(VkCommandBuffer cb, const VkExecJob *j, const float *u, const float *iv, const float *cam, VkImageView in,
                    VkImageView out) {
    uint32_t w = j->scene.width, h = j->scene.height, qw = (w + 3) / 4, qh = (h + 3) / 4;
    if (!hist_fit(R.ghist, &R.ghist_size, (VkDeviceSize)qw * qh * 48, &R.gprev_ok)) return;
    const float *vp = u + 3 * 4;
    float sx, sy;
    screen_signs(u, iv, &sx, &sy);
    float dx = cam[0] - R.gprev_cam[0], dy = cam[1] - R.gprev_cam[1], dz = cam[2] - R.gprev_cam[2];
    int ok = R.gprev_ok && R.gprev_w == w && R.gprev_h == h && dx * dx + dy * dy + dz * dz < 100.0f;
    // the G-buffer's albedo for this camera (vk_contact.c), read through an sRGB view: linear in the shader
    const VkExecTarget *at = &R.alb[(int)j->pc[15] & 15];
    VkImageView av = at->image && at->img && at->width == w && at->height == h
                         ? vk_exec_target_view(at->img, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0)
                         : VK_NULL_HANDLE;
    // its material targets (vk_contact.c): smoothness and shading class, which decide what reflects
    const VkExecTarget *mt = &R.mtl[(int)j->pc[15] & 15], *mi = &R.mid[(int)j->pc[15] & 15];
    VkImageView mv = VK_NULL_HANDLE, iv14 = VK_NULL_HANDLE;
    if (av && mt->image && mt->img && mi->image && mi->img && mt->width == w && mt->height == h && mi->width == w && mi->height == h) {
        mv = vk_exec_target_view(mt->img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0);
        iv14 = vk_exec_target_view(mi->img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0);
        if (!mv || !iv14) mv = iv14 = VK_NULL_HANDLE;
    }
    static int mtl_use = -1;   // TOTK_RT_MTL=0 (debug): reflections by planarity alone, as before the material targets
    if (mtl_use < 0) { const char *e = cfg_get("TOTK_RT_MTL"); mtl_use = !(e && *e == '0'); }
    if (!mtl_use) mv = iv14 = VK_NULL_HANDLE;
    R.mtl_on = mv != VK_NULL_HANDLE;
    if (!R.alb_logged) {
        R.alb_logged = 1;
        rt_log("vk: ray traced GI: the G-buffer's material targets %s", mv ? "found (reflections follow the smoothness)" :
               mtl_use ? "not found (reflections by planarity alone)" : "off (TOTK_RT_MTL=0)");
        rt_log("vk: ray traced GI: the G-buffer's albedo %s (format %d, %ux%u)", av ? "found" : "not found, the lit colour's hue instead",
               at->image ? (int)at->format : -1, at->width, at->height);
    }
    if (added_image(vk_ctx(), cb, w, h)) {   // what this pass adds, from 0 (rt_gi.comp binding 6, the late measure)
        VkClearColorValue z = {{0}};
        VkImageSubresourceRange rg = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, R.aimg, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg);
        mem_barrier(cb);
        R.a_ok = 1;
    } else R.a_ok = 0;
    float pc[64] = {j->pc[0], j->pc[1], j->pc[2], 0};
    memcpy(pc + 4, iv, 12 * sizeof(float));
    memcpy(pc + 16, R.gprev_vp, sizeof R.gprev_vp);
    pc[60] = cam_near(u);
    pc[32] = R.gi_len, pc[33] = R.gi_strength, pc[34] = R.gi_sat, pc[35] = (float)(R.gi_frame++ & 0xffffff);
    pc[36] = (float)w, pc[37] = (float)h, pc[38] = (float)qw, pc[39] = (float)qh;
    pc[40] = ok ? 1.0f : 0.0f, pc[41] = sx, pc[42] = sy, pc[43] = (float)((R.view >= 20 ? R.view : 0) + (R.gcache_on ? 100 : 0));
    pc[44] = R.veil_ok && R.vview ? -R.gi_far : R.gi_far, pc[45] = R.bright,   // negative: the veil bound (16)
    pc[46] = av ? R.gi_thr : -R.gi_thr,
    pc[47] = R.e_ok && R.eview ? -R.gi_emit : R.gi_emit;   // negative: the lights from the glow (binding 15)
    VkDescriptorImageInfo ii = {R.sampler, in, VK_IMAGE_LAYOUT_GENERAL}, oi = {VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL},
                          ai = {R.sampler, av ? av : in, VK_IMAGE_LAYOUT_GENERAL},
                          mi13 = {R.sampler, mv ? mv : (av ? av : in), VK_IMAGE_LAYOUT_GENERAL},
                          mi14 = {R.sampler, iv14 ? iv14 : (av ? av : in), VK_IMAGE_LAYOUT_GENERAL},
                          ei15 = {R.sampler, R.e_ok && R.eview ? R.eview : in, VK_IMAGE_LAYOUT_GENERAL},
                          vi16 = {R.sampler, R.veil_ok && R.vview ? R.vview : in, VK_IMAGE_LAYOUT_GENERAL},
                          ad6 = {VK_NULL_HANDLE, R.aview ? R.aview : out, VK_IMAGE_LAYOUT_GENERAL};
    // the sun (materials): its world direction, colour and the sky's, for lit hits; the measurement sums cleared
    int sun_on = R.mat && av && (R.gsun.buf || (make_buf(vk_ctx(), &R.gsun, GSUN_PRM + GSUN_PRM_SZ, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0) &&
                                                (R.gsun_rb.buf || (make_buf(vk_ctx(), &R.gsun_rb, 4 * GSUN_PRM, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 2) &&
                                                                   (memset(R.gsun_rb.map, 0, 4 * GSUN_PRM), 1))) &&
                                                (R.gsun_up.buf || make_buf(vk_ctx(), &R.gsun_up, 4 * GSUN_PRM_SZ, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1))));
    if (sun_on) {
        float L[3], ll = 0.0f, prm[GSUN_PRM_SZ / 4] = {0};
        for (int i = 0; i < 3; i++) L[i] = iv[i * 4] * j->pc[4] + iv[i * 4 + 1] * j->pc[5] + iv[i * 4 + 2] * j->pc[6], ll += L[i] * L[i];
        ll = ll > 1e-12f ? 1.0f / sqrtf(ll) : 0.0f;
        prm[0] = L[0] * ll, prm[1] = L[1] * ll, prm[2] = L[2] * ll, prm[3] = ll > 0.0f ? 1.0f : 0.0f;
        prm[4] = R.sun_col[0], prm[5] = R.sun_col[1], prm[6] = R.sun_col[2], prm[7] = (float)R.sun_est;
        prm[8] = R.sky_col[0], prm[9] = R.sky_col[1], prm[10] = R.sky_col[2], prm[11] = (float)R.sky_est;
        static float pbox[4] = {-1.0f};
        if (pbox[0] == -1.0f) {   // TOTK_RT_MPROBE_BOX=x0,y0,x1,y1 (debug): the probe grid over that part of the screen
            const char *e = cfg_get("TOTK_RT_MPROBE_BOX");
            pbox[0] = 0.0f;
            if (e && sscanf(e, "%f,%f,%f,%f", &pbox[0], &pbox[1], &pbox[2], &pbox[3]) != 4) pbox[0] = pbox[2] = 0.0f;
        }
        memcpy(prm + 12, pbox, sizeof pbox);
        if (R.hz_on) memcpy(prm + 16, R.hz, sizeof R.hz);
        vkCmdFillBuffer(cb, R.gsun.buf, 0, GSUN_PRM, 0);
        VkDeviceSize us = (VkDeviceSize)(R.gi_frame & 3) * GSUN_PRM_SZ;
        memcpy((char *)R.gsun_up.map + us, prm, sizeof prm);
        vmaFlushAllocation(vk_ctx()->vma, R.gsun_up.mem, us, sizeof prm);
        VkBufferCopy uc = {us, GSUN_PRM, sizeof prm};
        vkCmdCopyBuffer(cb, R.gsun_up.buf, R.gsun.buf, 1, &uc);
        mt_cal_clear(cb);
        mem_barrier(cb);
    }
    // view 26's right half goes in at the late hook (after its measure has read the game's whole picture) while that runs
    VkDeviceSize ssz = (VkDeviceSize)(w - (w + 1) / 2) * h * 8;
    if (R.view == 26 && R.late_pipe && R.late_ok > 0 && R.split_size < ssz) {
        free_buf(vk_ctx(), &R.split), R.split_size = 0;
        if (make_buf(vk_ctx(), &R.split, ssz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0)) R.split_size = ssz;
    }
    int defer = R.view == 26 && R.late_pipe && R.late_ok > 0 && R.split.buf && R.split_size >= ssz;
    pc[62] = defer ? 1.0f : 0.0f;
    static int app = -1;   // TOTK_RT_APP=0 (debug): hits without the appearance cache (the materials' fit alone)
    if (app < 0) { const char *e = cfg_get("TOTK_RT_APP"); app = e && *e ? atoi(e) : 1; }
    // TOTK_RT_GIMAT: 1 (default) GI hits off the screen shaded (materials, appearance cache); 0 from the light cache;
    // 2 / 3 (debug) every GI hit shaded / from the light cache. In pc[63]'s hundreds: 0 on, 1 off, 2, 3
    static int gimat = -1;
    if (gimat < 0) { const char *e = cfg_get("TOTK_RT_GIMAT"); gimat = e && *e ? atoi(e) : 1; }
    pc[63] = (float)app + 100.0f * (gimat == 1 ? 0.0f : gimat == 0 ? 1.0f : (float)gimat);
    R.split_pending = defer, R.split_w = w, R.split_h = h;
    VkDescriptorBufferInfo hin = {R.ghist[0].buf, 0, VK_WHOLE_SIZE}, hout = {R.ghist[1].buf, 0, VK_WHOLE_SIZE},
                           wb = {R.world.buf, 0, VK_WHOLE_SIZE}, gb = {R.gtab.buf, 0, VK_WHOLE_SIZE},
                           lb = {R.glights.buf, 0, VK_WHOLE_SIZE}, cb9 = {R.gcache.buf, 0, VK_WHOLE_SIZE},
                           sb10 = {sun_on ? R.gsun.buf : R.probe.buf, 0, VK_WHOLE_SIZE},
                           ab11 = {R.mat ? R.wattr.buf : R.probe.buf, 0, VK_WHOLE_SIZE},
                           sp12 = {defer ? R.split.buf : R.probe.buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr[17] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 6, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ad6},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 16, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &vi16},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 12, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &sp12},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 10, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &sb10},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 11, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &ab11},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 9, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &cb9},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &hin},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &hout},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 4, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &wb},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 5, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &gb},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 7, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ai},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 8, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &lb},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 13, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &mi13},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 14, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &mi14},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 15, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ei15}};
    uint32_t gx = (qw + 7) / 8, gy = (qh + 7) / 8;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.gi_pipe);
    rec_push(R.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 0, 17, wr);   // later pushes keep 6, 8, 13-16
    R.e_ok = 0;   // (the next GI pass needs the next frame's copy)
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 1, 1, &R.as_set, 0, NULL);
    if (MT.set) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 2, 1, &MT.set, 0, NULL);
    pc[3] = 5.0f;                   // the light list emptied, then filled from the lit scene (4x4 blocks)
    vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, 1, 1, 1);
    mem_barrier(cb);
    pc[3] = 6.0f;                   // the frame's lighting level, which a light must exceed several times
    vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, gx, gy, 1);
    mem_barrier(cb);
    if (sun_on) {   // the sun and sky colours measured (mode 12), read back 3 frames later into moving averages
        pc[3] = 12.0f;
        vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        vkCmdDispatch(cb, gx, gy, 1);
        mem_barrier(cb);
        // The sums (lit colour, albedo, and in the sun albedo x N.L) and histograms decay by 0.98 a frame, so a frame
        // counts by its blocks. The levels are the histograms' medians; the hues the ratios of sums (albedo-weighted,
        // which a dark albedo channel cannot blow up), scaled to those levels.
        const uint32_t *m = (const uint32_t *)R.gsun_rb.map + ((R.gi_frame + 1) & 3) * (GSUN_PRM / 4);
        for (int i = 0; i < 17; i++) R.sacc[i] = R.sacc[i] * 0.98 + (double)m[i];
        for (int i = 0; i < 128; i++) R.shacc[i] = R.shacc[i] * 0.98 + (double)m[32 + i];
        const double *S = R.sacc;
        double sky_med = hist_median(R.shacc), sun_med = hist_median(R.shacc + 64);
        if (S[16] >= 50.0 && sky_med > 0.0) {   // in the sun's shadow: the sky alone
            double c[3];
            for (int i = 0; i < 3; i++) c[i] = (S[10 + i] / 64.0) / (S[13 + i] / 256.0 > 1e-6 ? S[13 + i] / 256.0 : 1e-6);
            double l = lumd(c);
            for (int i = 0; i < 3; i++) R.sky_col[i] = (float)(l > 1e-6 ? c[i] * sky_med / l : sky_med);
            R.sky_est = 1;
        }
        if (S[3] >= 50.0 && sun_med > 0.0) {   // in the sun: the sky + the sun x N.L; the median is of (sky + sun N.L) / N.L
            double c[3], a[3] = {S[4], S[5], S[6]}, an[3] = {S[7], S[8], S[9]};
            for (int i = 0; i < 3; i++) {
                double v = S[i] / 64.0 - (R.sky_est ? R.sky_col[i] : 0.0) * S[4 + i] / 256.0, w = S[7 + i] / 256.0;
                c[i] = v > 0.0 && w > 1e-6 ? v / w : 0.0;
            }
            double inv_ndl = lumd(an) > 1e-6 ? lumd(a) / lumd(an) : 1.0, sky_l = R.sky_est ? lumd3f(R.sky_col) : 0.0;
            // the blocks the capture takes as sunlit but the game shades (under glass the capture does not hold, in the
            // shadow of what it left out) sit near the sky's level: the sun is the median of those well above it, unless
            // too few are (the sun behind a cloud)
            double up = hist_median_above(R.shacc + 64, 3.0 * sky_l * inv_ndl, 0.2);
            double sl = (up > 0.0 ? up : sun_med) - sky_l * inv_ndl, l = lumd(c);
            if (sl < 0.0) sl = 0.0;
            for (int i = 0; i < 3; i++) R.sun_col[i] = (float)(l > 1e-6 ? c[i] * sl / l : sl);
            R.sun_est = 1;
        }
        haze_fit(m + 160 + 384);
        if (!R.mprobe) { const char *e = cfg_get("TOTK_RT_MPROBE"); R.mprobe = 1 + (e && *e ? atoi(e) : 0); }
        if (R.mprobe > 1 && (R.gi_frame & (R.mprobe > 4 ? 0 : R.mprobe > 2 ? 31 : 255)) == 0) mprobe_log(m + 160);
        if ((R.gi_frame & 511) == 0)
            rt_log("vk: GI sun for lit hits: sun %.2f %.2f %.2f (%s, %u blocks), sky %.2f %.2f %.2f (%s, %u blocks)", R.sun_col[0],
                   R.sun_col[1], R.sun_col[2], R.sun_est ? "measured" : "not yet", m[3], R.sky_col[0], R.sky_col[1], R.sky_col[2],
                   R.sky_est ? "measured" : "not yet", m[16]),
            rt_log("vk: GI sun levels: medians in the sun %.2f (of those above the sky %.2f), in the shade %.2f",
                   hist_median(R.shacc + 64), hist_median_above(R.shacc + 64, 3.0 * lumd3f(R.sky_col) *
                   (lumd(R.sacc + 7) > 1e-6 ? lumd(R.sacc + 4) / lumd(R.sacc + 7) : 1.0), 0.2), hist_median(R.shacc));
        if (MT.ok && MT.cal.buf) {   // the materials' colours: measured on screen (mode 13), then each slot's updated (14)
            // the finished screen's colours: mode 13 leaves records for the late measure while it runs (nr.y)
            int late = R.late_pipe && MT.set && (VkDeviceSize)qw * qh <= MT_REC;
            pc[3] = 13.0f, pc[61] = late && R.late_ok > 0 ? 1.0f : 0.0f;
            if (R.late_ok > 0) R.late_ok--;
            vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
            vkCmdDispatch(cb, gx, gy, 1);
            mem_barrier(cb);
            pc[61] = 0.0f;
            R.meas_pending = late, R.meas_fr = (uint32_t)pc[35], R.meas_w = w, R.meas_h = h;
            pc[3] = 14.0f;
            vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
            vkCmdDispatch(cb, 2 * MT_MAX / 512, 8, 1);   // 8x8 groups: 2 x MT_MAX / 64 slots a row of 64
            mem_barrier(cb);
        }
        VkBufferCopy bc = {0, (VkDeviceSize)(R.gi_frame & 3) * GSUN_PRM, GSUN_PRM};
        vkCmdCopyBuffer(cb, R.gsun.buf, R.gsun_rb.buf, 1, &bc);
        mem_barrier(cb);
    }
    // the lighting level for the night tone: copied out each frame (4 slots), the slot written 3 frames ago read now
    // (lit sum, count, sky sum, count). The sky's brightness tells day from night; where too little sky shows (caves,
    // shrines, under a roof) the last value holds, and before any sky was seen it counts as day.
    if (R.glevel.buf || (make_buf(vk_ctx(), &R.glevel, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 2) && (memset(R.glevel.map, 0, 64), 1))) {
        const uint32_t *m = (const uint32_t *)R.glevel.map + ((R.gi_frame + 1) & 3) * 4;
        if (m[1]) {
            float lv = (float)m[0] / (64.0f * (float)m[1]);
            R.gi_level = R.glevel_n++ ? R.gi_level + (lv - R.gi_level) * 0.05f : lv;
            R.st_level += lv, R.st_level_n++;
        }
        R.sky_frac = (float)m[3] / (float)(qw * qh);
        if (m[3] && R.sky_frac >= 0.01f) {
            float sv = (float)m[2] / (64.0f * (float)m[3]);
            R.sky_level = R.sky_n++ ? R.sky_level + (sv - R.sky_level) * 0.05f : sv;
        }
        VkBufferCopy bc = {4, (VkDeviceSize)(R.gi_frame & 3) * 16, 16};
        vkCmdCopyBuffer(cb, R.glights.buf, R.glevel.buf, 1, &bc);
        mem_barrier(cb);
    }
    {
        float nf = R.tone_day > R.tone_night ? (R.tone_day - R.gi_level) / (R.tone_day - R.tone_night) : 0.0f;
        nf = nf < 0.0f ? 0.0f : nf > 1.0f ? 1.0f : nf;
        nf = R.glevel_n ? nf * nf * (3.0f - 2.0f * nf) : 0.0f;
        // and only where the sky says night: full at or below tone_sky_night, none at or above tone_sky_day
        float ns = R.sky_day > R.sky_night ? (R.sky_day - R.sky_level) / (R.sky_day - R.sky_night) : 1.0f;
        ns = ns < 0.0f ? 0.0f : ns > 1.0f ? 1.0f : ns;
        ns = R.sky_n ? ns * ns * (3.0f - 2.0f * ns) : 0.0f;
        if (ns < nf) nf = ns;
        pc[56] = R.tone_g[0], pc[57] = R.tone_g[1], pc[58] = R.tone_g[2], pc[59] = R.tone_eff = R.tone * nf;
        if (R.tone_late > 0) R.tone_late--, pc[59] = 0.0f;   // applied after the fog instead
        R.night_f = nf;
        if ((R.gi_frame & 511) == 0)
            rt_log("vk: GI lighting level %.2f, sky %.3f (%.0f%% of blocks, %s), night %.2f, tone %.2f", R.gi_level, R.sky_level,
                   100.0f * R.sky_frac, R.sky_n ? "seen" : "none yet", nf, R.tone_eff);
    }
    pc[3] = 4.0f;
    vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, gx, gy, 1);
    mem_barrier(cb);
    if (R.gcache_on) {   // the lit scene into the world light cache (mode 11), read by the trace for off-screen hits
        if (!R.gcache_clear) {
            vkCmdFillBuffer(cb, R.gcache.buf, 0, VK_WHOLE_SIZE, 0);
            mem_barrier(cb);
            R.gcache_clear = 1;
            rt_log("vk: ray traced GI: world light cache on (%u slots, %.0f MB)", 524288u, 524288.0 * 20 / 1048576.0);
        }
        pc[3] = 11.0f;
        vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        vkCmdDispatch(cb, gx, gy, 1);
        mem_barrier(cb);
    }
    pc[3] = 0.0f;
    vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, gx, gy, 1);   // trace: ghist[0] (last frame's) -> ghist[1]
    mem_barrier(cb);
    pc[3] = 3.0f;                   // temporal: ghist[1] -> ghist[0]
    hin.buffer = R.ghist[1].buf, hout.buffer = R.ghist[0].buf;
    rec_push(R.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 0, 11, wr);
    vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, gx, gy, 1);
    mem_barrier(cb);
    // the spatial denoiser (mode 7), three a-trous steps into ghist[1], which the trace no longer needs; ghist[0] stays the
    // temporal history. proj x/y/z carry the source offset, destination offset and step (the filter needs no projection).
    {
        float n = (float)qw * qh, keep[3] = {pc[0], pc[1], pc[2]};
        const float steps[3][3] = {{0, 0, 1}, {0, 1, 2}, {1, 0, 4}};   // (src offset in n, dst offset in n, step)
        for (int k = 0; k < 3; k++) {
            hin.buffer = k == 0 ? R.ghist[0].buf : R.ghist[1].buf, hout.buffer = R.ghist[1].buf;
            rec_push(R.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 0, 11, wr);
            pc[0] = steps[k][0] * n, pc[1] = steps[k][1] * n, pc[2] = steps[k][2], pc[3] = 7.0f;
            vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
            vkCmdDispatch(cb, gx, gy, 1);
            mem_barrier(cb);
        }
        pc[0] = keep[0], pc[1] = keep[1], pc[2] = keep[2];
    }
    // the reflections (modes 8, 9, 10) at half resolution: trace rhist[0] (last frame's) -> rhist[1], temporal -> rhist[0]
    // (the history), a-trous steps into rhist[1]; the apply reads them through binding 2
    uint32_t hw = (w + 1) / 2, hh = (h + 1) / 2;
    VkDeviceSize rneed = (VkDeviceSize)hw * hh * 48;
    if (R.refl > 0.0f && !R.rhist[0].buf && !R.rhist_logged) {
        VkCtx *c = vk_ctx();
        R.rhist_logged = 1;
        if (make_buf(c, &R.rhist[0], rneed, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0) &&
            make_buf(c, &R.rhist[1], rneed, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0)) {
            R.rhist_size = rneed;
            rt_log("vk: ray traced reflections on: strength %.2f, roughness %.2f, rays of %.0f m, F0 %.2f, %d frames, %d a-trous "
                   "steps, %ux%u", R.refl, R.refl_rough, R.refl_len, R.refl_f0, R.refl_frames, R.refl_blur, hw, hh);
        } else {
            free_buf(c, &R.rhist[0]), free_buf(c, &R.rhist[1]);
            rt_log("vk: ray traced reflections off: no memory for their history");
        }
    }
    int refl = R.refl > 0.0f && R.rhist[0].buf && R.rhist[1].buf && rneed <= R.rhist_size;
    VkBuffer rfinal = R.rhist[1].buf;
    float roff = 0.0f;
    pc[48] = 0.0f, pc[49] = R.refl_rough, pc[50] = R.refl_len, pc[51] = (float)R.refl_frames;
    // keep a modest floor F0 in daytime (0.10) rising to R.refl_f0 at night so polished floors stay reflective
    // even when looking steeply downward; negative signals the material targets are bound (rt_gi.comp mtl_on)
    float day_f0 = R.refl_f0 < 0.10f ? R.refl_f0 : 0.10f;
    pc[52] = (float)hw, pc[53] = (float)hh, pc[54] = day_f0 + (R.refl_f0 - day_f0) * R.night_f, pc[55] = 0.0f;
    if (R.mtl_on) pc[54] = -pc[54];
    if (refl) {
        float n = (float)hw * hh, keep[3] = {pc[0], pc[1], pc[2]}, okv = pc[40];
        uint32_t rx = (hw + 7) / 8, ry = (hh + 7) / 8;
        pc[40] = ok && R.rhist_ok ? 1.0f : 0.0f;
        pc[3] = 8.0f;
        hin.buffer = R.rhist[0].buf, hout.buffer = R.rhist[1].buf;
        rec_push(R.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 0, 11, wr);
        vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        vkCmdDispatch(cb, rx, ry, 1);
        mem_barrier(cb);
        pc[3] = 9.0f;
        hin.buffer = R.rhist[1].buf, hout.buffer = R.rhist[0].buf;
        rec_push(R.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 0, 11, wr);
        vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
        vkCmdDispatch(cb, rx, ry, 1);
        mem_barrier(cb);
        rfinal = R.rhist[0].buf;
        // a-trous: 1 step rhist[0] -> rhist[1] at n; 2 steps rhist[0] -> rhist[1] at 0 (step 1) -> rhist[1] at n (step 2)
        for (int k = 0; k < R.refl_blur; k++) {
            int last = k == R.refl_blur - 1;
            hin.buffer = k == 0 ? R.rhist[0].buf : R.rhist[1].buf, hout.buffer = R.rhist[1].buf;
            rec_push(R.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 0, 11, wr);
            pc[0] = 0.0f, pc[1] = last ? n : 0.0f, pc[2] = (float)(1 << k), pc[3] = 10.0f;
            vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
            vkCmdDispatch(cb, rx, ry, 1);
            mem_barrier(cb);
            rfinal = R.rhist[1].buf, roff = n;
        }
        pc[0] = keep[0], pc[1] = keep[1], pc[2] = keep[2], pc[40] = okv;
        pc[48] = R.refl, pc[55] = roff;
        R.rhist_ok = 1, R.st_refl++;
    } else {
        R.rhist_ok = 0;
    }
    pc[3] = 1.0f;                   // apply: full resolution, reads the denoised ghist[1] (3) and the reflections (2)
    hin.buffer = refl ? rfinal : R.ghist[0].buf, hout.buffer = R.ghist[1].buf;
    rec_push(R.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 0, 11, wr);
    vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    {   // the scene as this pass leaves it, for the veil at the late hook (TOTK_RT_GI_VEIL=0 off)
        static int von = -1;
        if (von < 0) { const char *e = cfg_get("TOTK_RT_GI_VEIL"); von = e && *e == '1'; }   // off by default: ~1 where measured
        if (!von) R.veil_ok = 0;
        if (von && j->scene.img && j->scene.img->image && veil_images(vk_ctx(), cb, w, h)) {
            mem_barrier(cb);
            VkImageCopy rg = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
            vkCmdCopyImage(cb, j->scene.img->image, VK_IMAGE_LAYOUT_GENERAL, R.pimg, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            R.post_ok = 1;
        }
    }
    if (sun_on && R.view == 26 && R.mprobe > 1) {   // the material probe (written by the apply) into this frame's readback slot
        mem_barrier(cb);
        VkBufferCopy pb = {4 * 160, (VkDeviceSize)(R.gi_frame & 3) * GSUN_PRM + 4 * 160, 4 * 384};
        vkCmdCopyBuffer(cb, R.gsun.buf, R.gsun_rb.buf, 1, &pb);
    }
    memcpy(R.gprev_vp, vp, sizeof R.gprev_vp);
    memcpy(R.gprev_cam, cam, sizeof R.gprev_cam);
    R.gprev_w = w, R.gprev_h = h, R.gprev_ok = 1, R.st_gi++, R.st_gi_alb += av != VK_NULL_HANDLE;
}

// The performance overlay's ray tracing rows (ui/menu.cpp): GPU ms a frame building the scene and tracing. 0: off.
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
static void mat_switch(VkCtx *c, int on) {
    on = on && R.mat_cap;
    if (on == R.mat) return;
    if (on && (!R.aclip.buf || !R.wattr.buf)) {
        VkDeviceSize ab = (VkDeviceSize)(R.low_verts + TOP_VERTS) * 32, wb = (VkDeviceSize)(CAP_TRIS + (R.cache ? CACHE_TRIS : 0)) * 32;
        if (!make_buf(c, &R.aclip, ab, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0) ||
            !make_buf(c, &R.wattr, wb, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0)) {
            static int logged;
            if (!logged++) rt_log("vk: ray tracing materials off: no memory for the UV capture (%llu MiB)", (unsigned long long)((ab + wb) >> 20));
            free_buf(c, &R.aclip), free_buf(c, &R.wattr);
            return;
        }
        rt_log("vk: ray tracing materials: the capture records albedo UVs (%llu + %llu MiB)", (unsigned long long)(ab >> 20),
               (unsigned long long)(wb >> 20));
    }
    // step 6: the varyings of draws with a program (ext) and the triangles' pointers into it (wext), made once
    const char *pe = cfg_get("TOTK_RT_PROG");
    if (on && MT.prog_feat && !MT.prog_ok && !(pe && *pe == '0') && R.xfb_nbuf >= 3 && R.xfb_stride >= 128) {
        VkDeviceSize xb = (VkDeviceSize)(XSINK + 16) * 16, wb = (VkDeviceSize)(CAP_TRIS + (R.cache ? CACHE_TRIS : 0)) * 4;
        if (make_buf(c, &MT.pext, xb, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0) &&
            make_buf(c, &MT.wext, wb, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0)) {
            VkDescriptorBufferInfo bi[2] = {{MT.pext.buf, 0, VK_WHOLE_SIZE}, {MT.wext.buf, 0, VK_WHOLE_SIZE}};
            VkWriteDescriptorSet w[2];
            for (int i = 0; i < 2; i++)
                w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = MT.set, .dstBinding = (uint32_t)(6 + i),
                                              .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[i]};
            vkUpdateDescriptorSets(c->device, 2, w, 0, NULL);
            ra_init(&R.xa, XC);
            MT.prog_ok = 1;
            rt_log("vk: ray tracing materials: the game's shaders at hits (step 6): varyings %llu MiB, pointers %llu MiB",
                   (unsigned long long)(xb >> 20), (unsigned long long)(wb >> 20));
        } else {
            free_buf(c, &MT.pext), free_buf(c, &MT.wext);
            rt_log("vk: ray tracing materials: no memory for the game's shaders at hits (step 6)");
        }
    }
    R.mat = on;
    if (on) R.wattr_clear = 1, R.mat_retire = 1;   // meshes cached while off have no UVs: all captured again
    __atomic_store_n(&R.mat_on, on, __ATOMIC_RELEASE);
    vk_pipeline_xfb_regen();
    R.gprev_ok = 0, R.rhist_ok = 0;
    rt_log("vk: ray tracing materials switched %s", on ? "on" : "off");
}

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
    VkDescriptorSetLayout sets[3];
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 112};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 3,
                                     .pSetLayouts = sets, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof rt_test_spv,
                                   .pCode = rt_test_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &R.dsl) != VK_SUCCESS ||
        vkCreateDescriptorSetLayout(c->device, &adci, NULL, &R.as_dsl) != VK_SUCCESS ||
        vkCreateDescriptorPool(c->device, &pci, NULL, &R.pool) != VK_SUCCESS ||
        !mt_init(c) || (sets[0] = R.dsl, sets[1] = R.as_dsl, sets[2] = MT.dsl, 0) ||
        vkCreatePipelineLayout(c->device, &li, NULL, &R.layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &R.sampler) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) {
        rt_log("vk: ray tracing test off: its pipeline could not be created");
        return 0;
    }
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = R.layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &R.pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) { rt_log("vk: ray tracing test off: its pipeline could not be created (%d)", (int)r); return 0; }
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
    R.ao_ok = R.geom && ao_init(c), R.sh_ok = R.geom && shadow_init(c);
    R.ao = R.ao && R.ao_ok, R.shadows = R.shadows && R.sh_ok;
    R.gi_ok = R.geom && gi_init(c), R.gi = R.gi && R.gi_ok;   // built always too: the menu's GI switch (vk_rt_live)
    if (R.cache && !cache_init(c)) {
        rt_log("vk: ray tracing mesh cache off: its pipeline or memory could not be created");
        R.cache = 0;
    }

    // The test sphere: 1 m radius around the origin of its instance
    VkBufferUsageFlags in = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    VkBufferUsageFlags sb = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;   // and read by the hit normals
    if (!make_buf(c, &R.vb, SPH_NV * 12, in | sb, 1) || !make_buf(c, &R.ib, SPH_NI * 4, in | sb, 1) ||
        !make_buf(c, &R.inst, (VkDeviceSize)8 * INST_MAX * sizeof(VkAccelerationStructureInstanceKHR), in, 1) ||
        !make_buf(c, &R.gtab, (VkDeviceSize)GT_MAX * 4, sb, 1) ||
        (R.geom && (!make_buf(c, &R.clip, (VkDeviceSize)CAP_VERTS * 16,
                              VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0) ||
                    !make_buf(c, &R.world, (VkDeviceSize)(CAP_TRIS + (R.cache ? CACHE_TRIS : 0)) * 48,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | in, 0))) ||
        !make_buf(c, &R.probe, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1) ||
        (R.geom && !make_buf(c, &R.dtab, 8 * 8192 * 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1)) ||
        (R.geom && !make_buf(c, &R.camb, 8 * 16 * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1))) {
        rt_log("vk: ray tracing test off: no memory for its buffers");
        return 0;
    }
    float *v = R.vb.map;
    for (int i = 0; i <= SPH_RINGS; i++) {
        float th = 3.14159265f * (float)i / SPH_RINGS;
        for (int k = 0; k <= SPH_SEGS; k++) {
            float ph = 6.2831853f * (float)k / SPH_SEGS;
            *v++ = sinf(th) * cosf(ph), *v++ = cosf(th), *v++ = sinf(th) * sinf(ph);
        }
    }
    uint32_t *ix = R.ib.map;
    for (int i = 0; i < SPH_RINGS; i++)
        for (int k = 0; k < SPH_SEGS; k++) {
            uint32_t a = (uint32_t)(i * (SPH_SEGS + 1) + k), b2 = a + SPH_SEGS + 1;
            *ix++ = a, *ix++ = b2, *ix++ = a + 1, *ix++ = a + 1, *ix++ = b2, *ix++ = b2 + 1;
        }
    vmaFlushAllocation(c->vma, R.vb.mem, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(c->vma, R.ib.mem, 0, VK_WHOLE_SIZE);

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
        const char *e = cfg_get("TOTK_RT_MAT");
        if (e && *e == '1') mat_switch(c, 1);
    }
    VkAccelerationStructureGeometryKHR sg = sphere_geometry(), ig = inst_geometry(0), cg = scene_geometry();
    if (!make_as(c, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &sg, SPH_NI / 3,
                 VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
                 &R.blas_buf, &R.blas, &scratch) ||
        (R.geom && !make_as(c, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &cg, R.scene_cap, SCENE_FLAGS, &R.scene_buf,
                            &R.scene, &scratch)) ||
        (R.geom && R.cache && !make_as(c, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, &cg, R.scene_cap, SCENE_FLAGS,
                                       &R.scene_buf2, &R.scene2, &scratch)) ||
        !make_as(c, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, &ig, INST_MAX,
                 VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR, &R.tlas_buf, &R.tlas, &scratch) ||
        !make_buf(c, &R.scratch, scratch + R.scratch_align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0)) {
        rt_log("vk: ray tracing test off: its acceleration structures could not be created");
        return 0;
    }
    VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = R.pool,
                                       .descriptorSetCount = 1, .pSetLayouts = &R.as_dsl};
    if (vkAllocateDescriptorSets(c->device, &dai, &R.as_set) != VK_SUCCESS) {
        rt_log("vk: ray tracing test off: no descriptor set for the TLAS");
        return 0;
    }
    VkWriteDescriptorSetAccelerationStructureKHR wa = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                                                       .accelerationStructureCount = 1, .pAccelerationStructures = &R.tlas};
    VkWriteDescriptorSet aw = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &wa, .dstSet = R.as_set,
                               .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    vkUpdateDescriptorSets(c->device, 1, &aw, 0, NULL);
    rt_log("vk: ray tracing test on: sphere of %d triangles (%s), captured scene %s (up to %u triangles, BLAS %.0f MiB), view %d, "
           "shadow strength %.2f, scratch %.1f MiB", SPH_NI / 3, R.sphere ? "shown" : "hidden", R.geom ? "on" : "off", CAP_TRIS,
           R.scene_buf.buf ? (double)R.scene_buf_size / 1048576.0 : 0.0, R.view, R.strength, (double)scratch / 1048576.0);
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
// under their identity (found by Gemini 4 Neon reviewing the sun-loss bug). The frame does not end here.
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

// Materials step 3 (docs/raytracing.md): the albedo textures ray hits sample, bindless. A captured G-buffer draw's albedo
// view (vk_exec_draw.c rt_mat_words, under the texture lock) gets a slot of tex2d[] or texarr[] (set 2 of the shaders
// that shade hits: binding 0, 1), written there at once (update after bind, partially bound, unused while pending);
// its texture word, 1 << 31 | array << 30 | generation << 16 | slot, rides with the draw to its triangles. When the
// game drops the image (vk_exec_image_gone, under the texture lock) its slots go back to the free list and their
// generation moves on in gen[] (binding 2, host-visible): a triangle whose word's generation differs samples nothing,
// so no shader reads a descriptor whose view is destroyed (the views go after the GPU is done with the frame) or a
// slot given to another texture since. The lock order: the texture lock, then MT.lock.

// The set layout always (the shaders that declare set 2 need it); the table itself when the device has the features
static int mt_init(VkCtx *c) {
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
    vkGetPhysicalDeviceFeatures2(c->phys, &f2);   // vk_device.c enables every feature the device has
    int feat = f12.descriptorBindingSampledImageUpdateAfterBind && f12.descriptorBindingPartiallyBound &&
               f12.descriptorBindingUpdateUnusedWhilePending && f12.shaderSampledImageArrayNonUniformIndexing;
    // step 6's ext and wext (bindings 6, 7) are made when materials first switch on: written then, while the set may be
    // in use (update after bind)
    MT.prog_feat = feat && f12.descriptorBindingStorageBufferUpdateAfterBind;
    VkDescriptorBindingFlags pbf = MT.prog_feat ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                                      VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT : 0;
    VkDescriptorBindingFlags bf[8] = {VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                          VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
                                      VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                          VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT, 0, 0, 0, 0, pbf, pbf};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                                                       .bindingCount = 8, .pBindingFlags = bf};
    VkDescriptorSetLayoutBinding b[8];
    for (int i = 0; i < 8; i++)
        b[i] = (VkDescriptorSetLayoutBinding){.binding = (uint32_t)i, .descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                                                : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                              .descriptorCount = i < 2 ? MT_MAX : 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = feat ? &bfi : NULL,
                                           .flags = feat ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0,
                                           .bindingCount = 8, .pBindings = b};
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * MT_MAX}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6}};
    VkDescriptorPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .flags = feat ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0, .maxSets = 1,
                                      .poolSizeCount = 2, .pPoolSizes = ps};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .maxLod = VK_LOD_CLAMP_NONE};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &MT.dsl) != VK_SUCCESS ||
        vkCreateDescriptorPool(c->device, &pci, NULL, &MT.pool) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &MT.samp) != VK_SUCCESS ||
        !make_buf(c, &MT.gen, 4 * MT_MAX * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1) ||
        !make_buf(c, &MT.pcode, (VkDeviceSize)PCODE_V4 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1) ||
        !make_buf(c, &MT.pmat, (VkDeviceSize)2 * MT_MAX * PM_WORDS * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1) ||
        !make_buf(c, &MT.cal, MT_CAL_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0))
        return 0;
    MT.cal_clear = 1;
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = MT.pool,
                                      .descriptorSetCount = 1, .pSetLayouts = &MT.dsl};
    if (vkAllocateDescriptorSets(c->device, &ai, &MT.set) != VK_SUCCESS) return 0;
    memset(MT.gen.map, 0xff, 2 * MT_MAX * 4);   // no word's generation (14 bits) matches
    memset((uint32_t *)MT.gen.map + 2 * MT_MAX, 0, 2 * MT_MAX * 4);   // no shader id
    vmaFlushAllocation(c->vma, MT.gen.mem, 0, VK_WHOLE_SIZE);
    memset(MT.pmat.map, 0, (size_t)2 * MT_MAX * PM_WORDS * 4);   // no material has a program
    vmaFlushAllocation(c->vma, MT.pmat.mem, 0, VK_WHOLE_SIZE);
    // (bindings 6 and 7 hold gen until materials make ext and wext: no record names a program before then)
    VkDescriptorBufferInfo bi[6] = {{MT.gen.buf, 0, VK_WHOLE_SIZE}, {MT.cal.buf, 0, VK_WHOLE_SIZE}, {MT.pcode.buf, 0, VK_WHOLE_SIZE},
                                    {MT.pmat.buf, 0, VK_WHOLE_SIZE}, {MT.gen.buf, 0, VK_WHOLE_SIZE}, {MT.gen.buf, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[6];
    for (int i = 0; i < 6; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = MT.set, .dstBinding = (uint32_t)(2 + i),
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[i]};
    vkUpdateDescriptorSets(c->device, 6, w, 0, NULL);
    nvn_hm_init(&MT.by_prog, sizeof(void *));
    nvn_hm_init(&MT.by_view[0], sizeof(MtKey)), nvn_hm_init(&MT.by_view[1], sizeof(MtKey));
    nvn_hm_init(&MT.by_fs, sizeof(void *));
    MT.nsid = 1;
    if (!feat) rt_log("vk: ray tracing materials: no albedo textures (the device lacks update-after-bind sampled images)");
    MT.ok = feat;
    return 1;
}

static void mt_gen_write(int k, uint32_t slot) {
    ((uint32_t *)MT.gen.map)[k * MT_MAX + slot] = MT.s[k][slot].used ? MT.s[k][slot].gen : 0xffffffffu;
    vmaFlushAllocation(vk_ctx()->vma, MT.gen.mem, (VkDeviceSize)(k * MT_MAX + slot) * 4, 4);
    uint32_t sid = 0;   // the slot's shader id (MT.lock held)
    const void *fs = MT.s[k][slot].used ? MT.s[k][slot].fs : NULL;
    if (fs) {
        sid = (uint32_t)(uintptr_t)nvn_hm_get(&MT.by_fs, &fs);
        if (!sid && MT.nsid < MT_SHADERS) sid = MT.nsid++, nvn_hm_put(&MT.by_fs, &fs, (void *)(uintptr_t)sid);
    }
    ((uint32_t *)MT.gen.map)[2 * MT_MAX + k * MT_MAX + slot] = sid;
    vmaFlushAllocation(vk_ctx()->vma, MT.gen.mem, (VkDeviceSize)(2 * MT_MAX + k * MT_MAX + slot) * 4, 4);
}

// The slot of material (view, fs) in table k, added when new (a flat one: view and image null, no descriptor)
static uint32_t mt_get(int k, VkImage image, VkImageView view, const void *fs) {
    MtKey key;
    memset(&key, 0, sizeof key);
    key.view = view, key.fs = fs;
    plat_mutex_lock(&MT.lock);
    uint32_t slot = (uint32_t)(uintptr_t)nvn_hm_get(&MT.by_view[k], &key);
    if (slot) {
        slot--;
    } else {
        if (MT.fresh[k] < MT_MAX) slot = MT.fresh[k]++;
        else if (MT.fcount[k]) slot = MT.fifo[k][MT.fhead[k]], MT.fhead[k] = (MT.fhead[k] + 1) % MT_MAX, MT.fcount[k]--;
        else { MT.st_full++; plat_mutex_unlock(&MT.lock); return 0; }
        MtSlot *e = &MT.s[k][slot];
        e->image = image, e->view = view, e->fs = fs, e->used = 1, e->gen = (uint16_t)((e->gen + 1) & 0x3fff);
        nvn_hm_put(&MT.by_view[k], &key, (void *)(uintptr_t)(slot + 1));
        if (view) {
            VkDescriptorImageInfo ii = {MT.samp, view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = MT.set, .dstBinding = (uint32_t)k,
                                      .dstArrayElement = slot, .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
            vkUpdateDescriptorSets(vk_ctx()->device, 1, &w, 0, NULL);
        } else {
            MT.st_flat++;
        }
        mt_gen_write(k, slot);
        MT.nused[k]++, MT.st_new++;
    }
    uint32_t word = 1u << 31 | (uint32_t)k << 30 | (uint32_t)MT.s[k][slot].gen << 16 | slot;
    plat_mutex_unlock(&MT.lock);
    return word;
}

uint32_t vk_rt_mat_tex(VkImage image, VkImageView view, int array, const void *fs) {
    if (!MT.ok || !R.mat || !image || !view) return 0;
    return mt_get(array ? 1 : 0, image, view, fs);
}

uint32_t vk_rt_mat_flat(const void *fs) {
    if (!MT.ok || !R.mat || !fs) return 0;
    return mt_get(0, VK_NULL_HANDLE, VK_NULL_HANDLE, fs);
}

// The game's distance haze: G = S T + F (1 - T), T = exp(-sigma d) per channel (the screen's colour G against the
// colour S the hit is lit with here; mat_measure's means per distance bin, 2 a stop from 16 m). For each sigma of a
// log grid F is the least-squares fit; the sigma with the least error wins. Bin means, not per-pixel regressions: our S
// differs from the game's pixel by pixel (shadows, AO), which biases a slope towards 0. TOTK_RT_HAZE=0 off.
static void haze_fit(const uint32_t *f) {
    static int env = -1;
    if (env < 0) { const char *e = cfg_get("TOTK_RT_HAZE"); env = !(e && *e == '0'); }
    for (int b = 0; b < 16; b++)
        for (int i = 0; i < 13; i++) R.hacc[b][i] = R.hacc[b][i] * 0.98 + (double)f[b * 16 + i];
    double d[16], mS[16][3], mG[16][3], w[16];
    int nb = 0, far = 0;
    for (int b = 0; b < 16; b++) {
        const double *h = R.hacc[b];
        // up to 1.2 km: past it the screen holds the far mountains by the horizon, brighter than one exponential that
        // fits the nearer bins (the fit would flip between the two); the captured scene reaches about 2 km
        if (h[12] < 40.0 || HZ_D0 * exp2((b + 0.5) / 2.0) > 1200.0) continue;
        d[nb] = HZ_D0 * exp2((b + 0.5) / 2.0), w[nb] = h[12] / (h[12] + 500.0);
        for (int c = 0; c < 3; c++) mS[nb][c] = h[c] / 64.0 / h[12], mG[nb][c] = h[3 + c] / 64.0 / h[12];
        far += d[nb] > 100.0, nb++;
    }
    if (nb < 4 || far < 2) return;   // the last fit stays (none: off)
    float sig[3], F[3];
    for (int c = 0; c < 3; c++) {
        // the colour at most 1.5 x the brightest bin's: a long half distance with a bright colour fits the near bins as
        // well, but would blow out the far hits
        double best = 1e300, bs = 0.0, bF = 0.0, fmax = 0.0;
        for (int k = 0; k < nb; k++) fmax = mG[k][c] > fmax ? mG[k][c] : fmax;
        fmax *= 1.5;
        for (int i = 0; i <= 56; i++) {   // half distances from 69 km down to 50 m (the near bins hold little haze)
            double sg = 1e-5 * exp2(i / 4.0), num = 0.0, den = 0.0, e = 0.0;
            if (sg > 0.693 / 50.0) break;
            for (int k = 0; k < nb; k++) {
                double T = exp(-sg * d[k]), u = 1.0 - T;
                num += w[k] * u * (mG[k][c] - mS[k][c] * T), den += w[k] * u * u;
            }
            double Fc = den > 1e-12 ? num / den : 0.0;
            if (Fc < 0.0) Fc = 0.0;
            if (Fc > fmax) Fc = fmax;
            for (int k = 0; k < nb; k++) {
                double T = exp(-sg * d[k]), r = mG[k][c] - mS[k][c] * T - Fc * (1.0 - T);
                e += w[k] * r * r;
            }
            if (e < best) best = e, bs = sg, bF = Fc;
        }
        sig[c] = (float)bs, F[c] = (float)bF;
    }
    if (!R.hz_on) for (int c = 0; c < 3; c++) R.hz[0][c] = sig[c], R.hz[0][4 + c] = F[c];
    for (int c = 0; c < 3; c++)   // smoothed (the sums move slowly, the grid's steps would show)
        R.hz[0][c] += 0.1f * (sig[c] - R.hz[0][c]), R.hz[0][4 + c] += 0.1f * (F[c] - R.hz[0][4 + c]);
    R.hz[0][3] = 1.0f, R.hz[0][7] = 0.0f;
    R.hz_on = env;
    if ((R.gi_frame & 511) == 0) {
        char line[1536];
        int o = 0;
        for (int k = 0; k < nb && o < (int)sizeof line - 80; k++)
            o += snprintf(line + o, sizeof line - o, " [%.0fm S %.2f G %.2f model %.2f]", d[k], lumd(mS[k]), lumd(mG[k]),
                          0.2126 * (mS[k][0] * exp(-R.hz[0][0] * d[k]) + R.hz[0][4] * (1.0 - exp(-R.hz[0][0] * d[k]))) +
                          0.7152 * (mS[k][1] * exp(-R.hz[0][1] * d[k]) + R.hz[0][5] * (1.0 - exp(-R.hz[0][1] * d[k]))) +
                          0.0722 * (mS[k][2] * exp(-R.hz[0][2] * d[k]) + R.hz[0][6] * (1.0 - exp(-R.hz[0][2] * d[k]))));
        rt_log("vk: GI haze (%s): half distance %.0f %.0f %.0f m, colour %.2f %.2f %.2f;%s", R.hz_on ? "on" : "off",
               0.693 / (R.hz[0][0] > 1e-9f ? R.hz[0][0] : 1e-9f), 0.693 / (R.hz[0][1] > 1e-9f ? R.hz[0][1] : 1e-9f),
               0.693 / (R.hz[0][2] > 1e-9f ? R.hz[0][2] : 1e-9f), R.hz[0][4], R.hz[0][5], R.hz[0][6], line);
    }
}

// TOTK_RT_MPROBE=1 (debug, view 26): the materials hit at the probe grid that are not measured yet (rt_gi.comp states 1
// and 5) or that the texture explains little of (3), with their slot, shader and its albedo analysis; =2 every one
static float half_f(uint32_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? 65504.0f : ldexpf((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}
static void mprobe_log(const uint32_t *p) {
    static const char *const kinds[GFX_ALB_KINDS] = {"unknown", "2d", "array", "no output", "no sample", "nested",
                                                     "no varying", "uv math", "layer unknown", "other type", "failed"};
    for (int i = 0; i < 32; i++) {
        const uint32_t *a = p + i * 12, *b = a + 4, *f = a + 8;
        uint32_t st = a[3] & 15, r = (a[3] >> 4) & 15, k = (a[3] >> 8) & 0xffff, ci = a[3] >> 24;
        float t, zs, sbt;
        memcpy(&t, &b[3], 4), memcpy(&zs, &b[1], 4), memcpy(&sbt, &b[2], 4);
        if (!a[0] && !a[1]) continue;
        if (R.mprobe < 3 && st != 1 && st != 3 && st != 5) continue;
        char key[33] = "-", info[160] = "";
        plat_mutex_lock(&MT.lock);
        const MtSlot *e = k < 2 * MT_MAX ? &MT.s[k / MT_MAX][k % MT_MAX] : NULL;
        if (e && e->used && e->fs) {
            const GfxShader *fs = e->fs;
            const uint8_t *kb = gfx_shader_key(fs);
            for (int j = 0; j < 16; j++) snprintf(key + 2 * j, 3, "%02x", kb[j]);
            const GfxAlbedo *al = gfx_shader_albedo(fs);
            snprintf(info, sizeof info, "%s tex binding %u uv loc %u comp %u/%u math %u layer loc %u cbuf %d off %u discard %u view %s",
                     al->kind < GFX_ALB_KINDS ? kinds[al->kind] : "?", al->tex_binding, al->uv_loc, al->uv_comp & 15,
                     al->uv_comp >> 4, al->uv_math, al->layer_loc, al->layer_cbuf, al->layer_off, al->discard, e->view ? "yes" : "none");
        }
        plat_mutex_unlock(&MT.lock);
        float v[8];   // the look rgb, the reflection strength, the ambient look rgb, the emission's luminance
        for (int j = 0; j < 4; j++) v[2 * j] = half_f(f[j] & 0xffff), v[2 * j + 1] = half_f(f[j] >> 16);
        rt_log("vk: material probe %d: triangle %u word %08x flags %x layer %u, state %u raw %u slot %u (sun blocker %d at %.2f, screen at %.1f, "
               "instance %u, t %.1f) look %.2f %.2f %.2f spec %.2f ambient look %.2f %.2f %.2f emission lum %.3f; fs %s %s", i, a[0],
               a[1], a[2] >> 16, a[2] & 0xffff, st, r, k, (int)b[0], sbt, zs, ci, t, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], key, info);
    }
}

// The measured colours cleared once, before the first pass that reads them
static void mt_cal_clear(VkCommandBuffer cb) {
    if (!MT.cal_clear || !MT.cal.buf) return;
    vkCmdFillBuffer(cb, MT.cal.buf, 0, VK_WHOLE_SIZE, 0);
    mem_barrier(cb);
    MT.cal_clear = 0;
}

void vk_rt_image_gone(VkImage image) {
    if (!MT.ok || !image) return;
    plat_mutex_lock(&MT.lock);
    for (int k = 0; k < 2; k++) {
        if (!MT.nused[k]) continue;
        for (uint32_t i = 0; i < MT.fresh[k]; i++) {
            MtSlot *e = &MT.s[k][i];
            if (!e->used || e->image != image) continue;
            MtKey key;
            memset(&key, 0, sizeof key);
            key.view = e->view, key.fs = e->fs;
            nvn_hm_del(&MT.by_view[k], &key);
            e->used = 0, e->image = VK_NULL_HANDLE, e->view = VK_NULL_HANDLE, e->fs = NULL;
            mt_gen_write(k, i);
            MT.fifo[k][(MT.fhead[k] + MT.fcount[k]) % MT_MAX] = i, MT.fcount[k]++;
            MT.nused[k]--, MT.st_gone++;
            __atomic_fetch_add(&MT.gone_gen, 1, __ATOMIC_RELEASE);
        }
    }
    plat_mutex_unlock(&MT.lock);
}

static void mt_release(VkCtx *c) {
    if (MT.samp) vkDestroySampler(c->device, MT.samp, NULL);
    if (MT.pool) vkDestroyDescriptorPool(c->device, MT.pool, NULL);
    if (MT.dsl) vkDestroyDescriptorSetLayout(c->device, MT.dsl, NULL);
    free_buf(c, &MT.gen), free_buf(c, &MT.cal);
    free_buf(c, &MT.pcode), free_buf(c, &MT.pmat), free_buf(c, &MT.pext), free_buf(c, &MT.wext);
    nvn_hm_clear(&MT.by_prog, NULL);
    MT.prog_ok = 0, MT.pcode_used = 0;
    memset(MT.pnote, 0, sizeof MT.pnote);
    MT.samp = VK_NULL_HANDLE, MT.pool = VK_NULL_HANDLE, MT.dsl = VK_NULL_HANDLE, MT.set = VK_NULL_HANDLE, MT.ok = 0;
}

int vk_rt_capturing(void) { return R.ok && R.geom && R.cap_ready && (R.ao || R.shadows || R.test || R.gi); }
int vk_rt_shadow_capturing(void) { return R.ok && R.shadow && R.cap_ready && (R.ao || R.shadows || R.test || R.gi); }

// The menu's ray tracing switches in play (vk_contact.c, at each G-buffer): rt_ao and rt_shadows read again every 15th
// frame. Off, a feature's passes stop and the game's own AO / shadow is used; with both off the capture stops too.
void vk_rt_live(void) {
    static uint32_t n;
    if (!R.ok || n++ % 15) return;
    const char *a = cfg_get("TOTK_RT_AO"), *sh = cfg_get("TOTK_RT_SHADOWS");
    int ao = R.ao_ok && (a && *a ? strcmp(a, "0") != 0 : R.test), s = R.sh_ok && sh && !strcmp(sh, "1");
    static int cyc = -1;   // TOTK_RT_LIVE_TEST=<k> (debug): every k checks the next of both, AO only, shadows only, none
    if (cyc < 0) { const char *t = cfg_get("TOTK_RT_LIVE_TEST"); cyc = t && *t ? atoi(t) : 0; }
    if (cyc > 0) {
        int st = (int)((n / 15) / (uint32_t)cyc % 4);
        ao = R.ao_ok && (st == 0 || st == 1), s = R.sh_ok && (st == 0 || st == 2);
    }
    if (ao != R.ao) {
        R.ao = ao, R.prev_ok = 0;
        rt_log("vk: ray traced AO switched %s", ao ? "on" : "off");
    }
    const char *g = cfg_get("TOTK_RT_GI");
    int gi = R.gi_ok && g && !strcmp(g, "1");
    const char *gs = cfg_get("TOTK_RT_GI_STRENGTH");
    int gp = gs && *gs ? atoi(gs) : 300;
    if (cyc > 0) {   // the debug cycle: GI on and off in turn, its strength 100, 300, 600
        uint32_t k = (n / 15) / (uint32_t)cyc;
        gi = R.gi_ok && k % 2 == 0, gp = k % 3 == 0 ? 100 : k % 3 == 1 ? 300 : 600;
    }
    if (gi != R.gi) {
        R.gi = gi, R.gprev_ok = 0, R.rhist_ok = 0;
        rt_log("vk: ray traced GI switched %s (strength %d%%)", gi ? "on" : "off", gp);
    }
    R.gi_strength = (float)(gp < 0 ? 0 : gp > 600 ? 600 : gp) / 100.0f;
    if (s != R.shadows) {
        __atomic_store_n(&R.simg_ready, 0, __ATOMIC_RELEASE);
        R.shadows = s, R.sprev_ok = 0;
        rt_log("vk: ray traced sun shadows switched %s", s ? "on" : "off");
    }
    {
        const char *l = cfg_get("TOTK_RT_LIGHTS");
        int lt = l && !strcmp(l, "1");
        if (lt != R.lights) R.lights = lt, rt_log("vk: ray-traced light shadows switched %s", lt ? "on" : "off");
    }
    if (!R.sr_env) R.shadow_radius = R.shadows ? 80.0f : R.ao ? 15.0f : R.max_dist;
    if (rt_quality() != R.qual) {   // the menu's quality: its values again, the histories restarted
        read_quality();
        R.prev_ok = R.sprev_ok = R.gprev_ok = R.rhist_ok = 0;
        rt_log("vk: ray tracing quality switched to %s (GI rays %.0f m, reflections %.0f m, shadows to %.0f m, AO %d rays)",
               R.qual == 0 ? "low" : R.qual == 1 ? "medium" : R.qual == 2 ? "high" : "ultra", R.gi_len, R.refl_len, R.sh_far, R.ao_rays);
    }
    {
        const char *mt = cfg_get("TOTK_RT_MAT");
        int mon = mt && *mt == '1';
        static int mcyc = -1;   // TOTK_RT_MAT_LIVE_TEST=<k> (debug): materials (and step 6) off and on every k checks
        if (mcyc < 0) { const char *t = cfg_get("TOTK_RT_MAT_LIVE_TEST"); mcyc = t && *t ? atoi(t) : 0; }
        if (mcyc > 0) mon = (n / 15) / (uint32_t)mcyc % 2 == 1;
        mat_switch(vk_ctx(), mon);
    }
    const char *as = cfg_get("TOTK_RT_AO_STRENGTH"), *ss = cfg_get("TOTK_RT_SHADOW_STRENGTH");
    int ai = as && *as ? atoi(as) : 100, si = ss && *ss ? atoi(ss) : 100;
    R.ao_strength = (float)(ai < 0 ? 0 : ai > 200 ? 200 : ai) / 100.0f, R.sh_pct = si < 0 ? 0 : si > 100 ? 100 : si;
    const char *rf = cfg_get("TOTK_RT_REFL"), *tn = cfg_get("TOTK_RT_TONE");
    R.sh_nogame = cfg_bool("TOTK_RT_SHADOW_ONLY") != 0;
    int rp = (int)qdef(rf, 0.0f, 100.0f, 100.0f, 100.0f), tp = tn && *tn ? atoi(tn) : 100;
    float refl = (float)(rp < 0 ? 0 : rp > 200 ? 200 : rp) / 100.0f;
    if ((refl > 0.0f) != (R.refl > 0.0f)) R.rhist_ok = 0;
    R.refl = refl, R.tone = (float)(tp < 0 ? 0 : tp > 100 ? 100 : tp) / 100.0f;
}

// A G-buffer draw that bound its transform feedback pipeline (vk_exec_draw.c): verts corners go to the next free range
// of the clip buffer. 0: none left (the draw renders without being captured). The recorder has no packets for these
// extension commands: what it queued goes first.
// id (rt_mesh_id, 0: none): the draw's identity for the mesh cache. 0 is also returned for a draw whose cached copy
// stands in for it this frame.
// The sun's shadow passes (scam: the cascade's view-projection rows, 16 floats): only a mesh to be cached is captured,
// unprojected through its cascade's camera (index ci, bits 8-11 of its facing), both sides shown (bit 4). The G-buffer
// identities and the meshes whose box is in the scene camera's view are left to the G-buffer; a mesh drawn in several
// cascades counts once a frame.
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

int vk_rt_materials(void) { return __atomic_load_n(&R.mat_on, __ATOMIC_ACQUIRE); }
int vk_rt_programs(void) { return MT.prog_ok && __atomic_load_n(&R.mat_on, __ATOMIC_ACQUIRE); }
int vk_rt_prog_due(uint32_t w) {
    uint32_t mi = ((w >> 30) & 1) * MT_MAX + (w & 0xffff);
    return MT.prog_ok && (w >> 31) && (w & 0xffff) < MT_MAX &&
           __atomic_load_n(&MT.pnote[mi], __ATOMIC_RELAXED) != __atomic_load_n(&R.cframe, __ATOMIC_RELAXED) + 1;
}

// Step 6: a draw of material word w (the albedo's, or a flat material's) whose fragment shader fs has a program: its
// record written (once a frame): the program (uploaded the first time), the textures' words and the uniforms.
void vk_rt_prog_note(uint32_t w, const void *fs, const uint32_t *tex, const uint32_t *uni) {
    if (!MT.prog_ok || !(w >> 31) || !fs) return;
    uint32_t mi = ((w >> 30) & 1) * MT_MAX + (w & 0xffff), f = __atomic_load_n(&R.cframe, __ATOMIC_RELAXED);
    if ((w & 0xffff) >= MT_MAX) return;
    const GfxProg *p = gfx_shader_prog(fs);
    if (p->status != GFX_PROG_OK || !p->code) return;
    plat_mutex_lock(&MT.plock);
    if (MT.pnote[mi] == f + 1) { plat_mutex_unlock(&MT.plock); return; }
    uint32_t off = (uint32_t)(uintptr_t)nvn_hm_get(&MT.by_prog, &fs);
    if (!off) {
        uint32_t n4 = p->words / 4;
        if (MT.pcode_used + n4 > PCODE_V4) { MT.st_prog_full++; plat_mutex_unlock(&MT.plock); return; }
        memcpy((uint32_t *)MT.pcode.map + (size_t)MT.pcode_used * 4, p->code, (size_t)p->words * 4);
        vmaFlushAllocation(vk_ctx()->vma, MT.pcode.mem, (VkDeviceSize)MT.pcode_used * 16, (VkDeviceSize)n4 * 16);
        off = MT.pcode_used + 1, MT.pcode_used += n4, MT.st_prog_up++;
        nvn_hm_put(&MT.by_prog, &fs, (void *)(uintptr_t)off);
    }
    uint32_t *r = (uint32_t *)MT.pmat.map + (size_t)mi * PM_WORDS;
    r[1] = w;
    for (int i = 0; i < 8; i++) r[2 + i] = i < p->ntex ? tex[i] : 0;
    for (int i = 0; i < (int)PM_WORDS - 10; i++) r[10 + i] = i < p->nuni ? uni[i] : 0;
    __atomic_store_n(&r[0], off, __ATOMIC_RELEASE);
    vmaFlushAllocation(vk_ctx()->vma, MT.pmat.mem, (VkDeviceSize)mi * PM_WORDS * 4, PM_WORDS * 4);
    MT.pnote[mi] = f + 1, MT.st_notes++;
    plat_mutex_unlock(&MT.plock);
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

// The image of what the GI's apply adds (w x h, RGBA16F: rgb added, a the luminance it wrote; storage and sampled), to GENERAL; 0 when it cannot be made
static int added_image(VkCtx *c, VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (R.aimg && R.aw == w && R.ah == h) return 1;
    if (R.aview) vkDestroyImageView(c->device, R.aview, NULL);
    if (R.aimg) vmaDestroyImage(c->vma, R.aimg, R.aimg_mem);
    R.aimg = VK_NULL_HANDLE, R.aview = VK_NULL_HANDLE;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R16G16B16A16_SFLOAT, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &R.aimg, &R.aimg_mem, NULL) != VK_SUCCESS) { R.aimg = VK_NULL_HANDLE; return 0; }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = R.aimg, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R16G16B16A16_SFLOAT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(c->device, &vi, NULL, &R.aview) != VK_SUCCESS) { R.aview = VK_NULL_HANDLE; return 0; }
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = R.aimg, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
    R.aw = w, R.ah = h;
    return 1;
}

// The veil's images (post, veil) for a w x h scene, to GENERAL; 0 when they cannot be made
static int veil_images(VkCtx *c, VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (R.pimg && R.vw == w && R.vh == h) return 1;
    if (R.pview) vkDestroyImageView(c->device, R.pview, NULL);
    if (R.vview) vkDestroyImageView(c->device, R.vview, NULL);
    if (R.pimg) vmaDestroyImage(c->vma, R.pimg, R.pimg_mem);
    if (R.vimg) vmaDestroyImage(c->vma, R.vimg, R.vimg_mem);
    R.pimg = R.vimg = VK_NULL_HANDLE, R.pview = R.vview = VK_NULL_HANDLE, R.post_ok = R.veil_ok = 0;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_B10G11R11_UFLOAT_PACK32, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_B10G11R11_UFLOAT_PACK32, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vmaCreateImage(c->vma, &ii, &ai, &R.pimg, &R.pimg_mem, NULL) != VK_SUCCESS) { R.pimg = VK_NULL_HANDLE; return 0; }
    vi.image = R.pimg;
    if (vkCreateImageView(c->device, &vi, NULL, &R.pview) != VK_SUCCESS) { R.pview = VK_NULL_HANDLE; return 0; }
    ii.format = vi.format = VK_FORMAT_R8_UNORM, ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    if (vmaCreateImage(c->vma, &ii, &ai, &R.vimg, &R.vimg_mem, NULL) != VK_SUCCESS) { R.vimg = VK_NULL_HANDLE; return 0; }
    vi.image = R.vimg;
    if (vkCreateImageView(c->device, &vi, NULL, &R.vview) != VK_SUCCESS) { R.vview = VK_NULL_HANDLE; return 0; }
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = R.pimg, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageMemoryBarrier2 bb[2] = {b, b};
    bb[1].image = R.vimg;
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 2, .pImageMemoryBarriers = bb};
    vkCmdPipelineBarrier2(cb, &d);
    R.vw = w, R.vh = h;
    rt_log("vk: ray traced GI: the veil of the passes after it measured (%ux%u)", w, h);
    return 1;
}

// The late hook: the veil from the finished light target v and the scene as the GI left it (rt_late.comp mode 2)
static void late_veil(VkCommandBuffer cb, VkImageView v, uint32_t w, uint32_t h) {
    R.post_ok = 0;
    if (!R.late_pipe || !MT.set || w != R.vw || h != R.vh || !R.pview || !R.vview) return;
    VkDescriptorImageInfo pi = {R.sampler, R.pview, VK_IMAGE_LAYOUT_GENERAL}, oi = {VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL},
                          vi = {VK_NULL_HANDLE, R.vview, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[3] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &pi},
                                  {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi},
                                  {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 6, .descriptorCount = 1,
                                   .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &vi}};
    struct { int32_t size[4]; float hs[4]; float hf[3]; uint32_t fr; int32_t md[4]; } pc = {
        {(int32_t)w, (int32_t)h, 0, 0}, {0, 0, 0, 0}, {0, 0, 0}, 0, {2, 0, 0, 0}};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.late_pipe);
    rec_push(R.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 0, 3, wr);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.ao_layout, 2, 1, &MT.set, 0, NULL);
    vkCmdPushConstants(cb, R.ao_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    mem_barrier(cb);
    R.veil_ok = 1;
}

// The glow: the scene colour before the composite (j->pc[13] 2: a job for this alone; 1: with the AO's injection)
static void emit_copy(VkCtx *c, VkCommandBuffer cb, const VkExecJob *j) {
    uint32_t w = j->scene.width, h = j->scene.height;
    if (!j->scene.img || !j->scene.img->image || !w || !h) return;
    if (R.eimg && (R.ew != w || R.eh != h)) {
        vkDestroyImageView(c->device, R.eview, NULL), vmaDestroyImage(c->vma, R.eimg, R.eimg_mem);
        R.eimg = VK_NULL_HANDLE, R.eview = VK_NULL_HANDLE, R.e_ok = 0;
    }
    if (!R.eimg) {
        VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_B10G11R11_UFLOAT_PACK32, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ii, &ai, &R.eimg, &R.eimg_mem, NULL) != VK_SUCCESS) { R.eimg = VK_NULL_HANDLE; return; }
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = R.eimg,
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(c->device, &vi, NULL, &R.eview) != VK_SUCCESS) {
            vmaDestroyImage(c->vma, R.eimg, R.eimg_mem), R.eimg = VK_NULL_HANDLE, R.eview = VK_NULL_HANDLE;
            return;
        }
        R.ew = w, R.eh = h;
        VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = R.eimg, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
        vkCmdPipelineBarrier2(cb, &d);
        rt_log("vk: ray traced GI: its lights from the G-buffer's glow (%ux%u copy before the composite)", w, h);
    }
    VkImageCopy rg = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImage(cb, j->scene.img->image, VK_IMAGE_LAYOUT_GENERAL, R.eimg, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    mem_barrier(cb);
    R.e_ok = 1;
}

void vk_rt_exec(VkExec *ex, const VkExecJob *j) {
    VkCtx *c = vk_ctx();
    if (!R.tried) init(c);
    if (!R.ok) return;
    if (!R.ao && !R.shadows && !R.test && !R.gi) { cache_skip(); return; }   // both switched off in the menu
    int gi_job = j->pc[12] > 1.5f;   // the GI after the composite (vk_contact.c)
    const float *u = R.cam[(int)j->pc[15] & 15];
    const float *iv = u + 11 * 4;   // rows 11-13: view space -> world
    float cam[3] = {iv[3], iv[7], iv[11]}, fwd[3] = {-iv[2], -iv[6], -iv[10]};
    float fl = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    if (!(fl > 1e-6f)) { cache_skip(); return; }   // no usable camera: nothing recorded
    float dx = cam[0] - R.center[0], dy = cam[1] - R.center[1], dz = cam[2] - R.center[2];
    int place = !R.placed || dx * dx + dy * dy + dz * dz > 80.0f * 80.0f;   // the camera moved on (also for the dump)
    if (place) R.probe_placed = 0, R.since_place = 0;
    // The sun toward world space, for the probe placement: the sphere 3 m from the probe toward the sun
    float sv[3] = {j->pc[4], j->pc[5], j->pc[6]}, L[3];
    for (int i = 0; i < 3; i++) L[i] = iv[i * 4] * sv[0] + iv[i * 4 + 1] * sv[1] + iv[i * 4 + 2] * sv[2];
    const float *pr = R.probe.map;
    int at_probe = R.sphere && !place && !R.probe_placed && ++R.since_place > 30 && pr[3] == 1.0f;
    VkImageView in = vk_exec_target_view(j->depth.img, VK_FORMAT_R32_SFLOAT, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
                                         0, 0, 1, 0);
    VkImageView out = vk_exec_target_view(j->scene.img, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_IMAGE_VIEW_TYPE_2D,
                                          VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0);
    if (!in || !out) { cache_skip(); return; }
    vk_exec_barrier(ex);   // as vk_contact_exec: a full barrier that also takes feedback-layout images to GENERAL
    // the passes below may write the scene colour (the GI, the test view): a fence write-back snapshot made before is
    // stale, as for the contact shadows (vk_contact_exec)
    vk_texture_storage_written(j->scene.img);
    VkCommandBuffer cb = ex->cb;
    if (R.wattr_clear && R.wattr.buf) {   // materials just switched on (mat_switch)
        vkCmdFillBuffer(cb, R.wattr.buf, 0, VK_WHOLE_SIZE, 0);
        if (MT.wext.buf) vkCmdFillBuffer(cb, MT.wext.buf, 0, VK_WHOLE_SIZE, 0);
        mem_barrier(cb);
        R.wattr_clear = 0;
    }
    static int eon = -1;   // TOTK_RT_GI_GLOW=0: the GI's lights by brightness over albedo, as before (debug)
    if (eon < 0) { const char *e = cfg_get("TOTK_RT_GI_GLOW"); eon = !(e && *e == '0'); }
    if (j->pc[13] > 0.5f && R.gi && eon) emit_copy(c, cb, j);
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
        R.dump_frames = place ? 0 : R.dump_frames + 1;   // builds since the camera arrived
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
    if (!R.blas_built) {
        VkAccelerationStructureGeometryKHR g = sphere_geometry();
        build_as(cb, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
                 &g, SPH_NI / 3, R.blas);
        mem_barrier(cb);
        R.blas_built = 1;
    }
    int tlas = R.geom || place || at_probe;
    if (place && !R.sphere) {   // hidden: the centre only follows the camera
        for (int i = 0; i < 3; i++) R.center[i] = cam[i];
        R.placed = 1;
        if (R.geom) place = 0;   // the instances do not change
    }
    if (place || at_probe) {
        for (int i = 0; i < 3; i++) R.center[i] = at_probe ? pr[i] + L[i] * 3.0f : cam[i] + fwd[i] / fl * 10.0f;
        if (at_probe) {
            R.probe_placed = 1;
            rt_log("vk: ray tracing test: probe at %.2f %.2f %.2f, sun (world) %.3f %.3f %.3f: sphere moved over it", pr[0], pr[1],
                   pr[2], L[0], L[1], L[2]);
        }
        R.inst_dirty = 1;
        R.placed = 1;
        rt_log("vk: ray tracing test: camera at %.2f %.2f %.2f looking %.3f %.3f %.3f, sphere at %.2f %.2f %.2f; sun (view) %.3f %.3f %.3f",
               cam[0], cam[1], cam[2], fwd[0] / fl, fwd[1] / fl, fwd[2] / fl, R.center[0], R.center[1], R.center[2],
               j->pc[4], j->pc[5], j->pc[6]);
    }
    uint32_t islot = R.cache ? R.qframe % 8 : 0, ninst = 2;
    if (R.cache) R.inst_dirty = 1;   // the cells change
    if (R.inst_dirty) {   // the scene (identity, mask 1) and the sphere (mask 2); a mask of 0 leaves one out
        VkAccelerationStructureDeviceAddressInfoKHR ai = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                                                          .accelerationStructure = R.blas};
        VkAccelerationStructureInstanceKHR inst[2] = {
            {.transform = {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}}, .mask = 0,
             .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR},
            {.transform = {{{1, 0, 0, R.center[0]}, {0, 1, 0, R.center[1]}, {0, 0, 1, R.center[2]}}},
             .instanceCustomIndex = 1, .mask = R.sphere ? 0x02 : 0,
             .flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR,
             .accelerationStructureReference = R.as_addr(c->device, &ai)}};
        if (R.geom) {
            ai.accelerationStructure = R.cur ? R.scene2 : R.scene;
            inst[0].mask = 0x01, inst[0].instanceCustomIndex = 0, inst[0].accelerationStructureReference = R.as_addr(c->device, &ai);
        } else {
            inst[0] = inst[1];   // no scene: the sphere twice
        }
        VkAccelerationStructureInstanceKHR *dst = (VkAccelerationStructureInstanceKHR *)R.inst.map + (size_t)islot * INST_MAX;
        memcpy(dst, inst, sizeof inst);
        if (R.cache) ninst += cache_instances(cam, dst + 2);
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
    if (gi_job) {   // after the composite: the lit scene gets one bounce (phase 7 probe)
        if (R.gi && R.gi_pipe) gi_exec(cb, j, u, iv, cam, in, out);
        if (dup || !R.ao) goto traced;   // a GI job that built the scene (no AO before the composite) also runs the AO
    }
    if (j->pc[14] > 0.5f) {   // the sun shadows, before the half-resolution lighting (vk_contact.c)
        static int skip = -1;   // TOTK_RT_SHADOW_SKIP (debug): 1 no dispatches in the first pass, 2 in neither
        if (skip < 0) { const char *e = cfg_get("TOTK_RT_SHADOW_SKIP"); skip = e && *e ? atoi(e) : 0; }
        if (R.shadows && !(skip >= 1 && j->pc[14] < 1.5f) && skip < 2) shadow_exec(cb, j, u, iv, cam, in);
        goto traced;
    }
    // shadows alone: nothing to do at the composite. The shadow views (9-14) too: without this they fell through to
    // the phase-1 test below, which darkens the composite with its own hard shadow ray (the review's "missing shapes")
    if (R.shadows && !R.ao && (R.view == 0 || R.view >= 9) && (R.view < 15 || R.view > 17)) goto traced;
    if (R.ao && (R.view == 0 || R.view >= 6) && (R.view < 15 || R.view > 17)) {
        const VkExecTarget *at = &R.aot[(int)j->pc[15] & 15];
        int inj = j->pc[13] > 0.5f && R.ao_inject && R.inj_pipe && at->image && at->img &&
                  at->width == (j->scene.width + 1) / 2 && at->height == (j->scene.height + 1) / 2;
        ao_exec(cb, j, u, iv, cam, in, out, inj ? at : NULL);
        if (inj) vk_texture_storage_written(at->img);   // the injection wrote the game's AO target (as vk_contact.c)
        goto traced;
    }
    float pcv[28] = {j->pc[0], j->pc[1], j->pc[2], (float)R.view,
                     iv[0], iv[1], iv[2], iv[3], iv[4], iv[5], iv[6], iv[7], iv[8], iv[9], iv[10], iv[11],
                     j->pc[4], j->pc[5], j->pc[6], R.strength,
                     (float)j->scene.width, (float)j->scene.height, R.bright, cam_near(u),
                     (float)(j->scene.width * 62 / 100), (float)(j->scene.height * 88 / 100), 0, 0};
    if (!R.logged) {
        R.logged = 1;
        rt_log("vk: ray tracing test on a %ux%u scene; inverse view rows %.3f %.3f %.3f %.2f / %.3f %.3f %.3f %.2f / %.3f %.3f %.3f %.2f",
               j->scene.width, j->scene.height, iv[0], iv[1], iv[2], iv[3], iv[4], iv[5], iv[6], iv[7], iv[8], iv[9], iv[10], iv[11]);
    }
    VkDescriptorImageInfo ii = {R.sampler, in, VK_IMAGE_LAYOUT_GENERAL}, oi = {VK_NULL_HANDLE, out, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo pbi = {R.probe.buf, 0, 16}, wbi = {R.geom ? R.world.buf : R.vb.buf, 0, VK_WHOLE_SIZE},
                           vbi = {R.vb.buf, 0, VK_WHOLE_SIZE}, ibi = {R.ib.buf, 0, VK_WHOLE_SIZE},
                           gbi = {R.gtab.buf, 0, VK_WHOLE_SIZE}, abi = {R.mat ? R.wattr.buf : R.probe.buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[8] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &oi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 3, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &pbi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 4, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &wbi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 5, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &vbi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 6, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &ibi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 7, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &gbi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 8, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &abi}};
    mt_cal_clear(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.pipe);
    rec_push(R.push, cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 0, 8, w);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 1, 1, &R.as_set, 0, NULL);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, R.layout, 2, 1, &MT.set, 0, NULL);
    vkCmdPushConstants(cb, R.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pcv, pcv);
    vkCmdDispatch(cb, (j->scene.width + 7) / 8, (j->scene.height + 7) / 8, 1);
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
    int rq = R.rq, pf = R.pf;   // the device's; the settings are read again (read_settings)
    memset(&R, 0, sizeof R);
    R.rq = rq, R.pf = pf;
}
