// Mouselook: the camera half of mcrib884/totk-mousecam (GPL-2.0, https://github.com/mcrib884/totk-mousecam, mod/source/
// program/main.cpp CamAlphaHook) as a host function of the recomp.
//
// The mod hooks one guest function, Callback(float deltaTime, long param2, long *param3): after the original ran, *param3 is
// a pointer to nine floats (position, focus, up) which the mod rewrites from its own yaw / pitch / radius. Here the patch set
// "mousecam" (recomp/patches-vNNN.txt) sends every `bl` of that function to host_camera below, which calls the guest function
// (rt_call: v0 still holds deltaTime), then does the same rewrite. Guest addresses are host addresses, so the nine floats are
// plain pointers.
//
// Differences from the mod:
//  - Mouse input comes from input_win.c (raw input, wheel, middle click), not from a companion process.
//  - Turn rate is per count, not per frame; zoom easing uses the host clock.
//  - Hands the camera back to the game when the mouse has been still for 350 ms, so the game's own follow / re-centre /
//    lock-on / cutscene cameras work; the mod overwrote the camera every frame from a yaw it initialised once.
//  - Zoom is an offset on the game's own radius, so it survives that hand-back.
//  - While the game is paused or the cursor is not captured nothing is rewritten and the old mouse-to-stick path serves the menus.
//
// PROBE MODE (TOTK_MOUSECAM_PROBE=1): looks for a game-state flag that says "Ultrahand is active". Mouselook is disabled
// for the run (the game gets the mouse like F3 off) and every F3 press writes build\mousecam_probe_NN.bin: the camera
// function's param2 object, the memory around the camera floats, and the first 0x200 bytes of every object param2 points
// to. tools/probe_diff.py compares the files.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "mousecam.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

// ---- per-version addresses: offsets from main's base. 0 = unknown, feature off for that version.
// CAM_OFF_*: entry of the camera update function (mod: CAMERA_ALPHA_UPDATE = 0x00a70604). Must be a function ENTRY with
//   `bl` callers: check with tools/find_calls.py (INTEGRATION.md step 1). The mod's value is NOT an entry in 1.4.3.
// PAUSE_OFF_*: the pause manager singleton pointer (mod: PAUSE_MGR_INSTANCE = 0x04728688), flags word at +0x30.
//   Unverified for any version: set to 0 to rely on the F3 toggle instead.
#define CAM_OFF_V121 0x00a70604ull
#define CAM_OFF_V143 0x0071986cull
#define PAUSE_OFF_V121 0x04728688ull
#define PAUSE_OFF_V143 0ull

// ---- tunables (mod/main.cpp and companion defaults)
#define RAD_PER_COUNT 0.0005f   // sens 1.0: companion scale (0.5/3) * mod g_sensitivity 0.003
#define PITCH_LIMIT 1.55f
#define ZOOM_STEP 1.0f          // g_zoomStep: units per wheel notch
#define MIN_RADIUS 2.0f
#define MAX_RADIUS 25.0f
#define ZOOM_EASE_PER_60HZ 0.15f // g_zoomLerp, per 1/60 s
#define STICK_DEAD 0.1f
#define STICK_RAD_PER_S 3.6f    // mod: 40 units * 0.003 rad per camera call, at 30 calls/s
#define IDLE_NS 350000000ull

static volatile int ml_enabled = 1;
static volatile int ml_probe_req;      // F3 pressed: the next camera call writes a probe snapshot
static volatile uint64_t ml_seen_ns;   // last time the hook handled a frame (captured, not paused)

void mousecam_toggle(void) {
    ml_enabled = !ml_enabled;
    ml_probe_req = 1;
    rt_log("mousecam: mouselook %s", ml_enabled ? "on" : "off");
}

int mousecam_suppress_stick(void) {
    uint64_t t = ml_seen_ns;
    return ml_enabled && t && kern_now_ns() - t < 250000000ull;
}

#ifdef RT_PATCH_PAGE

#define HOST_CAM (RT_PATCH_PAGE + 0xfd0)   // zstd uses +0xfe0 and +0xff0

static uint64_t g_fn;   // the guest camera function, resolved on first use

static uint64_t version_off(uint64_t v121, uint64_t v143) { return rt_game_version >= 143 ? v143 : v121; }

static int readable(uint64_t a, uint64_t n) {
    RtMemoryInfo mi;
    if (a < 0x1000 || a + n > RT_ADDRESS_LIMIT) return 0;
    return mem_query(a, &mi) == 0 && mi.state != 0 && (mi.perm & 1);
}

static int game_paused(void) {
    uint64_t off = version_off(PAUSE_OFF_V121, PAUSE_OFF_V143);
    if (!off) return 0;
    uint64_t a = ldr_module(0)->base + off;
    if (!readable(a, 8)) return 0;
    uint64_t inst = *(volatile uint64_t *)(uintptr_t)a;
    if (!inst || !readable(inst + 0x30, 4)) return 0;
    return *(volatile uint32_t *)(uintptr_t)(inst + 0x30) != 0;
}

// How long the mouse may rest before the camera goes back to the game (follow / re-centre / lock-on / cutscene cameras).
// TOTK_MOUSECAM_HOLD_MS: milliseconds, default 350; 0 = never hand back (the mod's behaviour: the camera keeps the mouse's
// direction while the player moves, but the game's own camera stays out for lock-on and cutscenes too).
static uint64_t idle_ns(void) {
    static int64_t ms = -1;
    if (ms < 0) {
        const char *v = cfg_get("TOTK_MOUSECAM_HOLD_MS");
        ms = v && *v ? (int64_t)strtoll(v, NULL, 10) : 0;   // unset: never hand back
        if (ms < 0) ms = 0;
        rt_log("mousecam: hand-back after %lld ms of no mouse movement%s", (long long)ms, ms ? "" : " (never)");
    }
    return ms ? (uint64_t)ms * 1000000ull : ~0ull;
}

typedef struct {
    int active;                // rotating: yaw / pitch below are ours; 0: the game's camera stands
    float yaw, pitch;
    float zoom, zoom_target;   // offset on the game's radius
    float off_yaw, off_pitch;  // relative mode: the mouse's rotation on top of the game's own camera
    float rate_yaw, rate_pitch; // relative mode, glide: the last turn rate in rad/s
    uint64_t last_move_ns, last_ns;
} Ml;
static Ml ml;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// cam: pos[3], focus[3], up[3] as the game left them.
static void apply(float *cam, float dx, float dy, float scroll, int reset, const float stick[2], float dt) {
    float *pos = cam, *foc = cam + 3, *up = cam + 6;
    float vx = pos[0] - foc[0], vy = pos[1] - foc[1], vz = pos[2] - foc[2];
    float r = sqrtf(vx * vx + vy * vy + vz * vz);
    if (!isfinite(r) || r < 0.1f) return;
    uint64_t now = kern_now_ns();

    const char *inv = cfg_get("TOTK_MOUSE_INVERT_Y");
    if (inv && *inv && strcmp(inv, "0")) dy = -dy;
    float rad = input_mouse_sens() * RAD_PER_COUNT;

    // stick (gamepad and arrow keys, not the mouse): the game's own camera input is overwritten while we rotate
    float sx = fabsf(stick[0]) > STICK_DEAD ? stick[0] : 0, sy = fabsf(stick[1]) > STICK_DEAD ? stick[1] : 0;
    float yaw_d = -dx * rad - sx * STICK_RAD_PER_S * dt;      // mod: g_yaw -= dx * sens (stick: dx -= stickX * 40)
    float pitch_d = dy * rad + sy * STICK_RAD_PER_S * dt;     // mod: g_pitch += dy * sens (stick: dy += stickY * 40)
    int moving = dx != 0 || dy != 0 || sx != 0 || sy != 0;

    if (moving) {
        ml.last_move_ns = now;
        if (!ml.active) {   // take over from the game's camera where it stands
            ml.yaw = atan2f(vx, vz);
            ml.pitch = asinf(clampf(vy / r, -1, 1));
            ml.active = 1;
        }
    } else if (ml.active && now - ml.last_move_ns > idle_ns()) {
        ml.active = 0;      // hand back
    }

    // zoom: wheel notches move the target, the offset follows it
    if (reset) ml.zoom_target = 0;
    if (fabsf(scroll) > 0.01f) ml.zoom_target -= scroll * ZOOM_STEP;
    ml.zoom_target = clampf(ml.zoom_target, MIN_RADIUS - r, MAX_RADIUS - r);
    float k = 1.0f - powf(1.0f - ZOOM_EASE_PER_60HZ, dt * 60.0f);
    float diff = ml.zoom_target - ml.zoom;
    ml.zoom = fabsf(diff) < 0.01f ? ml.zoom_target : ml.zoom + diff * k;
    float R = clampf(r + ml.zoom, MIN_RADIUS, MAX_RADIUS);
    if (!isfinite(R)) { ml.zoom = ml.zoom_target = 0; R = r; }

    if (ml.active) {
        ml.yaw += yaw_d;
        ml.pitch = clampf(ml.pitch + pitch_d, -PITCH_LIMIT, PITCH_LIMIT);
        float h = R * cosf(ml.pitch);
        pos[0] = foc[0] + h * sinf(ml.yaw);
        pos[1] = foc[1] + R * sinf(ml.pitch);
        pos[2] = foc[2] + h * cosf(ml.yaw);
        up[0] = 0; up[1] = 1; up[2] = 0;
    } else if (fabsf(R - r) > 1e-4f) {   // only zoom, along the game's own direction
        float s = R / r;
        pos[0] = foc[0] + vx * s;
        pos[1] = foc[1] + vy * s;
        pos[2] = foc[2] + vz * s;
    }
}

// ---- relative mode (default): the mouse does not replace the game's camera, it rotates it. Each frame the game's own camera
// (which follows Link, turns behind him when he runs, and keeps its distance) is read, and the mouse's accumulated yaw / pitch
// offset is added to its direction around the focus point. So the camera stays locked to Link like with F3 off, and the mouse
// still turns it directly. TOTK_MOUSECAM_MODE=absolute brings back the old mode (yaw / pitch owned by the mouse).
// TOTK_MOUSECAM_RECENTRE_MS=<ms>: after this long without mouse movement the offset eases back to 0 (default 0 = never).
// TOTK_MOUSECAM_PITCH_MIN=<radians>: lowest the camera may go below the focus (default -0.5; more negative = lower).
static int mode_relative(void) {
    static int m = -1;
    if (m < 0) {
        const char *v = cfg_get("TOTK_MOUSECAM_MODE");
        m = v && *v && !strcmp(v, "absolute") ? 0 : 1;
        rt_log("mousecam: mouse mode %s", m ? "relative (on top of the game's camera)" : "absolute");
    }
    return m;
}

static float pitch_min(void) {
    static float p = 1e9f;
    if (p > 1e8f) {
        const char *v = cfg_get("TOTK_MOUSECAM_PITCH_MIN");
        p = v && *v ? (float)strtod(v, NULL) : -0.5f;
        if (!(p > -PITCH_LIMIT && p < 0.5f)) p = -0.5f;
    }
    return p;
}

static uint64_t recentre_ns(void) {
    static int64_t ms = -1;
    if (ms < 0) {
        const char *v = cfg_get("TOTK_MOUSECAM_RECENTRE_MS");
        ms = v && *v ? (int64_t)strtoll(v, NULL, 10) : 0;
        if (ms < 0) ms = 0;
    }
    return (uint64_t)ms * 1000000ull;
}

// TOTK_MOUSECAM_GLIDE_MS=<ms>: how long the camera keeps turning after the mouse stops (default 0 = stops at once, as a
// normal mouse look). The game's own mouse-to-stick path sweeps on like this; 100-200 is a starting point.
static float glide_s(void) {
    static float s = -1;
    if (s < 0) {
        const char *v = cfg_get("TOTK_MOUSECAM_GLIDE_MS");
        s = v && *v ? (float)strtod(v, NULL) * 0.001f : 0.0f;
        if (!(s >= 0 && s < 2.0f)) s = 0;
        rt_log("mousecam: glide %.0f ms", s * 1000.0f);
    }
    return s;
}

// ---- over-the-shoulder pivot. The game does not aim its camera at Link's axis: its focus point sits SHIFT_R to the camera's right
// and SHIFT_B back toward the camera from it, and that shift turns with the game's yaw (build\mousecam_center.csv, F3 off, Link
// standing still: the raw focus traces a circle of radius ~0.21 and fits "axis + 0.205 * right + 0.065 * back" to 0.006, over
// 1500 frames at every pitch). Rotating pos and focus about the FOCUS leaves that shift fixed in the world while the view turns,
// so Link slides off-centre; rotating both about the axis (the pivot) keeps it locked to the view, as the game's own turn does.
// TOTK_MOUSECAM_SHIFT_R / TOTK_MOUSECAM_SHIFT_B: other values (0 and 0 = the old rotation about the focus).
static float shift_r(void) {
    static float v = -1e9f;
    if (v < -1e8f) {
        const char *e = cfg_get("TOTK_MOUSECAM_SHIFT_R");
        v = e && *e ? (float)strtod(e, NULL) : 0.205f;
        rt_log("mousecam: pivot shift right %.3f", v);
    }
    return v;
}
static float shift_b(void) {
    static float v = -1e9f;
    if (v < -1e8f) {
        const char *e = cfg_get("TOTK_MOUSECAM_SHIFT_B");
        v = e && *e ? (float)strtod(e, NULL) : 0.065f;
        rt_log("mousecam: pivot shift back %.3f", v);
    }
    return v;
}
// The game's focus is smoothed: it trails Link by about LEAD_CALLS times its own per-update step (first-order lag: after he stops, the
// focus coasts on for ~0.3 m at running speed, in the 1.4.3 CSV). Rotating about the focus-derived axis
// therefore turns the view about a point behind Link by that lag, and he slides sideways by 2 sin(a/2) * lag (0.3 m at 70 deg):
// off-centre whenever he moves with the mouse's turn applied. The pivot is moved forward by the lag, estimated from the raw focus's
// own per-update step: the focus advances by a fixed step per game update (0.065..0.085 m while running), while the host's clock
// between updates varies 12..17 ms, so a step per second would jitter by 15% and make the picture shake. After he stops the focus
// goes on for ~4.1 steps' worth. TOTK_MOUSECAM_LEAD_CALLS: updates (default 4.0; 0 = pivot on the axis behind him).
static float lead_calls(void) {
    static float v = -1e9f;
    if (v < -1e8f) {
        const char *e = cfg_get("TOTK_MOUSECAM_LEAD_CALLS");
        v = e && *e ? (float)strtod(e, NULL) : 4.0f;
        if (!(v >= 0 && v < 20.0f)) v = 4.0f;
        rt_log("mousecam: pivot lead %.1f camera updates", v);
    }
    return v;
}
static float wrap_rad(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a <= -3.14159265f) a += 6.2831853f;
    return a;
}
// The game's own yaw field (1.4.3: degrees at param2+0x71c, see tools/yaw_find.py), set by host_camera_body every frame. Used
// only when the camera looks (almost) straight down, where the yaw of pos - focus is ill defined: its offset to that yaw is
// learned while the camera is not steep.
static float ml_field_yaw;
static int ml_have_field, ml_have_phi;
static float ml_phi;

// ---- Link's own position (TOTK_MOUSECAM_LINK=0 turns the code below off). The pivot above is built from the game's smoothed
// focus and a guess of how far it trails him, which is why Link sat off-centre whenever he moved with the mouse's turn applied
// (the guess depends on the direction he runs in). The runtime already finds the player actor for the world clock
// (runtime/frame_rate.c player_pos, MacNativeGames world_survey.c); this reads the same position.
//   1.2.1: [main+0x4728538]  1.4.3: [main+0x3add238]  +0x1e8 scene (+0x50 count, +0x58 component list), component 13 = the resident
//   actors (+0x20 count, +0x28 links, 0x70 apart, +8 data, data+0x40 actor); the one named "Player" (actor+0x218) has its position at +0x2b4.
static uint64_t lk_actor;
static unsigned lk_wait;
static int lk_have;       // for the CENTER csv: this frame used his position
static float lk_pos[3], lk_w;

// TOTK_MOUSECAM_FLOOR=<m>: the camera is kept at least this far above Link's feet (default 0.35, 0 = off). The mouse's pitch lowers the
// camera past the game's own ground collision, so looking all the way down put it under the floor.
static float floor_margin(void) {
    static float m = -1;
    if (m < 0) {
        const char *v = cfg_get("TOTK_MOUSECAM_FLOOR");
        m = v && *v ? (float)strtod(v, NULL) : 0.35f;
        if (m < 0 || m > 3) m = 0.35f;
        rt_log("mousecam: camera floor %.2f m above Link's feet", m);
    }
    return m;
}

static uint64_t lk_ptr(uint64_t a) { return a && readable(a, 8) ? *(volatile uint64_t *)(uintptr_t)a : 0; }

static int lk_is_player(uint64_t actor) {
    uint64_t name = lk_ptr(actor + 0x218);
    return name && readable(name, 7) && !memcmp((const void *)(uintptr_t)name, "Player", 7);
}

static uint64_t lk_find(void) {
    uint64_t mod = lk_ptr(ldr_module(0)->base + (rt_game_version >= 143 ? 0x3add238ull : 0x4728538ull));
    uint64_t scene = mod ? lk_ptr(mod + 0x1e8) : 0;
    int32_t n = 0;
    if (!scene || !readable(scene + 0x50, 4)) return 0;
    n = *(volatile int32_t *)(uintptr_t)(scene + 0x50);
    if (n < 14 || n > 256) return 0;
    uint64_t comps = lk_ptr(scene + 0x58), res = comps ? lk_ptr(comps + 13 * 8) : 0;
    if (!res || !readable(res + 0x20, 4)) return 0;
    n = *(volatile int32_t *)(uintptr_t)(res + 0x20);
    if (n < 1 || n > 64) return 0;
    uint64_t links = lk_ptr(res + 0x28);
    for (int i = 0; links && i < n; i++) {
        uint64_t data = lk_ptr(links + (uint64_t)i * 0x70 + 8), actor = data ? lk_ptr(data + 0x40) : 0;
        if (actor && lk_is_player(actor)) return actor;
    }
    return 0;
}

// xyz: Link's position. False while he is not in the world (loading screen, title) or on a version without the offsets.
static int link_pos(float xyz[3]) {
    static int on = -1;
    if (on < 0) {
        const char *v = cfg_get("TOTK_MOUSECAM_LINK");
        on = v && *v && !strcmp(v, "0") ? 0 : 1;
        rt_log("mousecam: pivot on Link's own position: %s", on ? "on" : "off");
    }
    if (!on) return 0;
    if (!lk_actor || !lk_is_player(lk_actor)) {
        lk_actor = 0;
        if (lk_wait) { lk_wait--; return 0; }   // a full search is some dozens of guest reads: at most twice a second while he is missing
        lk_actor = lk_find();
        if (!lk_actor) { lk_wait = 30; return 0; }
        rt_log("mousecam: found the player actor at %llx", (unsigned long long)lk_actor);
    }
    if (!readable(lk_actor + 0x2b4, 12)) return 0;
    memcpy(xyz, (const void *)(uintptr_t)(lk_actor + 0x2b4), 12);
    return isfinite(xyz[0]) && isfinite(xyz[1]) && isfinite(xyz[2]) && fabsf(xyz[0]) + fabsf(xyz[2]) > 1.0f;
}

// The camera the mouse turned keeps Link where the game's own camera would have put him. Rotating the game's camera rigidly about
// Link's axis leaves him at the screen position the game's camera at its own yaw gave him; but that position depends on the game's
// yaw (when he runs sideways to the game's yaw the game's camera shows him off to the side and is about to turn after him; the mod's
// view is turned, he is running straight ahead in it). In the game's own camera the focus is Link's axis plus SHIFT_R to the camera's
// right and SHIFT_B back, plus the smoothed trail behind him (the world-fixed vector from the axis to the focus' trailing point).
// So, in the turned camera's frame, the lateral coordinate of Link relative to the focus is -SHIFT_R, and the one along the view is
// his trail along the view minus SHIFT_B. w = 0: the game's own framing is kept (no offset); w = 1: these targets.
// A: Link's x z. foc_raw: the game's focus. yawg: the game's yaw. pos / foc: the turned camera, moved sideways / along the view.
static void link_fix(const float A[2], const float foc_raw[3], float yawg, float w, float sr, float sb, float *pos, float *foc) {
    float rrx = cosf(yawg), rrz = -sinf(yawg), brx = sinf(yawg), brz = cosf(yawg);          // the game's right / back (x z)
    float ax = A[0] - foc_raw[0], az = A[1] - foc_raw[2];
    float lat0 = ax * rrx + az * rrz, dep0 = ax * brx + az * brz;                           // Link relative to the game's focus, game's frame
    float px = foc_raw[0] - sr * rrx - sb * brx, pz = foc_raw[2] - sr * rrz - sb * brz;      // the smoothed point behind him (no shoulder shift)
    float bx = pos[0] - foc[0], bz = pos[2] - foc[2], bh = hypotf(bx, bz);
    if (bh < 1e-3f) return;
    bx /= bh; bz /= bh;
    float rx = bz, rz = -bx;                                                                 // the turned camera's right
    float lat_t = lat0 + w * (-sr - lat0);
    float dep_t = dep0 + w * (((A[0] - px) * bx + (A[1] - pz) * bz - sb) - dep0);
    float cx = A[0] - foc[0], cz = A[1] - foc[2];
    float dl = (cx * rx + cz * rz) - lat_t, dd = (cx * bx + cz * bz) - dep_t;               // how far the focus must move
    float mx = dl * rx + dd * bx, mz = dl * rz + dd * bz;
    foc[0] += mx; foc[2] += mz;
    pos[0] += mx; pos[2] += mz;
}

static void apply_rel(float *cam, float dx, float dy, float scroll, int reset, float dt) {
    float *pos = cam, *foc = cam + 3, *up = cam + 6;
    float vx = pos[0] - foc[0], vy = pos[1] - foc[1], vz = pos[2] - foc[2];
    float r = sqrtf(vx * vx + vy * vy + vz * vz);
    if (!isfinite(r) || r < 0.1f) return;
    uint64_t now = kern_now_ns();

    // step of the game's raw focus per camera update (smoothed over ~4 updates); reset after a gap or a jump (teleport, cutscene cut)
    static uint64_t vt_ns;
    static float vlast[2], vvel[2];
    float lead_vx, lead_vz;
    {
        float fdt = (float)(now - vt_ns) * 1e-9f;
        if (!vt_ns || fdt >= 0.1f) {
            vvel[0] = vvel[1] = 0; vlast[0] = foc[0]; vlast[1] = foc[2]; vt_ns = now;
        } else if (fdt > 0.002f) {
            float sx = foc[0] - vlast[0], sz = foc[2] - vlast[1];
            if (hypotf(sx, sz) < 1.0f) {   // per call: the focus advances by a fixed step per game update, whatever the host timing
                vvel[0] += (sx - vvel[0]) * 0.25f;
                vvel[1] += (sz - vvel[1]) * 0.25f;
            } else vvel[0] = vvel[1] = 0;
            vlast[0] = foc[0]; vlast[1] = foc[2]; vt_ns = now;
        }
        lead_vx = vvel[0]; lead_vz = vvel[1];
    }

    const char *inv = cfg_get("TOTK_MOUSE_INVERT_Y");
    if (inv && *inv && strcmp(inv, "0")) dy = -dy;
    float rad = input_mouse_sens() * RAD_PER_COUNT;

    // gamepad / arrow-key stick input is left to the game's own camera here: it still gets it
    float yaw_d = -dx * rad, pitch_d = dy * rad;
    int moved = dx != 0 || dy != 0;
    float glide = glide_s();
    if (glide > 0) {   // coast: after the mouse stops, keep turning at the last speed, slowing down over `glide` seconds
        if (moved) {
            float a = 1.0f - expf(-dt / 0.03f);   // the turn rate (rad/s), smoothed over a few frames
            ml.rate_yaw += (yaw_d / dt - ml.rate_yaw) * a;
            ml.rate_pitch += (pitch_d / dt - ml.rate_pitch) * a;
        } else {
            float k = expf(-dt / glide);
            ml.rate_yaw *= k;
            ml.rate_pitch *= k;
            if (fabsf(ml.rate_yaw) < 0.01f) ml.rate_yaw = 0;
            if (fabsf(ml.rate_pitch) < 0.01f) ml.rate_pitch = 0;
            yaw_d = ml.rate_yaw * dt;
            pitch_d = ml.rate_pitch * dt;
        }
    }
    if (moved || yaw_d != 0 || pitch_d != 0) {
        if (moved) ml.last_move_ns = now;
        ml.off_yaw += yaw_d;
        ml.off_pitch += pitch_d;
        if (ml.off_yaw > 3.14159265f) ml.off_yaw -= 6.2831853f;
        if (ml.off_yaw < -3.14159265f) ml.off_yaw += 6.2831853f;
    } else if (recentre_ns() && now - ml.last_move_ns > recentre_ns()) {
        float k = expf(-dt * 4.0f);   // ease back to the game's own direction
        ml.off_yaw *= k;
        ml.off_pitch *= k;
    }

    // zoom: same as the absolute mode
    if (reset) ml.zoom_target = 0;
    if (fabsf(scroll) > 0.01f) ml.zoom_target -= scroll * ZOOM_STEP;
    ml.zoom_target = clampf(ml.zoom_target, MIN_RADIUS - r, MAX_RADIUS - r);
    float kz = 1.0f - powf(1.0f - ZOOM_EASE_PER_60HZ, dt * 60.0f);
    float diff = ml.zoom_target - ml.zoom;
    ml.zoom = fabsf(diff) < 0.01f ? ml.zoom_target : ml.zoom + diff * kz;
    float R = clampf(r + ml.zoom, MIN_RADIUS, MAX_RADIUS);
    if (!isfinite(R)) { ml.zoom = ml.zoom_target = 0; R = r; }

    float foc_raw[3] = {foc[0], foc[1], foc[2]};   // the game's own focus, for link_fix
    float yaw0 = atan2f(vx, vz), pitch0 = asinf(clampf(vy / r, -1, 1));
    float hz = hypotf(vx, vz);
    float yawg = yaw0;   // the game's yaw, from pos - focus; from the game's own field when the camera looks steeply down
    if (ml_have_field) {
        float fy = ml_field_yaw * 0.017453293f;
        if (hz > 3.0f) {
            float ph = wrap_rad(yaw0 - fy);
            if (!ml_have_phi) { ml_phi = ph; ml_have_phi = 1; }
            else ml_phi = wrap_rad(ml_phi + 0.02f * wrap_rad(ph - ml_phi));
        }
        if (ml_have_phi && hz < 1.0f) yawg = wrap_rad(fy + ml_phi);
    }
    // the pitch offset may not take the camera below pitch_min (or past the top limit); it never fights the game's own pitch
    float lo = fminf(pitch_min(), pitch0), hi = fmaxf(PITCH_LIMIT, pitch0);
    float pitch = clampf(pitch0 + ml.off_pitch, lo, hi);
    ml.off_pitch = pitch - pitch0;

    ml.active = fabsf(ml.off_yaw) > 1e-4f || fabsf(ml.off_pitch) > 1e-4f;
    if (ml.active) {
        // pivot C = focus - shift (in the game's camera frame): right = (cos y, -sin y), back = (sin y, cos y)
        float sr = shift_r(), sb = shift_b();
        float cx = foc[0] - sr * cosf(yawg) - sb * sinf(yawg);
        float cz = foc[2] + sr * sinf(yawg) - sb * cosf(yawg);
        // Link is ahead of the smoothed focus by lead * its velocity (world units, horizontal)
        cx += clampf(lead_calls() * lead_vx, -1.0f, 1.0f);
        cz += clampf(lead_calls() * lead_vz, -1.0f, 1.0f);
        // Link's own position, when it can be read and the camera is following him: the pivot is exactly his axis
        float lk[3], A[2] = {0, 0};
        int have_lk = link_pos(lk) && hypotf(lk[0] - foc[0], lk[2] - foc[2]) < 3.0f;
        if (have_lk) {
            A[0] = lk[0]; A[1] = lk[2];
            cx = A[0]; cz = A[1];
        }
        // turn pos and focus rigidly about the vertical axis through C (identical to the game's picture when off_yaw is 0)
        float ca = cosf(ml.off_yaw), sa = sinf(ml.off_yaw);
        float fxr = foc[0] - cx, fzr = foc[2] - cz;
        float fx1 = cx + fxr * ca + fzr * sa, fz1 = cz - fxr * sa + fzr * ca;
        float pxr = pos[0] - cx, pzr = pos[2] - cz;
        float px1 = cx + pxr * ca + pzr * sa, pz1 = cz - pxr * sa + pzr * ca;
        // then the pitch change and the zoom about the new focus, along the turned horizontal direction
        float ux = px1 - fx1, uz = pz1 - fz1, uh = hypotf(ux, uz);
        if (uh < 1e-4f) { ux = sinf(yawg + ml.off_yaw); uz = cosf(yawg + ml.off_yaw); uh = 1; }
        float h = R * cosf(pitch);
        foc[0] = fx1;
        foc[2] = fz1;
        pos[0] = fx1 + h * ux / uh;
        pos[1] = foc[1] + R * sinf(pitch);
        pos[2] = fz1 + h * uz / uh;
        up[0] = 0; up[1] = 1; up[2] = 0;
        if (have_lk && floor_margin() > 0 && pos[1] < lk[1] + floor_margin()) {
            // lower the pitch until the camera is on the floor line, keeping the distance to the focus
            float fl = lk[1] + floor_margin(), s2 = clampf((fl - foc[1]) / R, -1.0f, 1.0f);
            float hh = R * sqrtf(fmaxf(0.0f, 1.0f - s2 * s2));
            pos[0] = fx1 + hh * ux / uh;
            pos[1] = foc[1] + R * s2;
            pos[2] = fz1 + hh * uz / uh;
            ml.off_pitch = asinf(s2) - pitch0;   // so more mouse-down does not pile up unseen
        }
        lk_w = 0;
        if (have_lk) {   // the framing: full strength from ~30 degrees of turn on, so there is no jump when the offset comes and goes
            lk_w = clampf(fabsf(ml.off_yaw) / 0.5f, 0.0f, 1.0f);
            link_fix(A, foc_raw, yawg, lk_w, sr, sb, pos, foc);
        }
        lk_have = have_lk;
        lk_pos[0] = lk[0]; lk_pos[1] = lk[1]; lk_pos[2] = lk[2];
    } else if (fabsf(R - r) > 1e-4f) {   // only zoom, along the game's own direction (the game's up vector stays)
        float s = R / r;
        pos[0] = foc[0] + vx * s;
        pos[1] = foc[1] + vy * s;
        pos[2] = foc[2] + vz * s;
    }
}

static int ix(float v) { return v > -1e6f && v < 1e6f ? (int)v : 0; }

// ---- probe snapshots. File = records of { u64 tag, u64 addr, u64 src_off, u64 len } + len bytes.
//   tag 1: the param2 object (0x1000 bytes at p2)            addr = p2
//   tag 2: the memory around the camera floats               addr = camp - 0x100
//   tag 3: an object pointed to from p2's first 0x400 bytes  addr = target, src_off = offset in p2 of the pointer
#define PROBE_P2_LEN 0x1000
#define PROBE_PTR_SCAN 0x400
#define PROBE_PTR_LEN 0x200
#define PROBE_MAX_PTRS 120

static void probe_rec(FILE *f, uint64_t tag, uint64_t addr, uint64_t src, uint64_t len) {
    uint64_t h[4] = {tag, addr, src, len};
    fwrite(h, sizeof h, 1, f);
    fwrite((const void *)(uintptr_t)addr, 1, (size_t)len, f);
}

static void probe_dump(uint64_t p2, uint64_t camp) {
    static int seq;
    char name[96];
    snprintf(name, sizeof name, "build\\mousecam_probe_%02d.bin", seq);
    FILE *f = fopen(name, "wb");
    if (!f) { rt_log("mousecam: probe: cannot write %s", name); return; }
    int recs = 0, ptrs = 0;
    if (readable(p2, PROBE_P2_LEN)) {
        probe_rec(f, 1, p2, 0, PROBE_P2_LEN);
        recs++;
        for (uint64_t o = 0; o < PROBE_PTR_SCAN && ptrs < PROBE_MAX_PTRS; o += 8) {
            uint64_t v = *(volatile uint64_t *)(uintptr_t)(p2 + o);
            if (v < 0x100000000ull || (v & 7) || v == p2 || !readable(v, PROBE_PTR_LEN)) continue;
            probe_rec(f, 3, v, o, PROBE_PTR_LEN);
            recs++;
            ptrs++;
        }
    }
    if (camp > 0x100 && readable(camp - 0x100, 0x400)) { probe_rec(f, 2, camp - 0x100, 0, 0x400); recs++; }
    fclose(f);
    rt_log("mousecam: probe snapshot %d written to %s (%d records, %d pointed-to objects)", seq, name, recs, ptrs);
    seq++;
}


// ---- CENTER PROBE (TOTK_MOUSECAM_CENTER=1): one CSV row per camera update in build\mousecam_center.csv with the camera exactly
// as the game left it (raw) and as it leaves this function (final), plus the mouse state. For the "Link drifts off-centre while
// spinning with F3 on" problem: compare the raw focus point with F3 on and off. Costs nothing when the variable is unset.
static float cr_raw[9];
static int cr_ok;

static int center_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = cfg_get("TOTK_MOUSECAM_CENTER");
        on = v && *v && strcmp(v, "0") ? 1 : 0;
        if (on) rt_log("mousecam: CENTER PROBE: writing build\\mousecam_center.csv");
    }
    return on;
}

static void center_raw(uint64_t p3) {
    cr_ok = 0;
    if (!center_on() || !readable(p3, 8)) return;
    uint64_t cp = *(volatile uint64_t *)(uintptr_t)p3;
    if (!cp || !readable(cp, 36)) return;
    memcpy(cr_raw, (const void *)(uintptr_t)cp, sizeof cr_raw);
    cr_ok = 1;
}

static void center_log(uint64_t p3) {
    static FILE *f;
    static unsigned long rows;
    if (!cr_ok || rows >= 60000ul || !readable(p3, 8)) return;
    uint64_t cp = *(volatile uint64_t *)(uintptr_t)p3;
    if (!cp || !readable(cp, 36)) return;
    float fin[9];
    memcpy(fin, (const void *)(uintptr_t)cp, sizeof fin);
    if (!f) {
        f = fopen("build\\mousecam_center.csv", "w");
        if (!f) { rows = 60000ul; rt_log("mousecam: center probe: cannot write build\\mousecam_center.csv"); return; }
        fprintf(f, "n,ms,f3,active,off_yaw,off_pitch,zoom,"
                   "rpx,rpy,rpz,rfx,rfy,rfz,rux,ruy,ruz,"
                   "fpx,fpy,fpz,ffx,ffy,ffz,fux,fuy,fuz,lk,lkx,lky,lkz,lkw\n");
    }
    fprintf(f, "%lu,%llu,%d,%d,%.5f,%.5f,%.4f", rows, (unsigned long long)(kern_now_ns() / 1000000ull), (int)ml_enabled, ml.active,
            ml.off_yaw, ml.off_pitch, ml.zoom);
    for (int i = 0; i < 9; i++) fprintf(f, ",%.4f", cr_raw[i]);
    for (int i = 0; i < 9; i++) fprintf(f, ",%.4f", fin[i]);
    fprintf(f, ",%d,%.4f,%.4f,%.4f,%.3f", lk_have, lk_pos[0], lk_pos[1], lk_pos[2], lk_w);
    fputc('\n', f);
    if (++rows % 120 == 0) fflush(f);
}


// ---- TOTK_MOUSECAM_SYNC_P2=1 (1.4.3 only, off by default): the camera function's param2 object keeps a view matrix built from the
// camera it just computed: three rows { axis x, y, z, -axis . pos } for right, up and back, starting at float 15 (byte +0x3c) of
// param2. Probe snapshots show them equal to lookAt(pos, focus, up) of the nine camera floats to 3e-4. After the rewrite below
// those rows are stale (they still hold the game's own yaw); anything that reads them instead of the nine floats sees a camera
// that did not turn. This rewrites the rows to match the camera the mod leaves behind. An experiment for the "Link drifts
// off-centre with F3 on" problem: if it does not change the picture, unset it.
static int sync_p2_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = cfg_get("TOTK_MOUSECAM_SYNC_P2");
        on = v && *v && strcmp(v, "0") && rt_game_version >= 143 ? 1 : 0;
        rt_log("mousecam: sync param2 view matrix: %s", on ? "on" : "off");
    }
    return on;
}

static void sync_p2(uint64_t p2, const float *cam) {
    if (!sync_p2_on() || !readable(p2 + 0x3c, 48)) return;
    const float *pos = cam, *foc = cam + 3, *up = cam + 6;
    float b[3] = {pos[0] - foc[0], pos[1] - foc[1], pos[2] - foc[2]};
    float bl = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (!isfinite(bl) || bl < 1e-4f) return;
    for (int i = 0; i < 3; i++) b[i] /= bl;
    float r[3] = {up[1] * b[2] - up[2] * b[1], up[2] * b[0] - up[0] * b[2], up[0] * b[1] - up[1] * b[0]};   // up x back
    float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (!isfinite(rl) || rl < 1e-4f) return;
    for (int i = 0; i < 3; i++) r[i] /= rl;
    float u[3] = {b[1] * r[2] - b[2] * r[1], b[2] * r[0] - b[0] * r[2], b[0] * r[1] - b[1] * r[0]};         // back x right
    const float *ax[3] = {r, u, b};
    volatile float *m = (volatile float *)(uintptr_t)(p2 + 0x3c);
    for (int k = 0; k < 3; k++) {
        m[k * 4 + 0] = ax[k][0];
        m[k * 4 + 1] = ax[k][1];
        m[k * 4 + 2] = ax[k][2];
        m[k * 4 + 3] = -(ax[k][0] * pos[0] + ax[k][1] * pos[1] + ax[k][2] * pos[2]);
    }
}


// ---- TOTK_MOUSECAM_SCAN=1|2 (1.4.3): finds every copy of the game's camera that this function does not rewrite. SYNC_P2 only
// patches the view matrix at param2+0x3c; the "Link drifts off-centre with F3 on" problem survives it, so something else still
// holds the game's own yaw. Right after the original camera function ran (the memory then describes the game's own camera, before
// our rewrite) this scans param2 and the objects it points to (two pointer levels, 0x400 bytes each) for float triples equal to the
// camera's right / up / back axes (consecutive floats, or every 4th float = a matrix column), for the camera position, and for
// the translation word -axis.pos that follows a matrix row. A scan only counts when the game's yaw differs from the previous
// scan's by 0.25 rad, so static data cannot match twice: SPIN THE CAMERA WITH F3 OFF. Every hit is logged as
// "mousecam: scan: ..."; one seen in two scans is CONFIRMED.
// SCAN=2 additionally rewrites every confirmed hit each frame from the camera this function leaves behind.
#define SC_MAX_HITS 160
#define SC_MAX_OBJS 400
// verdict: 0 untested, 1 nudged (next frame decides), 2 output (the game rebuilds it each frame: safe to write), 3 state (it keeps
// what is written: the game may read it back, so it is never written)
typedef struct { int o1, o2, off, stride, kind, sign, tr, seen, verdict; float pert[3]; } ScHit;   // kind: 0 right, 1 up, 2 back, 3 position
#define SC_CAM_BACK 0x200
#define SC_CAM_LEN 0x900
static uint64_t sc_camp;   // address of the nine camera floats, refreshed every frame
static ScHit sc_hits[SC_MAX_HITS];
static int sc_nhits, sc_scans;
static float sc_last_yaw;
static int sc_have_yaw;

static int scan_mode(void) {
    static int m = -1;
    if (m < 0) {
        const char *v = cfg_get("TOTK_MOUSECAM_SCAN");
        m = v && *v ? atoi(v) : 0;
        if (m < 0 || m > 2) m = 0;
        if (m) rt_log("mousecam: SCAN mode %d: spin the camera with F3 off; hits are logged as 'mousecam: scan:'", m);
    }
    return m;
}

static int scan_deep(void) {   // TOTK_MOUSECAM_SCAN_DEEP=1: also follow pointers out of param2 (crashed the game once)
    static int d = -1;
    if (d < 0) { const char *v = cfg_get("TOTK_MOUSECAM_SCAN_DEEP"); d = v && *v && strcmp(v, "0") ? 1 : 0; }
    return d;
}

static int scan_min_seen(void) {   // TOTK_MOUSECAM_SCAN_SEEN=1: apply a hit from its first scan on (default 2: seen at two yaws)
    static int n = -1;
    if (n < 0) { const char *v = cfg_get("TOTK_MOUSECAM_SCAN_SEEN"); n = v && *v ? atoi(v) : 2; if (n < 1) n = 1; }
    return n;
}

// out[0..3] = right, up, back (unit), out[3] = position. false: degenerate.
static int cam_axes(const float *cam, float out[4][3]) {
    const float *pos = cam, *foc = cam + 3, *up = cam + 6;
    float b[3] = {pos[0] - foc[0], pos[1] - foc[1], pos[2] - foc[2]};
    float bl = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (!isfinite(bl) || bl < 1e-4f) return 0;
    for (int i = 0; i < 3; i++) b[i] /= bl;
    float r[3] = {up[1] * b[2] - up[2] * b[1], up[2] * b[0] - up[0] * b[2], up[0] * b[1] - up[1] * b[0]};
    float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (!isfinite(rl) || rl < 1e-4f) return 0;
    for (int i = 0; i < 3; i++) r[i] /= rl;
    float u[3] = {b[1] * r[2] - b[2] * r[1], b[2] * r[0] - b[0] * r[2], b[0] * r[1] - b[1] * r[0]};
    for (int i = 0; i < 3; i++) { out[0][i] = r[i]; out[1][i] = u[i]; out[2][i] = b[i]; out[3][i] = pos[i]; }
    return 1;
}

static int sc_close(const float *a, const float *b, float tol) {
    return fabsf(a[0] - b[0]) < tol && fabsf(a[1] - b[1]) < tol && fabsf(a[2] - b[2]) < tol;
}

// readable() only looks at the page the range starts on; this checks every page of it, so a copy can not run into unmapped memory.
static int readable_span(uint64_t a, uint64_t n) {
    for (uint64_t o = 0; o < n; o += 0x1000)
        if (!readable(a + o, 1)) return 0;
    return readable(a + n - 1, 1);
}

static void sc_hit(int o1, int o2, int off, int stride, int kind, int sign, int tr) {
    if (o1 == -2 && off >= SC_CAM_BACK - 4 && off < SC_CAM_BACK + 36) return;   // the nine camera floats themselves (position matches)
    for (int i = 0; i < sc_nhits; i++) {
        ScHit *h = &sc_hits[i];
        if (h->o1 == o1 && h->o2 == o2 && h->off == off && h->stride == stride && h->kind == kind && h->sign == sign) {
            if (++h->seen == scan_min_seen() && scan_min_seen() > 1)
                rt_log("mousecam: scan: CONFIRMED %s%s (p2 +0x%x, then +0x%x, then +0x%x) stride %d%s", sign < 0 ? "-" : "",
                       kind == 0 ? "right" : kind == 1 ? "up" : kind == 2 ? "back" : "pos", o1 < 0 ? 0 : o1, o2 < 0 ? 0 : o2, off,
                       stride, h->tr ? " (+translation word)" : "");
            return;
        }
    }
    if (sc_nhits >= SC_MAX_HITS) return;
    ScHit *h = &sc_hits[sc_nhits++];
    h->o1 = o1; h->o2 = o2; h->off = off; h->stride = stride; h->kind = kind; h->sign = sign; h->tr = tr; h->seen = 1; h->verdict = 0;
    rt_log("mousecam: scan: hit %s%s: %s p2 +0x%x%s+0x%x stride %d%s", sign < 0 ? "-" : "",
           kind == 0 ? "right" : kind == 1 ? "up" : kind == 2 ? "back" : "pos",
           o1 == -2 ? "near the camera floats (camp-0x200)" : o1 < 0 ? "directly in" : "in object behind", o1 < 0 ? 0 : o1, o2 < 0 ? " " : " ->", off, stride,
           tr ? " (+translation word)" : "");
}

static void sc_buf(const uint8_t *buf, int len, int o1, int o2, float ax[4][3]) {
    int nf = len / 4;
    for (int i = 0; i < nf; i++) {
        for (int stride = 1; stride <= 4; stride += 3) {
            if (i + 2 * stride >= nf) continue;
            float t[3];
            for (int k = 0; k < 3; k++) memcpy(&t[k], buf + (size_t)(i + k * stride) * 4, 4);
            if (!isfinite(t[0]) || !isfinite(t[1]) || !isfinite(t[2])) continue;
            for (int kind = 0; kind < 4; kind++) {
                for (int sign = 1; sign >= -1; sign -= 2) {
                    if (kind == 3 && sign < 0) continue;
                    float want[3] = {ax[kind][0] * sign, ax[kind][1] * sign, ax[kind][2] * sign};
                    if (kind < 3) {
                        int big = 0;
                        for (int k = 0; k < 3; k++) big += fabsf(want[k]) > 0.05f;
                        if (big < 2) continue;   // axis-aligned vectors are everywhere in memory
                        if (!sc_close(t, want, 2e-3f)) continue;
                    } else if (!sc_close(t, want, 0.02f) || fabsf(t[0]) + fabsf(t[2]) < 1.0f) continue;
                    int tr = 0;
                    if (kind < 3 && stride == 1 && i + 3 < nf) {
                        float w, expect = -(want[0] * ax[3][0] + want[1] * ax[3][1] + want[2] * ax[3][2]);
                        memcpy(&w, buf + (size_t)(i + 3) * 4, 4);
                        tr = isfinite(w) && fabsf(w - expect) < 0.05f;
                    }
                    sc_hit(o1, o2, i * 4, stride, kind, sign, tr);
                }
            }
        }
    }
}

static void scan_raw(uint64_t p2, uint64_t p3, uint64_t calln) {
    if (!scan_mode() || sc_scans >= 40 || calln < 600 || calln % 30 != 0 || !readable(p3, 8)) return;   // not while the game is still loading
    uint64_t camp = *(volatile uint64_t *)(uintptr_t)p3;
    if (!readable(camp, 36)) return;
    sc_camp = camp;
    float cam[9], ax[4][3];
    memcpy(cam, (const void *)(uintptr_t)camp, sizeof cam);
    if (!cam_axes(cam, ax)) return;
    float yaw = atan2f(cam[0] - cam[3], cam[2] - cam[5]);
    if (sc_have_yaw) {
        float d = fabsf(yaw - sc_last_yaw);
        if (d > 3.14159265f) d = 6.2831853f - d;
        if (d < 0.25f) return;
    }
    sc_have_yaw = 1;
    sc_last_yaw = yaw;
    sc_scans++;
    int before = sc_nhits;
    static uint8_t buf[0x1000];
    static uint64_t seen_obj[SC_MAX_OBJS];   // static: this runs on a guest stack
    int nobj = 0;
    if (!readable_span(p2, 0x1000)) return;
    memcpy(buf, (const void *)(uintptr_t)p2, 0x1000);
    sc_buf(buf, 0x1000, -1, -1, ax);
    {   // the object the nine camera floats live in
        static uint8_t bc[SC_CAM_LEN];
        if (camp >= SC_CAM_BACK && readable_span(camp - SC_CAM_BACK, SC_CAM_LEN)) {
            memcpy(bc, (const void *)(uintptr_t)(camp - SC_CAM_BACK), SC_CAM_LEN);
            sc_buf(bc, SC_CAM_LEN, -2, -1, ax);
        }
    }
    static uint8_t b1[0x400], b2[0x400];
    for (int o = 0; scan_deep() && o < 0x400 && nobj < SC_MAX_OBJS; o += 8) {
        uint64_t v;
        memcpy(&v, buf + o, 8);
        if (v < 0x100000000ull || (v & 7) || v == p2 || !readable_span(v, 0x400)) continue;
        int dup = 0;
        for (int k = 0; k < nobj; k++) dup |= seen_obj[k] == v;
        if (dup) continue;
        seen_obj[nobj++] = v;
        memcpy(b1, (const void *)(uintptr_t)v, 0x400);
        sc_buf(b1, 0x400, o, -1, ax);
        for (int o2 = 0; o2 < 0x200 && nobj < SC_MAX_OBJS; o2 += 8) {
            uint64_t w;
            memcpy(&w, b1 + o2, 8);
            if (w < 0x100000000ull || (w & 7) || w == p2 || w == v || !readable_span(w, 0x400)) continue;
            dup = 0;
            for (int k = 0; k < nobj; k++) dup |= seen_obj[k] == w;
            if (dup) continue;
            seen_obj[nobj++] = w;
            memcpy(b2, (const void *)(uintptr_t)w, 0x400);
            sc_buf(b2, 0x400, o, o2, ax);
        }
    }
    rt_log("mousecam: scan #%d at yaw %.2f: %d objects, %d new hits, %d total", sc_scans, yaw, nobj, sc_nhits - before, sc_nhits);
}

static uint64_t sc_base(uint64_t p2, const ScHit *h) {
    if (h->o1 == -2) return sc_camp >= SC_CAM_BACK ? sc_camp - SC_CAM_BACK : 0;
    if (h->o1 < 0) return p2;
    if (!readable(p2 + h->o1, 8)) return 0;
    uint64_t v = *(volatile uint64_t *)(uintptr_t)(p2 + h->o1);
    if (h->o2 < 0) return v;
    if (!readable(v + h->o2, 8)) return 0;
    return *(volatile uint64_t *)(uintptr_t)(v + h->o2);
}

// SCAN=2: after our rewrite, put the new camera into every confirmed hit.
// Which kinds of copy are written (TOTK_MOUSECAM_SCAN_GROUPS, default "A"): A = view matrices (rows with a translation word),
// B = camera-to-world matrices (columns, stride 4), C = position copies, D = bare direction vectors.
static int scan_group_on(const ScHit *h) {
    static const char *g;
    if (!g) { const char *v = cfg_get("TOTK_MOUSECAM_SCAN_GROUPS"); g = v && *v ? v : "A"; rt_log("mousecam: scan: writing groups %s", g); }
    char c = h->tr ? 'A' : h->stride == 4 ? 'B' : h->kind == 3 ? 'C' : 'D';
    return strchr(g, c) != NULL;
}

// Runaway guard: with mouselook on and the mouse still, the game's own yaw never moves. If it keeps turning while we write copies,
// something we wrote is read back; stop writing at once instead of spinning the camera.
static int sc_stopped, sc_run;
static float sc_prev_ry;

// Every frame, right after the original camera function ran: decide for each hit whether the game rebuilds it (safe to write) or
// keeps what is written there (state it may read back next frame: writing a rotated camera into it makes the camera spin by itself).
// Frame 1 nudges the copy by a hair, frame 2 looks whether the game replaced the nudge with the camera it just computed.
static void scan_verify(uint64_t p2, uint64_t p3) {
    if (scan_mode() != 2 || !sc_nhits || !readable(p3, 8)) return;
    uint64_t camp = *(volatile uint64_t *)(uintptr_t)p3;
    if (!readable(camp, 36)) return;
    sc_camp = camp;
    float cam[9], ax[4][3];
    memcpy(cam, (const void *)(uintptr_t)camp, sizeof cam);
    if (!cam_axes(cam, ax)) return;
    {
        float ry = atan2f(cam[0] - cam[3], cam[2] - cam[5]), d = fabsf(ry - sc_prev_ry);
        if (d > 3.14159265f) d = 6.2831853f - d;
        sc_prev_ry = ry;
        sc_run = ml.active && d > 0.02f ? sc_run + 1 : 0;
        if (sc_run >= 20 && !sc_stopped) {
            sc_stopped = 1;
            rt_log("mousecam: scan: the game's own camera keeps turning while we write (feedback): scan writes stopped");
        }
    }
    for (int i = 0; i < sc_nhits; i++) {
        ScHit *h = &sc_hits[i];
        if (h->seen < scan_min_seen() || h->verdict >= 2) continue;
        uint64_t base = sc_base(p2, h);
        if (base < 0x100000000ull) continue;
        uint64_t a = base + h->off;
        if (!readable_span(a, 4 * (2 * h->stride + 1) + 16)) continue;
        volatile float *m = (volatile float *)(uintptr_t)a;
        if (h->verdict == 0) {
            for (int k = 0; k < 3; k++) h->pert[k] = m[k * h->stride] + (k == 0 ? (h->kind == 3 ? 0.3f : 0.01f) : 0);
            for (int k = 0; k < 3; k++) m[k * h->stride] = h->pert[k];
            h->verdict = 1;
            continue;
        }
        float v[3], want[3];
        for (int k = 0; k < 3; k++) { v[k] = m[k * h->stride]; want[k] = ax[h->kind][k] * h->sign; }
        int persisted = sc_close(v, h->pert, 1e-6f);
        int rebuilt = sc_close(v, want, h->kind == 3 ? 0.002f : 6e-4f);
        h->verdict = rebuilt && !persisted ? 2 : 3;
        rt_log("mousecam: scan: %s%s at p2 +0x%x,+0x%x,+0x%x stride %d: %s", h->sign < 0 ? "-" : "",
               h->kind == 0 ? "right" : h->kind == 1 ? "up" : h->kind == 2 ? "back" : "pos", h->o1 < 0 ? 0 : h->o1,
               h->o2 < 0 ? 0 : h->o2, h->off, h->stride,
               h->verdict == 2 ? "rebuilt by the game each frame: will be rewritten" : "KEPT by the game (state): left alone");
    }
}

static void sc_write(uint64_t p2, const ScHit *h, int off, const float v[3], const float pos[3], int with_tr) {
    uint64_t base = sc_base(p2, h);
    if (base < 0x100000000ull) return;
    uint64_t a = base + off;
    if (!readable_span(a, 4 * (2 * h->stride + 1) + 16)) return;
    volatile float *m = (volatile float *)(uintptr_t)a;
    for (int k = 0; k < 3; k++) m[k * h->stride] = v[k] * h->sign;
    if (with_tr) m[3] = -h->sign * (v[0] * pos[0] + v[1] * pos[1] + v[2] * pos[2]);
}

// SCAN=2: after our rewrite, put the new camera into every confirmed hit. The up axis is too close to 0,1,0 to be matched on its
// own, so it is written where a matrix puts it: between a right hit and the back hit that goes with it (rows: right +0x10 up
// +0x20 back; columns of a 3x4: right +4 up +8 back).
static void scan_apply(uint64_t p2, const float *cam) {
    if (scan_mode() != 2 || sc_stopped) return;
    sc_camp = (uint64_t)(uintptr_t)cam;
    float ax[4][3];
    if (!cam_axes(cam, ax)) return;
    for (int i = 0; i < sc_nhits; i++) {
        const ScHit *h = &sc_hits[i];
        if (h->verdict != 2 || !scan_group_on(h)) continue;
        sc_write(p2, h, h->off, ax[h->kind], ax[3], h->tr);
        if (h->kind != 0) continue;
        int step = h->stride == 1 ? 0x10 : 4;
        for (int j = 0; j < sc_nhits; j++) {
            const ScHit *b = &sc_hits[j];
            if (b->kind == 2 && b->verdict == 2 && scan_group_on(b) && b->o1 == h->o1 && b->o2 == h->o2 && b->stride == h->stride &&
                b->sign == h->sign && b->off == h->off + 2 * step) {
                sc_write(p2, h, h->off + step, ax[1], ax[3], h->tr && b->tr);
                break;
            }
        }
    }
}

// ---- YAW PROBE (TOTK_MOUSECAM_YAWPROBE=1): finds where the game keeps its own camera yaw / pitch, so the mod can turn the camera
// by writing those instead of rewriting the nine floats afterwards (which leaves Link a few pixels off-centre).
// Use it with F3 OFF, so the game's own camera is turning: the first press of F3 turns mouselook off. Every YAWPROBE_EVERY-th camera
// call (default 4) it appends one snapshot to build\mousecam_yaw.bin, up to YAWPROBE_MAX (default 300), taken right after the
// game's camera function ran. Spin the camera several full circles, and tilt it up and down, while it records; the log says
// how many snapshots are done. Mouselook itself keeps working normally once F3 is on again.
// File = records of { u64 tag, u64 addr, u64 src, u64 len } + len bytes, a snapshot starting with tag 5:
//   5 the nine camera floats (addr = their address)   1 param2 (0x1000 bytes)
//   3 object pointed to from param2's first 0x400 bytes, src = offset of the pointer (0x200 bytes)
//   4 object pointed to from inside a tag 3 object's first 0x100 bytes, src = (param2 offset << 16) | offset in that object (0x100 bytes)
//   2 memory around the camera floats (addr = camp - 0x400, 0x800 bytes)
// tools/yaw_find.py reads the file and lists the fields that follow the camera's yaw and pitch.
#define YAWPROBE_P4_LEN 0x100
#define YAWPROBE_MAX_P4 300

static int yawprobe_int(const char *name, int dflt, int lo, int hi) {
    const char *v = cfg_get(name);
    long x = v && *v ? strtol(v, NULL, 10) : dflt;
    return (int)(x < lo ? lo : x > hi ? hi : x);
}

static void yawprobe(uint64_t p2, uint64_t p3) {
    static int on = -1, every, maxs;
    static FILE *f;
    static unsigned long calls, snaps;
    if (on < 0) {
        const char *v = cfg_get("TOTK_MOUSECAM_YAWPROBE");
        on = v && *v && strcmp(v, "0") ? 1 : 0;
        every = yawprobe_int("TOTK_MOUSECAM_YAWPROBE_EVERY", 4, 1, 1000);
        maxs = yawprobe_int("TOTK_MOUSECAM_YAWPROBE_MAX", 300, 1, 5000);
        if (on) rt_log("mousecam: YAW PROBE: with mouselook OFF (F3), spin and tilt the camera; writing build\\mousecam_yaw.bin "
                       "(every %d calls, up to %d snapshots)", every, maxs);
    }
    if (!on || ml_enabled || (long)snaps >= maxs) return;   // only the game's own camera is of interest
    if (++calls % (unsigned long)every) return;
    if (!readable(p3, 8) || !readable_span(p2, PROBE_P2_LEN)) return;
    uint64_t camp = *(volatile uint64_t *)(uintptr_t)p3;
    if (!camp || !readable_span(camp, 36)) return;
    if (!f) {
        f = fopen("build\\mousecam_yaw.bin", "wb");
        if (!f) { on = 0; rt_log("mousecam: yaw probe: cannot write build\\mousecam_yaw.bin"); return; }
    }
    uint64_t seen[1 + PROBE_MAX_PTRS + YAWPROBE_MAX_P4];
    int nseen = 0, n3 = 0, n4 = 0;
    seen[nseen++] = p2;
    probe_rec(f, 5, camp, 0, 36);
    probe_rec(f, 1, p2, 0, PROBE_P2_LEN);
    for (uint64_t o = 0; o < PROBE_PTR_SCAN && n3 < PROBE_MAX_PTRS; o += 8) {
        uint64_t v = *(volatile uint64_t *)(uintptr_t)(p2 + o);
        if (v < 0x100000000ull || (v & 7) || !readable_span(v, PROBE_PTR_LEN)) continue;
        int dup = 0;
        for (int i = 0; i < nseen; i++) if (seen[i] == v) { dup = 1; break; }
        if (dup) continue;
        seen[nseen++] = v;
        probe_rec(f, 3, v, o, PROBE_PTR_LEN);
        n3++;
        for (uint64_t o2 = 0; o2 < YAWPROBE_P4_LEN && n4 < YAWPROBE_MAX_P4; o2 += 8) {
            uint64_t w = *(volatile uint64_t *)(uintptr_t)(v + o2);
            if (w < 0x100000000ull || (w & 7) || !readable_span(w, YAWPROBE_P4_LEN)) continue;
            int dup2 = 0;
            for (int i = 0; i < nseen; i++) if (seen[i] == w) { dup2 = 1; break; }
            if (dup2) continue;
            seen[nseen++] = w;
            probe_rec(f, 4, w, (o << 16) | o2, YAWPROBE_P4_LEN);
            n4++;
        }
    }
    if (camp > 0x400 && readable_span(camp - 0x400, 0x800)) probe_rec(f, 2, camp - 0x400, 0, 0x800);
    fflush(f);
    snaps++;
    if (snaps == 1 || snaps % 25 == 0) {
        const float *c = (const float *)(uintptr_t)camp;
        float dx = c[0] - c[3], dy = c[1] - c[4], dz = c[2] - c[5];
        rt_log("mousecam: yaw probe: %lu snapshots (%d + %d objects each); camera yaw %.1f pitch %.1f deg%s", snaps, n3, n4,
               atan2f(dx, dz) * 57.29578f, atan2f(dy, sqrtf(dx * dx + dz * dz)) * 57.29578f,
               (long)snaps >= maxs ? " - DONE, close the game" : "");
    }
}

// ---- DRIVE MODE (1.4.3, OFF by default, TOTK_MOUSECAM_DRIVE=1 turns it on; tested: the game ignores these fields): turn the camera through the game's own yaw / pitch.
// tools/yaw_find.py found them in the camera function's param2 object: pitch in degrees at +0x718 and yaw in degrees at +0x71c
// (the nine camera floats are inside the same object, at +0x6e0). Before the camera function runs, the mouse's turn is added to
// those two fields; the game then builds position / focus / up from them itself, with whatever it keeps in step with its own yaw
// (the sideways shift that put Link a few pixels off-centre when the floats were rewritten afterwards). Nothing is rewritten
// after the call except the zoom, which only moves the camera along the line to the focus.
// The probe could not tell whether those fields are what the game builds the camera from or something it computes from the
// camera afterwards, so this checks itself: it compares what it added to the yaw with how far the camera's yaw really moved. If
// the camera does not follow (ratio below 0.5 after 20-60 degrees of turning) it switches itself off for the run and the old
// rewrite takes over, and the log says so.
// TOTK_MOUSECAM_YAW_OFF / TOTK_MOUSECAM_PITCH_OFF (e.g. 0x71c): other offsets in param2.
#define DRIVE_YAW_OFF 0x71cull
#define DRIVE_PITCH_OFF 0x718ull

static int drive_frame;                 // this call: the mouse was taken in drive_pre
static float drive_scroll, drive_dt;
static int drive_reset;
static int drive_failed, drive_verified;
static float drive_ap, drive_pre_yaw;   // degrees added to the yaw this call; camera yaw before the call
static int drive_pre_ok;
static double drive_sum_ap_me, drive_sum_aa, drive_sum_abs;

static int drive_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = cfg_get("TOTK_MOUSECAM_DRIVE");
        on = v && *v && strcmp(v, "0") ? 1 : 0;   // off by default: the game ignores the yaw field (tested, ratio 0.00)
        if (rt_game_version < 143) on = 0;   // the offsets were found on 1.4.3 only
        rt_log("mousecam: drive mode (turn through the game's own yaw / pitch): %s", on ? "on" : "off");
    }
    return on;
}

static uint64_t drive_off(const char *name, uint64_t dflt) {
    const char *v = cfg_get(name);
    return v && *v ? strtoull(v, NULL, 0) : dflt;
}

static float wrap_deg(float d) {
    while (d > 180.0f) d -= 360.0f;
    while (d <= -180.0f) d += 360.0f;
    return d;
}

static float cam_yaw_deg(const float *c) { return atan2f(c[0] - c[3], c[2] - c[5]) * 57.29578f; }

// Before the original camera function: take the mouse and turn the game's yaw / pitch. Sets drive_frame when it did.
static void drive_pre(uint64_t p2, uint64_t p3) {
    drive_frame = 0;
    drive_pre_ok = 0;
    if (!ml_enabled || drive_failed || !drive_enabled()) return;
    static uint64_t yoff, poff, foff;
    if (!yoff) {
        yoff = drive_off("TOTK_MOUSECAM_YAW_OFF", DRIVE_YAW_OFF);
        poff = drive_off("TOTK_MOUSECAM_PITCH_OFF", DRIVE_PITCH_OFF);
        foff = drive_off("TOTK_MOUSECAM_FLAG_OFF", 0x464);
    }
    if (!readable_span(p2, 0x1000)) return;
    // Ultrahand and similar: the game drives the camera and the stick itself (see the step-aside below)
    const char *av = cfg_get("TOTK_MOUSECAM_AUTO");
    if (!(av && *av && !strcmp(av, "0")) && *(volatile uint32_t *)(uintptr_t)(p2 + foff)) return;

    float dx, dy, scroll;
    int reset;
    if (!input_mouse_look_take(&dx, &dy, &scroll, &reset)) return;   // cursor not captured: the game keeps its camera
    drive_frame = 1;
    drive_scroll = scroll;
    drive_reset = reset;
    uint64_t now = kern_now_ns();
    drive_dt = ml.last_ns ? clampf((float)(now - ml.last_ns) * 1e-9f, 0.001f, 0.1f) : 0.016f;
    ml.last_ns = now;
    ml.active = 0;
    ml.off_yaw = ml.off_pitch = ml.rate_yaw = ml.rate_pitch = 0;
    if (game_paused()) { drive_scroll = 0; drive_reset = 0; return; }   // deltas dropped; the stick path serves the menu
    ml_seen_ns = now;

    volatile float *yaw = (volatile float *)(uintptr_t)(p2 + yoff), *pitch = (volatile float *)(uintptr_t)(p2 + poff);
    float y0 = *yaw, p0 = *pitch;
    if (!isfinite(y0) || !isfinite(p0) || fabsf(y0) > 720.0f || fabsf(p0) > 360.0f) {
        static int warned;
        if (!warned++) rt_log("mousecam: drive: yaw %.2f / pitch %.2f at param2+0x%llx / +0x%llx look wrong, not driving", y0, p0,
                              (unsigned long long)yoff, (unsigned long long)poff);
        return;
    }
    const char *inv = cfg_get("TOTK_MOUSE_INVERT_Y");
    if (inv && *inv && strcmp(inv, "0")) dy = -dy;
    float rad = input_mouse_sens() * RAD_PER_COUNT;
    float yaw_d = -dx * rad * 57.29578f, pitch_d = dy * rad * 57.29578f;   // same directions as the rewrite in apply_rel
    if (dx != 0 || dy != 0) ml.last_move_ns = now;
    float lo = fminf(pitch_min() * 57.29578f, p0), hi = fmaxf(PITCH_LIMIT * 57.29578f, p0);   // never fights the game's own pitch
    float p1 = clampf(p0 + pitch_d, lo, hi);
    if (yaw_d != 0) *yaw = wrap_deg(y0 + yaw_d);
    if (p1 != p0) *pitch = p1;
    drive_ap = yaw_d;
    if (readable(p3, 8)) {
        uint64_t cp = *(volatile uint64_t *)(uintptr_t)p3;
        if (cp && readable_span(cp, 36)) { drive_pre_yaw = cam_yaw_deg((const float *)(uintptr_t)cp); drive_pre_ok = 1; }
    }
}

// After the original: did the camera follow the yaw we wrote? Disables drive mode when it did not.
static void drive_post(uint64_t p3) {
    if (!drive_frame || !drive_pre_ok || fabsf(drive_ap) < 0.02f || drive_verified) return;
    if (!readable(p3, 8)) return;
    uint64_t cp = *(volatile uint64_t *)(uintptr_t)p3;
    if (!cp || !readable_span(cp, 36)) return;
    float me = wrap_deg(cam_yaw_deg((const float *)(uintptr_t)cp) - drive_pre_yaw);
    drive_sum_ap_me += (double)drive_ap * me;
    drive_sum_aa += (double)drive_ap * drive_ap;
    drive_sum_abs += fabsf(drive_ap);
    static int logged;
    if (logged < 12) { logged++; rt_log("mousecam: drive: added %.3f deg to the yaw, the camera's yaw moved %.3f deg", drive_ap, me); }
    if (drive_sum_abs < 20.0 || drive_sum_aa < 1e-6) return;
    double ratio = drive_sum_ap_me / drive_sum_aa;
    if (ratio >= 0.5) {
        drive_verified = 1;
        rt_log("mousecam: drive mode works: the camera followed the yaw field (ratio %.2f over %.0f deg)", ratio, drive_sum_abs);
    } else if (ratio < 0.2 || drive_sum_abs >= 60.0) {
        drive_failed = 1;
        rt_log("mousecam: drive mode does NOT turn the camera (ratio %.2f over %.0f deg): the fields are computed from the camera, "
               "not the other way round. Going back to rewriting the camera floats.", ratio, drive_sum_abs);
    }
}

// ---- keep the game's own camera state out of the rewrite. The game's follow camera eases from the camera it left LAST frame towards
// where Link is now. Rewriting pos / focus / up in place meant it eased from the mod's rotated camera, so when Link moved while the
// mouse had turned the view, the camera chased the wrong point. So: remember what the game left (raw) and what this function leaves
// (out); before the next call, if the nine floats still hold what we left, put the game's own back. TOTK_MOUSECAM_RESTORE=0 turns it off.
static float rs_raw[9], rs_out[9];
static uint64_t rs_cp;
static int rs_raw_ok, rs_out_ok;

static int restore_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = cfg_get("TOTK_MOUSECAM_RESTORE");
        on = v && *v && !strcmp(v, "0") ? 0 : 1;
        rt_log("mousecam: give the game its own camera back before each update: %s", on ? "on" : "off");
    }
    return on;
}

static void restore_pre(uint64_t p3) {
    if (!restore_on() || !rs_raw_ok || !rs_out_ok) return;
    rs_raw_ok = rs_out_ok = 0;
    if (!readable(p3, 8)) return;
    uint64_t cp = *(volatile uint64_t *)(uintptr_t)p3;
    if (!cp || cp != rs_cp || !readable_span(cp, 36)) return;
    if (memcmp((const void *)(uintptr_t)cp, rs_out, sizeof rs_out) != 0) return;   // someone else changed it: leave it
    memcpy((void *)(uintptr_t)cp, rs_raw, sizeof rs_raw);
}

static void restore_raw(uint64_t p3) {   // right after the game's own camera function
    rs_raw_ok = 0;
    if (!readable(p3, 8)) return;
    uint64_t cp = *(volatile uint64_t *)(uintptr_t)p3;
    if (!cp || !readable_span(cp, 36)) return;
    memcpy(rs_raw, (const void *)(uintptr_t)cp, sizeof rs_raw);
    rs_cp = cp;
    rs_raw_ok = 1;
}

static void restore_out(uint64_t p3) {   // after the mod is done with the camera
    rs_out_ok = 0;
    if (!rs_raw_ok || !readable(p3, 8)) return;
    uint64_t cp = *(volatile uint64_t *)(uintptr_t)p3;
    if (cp != rs_cp || !readable_span(cp, 36)) return;
    memcpy(rs_out, (const void *)(uintptr_t)cp, sizeof rs_out);
    rs_out_ok = 1;
}

// x0 = param2, x1 = param3 (a pointer to a pointer to the nine floats), v0 = deltaTime. Registered at HOST_CAM; the patched
// `bl` sites land here with the guest's registers.
static void host_camera_body(Cpu *c) {
    uint64_t p2 = c->x[0], p3 = c->x[1];
    if (!g_fn) {
        uint64_t off = version_off(CAM_OFF_V121, CAM_OFF_V143);
        g_fn = off ? ldr_module(0)->base + off : 0;
        if (!g_fn) rt_fatal("mousecam: no camera function offset for game version %d (runtime/mousecam.c)", rt_game_version);
    }
    restore_pre(p3);
    drive_pre(p2, p3);
    rt_call(c, g_fn, 2, p2, p3);   // the original; v0 (deltaTime) is untouched by the call setup
    drive_post(p3);
    restore_raw(p3);
    center_raw(p3);
    yawprobe(p2, p3);
    { static uint64_t scn; scan_raw(p2, p3, ++scn); }
    scan_verify(p2, p3);

    // TOTK_MOUSECAM_PROBE=1: snapshot on every F3 press, and leave the camera and the mouse to the game
    static int probe = -1;
    if (probe < 0) {
        const char *pv = cfg_get("TOTK_MOUSECAM_PROBE");
        probe = pv && *pv && strcmp(pv, "0") ? 1 : 0;
        if (probe) rt_log("mousecam: PROBE MODE: press F3 to write a snapshot; mouselook is disabled");
    }
    if (probe) {
        if (ml_probe_req) {
            ml_probe_req = 0;
            uint64_t cp = readable(p3, 8) ? *(volatile uint64_t *)(uintptr_t)p3 : 0;
            probe_dump(p2, cp);
        }
        return;
    }

    // TOTK_MOUSECAM_DEBUG=1: once a second, what the camera function was called with and what it left in the camera
    static int dbg = -1;
    if (dbg < 0) {
        const char *dv = cfg_get("TOTK_MOUSECAM_DEBUG");
        dbg = dv && *dv && strcmp(dv, "0") ? 1 : 0;
    }
    if (dbg == 1) {
        static uint64_t dbg_n, dbg_ns;
        uint64_t t = kern_now_ns();
        dbg_n++;
        if (t - dbg_ns > 1000000000ull) {
            dbg_ns = t;
            uint64_t cp = 0;
            float f[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
            if (readable(p3, 8)) cp = *(volatile uint64_t *)(uintptr_t)p3;
            if (cp && readable(cp, 36)) memcpy(f, (const void *)(uintptr_t)cp, sizeof f);
            rt_log("mousecam: dbg n=%llu p2=%llx p3=%llx cam=%llx pos=%d,%d,%d foc=%d,%d,%d up100=%d,%d,%d active=%d stickoff=%d",
                   (unsigned long long)dbg_n, (unsigned long long)p2, (unsigned long long)p3, (unsigned long long)cp,
                   ix(f[0]), ix(f[1]), ix(f[2]), ix(f[3]), ix(f[4]), ix(f[5]), ix(f[6] * 100), ix(f[7] * 100),
                   ix(f[8] * 100), ml.active, mousecam_suppress_stick());
        }
    }

    if (!ml_enabled) {   // F3 off: the game's camera, with no leftover mouse offset to jump back to
        ml.active = 0;
        ml.off_yaw = ml.off_pitch = ml.rate_yaw = ml.rate_pitch = 0;
        return;
    }
    if (!readable(p3, 8)) return;
    uint64_t camp = *(volatile uint64_t *)(uintptr_t)p3;
    if (!readable(camp, 36)) return;

    if (drive_frame) {   // the mouse already turned the game's own yaw / pitch before the call; only the zoom is left to do
        if (!game_paused()) {
            ml.off_yaw = ml.off_pitch = 0;
            apply_rel((float *)(uintptr_t)camp, 0, 0, drive_scroll, drive_reset, drive_dt);
        }
        return;
    }

    // Step aside while the game drives the camera and stick itself (Ultrahand holding an object). The probe on 1.4.3 found a
    // word in the camera function's param2 object, at +0x464 (+0x488 mirrors it), that is 0 in normal play, 30 while Ultrahand
    // holds an object and 0 again after it is put down. While it is non-zero the camera and the mouse are left to the game
    // (the old mouse-to-stick path, as with F3 off); mouselook resumes as soon as it is 0 again. Every change of the word is
    // logged, so other game states that set it show up in the log.
    // TOTK_MOUSECAM_AUTO=0 turns this off. TOTK_MOUSECAM_FLAG_OFF=<offset, e.g. 0x488> picks another word.
    {
        static int auto_on = -1;
        static uint64_t flag_off;
        static uint32_t last_v = 0xffffffffu;
        if (auto_on < 0) {
            const char *av = cfg_get("TOTK_MOUSECAM_AUTO");
            auto_on = av && *av && !strcmp(av, "0") ? 0 : 1;
            if (rt_game_version < 143) auto_on = 0;   // the offset was found on 1.4.3 only
            const char *fo = cfg_get("TOTK_MOUSECAM_FLAG_OFF");
            flag_off = fo && *fo ? strtoull(fo, NULL, 0) : 0x464;
            rt_log("mousecam: step aside while param2+0x%llx is non-zero: %s", (unsigned long long)flag_off,
                   auto_on ? "on" : "off");
        }
        if (auto_on && readable(p2 + flag_off, 4)) {
            uint32_t v = *(volatile uint32_t *)(uintptr_t)(p2 + flag_off);
            if (v != last_v) {
                rt_log("mousecam: param2+0x%llx is now %u: %s", (unsigned long long)flag_off, (unsigned)v,
                       v ? "leaving camera and mouse to the game" : "mouselook resumes");
                last_v = v;
            }
            if (v) {
                float tx, ty, ts;
                int tr;
                input_mouse_look_take(&tx, &ty, &ts, &tr);   // drop the deltas
                ml.active = 0;
                ml.off_yaw = ml.off_pitch = ml.rate_yaw = ml.rate_pitch = 0;
                ml_seen_ns = 0;                              // the mouse feeds the stick again
                return;
            }
        }
    }

    float dx, dy, scroll;
    int reset;
    if (!input_mouse_look_take(&dx, &dy, &scroll, &reset)) {   // cursor not captured: the game keeps its camera
        ml.active = 0;
        ml.off_yaw = ml.off_pitch = ml.rate_yaw = ml.rate_pitch = 0;
        return;
    }
    if (game_paused()) return;   // deltas dropped; the stick path serves the menu

    uint64_t now = kern_now_ns();
    float dt = ml.last_ns ? clampf((float)(now - ml.last_ns) * 1e-9f, 0.001f, 0.1f) : 0.016f;
    ml.last_ns = now;
    ml_seen_ns = now;

    ml_have_field = 0;
    if (rt_game_version >= 143 && readable(p2 + DRIVE_YAW_OFF, 4)) {
        float fy = *(volatile float *)(uintptr_t)(p2 + DRIVE_YAW_OFF);
        if (isfinite(fy) && fabsf(fy) <= 360.0f) { ml_field_yaw = fy; ml_have_field = 1; }
    }
    if (mode_relative()) {
        apply_rel((float *)(uintptr_t)camp, dx, dy, scroll, reset, dt);
        sync_p2(p2, (const float *)(uintptr_t)camp);
        scan_apply(p2, (const float *)(uintptr_t)camp);
        return;
    }
    int32_t rs[2];
    input_stick_r_pad(rs);
    float stick[2] = {rs[0] / 32767.0f, rs[1] / 32767.0f};
    apply((float *)(uintptr_t)camp, dx, dy, scroll, reset, stick, dt);
    sync_p2(p2, (const float *)(uintptr_t)camp);
    scan_apply(p2, (const float *)(uintptr_t)camp);
}

static void host_camera(Cpu *c) {
    uint64_t p3 = c->x[1];   // x1 is not touched by the body's rt_call setup on our side; read it first anyway
    host_camera_body(c);
    restore_out(p3);
    if (center_on()) center_log(p3);
}

__attribute__((constructor)) static void mousecam_register(void) { rt_register_host(HOST_CAM, host_camera); }

#endif   // RT_PATCH_PAGE
