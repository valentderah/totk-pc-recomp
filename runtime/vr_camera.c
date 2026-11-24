// VR camera (TOTK_VR / --vr; install side, compiled with patches.h like mouse_direct.c). Hosts for the `vr` patch set
// in recomp/patches-v143.txt:
// - the game draws one frame per eye in turn (stereo pairs): each frame's eye decides its game step (frame_rate.c) and
//   the head pose it is drawn with;
// - the renderer draws from a head copy of the game camera (render camera records, setView, event cameras), while the
//   game logic (movement, aim, culling decisions of its own) keeps its own camera;
// - the VR camera sits on a level orbit behind Link (the right stick's pitch, ragdolls and the game's camera height
//   changes don't move it), in the game's normal camera modes; other modes use the game camera with the head;
// - head aim (bow, throws, the camera / scope viewfinder), the HUD's per-layout markers and the scene pipeline's stage
//   markers for the HUD replay (vk_exec.c).
// The render camera build 0x7b0c78 (main+0x7af390 -> page+0xdc0) fills a double-buffered object: +0x88 inverse view
// (3x4, row-major), +0x14c view (3x4), +0x17c / +0x198 the look-at point, +0x210 a copy of the view, +0x250 a second view.
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "patches.h"
#include "platform/platform.h"

#define G(a) (0x7000000000ULL + (a))
#define HOST_CAM_BUILD (RT_PATCH_PAGE + 0xdc0)

int vk_xr_world(void);
int vk_xr_replay_stereo(void);

static _Atomic unsigned builds;

int vk_xr_eye_view(int eye, float pos[3], float rot[9], float *dx);   // vk_xr.c

static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The main camera (a sead::Camera, matrix at +8) gets the headset on top after each camera update
// (host_cam_update). The camera: the game camera's position and yaw (pitch and roll dropped, so the horizon stays level
// whatever the game camera does), then the eye's pose from the headset in game units of TOTK_VR_SCALE per metre. A
// view's rows are the camera's right, up and back axes in world space (it looks along -back, as an OpenXR view does)
// and its translation -R c.
static struct { float hp[3], hm[9], dx; int have, eye; unsigned serial, noted; uint64_t t; unsigned xr; } FP;   // the frame's head pose (calc thread)
// Each eye's frame gets that eye's pose (the eyes sit either side of the head).
// TOTK_VR_HEAD_POSITION (default 0): the headset's position moves the camera too. Off, only its rotation counts and the
// eyes sit at the game camera: the position is measured from the play space's origin, and in the game camera's turning
// frame such an offset swings around the camera whenever the game camera turns.
static int head_position(void) { return cfg_bool("TOTK_VR_HEAD_POSITION"); }
unsigned vk_xr_note_replay(float yaw, const float cam[3], float scale);   // vk_xr.c
int vr_camera_replay(void);
// TOTK_VR_ONEPOSE: the whole frame from one head pose, the one the frame
// started with (the renderer's copy: shadows, effects); the replay uses that pose too instead of a newer one, and the
// pair is labelled with it, so SteamVR's reprojection turns the finished picture by the exact difference to the head at
// display time. Nothing estimated, everything in the frame agrees.
static int onepose(void) { return vr_camera_replay() && cfg_bool("TOTK_VR_ONEPOSE"); }
static _Atomic uint64_t main_cam;   // (declared again below)
static void vr_frame_pose(int eye) {
    FP.have = vk_xr_eye_view(eye, FP.hp, FP.hm, &FP.dx), FP.eye = eye, FP.serial++, FP.t = kern_now_ns();
    if (FP.have && !head_position()) FP.hp[0] = FP.hp[1] = FP.hp[2] = 0;
    if (FP.have && vk_xr_replay_stereo())
        for (int k = 0; k < 3; k++) FP.hp[k] += FP.dx * FP.hm[k];   // hm row 0: the head's right axis (tracking space)
    FP.xr = FP.have && onepose() ? vk_xr_note_replay(0, (const float[3]){0, 0, 0}, 1) : 0;   // its views, for the pair's label
    { void vk_xr_replay_set_eye(unsigned s, int eye); if (FP.xr) vk_xr_replay_set_eye(FP.xr, eye); }
}
void vk_xr_note_game(int eye, float yaw, const float cam[3], float scale);   // vk_xr.c

// The headset on top of a view (3x4, in place), with an explicit head pose (note: queue this frame's eye entry).
static int vap_free;   // (defined below, with the event copies)
static void vr_apply_pose(float *V, const float hp_[3], const float hm_[9], int note);
int vr_camera_replay(void);
// The renderer's head copies (render camera slots, setView copies).
static void vr_apply(float *V) {
    vr_apply_pose(V, FP.hp, FP.hm, 1);
}
// TOTK_VR_LEVEL (default 1): the game camera's height change as it orbits Link (right stick up/down) leaves the VR
// camera alone: it sits on the level orbit, at the orbit point (render camera +0x17c, Link's look-at point, on the
// camera's back axis) plus the horizontal distance and height the game camera has when it
// looks about level (learned, smoothed). The orbit distance per camera version comes from the builds (orbit ring).
int vr_camera_replay(void);
static struct { float cam[3], dist; } orb[16];
static _Atomic unsigned orb_n;
static float lvl_d, lvl_h;
// The game's own camera mode: the player camera's "Chase" calculation 0x19651c (the
// Normal* camera modes: the orbit the level lock rebuilds) runs once per frame only while such a mode is active; ladders
// (ClimbObj), lock-on, aiming, warps, talks and cutscenes use other modes. Its call at main+0x18e480 -> page+0xd40 notes
// the frame and the mode record ([node+0x28], a PlayerCameraParam record: Normal, Normal_Indoor, Normal_Swim, ...).
// The level lock and the pin act only while it runs; in every other mode the VR camera is the game's own camera (its
// position, collisions and framing) with the head (TOTK_VR_CHASEGATE=0 turns it off, always).
static _Atomic unsigned chase_tick;   // render camera builds (2 per frame) when the Chase calculation last ran
static _Atomic uint64_t chase_param;
static int chase_active(void) {
    static int on = -1;
    if (on < 0) { const char *e = cfg_get("TOTK_VR_CHASEGATE"); on = !e || *e != '0'; }
    if (!on) return 1;
    return (int)(atomic_load(&builds) - atomic_load(&chase_tick)) <= 6;
}
int vr_chase_active(void) { return chase_active(); }   // vk_xr.c: the reticle's mode (game camera in its Chase / normal mode)
// The level camera's height above the learned one (TOTK_VR_CAM_HEIGHT, m, live; F1 > VR)
static float lvl_hh(void) { return lvl_h + (float)cfg_double("TOTK_VR_CAM_HEIGHT"); }
// Collisions: the game's own camera comes in when a wall or an edge is behind Link; the level orbit never
// sits further out than the game camera's own horizontal distance from Link (smoothed; the height shrinks with it, so the
// view angle stays). TOTK_VR_COLLIDE=0: off.
static float coll_k = 1;
static float lvl_dist(void) { return lvl_d * coll_k; }
static float lvl_height(void) { return lvl_hh() * coll_k; }
static int lvl_ok;
static float orb_tgt[3];   // the newest orbit point (look-at point)
// Link's position as the level lock's anchor (TOTK_VR_LINKANCHOR): the camera's look-at point moves away from
// Link while targeting / with the shield up (L2), and a view placed from it would go with it. Read once per frame (rslot_frame), the lock is learned and applied relative to Link.
int vr_player_pos(float xyz[3]);   // frame_rate.c
static float link_pos[3];
static _Atomic int link_ok;
// Link ragdolls (a heavy hit, a lightning strike; following him as he rolls makes the view jitter): the view's anchor
// stays where it was when the ragdoll began, and glides back to him once he gets up.
// TOTK_VR_RAGLOCK=0: off.
int player_ragdoll(void);   // mouse_direct.c
#define RAG_GLIDE_S 0.6f      // seconds back to Link after he starts to stand up
#define RAG_JUMP_M 8.0f       // further away than this he was moved (reload, warp): no glide
static struct { int on, gliding; float held[3], from[3]; int64_t t_free; } RAG;
static uint8_t rag_cam[0x200];   // the game camera object of the last normal frame (after the pin): an event camera's stand-in while Link ragdolls
static int rag_cam_ok;
static float rag_off[3];      // the camera's offset from the anchor in the last normal frame (modes without the level lock)
static int rag_off_ok;
// lp: Link's position read this frame; changed to the anchor's while the lock holds or glides back. now in ns.
static void rag_anchor(float lp[3], int64_t now) {
    static int en = -1, seen;
    static float good[3];
    if (en < 0) { const char *e = cfg_get("TOTK_VR_RAGLOCK"); en = !e || *e != '0'; }
    if (!en) return;
    int rag = player_ragdoll();
    // The ragdoll flag is also set while Link flies on the paraglider: no hold while his current verb is a movement he
    // steers himself (otherwise the view stays behind at the ledge).
    // Verb ids from the player verb list (names at main+0x3636400): 5 Glide, 6 Floating, 7 Fly, 18 WaterDive, 19 AirDive,
    // 20 GuideDive, 24-27 Ride/RideRobot/RideDragon/RideJump, 30 Surf, 31 Wakeboarding, 56 HangingFlight,
    // 94 ParasailGlide, 95 AnimalGlide, 199 RideSled. TOTK_VR_RAGFREE=comma list replaces it ("-" = none).
    if (rag) {
        static int nfree = -1, freev[32];
        if (nfree < 0) {
            const char *e = cfg_get("TOTK_VR_RAGFREE");
            static const int def[] = {5, 6, 7, 18, 19, 20, 24, 25, 26, 27, 30, 31, 56, 94, 95, 199};
            nfree = 0;
            if (e && *e) { for (const char *q = e; *q && nfree < 32;) { if (*q >= '0' && *q <= '9') { freev[nfree++] = atoi(q); while (*q >= '0' && *q <= '9') q++; } else q++; } }
            else for (size_t i = 0; i < sizeof def / sizeof *def; i++) freev[nfree++] = def[i];
        }
        int32_t v[5];
        int player_verbs(int32_t out[5]);
        if (player_verbs(v))
            for (int i = 0; i < nfree; i++)
                if (v[0] == freev[i]) {
                    static unsigned nl;
                    if (nl++ < 10) rt_log("vr: ragdoll flag while Link steers himself (verb %d): view not held", v[0]);
                    rag = 0;
                    break;
                }
    }
    if (rag && !RAG.on) {
        memcpy(RAG.held, seen ? good : lp, 12);   // the last position before the ragdoll (a glide back still running ends here)
        RAG.on = 1, RAG.gliding = 0;
        static unsigned nl;
        if (nl++ < 20) rt_log("vr: Link ragdolls: view held at %.2f %.2f %.2f", RAG.held[0], RAG.held[1], RAG.held[2]);
    } else if (!rag && RAG.on) {
        RAG.on = 0, RAG.t_free = now, memcpy(RAG.from, RAG.held, 12);
        float d[3] = {RAG.held[0] - lp[0], RAG.held[1] - lp[1], RAG.held[2] - lp[2]};
        float m = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        RAG.gliding = m < RAG_JUMP_M;
        static unsigned nl;
        if (nl++ < 20) rt_log("vr: Link stands up: view %s %.2f m back to him", RAG.gliding ? "glides" : "jumps", m);
    }
    if (RAG.on) memcpy(lp, RAG.held, 12);
    else {
        float raw[3];
        memcpy(raw, lp, 12);
        if (RAG.gliding) {
            float t = (float)((now - RAG.t_free) / 1e9) / RAG_GLIDE_S;
            if (t >= 1) RAG.gliding = 0;
            else {
                float e = t * t * (3 - 2 * t);   // smooth at both ends
                for (int k = 0; k < 3; k++) lp[k] = RAG.from[k] + (raw[k] - RAG.from[k]) * e;
            }
        }
        memcpy(good, RAG.gliding ? lp : raw, 12), seen = 1;
    }
}
static _Atomic int orb_tgt_ok;
// The newest look-at point (Link): the aim tracker (vk_contact.c) starts the game's aim ray past it.
int vr_orbit_target(float t[3]) {
    if (!atomic_load(&orb_tgt_ok)) return 0;
    memcpy(t, orb_tgt, 12);
    return 1;
}
static float pin_last[3];   // the position the pin wrote last (rslot_frame)
static void orbit_note(const float cam[3], const float tgt[3], const float back[3]) {
    float d[3] = {cam[0] - tgt[0], cam[1] - tgt[1], cam[2] - tgt[2]}, dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    unsigned n = atomic_load(&orb_n);
    memcpy(orb[n & 15].cam, cam, 12), orb[n & 15].dist = dist;
    memcpy(orb_tgt, tgt, 12), atomic_store(&orb_tgt_ok, 1);
    atomic_store(&orb_n, n + 1);
    // learned only while the game camera rests about level (within 15 degrees, pitch and distance steady for 20 frames):
    // sweeping through level with the stick changes the orbit distance fast (TOTK pulls back as it looks down)
    static float last_by, last_d;
    static int steady;
    steady = fabsf(back[1] - last_by) < 0.002f && fabsf(dist - last_d) < 0.01f * (dist + 1) ? steady + 1 : 0;
    last_by = back[1], last_d = dist;
    // Not from the position the pin wrote (after a teleport the game does not rebuild its camera until the
    // stick moves; the lock read its own pinned position back as the game's and the learned distance crept up to 9 m at
    // Link's height, the view inside ledges), and only orbits like TOTK's own (0.8..4 m above Link, 2..12 m behind;
    // the teleport arrival camera sits at Link's height). TOTK_VR_LVLGUARD=0 turns it off.
    static int guard = -1;
    if (guard < 0) { const char *e = cfg_get("TOTK_VR_LVLGUARD"); guard = !e || *e != '0'; }
    int self = fabsf(cam[0] - pin_last[0]) + fabsf(cam[1] - pin_last[1]) + fabsf(cam[2] - pin_last[2]) < 1e-3f;
    if (guard && self) return;
    if (fabsf(back[1]) < 0.26f && dist > 0.3f && steady >= 20) {
        // the camera's offset from the anchor: along its horizontal back direction, and height (the look-at point lies on
        // the camera's axis; Link, the anchor with TOTK_VR_LINKANCHOR, lies below and a little aside of it)
        float bl = sqrtf(back[0] * back[0] + back[2] * back[2]);
        float hd = bl > 1e-3f ? (d[0] * back[0] + d[2] * back[2]) / bl : dist, hh = d[1];
        if (guard && atomic_load(&link_ok) && !(hh > 0.8f && hh < 4 && hd > 2 && hd < 12)) return;
        if (!lvl_ok) lvl_d = hd, lvl_h = hh, lvl_ok = 1;
        else lvl_d += 0.05f * (hd - lvl_d), lvl_h += 0.05f * (hh - lvl_h);
        static unsigned nl;
        if (++nl % 600 == 1) rt_log("vr: level camera %.2f behind and %.2f above the orbit point (game camera pitch %.1f deg)", lvl_d, lvl_h, asinf(back[1]) * 57.2958f);
    }
}
static int orbit_dist(const float cam[3], float *dist) {
    unsigned n = atomic_load(&orb_n);
    float best = 1e9f;
    for (unsigned k = 0; k < 16 && k < n; k++) {
        const float *c = orb[(n - 1 - k) & 15].cam;
        float e = fabsf(c[0] - cam[0]) + fabsf(c[1] - cam[1]) + fabsf(c[2] - cam[2]);
        if (e < best) best = e, *dist = orb[(n - 1 - k) & 15].dist;
    }
    return n && best < 0.5f;
}
// The game camera's position seen from the VR camera (the level lock moves the VR camera off it: the bow camera is ~1 m
// behind Link, the VR camera at the level orbit's ~5 m), in the VR camera's yaw frame, metres: vk_xr.c puts the aim
// reticle where the game's aim ray (from the game camera) hits the scene.
static float aim_off[3];
void vr_aim_offset(float o[3]) { memcpy(o, aim_off, sizeof aim_off); }
static void vr_apply_pose(float *V, const float hp_[3], const float hm_[9], int note) {
    float cam[3];   // camera position = -R^T t
    for (int k = 0; k < 3; k++) cam[k] = -(V[0 * 4 + k] * V[3] + V[1 * 4 + k] * V[7] + V[2 * 4 + k] * V[11]);
    static float yaw_b[3] = {0, 0, 1};
    // the yaw from the right axis (level: the game camera has no roll), stable when the camera looks straight up/down
    float bx = -V[2], bz = V[0], n = sqrtf(bx * bx + bz * bz);
    if (n > 1e-3f) yaw_b[0] = bx / n, yaw_b[1] = 0, yaw_b[2] = bz / n;
    float dist;
    int lv = !vap_free && vr_camera_replay() && lvl_ok && cfg_bool("TOTK_VR_LEVEL") && chase_active();
    int hit = lv && orbit_dist(cam, &dist);
    // No build of this camera yet (setView runs before the frame's render camera is built, so during camera motion
    // the newest version is not in the ring): the orbit point is where the camera's view line passes the newest look-at
    // point.
    if (lv && !hit && atomic_load(&orb_tgt_ok)) {
        float d[3] = {cam[0] - orb_tgt[0], cam[1] - orb_tgt[1], cam[2] - orb_tgt[2]};
        dist = d[0] * V[8] + d[1] * V[9] + d[2] * V[10];
        hit = dist > 0.1f;
    }
    if (lv) {   // diagnostic: how often a camera version finds its orbit distance
        static unsigned nh, nm;
        hit ? nh++ : nm++;
        if (nh + nm >= 2000) rt_log("vr: level lock: %u camera versions locked, %u not", nh, nm), nh = nm = 0;
    }
    float game_cam[3] = {cam[0], cam[1], cam[2]};
    if (lv && atomic_load(&link_ok) && cfg_bool("TOTK_VR_LINKANCHOR")) {   // Link + the learned level offset
        for (int k = 0; k < 3; k++) cam[k] = link_pos[k] + lvl_dist() * yaw_b[k] + (k == 1 ? lvl_height() : 0);
        hit = 0;
    }
    // Every other camera mode (bow, ladder, lock-on, shield, events): the game camera's own
    // horizontal place, but its height held at Link + the level height, so right-stick tilt never moves the view up or
    // down. TOTK_VR_LOCKALL=0: those modes keep the game camera's height.
    static int lockall = -1;
    if (lockall < 0) { const char *e = cfg_get("TOTK_VR_LOCKALL"); lockall = !e || *e != '0'; }
    if (!lv && !vap_free && lockall && vr_camera_replay() && lvl_ok && cfg_bool("TOTK_VR_LEVEL") && atomic_load(&link_ok)) {
        cam[1] = link_pos[1] + lvl_height();
        static unsigned nl;
        if (nl < 3) nl++, rt_log("vr: level lock in a non-follow camera mode: height held %.2f above Link", lvl_height());
    }
    if (hit) {
        for (int k = 0; k < 3; k++)   // orbit point = cam - dist * back; then the level orbit
            cam[k] = cam[k] - dist * V[8 + k] + lvl_d * yaw_b[k] + (k == 1 ? lvl_hh() : 0);
    }
    if (atomic_load(&link_ok) && !vap_free) {   // Link ragdolls in a mode without the level lock (death...): the camera stays
        if (RAG.on) { if (!lv && rag_off_ok) for (int k = 0; k < 3; k++) cam[k] = RAG.held[k] + rag_off[k]; }
        else if (!RAG.gliding) { for (int k = 0; k < 3; k++) rag_off[k] = cam[k] - link_pos[k]; rag_off_ok = 1; }
    }
    const float U[3] = {0, 1, 0}, *B = yaw_b;
    float Rr[3] = {U[1] * B[2] - U[2] * B[1], U[2] * B[0] - U[0] * B[2], U[0] * B[1] - U[1] * B[0]};
    const float *base[3] = {Rr, U, B};
    float scale = (float)cfg_double("TOTK_VR_SCALE");
    if (!(scale > 0.01f)) scale = 1;
    {   // where the game camera sits seen from the VR camera, in its yaw frame (m): the aim reticle starts there
        float o[3] = {game_cam[0] - cam[0], game_cam[1] - cam[1], game_cam[2] - cam[2]};
        for (int i = 0; i < 3; i++) aim_off[i] = dot3(o, base[i]) / scale;
    }
    float ax[3][3], p[3];
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) ax[i][k] = base[0][k] * hm_[i * 3] + base[1][k] * hm_[i * 3 + 1] + base[2][k] * hm_[i * 3 + 2];
    for (int k = 0; k < 3; k++) p[k] = cam[k] + scale * (base[0][k] * hp_[0] + base[1][k] * hp_[1] + base[2][k] * hp_[2]);
    for (int j = 0; j < 3; j++) {
        for (int k = 0; k < 3; k++) V[j * 4 + k] = ax[j][k];
        V[j * 4 + 3] = -dot3(ax[j], p);
    }
    if (note && FP.noted != FP.serial) {   // once per frame: the frame's eye queue entry (vk_xr.c)
        FP.noted = FP.serial;
        vk_xr_note_game(FP.eye, atan2f(B[0], B[2]), cam, scale);   // yaw frame Ry(yaw): back axis (sin, 0, cos)
    }
}

// setView (every `bl 0x649ed0`): learns which matrix is the main camera's (the one main+0x875140 passes).
static _Atomic uint64_t main_cam;
static float main_view[12];   // the main camera's matrix as setView got it this frame (the buffer is reused for other views)
static _Atomic int main_view_ok;
static _Atomic int pin_on;   // the game camera is pinned this frame (rslot_frame)
// The last main cameras, newest at ring index mv_n - 1: the scene marker carries eight (the render thread records a
// frame while the game thread may already have set up the next one); the replay labels the frame's picture with the
// newest that is a game camera.
static float mv_ring[16][12];
static _Atomic unsigned mv_n;
static void mv_push(const float *v) {
    unsigned n = atomic_load(&mv_n);
    if (n && !memcmp(mv_ring[(n - 1) & 15], v, sizeof mv_ring[0])) return;
    memcpy(mv_ring[n & 15], v, sizeof mv_ring[0]);
    atomic_store(&mv_n, n + 1);
}
// The view object's head versions with the game camera each was put on (newest first) for the replay.
static float vo_ring[1024][2][12];   // (deep: under CPU load the replay runs far behind the game thread)
static struct { float hp[3], hm[9]; unsigned xr; } vo_pose[1024];   // the frame pose each head version was made with (TOTK_VR_ONEPOSE)
static _Atomic unsigned vo_n;
static void vo_push(const float *g, const float *h) {
    unsigned n = atomic_load(&vo_n);
    if (n && !memcmp(vo_ring[(n - 1) & 1023][1], h, 48)) return;
    memcpy(vo_ring[n & 1023][0], g, 48), memcpy(vo_ring[n & 1023][1], h, 48);
    memcpy(vo_pose[n & 1023].hp, FP.hp, 12), memcpy(vo_pose[n & 1023].hm, FP.hm, 36), vo_pose[n & 1023].xr = FP.xr;
    atomic_store(&vo_n, n + 1);
}
static float vo_far = 25000;   // the main view's far plane (view object +0x21c), for the aim tracker
float vr_far_plane(void) { return vo_far; }
static unsigned vo_at;   // the replayed scope's ring position (vr_vo_set_scope), 0 = newest
static int vr_scope_pose(float hp[3], float hm[9], unsigned *xr) {   // the replayed frame's pose (the newest head version at its record time)
    unsigned n = atomic_load(&vo_n);
    if (!vo_at || n - vo_at >= 1000 || !vo_pose[(vo_at - 1) & 1023].xr) return 0;
    memcpy(hp, vo_pose[(vo_at - 1) & 1023].hp, 12), memcpy(hm, vo_pose[(vo_at - 1) & 1023].hm, 36), *xr = vo_pose[(vo_at - 1) & 1023].xr;
    return 1;
}
void vr_vo_set_scope(uint64_t serial) {
    vo_at = (unsigned)serial;
}
int vr_vo_pairs(float g[][12], float h[][12], int max) {
    unsigned n = atomic_load(&vo_n);
    if (vo_at && vo_at + 2 <= n && n - (vo_at + 2) <= 1000) n = vo_at + 2;   // up to the scope's record time (+2: its own setViews may come just after)
    int k = 0;
    for (; k < max && (unsigned)k < n && k < 8; k++) memcpy(g[k], vo_ring[(n - 1 - k) & 1023][0], 48), memcpy(h[k], vo_ring[(n - 1 - k) & 1023][1], 48);
    return k;
}
static void mv_get(float out[96]) {
    unsigned n = atomic_load(&mv_n);
    for (unsigned k = 0; k < 8; k++)
        if (n > k) memcpy(out + 12 * k, mv_ring[(n - 1 - k) & 15], 48);
        else memset(out + 12 * k, 0, 48);
}
// TOTK_VR_CAMERA=game (default): the main camera itself gets the headset (host_cam_update), so movement and aim follow
// the head too. render (unfinished): only views handed to the renderer get it, the game keeps its camera.
// The world turns but characters don't: more readers take the camera through other pointers (still reading the
// original with every hook below on: 0x2348c via 0x6822f0, 0x681310, setView from 0x2455580; game-side 0x81bdc0,
// 0x6c7868, 0x580ff0, 0x8a8830, 0x6dfbc0, 0xf407dc, 0x237db78, the 0xbe5b0 chain).
// TOTK_VR_CAMERA=replay (step 4): the game camera keeps the game's view (no head in it); the head goes into the frame's
// camera rows of both eyes at replay time (vk_exec.c / vk_exec_draw.c), with the pose read then (vr_head_for).
int vr_camera_replay(void) {
    return vk_xr_world();
}
unsigned vk_xr_note_replay(float yaw, const float cam[3], float scale);   // vk_xr.c: the poses the replay drew with
// The newest game camera's axes in tracking space (the yaw frame vr_apply puts the head in; its origin is the game
// camera's position): right, up and back, rows of a 3x3. For the aim reticle layer (vk_xr.c): the game aims along -back.
// The game camera's own axes (the aim: bow, throw, Ultrahand reticles), from the camera object the game updates; the
// head never goes into it. read from the newest camera version instead, the throw marker followed the head
// (the render camera object's views are built from the head copy and land in that ring). TOTK_VR_AIMAXES=ring.
int vr_game_axes(float out[9]) {
    float gm[12];
    const float *G = NULL;
    uint64_t m = atomic_load(&main_cam);
    if (m) memcpy(gm, (const void *)(uintptr_t)m, sizeof gm), G = gm;
    if (!G || !(G[0] == G[0])) {
        unsigned n = atomic_load(&mv_n);
        if (!n) return 0;
        G = mv_ring[(n - 1) & 15];
    }
    float bx = G[8], bz = G[10], len = sqrtf(bx * bx + bz * bz);
    if (len < 1e-3f) return 0;
    const float B[3] = {bx / len, 0, bz / len}, U[3] = {0, 1, 0};
    const float Rr[3] = {U[1] * B[2] - U[2] * B[1], U[2] * B[0] - U[0] * B[2], U[0] * B[1] - U[1] * B[0]};
    for (int i = 0; i < 3; i++) {
        const float *a = G + 4 * i;
        out[3 * i] = dot3(a, Rr), out[3 * i + 1] = dot3(a, U), out[3 * i + 2] = dot3(a, B);
    }
    return 1;
}
// A game camera's yaw (of its back axis about +y) and position: the pair's label (seen_now).
void vr_game_yaw_cam(const float G[12], float *yaw, float cam[3]) {
    for (int k = 0; k < 3; k++) cam[k] = -(G[0 * 4 + k] * G[3] + G[1 * 4 + k] * G[7] + G[2 * 4 + k] * G[11]);
    float bx = G[8], bz = G[10], n = sqrtf(bx * bx + bz * bz);
    *yaw = n > 1e-3f ? atan2f(bx / n, bz / n) : 0;
}
// The head pose on top of a game camera view (the frame's own pose with TOTK_VR_ONEPOSE). pose: read a new pose (once
// per frame); with pose 0 the last one read is used. Returns 0 without a pose.
static struct { float hp[3], hm[9]; int have; unsigned serial; } RP;
unsigned vr_head_for(const float G[12], float H[12], int pose) {   // the pose's serial (vk_xr_replay_label), 0 without
    if (pose && onepose()) {   // the frame's own pose (the renderer used it), its serial labels the pair
        RP.have = vr_scope_pose(RP.hp, RP.hm, &RP.serial);
        static unsigned nok, nbad;
        RP.have ? nok++ : nbad++;
        if ((nok + nbad) % 600 == 0 || (!RP.have && nbad < 20)) {
            unsigned n = atomic_load(&vo_n);
            rt_log("vr: one pose: %u frames with their pose, %u without (vo_at %u, ring %u, xr %u)", nok, nbad, vo_at, n, vo_at ? vo_pose[(vo_at - 1) & 1023].xr : 0);
        }
        if (!RP.have) return 0;
        memcpy(H, G, 48);
        vr_apply_pose(H, RP.hp, RP.hm, 0);
        return RP.serial;
    }
    if (pose) {
        float dx = 0;
        RP.have = vk_xr_eye_view(0, RP.hp, RP.hm, &dx);
        if (RP.have && !head_position()) RP.hp[0] = RP.hp[1] = RP.hp[2] = 0;
        if (RP.have) for (int k = 0; k < 3; k++) RP.hp[k] += dx * RP.hm[k];   // the left eye
    }
    if (!RP.have) return 0;
    memcpy(H, G, 48);
    vr_apply_pose(H, RP.hp, RP.hm, 0);
    if (pose) {   // the pose read now, kept for the label of the pair drawn with it
        float cam[3], yaw;
        vr_game_yaw_cam(G, &yaw, cam);
        float scale = (float)cfg_double("TOTK_VR_SCALE");
        RP.serial = vk_xr_note_replay(yaw, cam, scale > 0.01f ? scale : 1);
    }
    return RP.serial ? RP.serial : 1;
}
static _Alignas(16) uint8_t view_copy[0x100];
// Render camera records (TOTK_VR_RSLOT). The renderer's records hold {camera object, projection object} pointer pairs
// (a viewport record, the character renderer's, layer records); its CPU code builds everything it draws from that
// camera (shadow lookups = light x inverse camera, the cloud dome, culling). Those records get a copy of the camera
// object whose matrix carries the frame's head pose, refreshed every frame; gameplay reaches the camera through its own
// pointers and keeps the game's.
static _Alignas(16) uint8_t shadow_buf[2][0x200];   // the camera object (0x90) and the projection object after it (code reaches it by offset)
static _Atomic int shadow_cur;                      // the copy the records point at; the other is written (rewritten in place
#define shadow_cam (shadow_buf[atomic_load(&shadow_cur)])   // while the renderer read it on another thread: shadow flashes)
static int is_shadow(uint64_t p) { return p == (uint64_t)(uintptr_t)shadow_buf[0] || p == (uint64_t)(uintptr_t)shadow_buf[1]; }
// Head copies of the game's own event cameras (talk, title, arrivals), so they are head-tracked 3D too: same layout
// (camera object + projection after it).
static _Alignas(16) uint8_t ev_buf[2][0x200];
static int is_ev(uint64_t p) { return p == (uint64_t)(uintptr_t)ev_buf[0] || p == (uint64_t)(uintptr_t)ev_buf[1]; }
static int vap_free;   // vr_apply_pose without the level lock rules (event cameras keep their own place)
static uint64_t rslot[32];
static _Atomic int nrslot;
static _Atomic uint64_t rslot_cam;   // the camera object the slots were found for
static int rslot_on(void) { return vr_camera_replay() && cfg_bool("TOTK_VR_RSLOT"); }
static void vr_apply(float *V);
static _Atomic int shadow_ok;   // a copy has been made (rslot_frame)
// The renderer's records get their camera from the camera manager's holder (+8 camera, +0x10 projection) in two
// stores: main+0xcd2a04 `str x11, [x9, #0x78]` (viewport record) and main+0x581354 `str x0, [x22, #8]!` (a layer
// record). Both become calls here that store the head copy in place of the real camera and remember the record.
static uint64_t rec_subst(uint64_t v, uint64_t slot) {
    if (!rslot_on() || !v) return v;
    int n = atomic_load(&nrslot), have = 0;   // remembered always: written before the main camera is known (loading),
    for (int i = 0; i < n; i++) have |= rslot[i] == slot;   // rslot_frame points it at the copy later
    if (!have && n < 32) {
        rslot[n] = slot, atomic_store(&nrslot, n + 1);
        static unsigned nl;
        if (nl < 16) nl++, rt_log("vr: render camera record %#llx (camera %#llx) gets the head copy", (unsigned long long)slot, (unsigned long long)v);
    }
    uint64_t m = atomic_load(&main_cam);
    if (!m || v != m - 8 || !vk_xr_world() || !atomic_load(&shadow_ok)) return v;
    return (uint64_t)(uintptr_t)shadow_cam;
}
static _Atomic uint64_t cam_holder;   // the camera manager's holder (+8 camera, +0x10 projection): x10 at main+0xcd2a04
static void host_rec_viewport(Cpu *c) {   // str x11, [x9, #0x78]
    if (c->x[10] && *(const uint64_t *)(uintptr_t)(c->x[10] + 8) == c->x[11] && atomic_exchange(&cam_holder, c->x[10]) != c->x[10])
        rt_log("vr: camera manager holder %#llx (camera %#llx)", (unsigned long long)c->x[10], (unsigned long long)c->x[11]);
    uint64_t put = rec_subst(c->x[11], c->x[9] + 0x78);
    *(uint64_t *)(uintptr_t)(c->x[9] + 0x78) = put;
}
static void host_rec_layer(Cpu *c) {      // str x0, [x22, #8]!
    c->x[22] += 8;
    *(uint64_t *)(uintptr_t)c->x[22] = rec_subst(c->x[0], c->x[22]);
}
// Once per camera update and frame start: the copy follows the camera object, its matrix gets the head, the slots point
// at it (a slot the game has pointed elsewhere is dropped).
int vr_pairs(void);
static void rslot_frame(void) {
    uint64_t m = atomic_load(&main_cam);
    if (!m || !rslot_on() || !vk_xr_world()) return;
    uint64_t cam = m - 8;
    if (is_shadow(cam)) return;
    uint64_t was = atomic_load(&rslot_cam);
    if (was != cam) atomic_store(&rslot_cam, cam);
    {   // TOTK_VR_NUDGE (degrees): a game camera lying exactly on world axes (ladders, the level shield camera) is
        // turned by a tiny yaw + pitch, so camera rows are never mistaken for constant world vectors. Only while aligned (a nudged camera is no longer aligned, so a matrix the game doesn't rebuild is not
        // turned again).
        static float nud = -1;
        if (nud < 0) { const char *e = cfg_get("TOTK_VR_NUDGE"); nud = e ? (float)atof(e) : 0.05f; }
        if (nud > 0) {
            float *M = (float *)(uintptr_t)m;
            int al = 0;
            for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) { float a = fabsf(M[r * 4 + c]); if (a < 1e-6f || fabsf(a - 1) < 1e-6f) al++; }
            if (al >= 3) {
                float p[3];
                for (int k = 0; k < 3; k++) p[k] = -(M[k] * M[3] + M[4 + k] * M[7] + M[8 + k] * M[11]);
                float a = nud * 0.0174533f, ca = cosf(a), sa = sinf(a);
                for (int r = 0; r < 3; r++) {   // world yaw by a, then world pitch (about x) by a
                    float x = M[r * 4], y = M[r * 4 + 1], z = M[r * 4 + 2];
                    float x1 = ca * x + sa * z, z1 = -sa * x + ca * z;
                    float y2 = ca * y - sa * z1, z2 = sa * y + ca * z1;
                    M[r * 4] = x1, M[r * 4 + 1] = y2, M[r * 4 + 2] = z2;
                    M[r * 4 + 3] = -(M[r * 4] * p[0] + M[r * 4 + 1] * p[1] + M[r * 4 + 2] * p[2]);
                }
                static unsigned nl;
                if (nl < 5) nl++, rt_log("vr: game camera on world axes (%d entries), nudged %.3f degrees", al, nud);
            }
        }
    }
    int nx = !atomic_load(&shadow_cur);
    uint8_t *sc = shadow_buf[nx];
    memcpy(sc, (const void *)(uintptr_t)cam, sizeof shadow_buf[0]);
    {   // the orbit (TOTK_VR_LEVEL) from the game's own camera object: position +0x38, look-at point +0x44 (not from the
        // render camera object: that one is built from the head copy, so its height follows the head's pitch)
        const float *g = (const float *)(uintptr_t)m;
        float gb[3] = {g[8], g[9], g[10]}, lp[3];
        int lk = cfg_bool("TOTK_VR_LINKANCHOR") && vr_player_pos(lp);
        // no Link yet (title screen: the player reads (0, -0.15, 0)): no anchor and no pin (the title camera would be
        // pinned near the world origin)
        if (lk && fabsf(lp[0]) + fabsf(lp[2]) < 1 && fabsf(lp[1]) < 1) lk = 0;
        if (lk) rag_anchor(lp, kern_now_ns());
        int pinned = 0;
        if (lk) {
            static float last[3];
            static int was;
            // the learned offset is relative to Link: relearn quickly after switching anchors
            if (!was) lvl_ok = 0;
            memcpy(link_pos, lp, 12), atomic_store(&link_ok, 1), was = 1;
            memcpy(last, lp, 12);
        } else atomic_store(&link_ok, 0);
        if (!RAG.on && !RAG.gliding)   // (the orbit is learned relative to Link: not while the anchor is held)
            orbit_note((const float *)(uintptr_t)(cam + 0x38), lk ? lp : (const float *)(uintptr_t)(cam + 0x44), gb);
        {   // collisions: the game camera's own horizontal distance from Link (not a position the pin wrote)
            static int on = -1;
            if (on < 0) { const char *e = cfg_get("TOTK_VR_COLLIDE"); on = e && *e == '1'; }   // default off: the game camera's distance also changes with the stick's pitch (the view pumps in and out)
            const float *gp = (const float *)(uintptr_t)(cam + 0x38);
            int self = fabsf(gp[0] - pin_last[0]) + fabsf(gp[1] - pin_last[1]) + fabsf(gp[2] - pin_last[2]) < 1e-3f;
            if (!on || !lk || !lvl_ok || !chase_active()) coll_k = 1;
            else if (!self) {
                float dx = gp[0] - lp[0], dz = gp[2] - lp[2], hd = sqrtf(dx * dx + dz * dz);
                float k = lvl_d > 0.5f ? hd / lvl_d : 1;
                if (k > 1) k = 1;
                if (k < 0.2f) k = 0.2f;
                coll_k += (k < coll_k ? 0.35f : 0.08f) * (k - coll_k);   // in quickly, out slowly
                static unsigned nl;
                if (k < 0.9f && nl < 20) nl++, rt_log("vr: the game camera is %.2f m from Link (level orbit %.2f): VR camera comes in", hd, lvl_d);
            }
        }
        // TOTK_VR_PINCAM: the game camera itself sits at the locked spot (Link + the level
        // offset along its yaw), keeping its rotation: everything that reads the camera position (sky volumes, probes,
        // the shield camera's height and distance, ...) agrees with the view without hunting each reader down. Matrix
        // translation, position (+0x38) and look-at point (+0x44) together. Learned above from the unpinned position.
        if (lk && lvl_ok && cfg_bool("TOTK_VR_LEVEL") && cfg_bool("TOTK_VR_PINCAM") && chase_active()) {
            float *M = (float *)(uintptr_t)m, *pos = (float *)(uintptr_t)(cam + 0x38), *at = (float *)(uintptr_t)(cam + 0x44);
            float bx = -M[2], bz = M[0], bn = sqrtf(bx * bx + bz * bz);
            if (bn > 1e-3f) {
                bx /= bn, bz /= bn;
                float np[3] = {lp[0] + lvl_dist() * bx, lp[1] + lvl_height(), lp[2] + lvl_dist() * bz};
                float d = sqrtf((at[0] - pos[0]) * (at[0] - pos[0]) + (at[1] - pos[1]) * (at[1] - pos[1]) + (at[2] - pos[2]) * (at[2] - pos[2]));
                if (!(d > 0.01f)) d = 1;
                for (int r = 0; r < 3; r++) M[r * 4 + 3] = -(M[r * 4] * np[0] + M[r * 4 + 1] * np[1] + M[r * 4 + 2] * np[2]);
                for (int k = 0; k < 3; k++) pos[k] = np[k], at[k] = np[k] - d * M[8 + k];
                memcpy(pin_last, np, 12);
                memcpy(sc, (const void *)(uintptr_t)cam, sizeof shadow_buf[0]);   // the copy from the pinned camera
                pinned = 1;
                static unsigned nl;
                if (nl < 3) nl++, rt_log("vr: game camera pinned at %.2f %.2f %.2f (Link %.2f %.2f %.2f)", np[0], np[1], np[2], lp[0], lp[1], lp[2]);
            }
        }
        if (!RAG.on && lk) memcpy(rag_cam, (const void *)(uintptr_t)cam, sizeof rag_cam), rag_cam_ok = 1;
        atomic_store(&pin_on, pinned);
    }
    {   // Stereo pairs, one game camera per pair: the game turns its camera by a fixed amount per frame, not per unit of
        // game time, so the short right-eye step alone does not keep the eyes together (the right frame's view would be
        // turned about as far as a whole pair). The right frame draws from the left frame's game camera (copied before
        // the head goes on); each eye still gets its own head pose, which SteamVR reprojects.
        static uint8_t left_cam[sizeof shadow_buf[0]];
        static int left_ok = -1;
        if (vr_pairs() && FP.have) {
            if (FP.eye == 0) memcpy(left_cam, sc, sizeof left_cam), left_ok = 1;
            else if (FP.eye == 1 && left_ok == 1) {
                memcpy(sc, left_cam, sizeof left_cam);
                static unsigned nl;
                if (!nl++) rt_log("vr: stereo pairs: the right eye draws from its left frame's game camera");
            }
        }
    }
    if (FP.have && FP.xr && vr_pairs()) {   // the frame's label: the game camera it is drawn with (vk_xr.c rp_game)
        void vk_xr_replay_set_game(unsigned s, float yaw, const float cam[3]);
        void vr_game_yaw_cam(const float G[12], float *yaw, float cam[3]);
        float gy, gc[3];
        vr_game_yaw_cam((const float *)(sc + 8), &gy, gc);
        vk_xr_replay_set_game(FP.xr, gy, gc);
    }
    if (FP.have) {
        float *H = (float *)(sc + 8);
        vr_apply(H);
        vo_push((const float *)(uintptr_t)m, H);
        // the look-at fields (+0x38 position, +0x44 look-at point, +0x50 up) to match: the renderer reads them too
        // (with only the matrix turned the picture tore apart)
        float *pos = (float *)(sc + 0x38), *at = (float *)(sc + 0x44), *up = (float *)(sc + 0x50);
        float d = sqrtf((at[0] - pos[0]) * (at[0] - pos[0]) + (at[1] - pos[1]) * (at[1] - pos[1]) + (at[2] - pos[2]) * (at[2] - pos[2]));
        if (!(d > 0.01f)) d = 1;
        // at the level orbit's distance, not the game's (TOTK's orbit distance changes with its pitch: with the stick
        // moving up and down, a ring of light in the sky moved and changed size)
        if (lvl_ok && cfg_bool("TOTK_VR_LEVEL") && chase_active()) d = sqrtf(lvl_dist() * lvl_dist() + lvl_height() * lvl_height());
        for (int k = 0; k < 3; k++) pos[k] = -(H[k] * H[3] + H[4 + k] * H[7] + H[8 + k] * H[11]);
        for (int k = 0; k < 3; k++) at[k] = pos[k] - d * H[8 + k], up[k] = H[4 + k];
    }
    atomic_store(&shadow_cur, nx), atomic_store(&shadow_ok, 1);
    int n = atomic_load(&nrslot), rec_tot = 0, rec_other = 0;
    static uint64_t ev_src;   // the event camera the event copies are made from
    uint64_t evsrc = 0;
    // The pair decision's override of a right frame's records lasts that frame only (written and left in place,
    // the game never stored its cutscene camera into them again, the next left frame saw the normal camera too, and the
    // cutscene stayed on the NPC): the game's own values go back before this frame reads them.
    static struct { uint64_t rec, orig, put; } PDR[32];
    static int pdr_n;
    for (int k = 0; k < pdr_n; k++) {
        uint64_t v = 0;
        if (plat_mem_read(PDR[k].rec, &v, 8) && v == PDR[k].put) plat_mem_poke(PDR[k].rec, &PDR[k].orig, 8);
    }
    pdr_n = 0;
    for (int i = 0; i < n; i++) {   // through plat_mem_*: a record freed while loading may be unmapped
        uint64_t v = 0, sh = (uint64_t)(uintptr_t)shadow_buf[nx];
        int ok = plat_mem_read(rslot[i], &v, 8);
        if (ok && i < 2) rec_tot++, rec_other += v && !is_shadow(v) && v != cam;   // the head lock below
        if (ok && i < 2 && v && !is_shadow(v) && v != cam) evsrc = is_ev(v) ? ev_src : v;
        if (ok && (v == cam || (is_shadow(v) && v != sh))) {
            ok = plat_mem_poke(rslot[i], &sh, 8);
        } else if (ok) continue;   // the copy already, or another camera: kept (the game switches between two
        // camera objects, e.g. for the shield camera); pointed at the copy once that camera is the main one
        if (!ok) rslot[i] = rslot[--n], i--, atomic_store(&nrslot, n);   // unreadable (freed while loading)
    }
    {   // the head lock (vk_xr.c vk_xr_head_lock): the render camera records hold a camera of the game's own (event, talk)
        static int on = -1;
        if (on < 0) { const char *e = cfg_get("TOTK_VR_HEADLOCK"); on = !(e && *e == '0'); }
        void vk_xr_head_lock(int on);
        // TOTK_VR_EV3D (default 1): the event camera gets a head copy like the main camera (head-tracked 3D, both
        // eyes); TOTK_VR_EV3D_SKIP = hex class offsets (vtable - main) kept on the held-still picture (cutscenes?)
        static int ev3d = -1;
        if (ev3d < 0) { const char *e = cfg_get("TOTK_VR_EV3D"); ev3d = !(e && *e == '0'); }
        int evm = ev3d && rec_tot && rec_other == rec_tot && evsrc && FP.have;
        // only once the game's camera has been on for TOTK_VR_EV3D_DELAY frames in a row (default 3): one-frame captures
        // (a new weapon's icon is shot with the same reused camera object for a frame) render untouched
        static unsigned ev_run;
        static int ev_delay = -1;
        if (ev_delay < 0) { const char *e = cfg_get("TOTK_VR_EV3D_DELAY"); ev_delay = e ? atoi(e) : 3; }
        ev_run = rec_tot && rec_other == rec_tot && evsrc ? ev_run + 1 : 0;
        if ((int)ev_run < ev_delay) evm = 0;
        if (evm) {
            uint64_t vt = *(volatile uint64_t *)(uintptr_t)evsrc, vo = vt >= G(0) ? vt - G(0) : vt;
            static uint64_t logged[16]; static int nlg;
            int seen = 0;
            for (int k = 0; k < nlg; k++) seen |= logged[k] == vo;
            if (!seen && nlg < 16) logged[nlg++] = vo, rt_log("vr: event camera %#llx, class main+%#llx: head-tracked 3D", (unsigned long long)evsrc, (unsigned long long)vo);
        }
        // One decision per stereo pair: which camera the frame draws from (head-tracked event copy / the game's own
        // camera held still / the normal head copy). Decided per frame, a cut between a left and its right frame gives
        // the eyes different shots, or one eye held and the other head-tracked; so the left frame decides and its right
        // frame follows. TOTK_VR_PAIRDEC=0: per frame.
        static struct { unsigned serial; int have, evm, lock; uint64_t evsrc; } PDL;
        static int pdec = -1;
        if (pdec < 0) { const char *e = cfg_get("TOTK_VR_PAIRDEC"); pdec = !(e && *e == '0'); }
        int lockdec = rec_other == rec_tot && !evm, force_ev = 0;
        uint64_t force_rec = 0;   // the right frame's records go to this camera (a shadow copy or the left's raw camera)
        if (pdec && vr_pairs() && FP.have) {
            if (FP.eye == 0) PDL.serial = FP.serial, PDL.have = 1, PDL.evm = evm, PDL.lock = lockdec, PDL.evsrc = evm ? evsrc : (rec_other == rec_tot ? evsrc : 0);
            else if (FP.eye == 1 && PDL.have && FP.serial == PDL.serial + 1) {
                uint64_t now_src = evm ? evsrc : (rec_other == rec_tot ? evsrc : 0);
                int differs = PDL.evm != evm || PDL.evsrc != now_src || PDL.lock != lockdec;
                if (differs) {
                    static unsigned long nd;
                    if (++nd <= 20 || nd % 200 == 0) rt_log("vr: the game switched cameras between a pair's eyes: the right eye follows its left (%s -> %s, %lu so far)",
                                                            PDL.evm ? "event 3D" : PDL.evsrc ? "held" : "normal", evm ? "event 3D" : now_src ? "held" : "normal", nd);
                    evm = PDL.evm, lockdec = PDL.lock;
                    if (PDL.evm) evsrc = PDL.evsrc, force_ev = 1;
                    else force_rec = PDL.evsrc ? PDL.evsrc : (uint64_t)(uintptr_t)shadow_buf[nx];
                }
            }
        }
        if (force_rec) {   // the right frame draws from what its left frame drew from
            for (int i = 0; i < n && i < 32; i++) {
                uint64_t v = 0;
                if (!plat_mem_read(rslot[i], &v, 8) || v == force_rec || !v) continue;
                if (is_shadow(force_rec) ? !is_shadow(v) && v != cam : i < 2) {
                    if (plat_mem_poke(rslot[i], &force_rec, 8) && pdr_n < 32) PDR[pdr_n].rec = rslot[i], PDR[pdr_n].orig = v, PDR[pdr_n].put = force_rec, pdr_n++;
                }
            }
        }
        static uint8_t ev_good[0x200];
        static uint64_t ev_good_src;   // the event camera whose last normal place ev_good holds (0: none)
        if (!evm) ev_good_src = 0;     // (the game reuses one camera object for every event)
        if (evm) {
            static int enx;
            static uint8_t ev_left[0x200];
            static unsigned ev_left_serial;
            static int ev_left_ok;
            enx = !enx;
            uint8_t *eb = ev_buf[enx];
            if (evsrc != ev_src) ev_left_ok = 0;
            ev_src = evsrc;
            memcpy(eb, (const void *)(uintptr_t)evsrc, 0x200);
            // Link ragdolls (dying: the game's death camera follows him rolling): the view stays where it was, at the event
            // camera's own last place when it was running before the ragdoll began, else at the last normal camera
            if (!RAG.on) memcpy(ev_good, eb, sizeof ev_good), ev_good_src = evsrc;
            else if (ev_good_src == evsrc || rag_cam_ok) {
                memcpy(eb, ev_good_src == evsrc ? ev_good : rag_cam, 0x200);
                static unsigned nl;
                if (nl++ < 10) rt_log("vr: event camera while Link ragdolls: the view stays held at %s", ev_good_src == evsrc ? "its own place" : "the normal camera's");
            }
            if (vr_pairs()) {   // the right eye from its left frame's event camera (as for the main camera), only from the
                // frame right before: the camera object is reused for every event, and an older left copy (a teleport
                // departure) would be handed to a later one-frame shot (a weapon icon would show that old scene)
                if (FP.eye == 0) memcpy(ev_left, eb, sizeof ev_left), ev_left_ok = 1, ev_left_serial = FP.serial;
                else if (ev_left_ok && FP.serial == ev_left_serial + 1) memcpy(eb, ev_left, sizeof ev_left);
            }
            float *H = (float *)(eb + 8), *pos = (float *)(eb + 0x38), *at = (float *)(eb + 0x44), *up = (float *)(eb + 0x50);
            if (FP.xr && vr_pairs()) {   // the frame's label: the event camera it is drawn with
                void vk_xr_replay_set_game(unsigned s, float yaw, const float cam[3]);
                void vr_game_yaw_cam(const float G[12], float *yaw, float cam[3]);
                float gy, gc[3];
                vr_game_yaw_cam(H, &gy, gc);
                vk_xr_replay_set_game(FP.xr, gy, gc);
            }
            float d = sqrtf((at[0] - pos[0]) * (at[0] - pos[0]) + (at[1] - pos[1]) * (at[1] - pos[1]) + (at[2] - pos[2]) * (at[2] - pos[2]));
            if (!(d > 0.01f)) d = 1;
            float ao[3];
            memcpy(ao, aim_off, sizeof ao);
            vap_free = 1, vr_apply_pose(H, FP.hp, FP.hm, 0), vap_free = 0;
            memcpy(aim_off, ao, sizeof ao);
            for (int k = 0; k < 3; k++) pos[k] = -(H[k] * H[3] + H[4 + k] * H[7] + H[8 + k] * H[11]);
            for (int k = 0; k < 3; k++) at[k] = pos[k] - d * H[8 + k], up[k] = H[4 + k];
            uint64_t evp = (uint64_t)(uintptr_t)eb;
            for (int i = 0; i < n && i < 32; i++) {
                uint64_t v = 0;
                if (!plat_mem_read(rslot[i], &v, 8)) continue;
                if (v == evsrc || (is_ev(v) && v != evp)) plat_mem_poke(rslot[i], &evp, 8);
                else if (force_ev && i < 2 && v && v != evp) {   // (one frame only: the game's value goes back next frame)
                    if (plat_mem_poke(rslot[i], &evp, 8) && pdr_n < 32) PDR[pdr_n].rec = rslot[i], PDR[pdr_n].orig = v, PDR[pdr_n].put = evp, pdr_n++;
                }
            }
        }
        if (!evm && ev_src) {   // event over (or 3D off): every record still on an event copy gets the game's camera back
            // (a record the game does not store again would keep pointing at the frozen copy: weapon icons drawn later
            // would show the scene of a teleport arrival)
            int back = 0;
            for (int i = 0; i < n && i < 32; i++) {
                uint64_t v = 0;
                if (!plat_mem_read(rslot[i], &v, 8) || !is_ev(v)) continue;
                back += plat_mem_poke(rslot[i], &ev_src, 8);
            }
            static unsigned nl;
            if (back && nl < 20) nl++, rt_log("vr: event camera over: %d render records back on the game's camera %#llx", back, (unsigned long long)ev_src);
        }
        void vk_xr_head_lock_frame(unsigned serial, int on);
        if (on && rec_tot) vk_xr_head_lock_frame(FP.xr, lockdec);
    }
}
static void host_scene_view(Cpu *c) {
    if (c->x[30] == G(0x875144) || c->x[30] == 0x875144) {
        // through a render camera slot the main setView gets the head copy: the main camera stays the game's object
        // (its matrix is the game camera: history, aim, level lock), the view gets the copy as it is
        uint64_t mc = c->x[1];
        if (is_shadow(mc - 8) || is_ev(mc - 8)) mc = atomic_load(&rslot_cam) ? atomic_load(&rslot_cam) + 8 : 0;
        if (!mc) goto main_done;
        if (atomic_exchange(&main_cam, mc) != mc) {
            static unsigned nl;
            if (nl < 60) nl++, rt_log("vr: main camera now %#llx (setView got %#llx%s)", (unsigned long long)mc, (unsigned long long)c->x[1], is_shadow(c->x[1] - 8) ? ", the head copy" : "");
        }
        {   // diagnostic (culling hunt): the renderer view object the main camera goes into
            static uint64_t last_dst;
            static unsigned nl;
            if (c->x[0] != last_dst && nl < 20) nl++, last_dst = c->x[0], rt_log("vr: main camera setView into view object %#llx (matrix %#llx, projection %#llx)", (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
        }
        memcpy(main_view, (const void *)(uintptr_t)mc, sizeof main_view);
        atomic_store(&main_view_ok, 1), mv_push(main_view);
        {   // diagnostic: how fast the rendered camera turns (angle between this frame's and the last frame's back axis)
            static float lb[3];
            static double sum, mx;
            static unsigned n;
            const float *v = main_view;
            float b[3] = {v[8], v[9], v[10]}, d = b[0] * lb[0] + b[1] * lb[1] + b[2] * lb[2];
            if (d > 1) d = 1;
            double a = acos(d) * 57.2958;
            if (n || lb[0] || lb[1] || lb[2]) { sum += a; if (a > mx) mx = a; }
            memcpy(lb, b, sizeof lb);
            if (++n % 600 == 0) rt_log("vr: camera turn per frame over 600 frames: mean %.2f deg, max %.2f deg", sum / 600, mx), sum = mx = 0;
        }
    }
main_done:;
    uint64_t m = c->x[1];
    int near_main = 0;   // a view filled from a camera close to the main one through another matrix (while the
    // game camera moves, a second, blended camera a few degrees off reaches setView elsewhere; with no head copy its rows
    // kept the game camera: the picture followed the game camera in motion and snapped to the head when it stopped)
    if (vr_camera_replay() && vk_xr_world() && FP.have && m && m != atomic_load(&main_cam) && !is_shadow(m - 8) && atomic_load(&main_view_ok) &&
        cfg_double("TOTK_VR_NEARVIEW") > 0) {
        const float *a = (const float *)(uintptr_t)m, *b = main_view;
        float ca[3], cb[3];
        for (int k = 0; k < 3; k++) ca[k] = -(a[k] * a[3] + a[4 + k] * a[7] + a[8 + k] * a[11]), cb[k] = -(b[k] * b[3] + b[4 + k] * b[7] + b[8 + k] * b[11]);
        float dd = fabsf(ca[0] - cb[0]) + fabsf(ca[1] - cb[1]) + fabsf(ca[2] - cb[2]);
        float dot = a[8] * b[8] + a[9] * b[9] + a[10] * b[10], up = a[4] * b[4] + a[5] * b[5] + a[6] * b[6];
        near_main = dd < (float)cfg_double("TOTK_VR_NEARVIEW") && dot > 0.9f && up > 0.9f && a[0] == a[0];
        static unsigned nl;
        if (near_main && nl < 20) nl++, rt_log("vr: setView from %#llx with a camera near the main one (%.2f away, axes dot %.4f): head copy", (unsigned long long)(c->x[30] - G(0)), dd, dot);
    }
    uint64_t hv_site = c->x[30] >= G(0) ? c->x[30] - G(0) : c->x[30];
    if (vr_camera_replay() && vk_xr_world() && FP.have && m && (m == atomic_load(&main_cam) || near_main)) {
        // Every view filled from the main camera (the scene's render views: culling, light culling and most of the
        // frame's camera data are built from them, not only main+0x875140's) gets the frame's head pose on the game
        // camera; the game camera itself stays the game's. Without it, everything outside the game camera's view cone
        // is culled when the head turns. The head versions are kept for the pair's label (vo ring).
        float g[12];
        memcpy(g, (const void *)(uintptr_t)m, 48);
        memcpy(view_copy, (const void *)(uintptr_t)m, sizeof view_copy);
        vr_apply((float *)view_copy);
        vo_push(g, (const float *)view_copy);
        m = (uint64_t)(uintptr_t)view_copy;
        {   // each setView site that gets a head copy, once
            static uint64_t seen[64]; static int ns;
            int k = 0;
            for (int i = 0; i < ns; i++) k |= seen[i] == hv_site;
            if (!k && ns < 64) seen[ns++] = hv_site, rt_log("vr: setView site %#llx gets a head copy", (unsigned long long)hv_site);
        }
    }
    uint64_t dst = c->x[0];
    c->x[0] = rt_call(c, G(0x649ed0), 3, c->x[0], m, c->x[2]);
    if (dst && (c->x[30] == G(0x875144) || c->x[30] == 0x875144) && atomic_load(&main_view_ok)) {   // the far plane (aim tracker)
        const float *pl = (const float *)(uintptr_t)(dst + 0x21c), *V = main_view;
        float cam[3];
        for (int k = 0; k < 3; k++) cam[k] = -(V[k] * V[3] + V[4 + k] * V[7] + V[8 + k] * V[11]);
        float f = -(pl[0] * cam[0] + pl[1] * cam[1] + pl[2] * cam[2] + pl[3]);
        if (f > 10 && f < 1e7f) vo_far = f;
    }
}

// The character renderer reads the camera object it is handed (main+0x67e4ac `bl 0x6822f0(x0, x1, camera object x2,
// projection x3)` -> page+0xf30, main+0x67e4a0 `bl 0x681310` -> page+0xf40) and stores it in its own record: it gets
// the head copy instead of the game camera.
static void render_cam_call(Cpu *c, uint64_t fn) {
    uint64_t a[8];
    for (int i = 0; i < 8; i++) a[i] = c->x[i];
    uint64_t m = atomic_load(&main_cam);
    if (rslot_on() && vk_xr_world() && m && a[2] + 8 == m && atomic_load(&shadow_ok)) a[2] = (uint64_t)(uintptr_t)shadow_cam;
    c->x[0] = rt_call(c, G(fn), 8, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}
static void host_render_cam(Cpu *c) { render_cam_call(c, 0x6822f0); }
static void host_render_cam2(Cpu *c) { render_cam_call(c, 0x681310); }

static void host_chase(Cpu *c) {   // main+0x18e480 `bl 0x19651c` -> page+0xd40: the Chase camera calculation
    uint64_t a[8];
    for (int i = 0; i < 8; i++) a[i] = c->x[i];
    c->x[0] = rt_call(c, G(0x19651c), 8, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    atomic_store(&chase_tick, atomic_load(&builds));
    uint64_t prm = a[0] ? *(volatile uint64_t *)(uintptr_t)(a[0] + 0x28) : 0;
    if (atomic_exchange(&chase_param, prm) != prm) {
        static unsigned nl;
        float r = prm ? *(volatile float *)(uintptr_t)(prm + 0xe8) : 0;
        if (nl < 40) nl++, rt_log("vr: game camera mode: Chase record %#llx (radius %.2f)", (unsigned long long)prm, r);
    }
}
// ---- Stereo pairs (TOTK_VR_PAIRS=1, after the TOTK-VR-RECOMP fork's vr_pair.h): the game draws each eye
// as a frame of its own. A left frame takes the pair's game time (twice a frame's step, less the shortest), a right
// frame the shortest step the game allows (frame_rate.c STEP_MIN_S, 1/240 s), so both eyes show one moment; the
// renderer's camera copy gets that frame's eye, so the game itself draws everything (HUD markers, shadows, culling)
// for that eye's real camera and the replay patches nothing. A frame's eye is fixed by whichever comes first after
// the last present: its game step (frame_step_present) or its camera update; the present closes it.
static _Atomic int pr_lock_;
static void pr_lock(void) { while (atomic_exchange(&pr_lock_, 1)) __builtin_ia32_pause(); }
static void pr_unlock(void) { atomic_store(&pr_lock_, 0); }
static struct { int open, eye, next; unsigned long pairs; } PR;
int vr_pairs(void) {
    return vk_xr_world();
}
static int pr_eye_locked(void) {
    if (!PR.open) PR.eye = PR.next, PR.open = 1;
    return PR.eye;
}
static int pair_eye(void) {
    pr_lock();
    int e = pr_eye_locked();
    pr_unlock();
    return e;
}
double vr_pair_frame_step(double step, double min) {   // frame_rate.c: the step the next frame takes
    if (!vr_pairs()) { pr_lock(); PR.open = 0, PR.next = 0; pr_unlock(); return step; }
    int e = pair_eye();
    // The game reads the step one frame late: its main thread is already updating
    // the next frame when this present stores the step, so a stored step lands on the frame after the next one, the
    // other eye's. Stored for this frame's eye, right frames got the long step and the eyes of a pair came almost a whole
    // frame apart (the camera following Link moved between them: near things and Link would not fuse while running).
    // TOTK_VR_PAIRLAG=0: the step for this frame's eye; default 1: for the eye after it.
    static int lag = -1;
    if (lag < 0) { const char *v = cfg_get("TOTK_VR_PAIRLAG"); lag = v && *v ? atoi(v) : 1; rt_log("vr: stereo pairs: the step is stored %d frame(s) ahead", lag); }
    if (lag & 1) e = !e;
    return e == 0 ? (2 * step - min > min ? 2 * step - min : min) : min;
}
void vr_pair_presented(void) {   // nvn_api_obj.c QueuePresentTexture, before the next frame's step
    pr_lock();
    int eye = PR.open ? PR.eye : -1;
    PR.next = eye == 0 ? 1 : 0, PR.open = 0;
    if (eye == 1 && ++PR.pairs == 1) rt_log("vr: stereo pairs: a game frame per eye, the right one on the shortest step");
    pr_unlock();
}
// Head aim (TOTK_VR_HEAD_AIM, live, F1 > VR): while the bow (ZR) or a throw (R) is held, the
// game camera itself gets the head's rotation after its update, so the game aims where the head looks (arrow direction,
// Link turning, the reticle on vr_game_axes). The renderer's copy is still built from the unturned camera + the head,
// so the view does not change. Right before the next camera update the game's own camera comes back (matrix, look-at
// point, up), so its controller never sees the head and nothing piles up.
static struct { int on; float orig[12], written[12], at[3], up[3]; uint64_t m; } HA;
// Viewfinders: while the game draws the camera's (CameraPointer_00) or the scope's layout, the game
// aims with the head (photo targets, quest pictures: the game picks them at its own camera's centre) and the HUD panel
// is locked to the head (vk_xr.c hud_pose), so the viewfinder frames what the head looks at. Seen by the layout hook
// (host_layout), so it lasts exactly as long as the game shows the viewfinder. TOTK_VR_VIEWFINDER=list replaces the
// names (comma-separated), "-" turns it off.
static _Atomic int64_t viewfinder_ns;
int vr_viewfinder_active(void) { int64_t t = atomic_load(&viewfinder_ns); return t && kern_now_ns() - t < 200000000ll; }
static int viewfinder_layout(const char *name) {
    static const char *list;
    static int init;
    if (!init) { list = cfg_get("TOTK_VR_VIEWFINDER"); init = 1; }
    const char *l = list && *list ? list : "CameraPointer_00,Scope_00";
    if (*l == '-') return 0;
    size_t L = strlen(name);
    for (const char *q = l; (q = strstr(q, name)); q += L)
        if ((q == l || q[-1] == ',') && (q[L] == ',' || !q[L])) return 1;
    return 0;
}
int vr_head_aim_active(void) { return HA.on; }   // vk_xr.c: the reticle goes in front of the head while the game aims with it
static void head_aim_restore(void) {
    if (!HA.on) return;
    HA.on = 0;
    float *M = (float *)(uintptr_t)HA.m;
    if (memcmp(M, HA.written, 48)) return;   // rebuilt by the game meanwhile: leave its own
    memcpy(M, HA.orig, 48);
    memcpy((float *)(uintptr_t)(HA.m - 8 + 0x44), HA.at, 12), memcpy((float *)(uintptr_t)(HA.m - 8 + 0x50), HA.up, 12);
}
// The game's aim ray while head aim holds the camera (vk_contact.c aim tracker, the bow / throw reticle): origin at the
// game camera, direction the game aims along (world units, game coordinates). 0 when head aim has not set it lately.
static struct { _Atomic unsigned seq; float o[3], f[3]; _Atomic int64_t t; } HRAY;
int vr_aim_ray(float o[3], float f[3]) {
    int64_t t = atomic_load(&HRAY.t);
    if (!t || kern_now_ns() - t > 300000000ll) return 0;
    for (int tries = 0; tries < 8; tries++) {
        unsigned a = atomic_load_explicit(&HRAY.seq, memory_order_acquire);
        if (a & 1) continue;
        memcpy(o, HRAY.o, 12), memcpy(f, HRAY.f, 12);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&HRAY.seq, memory_order_relaxed) == a) return 1;
    }
    return 0;
}
static void head_aim_apply(uint64_t m) {
    uint64_t input_buttons_last(void);
    if (!m || !FP.have || !vk_xr_world() || !cfg_bool("TOTK_VR_HEAD_AIM")) return;
    // held, or released within TOTK_VR_HEAD_AIM_HOLD ms (default 400): the shot / throw happens on the release, and the
    // first build stopped turning the camera exactly then
    static int64_t last_held;
    static int hold_ms = -1;
    if (hold_ms < 0) { const char *e = cfg_get("TOTK_VR_HEAD_AIM_HOLD"); hold_ms = e ? atoi(e) : 400; }
    int64_t now = kern_now_ns();
    if ((input_buttons_last() & ((1ull << 9) | (1ull << 7))) || vr_viewfinder_active()) last_held = now;   // B_ZR = 9, B_R = 7 (input_map.h); the camera / scope
    else if (!last_held || now - last_held > (int64_t)hold_ms * 1000000) return;
    float *M = (float *)(uintptr_t)m, *at = (float *)(uintptr_t)(m - 8 + 0x44), *up = (float *)(uintptr_t)(m - 8 + 0x50);
    const float *pos = (const float *)(uintptr_t)(m - 8 + 0x38);
    float p[3], H[12];
    for (int k = 0; k < 3; k++) p[k] = -(M[k] * M[3] + M[4 + k] * M[7] + M[8 + k] * M[11]);
    float d = sqrtf((at[0] - pos[0]) * (at[0] - pos[0]) + (at[1] - pos[1]) * (at[1] - pos[1]) + (at[2] - pos[2]) * (at[2] - pos[2]));
    if (!(d > 0.01f)) d = 1;
    memcpy(H, M, 48);
    vr_apply_pose(H, (const float[3]){0, 0, 0}, FP.hm, 0);
    for (int r = 0; r < 3; r++) H[r * 4 + 3] = -(H[r * 4] * p[0] + H[r * 4 + 1] * p[1] + H[r * 4 + 2] * p[2]);
    if (!(H[0] == H[0])) return;
    HA.m = m, memcpy(HA.orig, M, 48), memcpy(HA.at, at, 12), memcpy(HA.up, up, 12);
    memcpy(M, H, 48);
    for (int k = 0; k < 3; k++) at[k] = pos[k] - d * H[8 + k], up[k] = H[4 + k];
    memcpy(HA.written, H, 48), HA.on = 1;
    atomic_fetch_add_explicit(&HRAY.seq, 1, memory_order_acq_rel);
    for (int k = 0; k < 3; k++) HRAY.o[k] = p[k], HRAY.f[k] = -H[8 + k];
    atomic_fetch_add_explicit(&HRAY.seq, 1, memory_order_acq_rel);
    atomic_store(&HRAY.t, now);
    static unsigned nl;
    if (nl < 3) nl++, rt_log("vr: head aim: the game camera follows the head while aiming");
}

// main+0x627dc8 `bl 0x628460` (the camera update) -> page+0xf10: after it, the frame's head pose is taken, the renderer's
// copy of the camera is made (rslot_frame) and, while aiming, the game camera follows the head (head_aim_apply).
static void host_cam_update(Cpu *c) {
    head_aim_restore();
    uint64_t a[8];
    for (int i = 0; i < 8; i++) a[i] = c->x[i];
    c->x[0] = rt_call(c, G(0x628460), 8, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    // render camera slots: the frame's head pose and the copy change here, once per frame, together with the game's own
    // camera (at the render phase start 0x74214 it ran ~10 times a frame, once per view: flicker)
    if (rslot_on() && vk_xr_world()) vr_frame_pose(vr_pairs() ? pair_eye() : 0);
    rslot_frame();
    head_aim_apply(atomic_load(&main_cam));   // after the renderer's copy was taken from the game's own camera
}

static void host_cam_build(Cpu *c) {
    uint64_t a[8];
    for (int i = 0; i < 8; i++) a[i] = c->x[i];
    c->x[0] = rt_call(c, G(0x7b0c78), 8, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    if (!vk_xr_world() || !a[0]) return;
    float *V = (float *)(uintptr_t)(a[0] + 0x14c), *P = (float *)(uintptr_t)(a[0] + 0x17c);
    // The builder runs twice per frame (two render camera objects, both the main camera). A new frame starts each time
    // the first object seen is built again.
    static _Atomic uint64_t first_obj;
    uint64_t expect = 0;
    atomic_compare_exchange_strong(&first_obj, &expect, a[0]);
    int new_frame = a[0] == atomic_load(&first_obj);
    atomic_fetch_add(&builds, 1);
    if (vr_camera_replay() && vk_xr_world() && cfg_bool("TOTK_VR_RCAM")) {   // the render camera's views are camera
        // versions too (in motion they differ slightly from the main camera matrix); views built from the head copy
        // (with the render camera records) are head versions, not game ones
        const float *S0 = (const float *)(shadow_buf[0] + 8), *S1 = (const float *)(shadow_buf[1] + 8);
        int from_head = rslot_on() && atomic_load(&nrslot) && (!memcmp(V, S0, 36) || !memcmp(V, S1, 36));
        // only views near the main camera (on a ladder the render camera object holds the game's own ladder camera,
        // ~5 m from the pinned one; turned around that spot, the ladder and Link would swing with the head).
        // TOTK_VR_RCAMNEAR (units, default 3; 0 = any distance).
        static float rnear = -1;
        if (rnear < 0) { const char *e = cfg_get("TOTK_VR_RCAMNEAR"); rnear = e ? (float)atof(e) : 3; }
        const float *views[2] = {V, (const float *)(uintptr_t)(a[0] + 0x250)};
        for (int q = 0; q < 2 && !from_head; q++) {
            int ok = 1;
            if (rnear > 0 && atomic_load(&main_view_ok) && atomic_load(&pin_on)) {   // (only while the pin holds the game
                // camera somewhere the game's own views are not: title, teleports and cutscenes keep all views)
                const float *x = views[q], *b = main_view;
                float d = 0;
                for (int k = 0; k < 3; k++) d += fabsf(-(x[k] * x[3] + x[4 + k] * x[7] + x[8 + k] * x[11]) + (b[k] * b[3] + b[4 + k] * b[7] + b[8 + k] * b[11]));
                // only a copy of the game camera from before the pin (same rotation, other position) is a stray; a
                // teleport's cinematic camera (3-9 m off, its own rotation) is kept
                float rd = 0;
                for (int k = 0; k < 11; k++) if (k != 3 && k != 7) rd += fabsf(x[k] - b[k]);
                static double rdmax = -1;
                if (rdmax < 0) { const char *e = cfg_get("TOTK_VR_RCAMROT"); rdmax = e ? atof(e) : 0.3; }   // ladder strays 0..0.035, a warp's cinematic camera 1.1..4
                ok = d < rnear || rd > rdmax;
            }
            if (ok) mv_push(views[q]);
        }
    }
    if (new_frame && !(rslot_on() && atomic_load(&nrslot))) {   // the orbit (TOTK_VR_LEVEL; with the slots: rslot_frame)
        float cam[3], gb[3] = {V[8], V[9], V[10]};
        for (int k = 0; k < 3; k++) cam[k] = -(V[k] * V[3] + V[4 + k] * V[7] + V[8 + k] * V[11]);
        orbit_note(cam, P, gb);
    }
    // the frame's head pose, also when the game skips its camera update (with the render camera slots: rslot_frame)
    if (new_frame && !rslot_on()) vr_frame_pose(pair_eye());
}

// Scene markers -> page+0xd30: the scene pipeline call and its shadow stage. The host finds its call site from the
// return address, puts a begin marker into the command buffer being recorded, calls the original target, then an end
// marker. The replay (vk_exec.c) finds the frame's finished scene image and the HUD after it from these.
static const struct { uint32_t site, target; } stage_sites[] = {
    {0x98540, 0xf01e4},   // the scene pipeline call itself: kinds 3 / 4 bracket the whole scene
    {0xf05a8, 0xf56d4},   // its shadow stage: kinds 1 / 2 (shadow maps are the output's size in VR: no scene image)
};
void nvn_view_mark(int kind, uint64_t site, uint64_t arg, const float *view);   // nvn_api_cmd.c
// main+0x10c71b4 `bl 0x10cc850` in the HUD's per-layout draw 0x10c7188 (x1 = the layout holder, its name at +0x30)
// -> page+0xd90: a kind 7 marker says which layout draws next (arg 1: Pointer_00, the bow / Recall / Ultrahand
// reticle), so the replay can put that layout on its own layer in the world (vk_exec.c HUD, vk_xr.c reticle).
static void host_layout(Cpu *c) {
    uint64_t a[8];
    for (int i = 0; i < 8; i++) a[i] = c->x[i];
    static int marks = -1;
    if (marks < 0) marks = cfg_bool("TOTK_VR");
    if (marks && a[1]) {
        const char *name = *(const char *const *)(uintptr_t)(a[1] + 0x30);
        int ok = name && (uintptr_t)name > 0x10000;
        // 1: the aim reticle; 2: world markers, anchored to things in the world (health bars, lock-on, labels, speech
        // balloons): their own layer over the 3D view, not the floating HUD panel
        static const char *const world[] = {"EnemyInfo_00", "AttentionLockOn_00", "AttentionBalloon_00", "BalloonMessage_00"};   // (PlayerStatus3D_00, the stamina wheel, stays on the panel: on the world layer it covered the view)
        int cls = ok && !strncmp(name, "Pointer_00", 11) ? 1 : 0;
        static int stm = -1;   // 3: the stamina wheel, moved to a fixed spot on the HUD panel (vk_exec.c STM; TOTK_VR_STAMINA_MOVE=0: where the game puts it)
        if (stm < 0) { const char *e = cfg_get("TOTK_VR_STAMINA_MOVE"); stm = !(e && *e == '0'); }
        if (ok && !cls && stm && !strcmp(name, "PlayerStatus3D_00")) cls = 3;
        // the quick-select strips (D-pad left/right/up, L): the game stops drawing the scene and shows a frozen picture
        // of it behind them (ShortCutBG_00, 4: left off the panel); the strips themselves are 5 (the panel, and the
        // headset keeps the 3D view instead of putting the whole frame on the panel)
        if (ok && !cls && !strncmp(name, "ShortCut", 8)) cls = strcmp(name, "ShortCutBG_00") ? 5 : 4;
        // full-screen fades (6): part of the picture itself (a fade covers the view), never left out of it in
        // stereo pairs (left out, the game stayed on its loading screen)
        if (ok && !cls && (strstr(name, "Fade") || !strncmp(name, "Loading", 7))) cls = 6;
        for (size_t i = 0; ok && !cls && i < sizeof world / sizeof *world; i++)
            if (!strcmp(name, world[i]) && cfg_bool("TOTK_VR_WORLDHUD")) cls = 2;
        // A full-screen menu (the map, the inventory/pause menus) starts drawing: marker 9, so the replay drops an eye
        // picture still waiting to be copied (it would be copied after the menu was drawn over the same memory)
        {
            int mk = !ok ? 0 : !strcmp(name, "AppMap_00") ? 1 : !strcmp(name, "AppMenuBG_00") ? 2 : !strcmp(name, "PauseMenuBG_00") ? 3 : 0;
            static unsigned long mc[4];
            if (mk && ++mc[mk] % 300 == 1) rt_log("vr: full-screen menu layout %s drawn (%lu times so far)", name, mc[mk]);
            if (mk >= 2) nvn_view_mark(9, 0x10c71b5, (uint64_t)mk, NULL);   // AppMap_00 is drawn every frame (the minimap): only the menu backgrounds count
        }
        if (ok && viewfinder_layout(name)) {
            if (!vr_viewfinder_active()) rt_log("vr: viewfinder %s shown: the game aims with the head, the HUD panel follows the head", name);
            atomic_store(&viewfinder_ns, kern_now_ns());
        }
        nvn_view_mark(7, 0x10c71b4, (uint64_t)cls, NULL);
    }
    c->x[0] = rt_call(c, G(0x10cc850), 8, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}
static void host_stage(Cpu *c) {
    uint64_t site = (c->x[30] & 0xffffffffull) - 4;   // guest return address - 4, image offset
    if (c->x[30] >= 0x7000000000ull) site = c->x[30] - 0x7000000000ull - 4;
    uint32_t target = 0;
    for (size_t i = 0; i < sizeof stage_sites / sizeof *stage_sites; i++)
        if (stage_sites[i].site == site) { target = stage_sites[i].target; break; }
    if (!target) rt_fatal("vr_camera: stage host called from unknown site %#llx", (unsigned long long)site);
    static int marks = -1;
    if (marks < 0) marks = cfg_bool("TOTK_VR");
    uint64_t a[8];
    for (int i = 0; i < 8; i++) a[i] = c->x[i];
    int whole = site == 0x98540;
    // the scene begin carries the last main cameras (the pair's label) and the view-object head ring's position at
    // record time, so the replay, 3-4 frames later, takes this frame's pose and not a newer one
    float cands[96];
    if (whole) mv_get(cands);
    if (marks) nvn_view_mark(whole ? 3 : 1, site, whole ? (uint64_t)atomic_load(&vo_n) : a[1], whole && atomic_load(&main_view_ok) ? cands : NULL);
    c->x[0] = rt_call(c, G(target), 8, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    if (marks) nvn_view_mark(whole ? 4 : 2, site, a[1], NULL);
}

__attribute__((constructor)) static void vr_camera_register(void) {
    rt_register_host(HOST_CAM_BUILD, host_cam_build);
    rt_register_host(RT_PATCH_PAGE + 0xf00, host_scene_view);
    rt_register_host(RT_PATCH_PAGE + 0xf10, host_cam_update);
    rt_register_host(RT_PATCH_PAGE + 0xd40, host_chase);
    rt_register_host(RT_PATCH_PAGE + 0xf30, host_render_cam);
    rt_register_host(RT_PATCH_PAGE + 0xf40, host_render_cam2);
    rt_register_host(RT_PATCH_PAGE + 0xd30, host_stage);   // (0xf50 until upstream's free camera took it)
    rt_register_host(RT_PATCH_PAGE + 0xd90, host_layout);
    rt_register_host(RT_PATCH_PAGE + 0xd80, host_rec_viewport);
    rt_register_host(RT_PATCH_PAGE + 0xd70, host_rec_layer);
}
