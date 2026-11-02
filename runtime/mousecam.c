// Mouselook camera: rewrite the nine floats of the camera update (position, focus, up)
// from yaw / pitch / radius driven by the mouse, instead of the game's right stick.
// The camera-maths idea (yaw / pitch / radius from a host function) follows
// mcrib884/totk-mousecam (https://github.com/mcrib884/totk-mousecam, GPL-2.0); this file
// is written for the recomp's host functions and does not contain that mod's code.
//
// The patch set "mousecam" (recomp/patches-v143.txt) sends every `bl` of the camera update
// function (main+0x71986c on 1.4.3) to host_camera below, which calls the guest function
// (rt_call: v0 still holds deltaTime) and rewrites the nine floats (position, focus, up)
// from yaw / pitch / radius. Guest addresses are host addresses, so the nine floats are
// plain pointers.
//
// TOTK_MOUSECAM_MODE=absolute keeps the mouse's owned yaw / pitch on top of the camera
// (default relative). TOTK_MOUSECAM_PROBE=1 / SCAN=1|2 / YAWPROBE=1 / DRIVE=1 are research
// tools; they are off unless set and log to the runtime log / build\.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "mousecam.h"
#include "platform/platform.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

// ---- per-version addresses: offsets from main's base. 0 = unknown, feature off for that version.
// CAM_OFF_*: entry of the camera update function. Must be a function ENTRY with `bl` callers.
// PAUSE_OFF_*: the pause manager singleton pointer, flags word at +0x30. Unverified on 1.4.3 (0).
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
#define ZOOM_EASE_PER_60HZ 0.15f
#define STICK_DEAD 0.1f
#define STICK_RAD_PER_S 3.6f
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
    if (a < 0x1000 || a + n > RT_ADDRESS_LIMIT) return 0;
    return plat_mem_readable(a, n);
}

static int readable_span(uint64_t a, uint64_t n) {
    for (uint64_t o = 0; o < n; o += 0x1000)
        if (!readable(a + o, 1)) return 0;
    return readable(a + n - 1, 1);
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

// How long the mouse may rest before the camera goes back to the game.
// TOTK_MOUSECAM_HOLD_MS: milliseconds, default 350; 0 = never hand back.
static uint64_t idle_ns(void) {
    static int64_t ms = -1;
    if (ms < 0) {
        const char *v = cfg_get("TOTK_MOUSECAM_HOLD_MS");
        ms = v && *v ? (int64_t)strtoll(v, NULL, 10) : 350;
        if (ms < 0) ms = 0;
        rt_log("mousecam: hand-back after %lld ms of no mouse movement%s", (long long)ms, ms ? "" : " (never)");
    }
    return ms ? (uint64_t)ms * 1000000ull : ~0ull;
}

typedef struct {
    int active;
    float yaw, pitch;
    float zoom, zoom_target;
    float base_r;          // radius frozen while orbiting, so the orbit stays a perfect circle
    float off_yaw, off_pitch;
    float rate_yaw, rate_pitch;
    uint64_t last_move_ns, last_ns;
} Ml;
static Ml ml;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// cam: pos[3], focus[3], up[3] as the game left them. Absolute mode (owned yaw / pitch).
static void apply(float *cam, float dx, float dy, float scroll, int reset, const float stick[2], float dt) {
    float *pos = cam, *foc = cam + 3, *up = cam + 6;
    float vx = pos[0] - foc[0], vy = pos[1] - foc[1], vz = pos[2] - foc[2];
    float r = sqrtf(vx * vx + vy * vy + vz * vz);
    if (!isfinite(r) || r < 0.1f) return;

    uint64_t now = kern_now_ns();

    const char *inv = cfg_get("TOTK_MOUSE_INVERT_Y");
    if (inv && *inv && strcmp(inv, "0")) dy = -dy;
    float rad = input_mouse_sens() * RAD_PER_COUNT;

    float sx = fabsf(stick[0]) > STICK_DEAD ? stick[0] : 0, sy = fabsf(stick[1]) > STICK_DEAD ? stick[1] : 0;
    float yaw_d = -dx * rad - sx * STICK_RAD_PER_S * dt;
    float pitch_d = dy * rad + sy * STICK_RAD_PER_S * dt;
    int moving = dx != 0 || dy != 0 || sx != 0 || sy != 0;

    if (moving) {
        ml.last_move_ns = now;
        if (!ml.active) {   // activating: freeze the radius so the orbit stays a circle
            ml.yaw = atan2f(vx, vz);
            ml.pitch = asinf(clampf(vy / r, -1, 1));
            ml.base_r = r;
            ml.active = 1;
        }
    } else if (ml.active && now - ml.last_move_ns > idle_ns()) {
        ml.active = 0;
    }
    if (!ml.active) ml.base_r = r;   // inactive: track the game's radius for zoom

    if (reset) ml.zoom_target = 0;
    if (fabsf(scroll) > 0.01f) ml.zoom_target -= scroll * ZOOM_STEP;
    ml.zoom_target = clampf(ml.zoom_target, MIN_RADIUS - r, MAX_RADIUS - r);
    float k = 1.0f - powf(1.0f - ZOOM_EASE_PER_60HZ, dt * 60.0f);
    float diff = ml.zoom_target - ml.zoom;
    ml.zoom = fabsf(diff) < 0.01f ? ml.zoom_target : ml.zoom + diff * k;
    float base = ml.active ? ml.base_r : r;
    float R = clampf(base + ml.zoom, MIN_RADIUS, MAX_RADIUS);
    if (!isfinite(R)) { ml.zoom = ml.zoom_target = 0; R = r; }

    if (ml.active) {
        ml.yaw += yaw_d;
        ml.pitch = clampf(ml.pitch + pitch_d, -PITCH_LIMIT, PITCH_LIMIT);
        float h = R * cosf(ml.pitch);
        pos[0] = foc[0] + h * sinf(ml.yaw);
        pos[1] = foc[1] + R * sinf(ml.pitch);
        pos[2] = foc[2] + h * cosf(ml.yaw);
        up[0] = 0; up[1] = 1; up[2] = 0;
    } else if (fabsf(R - r) > 1e-4f) {
        float s = R / r;
        pos[0] = foc[0] + vx * s;
        pos[1] = foc[1] + vy * s;
        pos[2] = foc[2] + vz * s;
    }
}

// ---- relative mode (default): the mouse rotates the game's own camera.
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

// ---- over-the-shoulder pivot. TOTK_MOUSECAM_SHIFT_R / TOTK_MOUSECAM_SHIFT_B.
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

// The game's own yaw field (1.4.3: degrees at param2+0x71c). Used when the camera looks steeply down.
#define DRIVE_YAW_OFF 0x71cull
#define DRIVE_PITCH_OFF 0x718ull

static float ml_field_yaw;
static int ml_have_field, ml_have_phi;
static float ml_phi;

// ---- Link's own position (TOTK_MOUSECAM_LINK=0 turns the code below off).
// 1.2.1: [main+0x4728538]  1.4.3: [main+0x3add238]  +0x1e8 scene (+0x50 count, +0x58 component list),
// component 13 = the resident actors (+0x20 count, +0x28 links, 0x70 apart, +8 data, data +0x40 actor);
// the one named "Player" (actor+0x218) has its position at +0x2b4.
static uint64_t lk_actor;
static unsigned lk_wait;
static float lk_w;

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

static int hold_yaw_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = cfg_get("TOTK_MOUSECAM_HOLD_YAW");
        on = v && *v && !strcmp(v, "0") ? 0 : 1;
        rt_log("mousecam: hold the view steady while running: %s", on ? "on" : "off");
    }
    return on;
}

static int hold_pitch_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = cfg_get("TOTK_MOUSECAM_HOLD_PITCH");
        on = v && *v && !strcmp(v, "0") ? 0 : 1;
        rt_log("mousecam: hold the pitch steady while running: %s", on ? "on" : "off");
    }
    return on;
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
        if (lk_wait) { lk_wait--; return 0; }
        lk_actor = lk_find();
        if (!lk_actor) { lk_wait = 30; return 0; }
        rt_log("mousecam: found the player actor at %llx", (unsigned long long)lk_actor);
    }
    if (!readable(lk_actor + 0x2b4, 12)) return 0;
    memcpy(xyz, (const void *)(uintptr_t)(lk_actor + 0x2b4), 12);
    return isfinite(xyz[0]) && isfinite(xyz[1]) && isfinite(xyz[2]) && fabsf(xyz[0]) + fabsf(xyz[2]) > 1.0f;
}

// Keep Link framed as the game would: shift the turned camera so his lateral / depth match.
static void link_fix(const float A[2], const float foc_raw[3], float yawg, float w, float sr, float sb, float *pos, float *foc) {
    float rrx = cosf(yawg), rrz = -sinf(yawg), brx = sinf(yawg), brz = cosf(yawg);
    float ax = A[0] - foc_raw[0], az = A[1] - foc_raw[2];
    float lat0 = ax * rrx + az * rrz, dep0 = ax * brx + az * brz;

    float px = foc_raw[0] - sr * rrx - sb * brx, pz = foc_raw[2] - sr * rrz - sb * brz;
    float bx = pos[0] - foc[0], bz = pos[2] - foc[2], bh = hypotf(bx, bz);
    if (bh < 1e-3f) return;
    bx /= bh; bz /= bh;
    float rx = bz, rz = -bx;

    float lat_t = lat0 + w * (-sr - lat0);
    float dep_t = dep0 + w * (((A[0] - px) * bx + (A[1] - pz) * bz - sb) - dep0);
    float cx = A[0] - foc[0], cz = A[1] - foc[2];
    float dl = (cx * rx + cz * rz) - lat_t, dd = (cx * bx + cz * bz) - dep_t;
    float mx = dl * rx + dd * bx, mz = dl * rz + dd * bz;
    foc[0] += mx; foc[2] += mz;
    pos[0] += mx; pos[2] += mz;
}

static void apply_rel(float *cam, float dx, float dy, float scroll, int reset, float dt) {
    float *pos = cam, *foc = cam + 3, *up = cam + 6;
    float vx = pos[0] - foc[0], vy = pos[1] - foc[1], vz = pos[2] - foc[2];
    float r = sqrtf(vx * vx + vy * vy + vz * vz);
    if (!isfinite(r) || r < 0.1f) return;
    if (!ml.active) ml.base_r = r;   // inactive: track game radius (capture on activation)
    uint64_t now = kern_now_ns();

    // step of the game's raw focus per camera update (smoothed over ~4 updates)
    static uint64_t vt_ns;
    static float vlast[2], vvel[2];
    float lead_vx, lead_vz;
    {
        float fdt = (float)(now - vt_ns) * 1e-9f;
        if (!vt_ns || fdt >= 0.1f) {
            vvel[0] = vvel[1] = 0; vlast[0] = foc[0]; vlast[1] = foc[2]; vt_ns = now;
        } else if (fdt > 0.002f) {
            float sx = foc[0] - vlast[0], sz = foc[2] - vlast[1];
            if (hypotf(sx, sz) < 1.0f) {
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

    // gamepad / arrow-key stick input is left to the game's own camera here
    float yaw_d = -dx * rad, pitch_d = dy * rad;
    int moved = dx != 0 || dy != 0;
    float glide = glide_s();
    if (glide > 0) {
        if (moved) {
            float a = 1.0f - expf(-dt / 0.03f);
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
        float k = expf(-dt * 4.0f);
        ml.off_yaw *= k;
        ml.off_pitch *= k;
    }

    if (reset) ml.zoom_target = 0;
    if (fabsf(scroll) > 0.01f) ml.zoom_target -= scroll * ZOOM_STEP;
    ml.zoom_target = clampf(ml.zoom_target, MIN_RADIUS - r, MAX_RADIUS - r);
    float kz = 1.0f - powf(1.0f - ZOOM_EASE_PER_60HZ, dt * 60.0f);
    float diff = ml.zoom_target - ml.zoom;
    ml.zoom = fabsf(diff) < 0.01f ? ml.zoom_target : ml.zoom + diff * kz;
    float base = (fabsf(ml.off_yaw) > 1e-4f || fabsf(ml.off_pitch) > 1e-4f) ? ml.base_r : r;
    float R = clampf(base + ml.zoom, MIN_RADIUS, MAX_RADIUS);
    if (!isfinite(R)) { ml.zoom = ml.zoom_target = 0; R = r; }

    float foc_raw[3] = {foc[0], foc[1], foc[2]};
    float yaw0 = atan2f(vx, vz), pitch0 = asinf(clampf(vy / r, -1, 1));
    float hz = hypotf(vx, vz);
    float yawg = yaw0;
    if (ml_have_field) {
        float fy = ml_field_yaw * 0.017453293f;
        if (hz > 3.0f) {
            float ph = wrap_rad(yaw0 - fy);
            if (!ml_have_phi) { ml_phi = ph; ml_have_phi = 1; }
            else ml_phi = wrap_rad(ml_phi + 0.02f * wrap_rad(ph - ml_phi));
        }
        if (ml_have_phi && hz < 1.0f) yawg = wrap_rad(fy + ml_phi);
    }

    // hold: take the game's own follow-turn off the offset so the displayed yaw stays put
    {
        static float hold_prev, hold_sum, hold_pprev;
        static uint64_t hold_t, hold_log_t;
        static int hold_have;
        int hold_ok = !reset && hold_have && now - hold_t < 100000000ull;
        if (hold_pitch_on() && hold_ok && fabsf(pitch0 - hold_pprev) < 0.12f)
            ml.off_pitch -= pitch0 - hold_pprev;
        hold_pprev = pitch0;
        if (hold_yaw_on() && hold_ok && hz > 1.0f) {
            float d = wrap_rad(yawg - hold_prev);
            if (fabsf(d) < 0.12f) {
                ml.off_yaw = wrap_rad(ml.off_yaw - d);
                hold_sum += d;
            }
        }
        {
            static int dbg = -1;
            if (dbg < 0) {
                const char *dv = cfg_get("TOTK_MOUSECAM_DEBUG");
                dbg = dv && *dv && strcmp(dv, "0") ? 1 : 0;
            }
            if (dbg && now - hold_log_t > 1000000000ull) {
                if (hold_log_t && fabsf(hold_sum) > 1e-4f)
                    rt_log("mousecam: the game's own camera turned %.2f deg in the last second (held: %s), mouse offset now %.1f deg",
                           hold_sum * 57.29578f, hold_yaw_on() ? "yes" : "no", ml.off_yaw * 57.29578f);
                hold_log_t = now;
                hold_sum = 0;
            }
        }
        hold_prev = yawg;
        hold_t = now;
        hold_have = hz > 1.0f;
    }

    float lo = fminf(pitch_min(), pitch0), hi = fmaxf(PITCH_LIMIT, pitch0);
    float pitch = clampf(pitch0 + ml.off_pitch, lo, hi);
    ml.off_pitch = pitch - pitch0;

    ml.active = fabsf(ml.off_yaw) > 1e-4f || fabsf(ml.off_pitch) > 1e-4f;
    if (ml.active) {
        float sr = shift_r(), sb = shift_b();
        float cx = foc[0] - sr * cosf(yawg) - sb * sinf(yawg);
        float cz = foc[2] + sr * sinf(yawg) - sb * cosf(yawg);

        cx += clampf(lead_calls() * lead_vx, -1.0f, 1.0f);
        cz += clampf(lead_calls() * lead_vz, -1.0f, 1.0f);

        float lk[3], A[2] = {0, 0};
        int have_lk = link_pos(lk) && hypotf(lk[0] - foc[0], lk[2] - foc[2]) < 3.0f;
        if (have_lk) {
            A[0] = lk[0]; A[1] = lk[2];
            cx = A[0]; cz = A[1];
        }

        float ca = cosf(ml.off_yaw), sa = sinf(ml.off_yaw);
        float fxr = foc[0] - cx, fzr = foc[2] - cz;
        float fx1 = cx + fxr * ca + fzr * sa, fz1 = cz - fxr * sa + fzr * ca;
        float pxr = pos[0] - cx, pzr = pos[2] - cz;
        float px1 = cx + pxr * ca + pzr * sa, pz1 = cz - pxr * sa + pzr * ca;

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
            float fl = lk[1] + floor_margin(), s2 = clampf((fl - foc[1]) / R, -1.0f, 1.0f);
            float hh = R * sqrtf(fmaxf(0.0f, 1.0f - s2 * s2));
            pos[0] = fx1 + hh * ux / uh;
            pos[1] = foc[1] + R * s2;
            pos[2] = fz1 + hh * uz / uh;
            ml.off_pitch = asinf(s2) - pitch0;
        }
        lk_w = 0;
        if (have_lk) {
            lk_w = clampf(fabsf(ml.off_yaw) / 0.5f, 0.0f, 1.0f);
            link_fix(A, foc_raw, yawg, lk_w, sr, sb, pos, foc);
        }
    } else if (fabsf(R - r) > 1e-4f) {
        float s = R / r;
        pos[0] = foc[0] + vx * s;
        pos[1] = foc[1] + vy * s;
        pos[2] = foc[2] + vz * s;
    }
}

// ---- DRIVE MODE (1.4.3, OFF by default, TOTK_MOUSECAM_DRIVE=1): turn through the game's own
// yaw / pitch at param2+0x71c / +0x718. Self-disables when the game ignores the fields.
static int drive_frame;
static float drive_scroll, drive_dt;
static int drive_reset;
static int drive_failed, drive_verified;
static float drive_ap, drive_pre_yaw;
static int drive_pre_ok;
static double drive_sum_ap_me, drive_sum_aa, drive_sum_abs;

static int drive_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *v = cfg_get("TOTK_MOUSECAM_DRIVE");
        on = v && *v && strcmp(v, "0") ? 1 : 0;
        if (rt_game_version < 143) on = 0;
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
    const char *av = cfg_get("TOTK_MOUSECAM_AUTO");
    if (!(av && *av && !strcmp(av, "0")) && *(volatile uint32_t *)(uintptr_t)(p2 + foff)) return;

    float dx, dy, scroll;
    int reset;
    if (!input_mouse_look_take(&dx, &dy, &scroll, &reset)) return;
    drive_frame = 1;
    drive_scroll = scroll;
    drive_reset = reset;

    uint64_t now = kern_now_ns();
    drive_dt = ml.last_ns ? clampf((float)(now - ml.last_ns) * 1e-9f, 0.001f, 0.1f) : 0.016f;
    ml.last_ns = now;
    ml.active = 0;
    ml.off_yaw = ml.off_pitch = ml.rate_yaw = ml.rate_pitch = 0;
    if (game_paused()) { drive_scroll = 0; drive_reset = 0; return; }
    ml_seen_ns = now;

    volatile float *yaw = (volatile float *)(uintptr_t)(p2 + yoff), *pitch = (volatile float *)(uintptr_t)(p2 + poff);
    float y0 = *yaw, p0 = *pitch;
    if (!isfinite(y0) || !isfinite(p0) || fabsf(y0) > 720.0f || fabsf(p0) > 360.0f) {
        static int warned;
        if (!warned++) rt_log("mousecam: drive: yaw %.2f / pitch %.2f at p2+0x%llx / +0x%llx look wrong, not driving",
                              y0, p0, (unsigned long long)yoff, (unsigned long long)poff);
        return;
    }
    const char *inv = cfg_get("TOTK_MOUSE_INVERT_Y");
    if (inv && *inv && strcmp(inv, "0")) dy = -dy;
    float rad = input_mouse_sens() * RAD_PER_COUNT;
    float yaw_d = -dx * rad * 57.29578f, pitch_d = dy * rad * 57.29578f;
    if (dx != 0 || dy != 0) ml.last_move_ns = now;
    float lo = fminf(pitch_min() * 57.29578f, p0), hi = fmaxf(PITCH_LIMIT * 57.29578f, p0);
    float p1 = clampf(p0 + pitch_d, lo, hi);
    if (yaw_d != 0) *yaw = wrap_deg(y0 + yaw_d);
    if (p1 != p0) *pitch = p1;
    drive_ap = yaw_d;
    if (readable(p3, 8)) {
        uint64_t cp = *(volatile uint64_t *)(uintptr_t)p3;
        if (cp && readable_span(cp, 36)) { drive_pre_yaw = cam_yaw_deg((const float *)(uintptr_t)cp); drive_pre_ok = 1; }
    }
}

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
        rt_log("mousecam: drive mode does NOT turn the camera (ratio %.2f over %.0f deg): going back to rewriting the camera floats",
               ratio, drive_sum_abs);
    }
}

// ---- keep the game's own camera state out of the rewrite (follow-camera chase).
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
    if (memcmp((const void *)(uintptr_t)cp, rs_out, sizeof rs_out) != 0) return;
    memcpy((void *)(uintptr_t)cp, rs_raw, sizeof rs_raw);
}

static void restore_raw(uint64_t p3) {
    rs_raw_ok = 0;
    if (!readable(p3, 8)) return;
    uint64_t cp = *(volatile uint64_t *)(uintptr_t)p3;
    if (!cp || !readable_span(cp, 36)) return;
    memcpy(rs_raw, (const void *)(uintptr_t)cp, sizeof rs_raw);
    rs_cp = cp;
    rs_raw_ok = 1;
}

static void restore_out(uint64_t p3) {
    rs_out_ok = 0;
    if (!rs_raw_ok || !readable(p3, 8)) return;
    uint64_t cp = *(volatile uint64_t *)(uintptr_t)p3;
    if (cp != rs_cp || !readable_span(cp, 36)) return;
    memcpy(rs_out, (const void *)(uintptr_t)cp, sizeof rs_out);
    rs_out_ok = 1;
}

// x0 = param2, x1 = param3 (pointer to pointer to nine floats), v0 = deltaTime.
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

    // TOTK_MOUSECAM_PROBE=1: snapshot on every F3 press; mouselook disabled for the run.
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
            rt_log("mousecam: probe p2=%llx cam=%llx", (unsigned long long)p2, (unsigned long long)cp);
        }
        return;
    }

    if (!ml_enabled) {
        ml.active = 0;
        ml.off_yaw = ml.off_pitch = ml.rate_yaw = ml.rate_pitch = 0;
        return;
    }
    if (!readable(p3, 8)) return;
    uint64_t camp = *(volatile uint64_t *)(uintptr_t)p3;
    if (!readable(camp, 36)) return;

    if (drive_frame) {
        if (!game_paused()) {
            ml.off_yaw = ml.off_pitch = 0;
            apply_rel((float *)(uintptr_t)camp, 0, 0, drive_scroll, drive_reset, drive_dt);
        }
        return;
    }

    // Step aside while the game drives the camera itself (Ultrahand, etc.): p2+0x464 non-zero.
    {
        static int auto_on = -1;
        static uint64_t flag_off;
        static uint32_t last_v = 0xffffffffu;
        if (auto_on < 0) {
            const char *av = cfg_get("TOTK_MOUSECAM_AUTO");
            auto_on = av && *av && !strcmp(av, "0") ? 0 : 1;
            if (rt_game_version < 143) auto_on = 0;
            const char *fo = cfg_get("TOTK_MOUSECAM_FLAG_OFF");
            flag_off = fo && *fo ? strtoull(fo, NULL, 0) : 0x464;
            rt_log("mousecam: step aside while p2+0x%llx is non-zero: %s", (unsigned long long)flag_off,
                   auto_on ? "on" : "off");
        }
        if (auto_on && readable(p2 + flag_off, 4)) {
            uint32_t v = *(volatile uint32_t *)(uintptr_t)(p2 + flag_off);
            if (v != last_v) {
                rt_log("mousecam: p2+0x%llx is now %u: %s", (unsigned long long)flag_off, (unsigned)v,
                       v ? "leaving camera and mouse to the game" : "mouselook resumes");
                last_v = v;
            }
            if (v) {
                float tx, ty, ts;
                int tr;
                input_mouse_look_take(&tx, &ty, &ts, &tr);   // drop the deltas
                ml.active = 0;
                ml.off_yaw = ml.off_pitch = ml.rate_yaw = ml.rate_pitch = 0;
                ml_seen_ns = 0;
                return;
            }
        }
    }

    float dx, dy, scroll;
    int reset;
    if (!input_mouse_look_take(&dx, &dy, &scroll, &reset)) {
        ml.active = 0;
        ml.off_yaw = ml.off_pitch = ml.rate_yaw = ml.rate_pitch = 0;
        return;
    }
    if (game_paused()) return;

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
        return;
    }
    int32_t rs[2];
    input_stick_r_pad(rs);
    float stick[2] = {rs[0] / 32767.0f, rs[1] / 32767.0f};
    apply((float *)(uintptr_t)camp, dx, dy, scroll, reset, stick, dt);
}

static void host_camera(Cpu *c) {
    uint64_t p3 = c->x[1];
    host_camera_body(c);
    restore_out(p3);
}

__attribute__((constructor)) static void mousecam_register(void) { rt_register_host(HOST_CAM, host_camera); }

#endif   // RT_PATCH_PAGE
