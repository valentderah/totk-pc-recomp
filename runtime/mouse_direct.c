// Direct mouse input for TotK 1.4.3. The camera controller's input update (main+0x874c1c) turns HID stick floats
// into a radial dead zone, clamps the radius to one and applies StickExponent. The mouse enters after all three,
// at main+0x874cb4, before the game's X/Y inversion and input-activity flags. It never becomes a HID stick value.
// Free orbit removes that sample again before the camera's additional vertical square/velocity filters and adds
// raw-count angular deltas at its spherical-angle output instead. Ultrahand has a separate post-curve hook.
// The ordinary stick's result stays, so a controller still uses the game's own response.
//
// This is deliberately not a render-camera override: the game remains responsible for aiming, camera collision,
// lock-on and scripted cameras. Which of its other modes consume this camera input needs in-game verification.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_wsys.h"
#include "platform/platform.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

static int direct_mode(void) {
    const char *mode = cfg_get("TOTK_MOUSE_MODE");
    return mode && !strcmp(mode, "direct") && rt_patch_enabled(rt_patch_set("mouse_direct"));
}

static uint64_t last_update;
static uint64_t hand_update;
static uint64_t shake_started, shake_until;
static int shake_injected;
static int accepts_mouse;

static int hand_active(uint64_t now) {
    uint64_t last = __atomic_load_n(&hand_update, __ATOMIC_ACQUIRE);
    return last && now - last < 150000000ull;
}

int input_direct_mouse_active(void) {
    uint64_t last = __atomic_load_n(&last_update, __ATOMIC_ACQUIRE);
    return direct_mode() && (__atomic_load_n(&accepts_mouse, __ATOMIC_RELAXED) || hand_active(plat_now_ns())) && last &&
           plat_now_ns() - last < 150000000ull;
}

int input_direct_mouse_enabled(void) { return direct_mode(); }

void input_direct_mouse_radial(int32_t dx, int32_t dy, float sensitivity, float cursor[2], int32_t out[2]) {
    if (!(sensitivity > 0)) { cursor[0] = cursor[1] = 0; out[0] = out[1] = 0; return; }
    cursor[0] += dx * (double)sensitivity / 200;
    cursor[1] -= dy * (double)sensitivity / 200;
    double length = hypot(cursor[0], cursor[1]);
    if (length > 1) { cursor[0] /= length; cursor[1] /= length; }
    out[0] = (int32_t)lrint(cursor[0] * 32767);
    out[1] = (int32_t)lrint(cursor[1] * 32767);
}

void input_direct_mouse_row(int32_t dx, float sensitivity, uint64_t now, InputMouseRow *row, int32_t out[2]) {
    const float step_counts = 32;
    out[0] = out[1] = 0;
    if (!(sensitivity > 0)) { *row = (InputMouseRow){0}; return; }
    if (dx) {
        // Reversing direction cancels any remainder from the previous drag. Bound a fast flick to one pending step;
        // a stopped mouse never starts another pulse, regardless of how often HID is polled.
        if ((dx < 0 && row->distance > 0) || (dx > 0 && row->distance < 0)) row->distance = 0;
        row->distance = fmaxf(-2 * step_counts, fminf(2 * step_counts, row->distance + dx * sensitivity));
    }
    if (row->direction) {
        if (now < row->pulse_until && (!dx || (dx > 0 ? 1 : -1) == row->direction)) {
            out[0] = row->direction * 32767;
            return;
        }
        row->direction = 0;
        row->neutral_until = now + 32000000ull;
        return;   // publish neutral even when a slow frame skipped the pulse's deadline
    }
    if (!dx || now < row->neutral_until || fabsf(row->distance) < step_counts) return;
    row->direction = row->distance > 0 ? 1 : -1;
    row->distance = fmodf(row->distance, step_counts);
    row->pulse_until = now + 32000000ull;
    out[0] = row->direction * 32767;
}

int input_direct_mouse_shake(int allow, int32_t out[2]) {
    uint64_t now = plat_now_ns(), until = __atomic_load_n(&shake_until, __ATOMIC_ACQUIRE);
    uint64_t start = __atomic_load_n(&shake_started, __ATOMIC_RELAXED);
    int ready = allow && direct_mode() && hand_active(now) && ikb_captured() && ikb_foreground() && input_cursor_allowed();
    if (!ready) __atomic_store_n(&shake_until, 0, __ATOMIC_RELEASE);
    int active = ready && now >= start && now < until;
    out[0] = active ? ((now - start) / 40000000ull & 1 ? -32767 : 32767) : 0;
    out[1] = 0;
    __atomic_store_n(&shake_injected, active, __ATOMIC_RELAXED);
    return active;
}

#ifdef RT_PATCH_PAGE
#define HOST_MOUSE_DIRECT (RT_PATCH_PAGE + 0xfd0)
#define HOST_MOUSE_ACTIVITY (RT_PATCH_PAGE + 0xfc0)
#define HOST_MOUSE_ORBIT_BEGIN (RT_PATCH_PAGE + 0xfb0)
#define HOST_MOUSE_ORBIT_ANGLES (RT_PATCH_PAGE + 0xfa0)
#define HOST_MOUSE_HAND (RT_PATCH_PAGE + 0xf90)
static _Thread_local uint64_t mouse_input_object;

typedef struct {
    uint64_t object, generation, when;
    float pad[2], motion[2], angle[2];   // post-inversion controller input, mouse input, mouse yaw/pitch in degrees
    int camera_enabled;
} MouseSample;
static MouseSample sample;
static PlatMutex sample_lock = PLAT_MUTEX_INIT;
static _Thread_local struct { uint64_t object; MouseSample mouse; } orbit;
typedef struct {
    uint64_t object, generation, input;
    float pitch, original_target;
    int hold_pitch, target_saved;
} OrbitTrack;
static OrbitTrack orbit_seen[16];

// sample_lock held. Camera objects can coexist while the game blends between their outputs.
static OrbitTrack *orbit_track(uint64_t object) {
    int oldest = 0;
    for (int i = 0; i < 16; i++) {
        if (orbit_seen[i].object == object) return &orbit_seen[i];
        if (orbit_seen[i].generation < orbit_seen[oldest].generation) oldest = i;
    }
    orbit_seen[oldest] = (OrbitTrack){.object = object};
    return &orbit_seen[oldest];
}

static int orbit_sample_once(uint64_t object, uint64_t generation) {
    plat_mutex_lock(&sample_lock);
    OrbitTrack *track = orbit_track(object);
    int fresh = track->generation != generation;
    if (fresh) track->generation = generation;
    plat_mutex_unlock(&sample_lock);
    return fresh;
}

static float guest_float(uint64_t at) { float f; memcpy(&f, (void *)(uintptr_t)at, sizeof f); return f; }
static void guest_float_store(uint64_t at, float f) { memcpy((void *)(uintptr_t)at, &f, sizeof f); }
static uint64_t guest_pointer(uint64_t at) { uint64_t p; memcpy(&p, (void *)(uintptr_t)at, sizeof p); return p; }
static MouseSample sample_read(void) {
    plat_mutex_lock(&sample_lock);
    MouseSample m = sample;
    plat_mutex_unlock(&sample_lock);
    return m;
}
static float wrap_degrees(float a) { return remainderf(a, 360.0f); }

// main+0x191d78 builds the pitch target from +0x15c and +0x11c; the orbit update filters +0x108 toward it and adds
// (1 - +0x88) * +0x10c at output. Updating only +0x108 makes the camera spring back to the unchanged target.
static float orbit_pitch_offset(uint64_t object) {
    return (1 - guest_float(object + 0x88)) * guest_float(object + 0x10c);
}

static void orbit_pitch_target(uint64_t object, float pitch) {
    guest_float_store(object + 0x15c, pitch - orbit_pitch_offset(object) - guest_float(object + 0x11c));
}

static void orbit_pitch_release(OrbitTrack *track) {
    if (track->target_saved) guest_float_store(track->object + 0x15c, track->original_target);
    track->hold_pitch = track->target_saved = 0;
}

static void mouse_shake_feed(int32_t dx, int32_t dy, uint64_t now) {
    static struct { int sign, confirmed, reversals; double distance; uint64_t last; } stroke[2];
    static uint64_t cooldown;
    if (!hand_active(now)) { memset(stroke, 0, sizeof stroke); return; }
    if (now < cooldown) return;
    int32_t delta[2] = {dx, dy};
    for (int i = 0; i < 2; i++) {
        if (!delta[i]) continue;
        if (stroke[i].last && now - stroke[i].last > 400000000ull) memset(&stroke[i], 0, sizeof stroke[i]);
        int sign = delta[i] > 0 ? 1 : -1;
        if (sign != stroke[i].sign) stroke[i].sign = sign, stroke[i].distance = 0;
        stroke[i].distance += fabs((double)delta[i]);
        if (stroke[i].distance < 24) continue;
        if (stroke[i].confirmed && sign != stroke[i].confirmed) stroke[i].reversals++;
        stroke[i].confirmed = sign;
        stroke[i].last = now;
        stroke[i].distance = 0;
        if (stroke[i].reversals >= 3) {
            __atomic_store_n(&shake_started, now, __ATOMIC_RELAXED);
            __atomic_store_n(&shake_until, now + 320000000ull, __ATOMIC_RELEASE);
            cooldown = now + 700000000ull;
            memset(stroke, 0, sizeof stroke);
            rt_log("mouse: detach shake gesture sent to the game's stick-shake detector");
            return;
        }
    }
}

// A host call at an instruction site, not an AAPCS64 function replacement. Preserve every guest register except
// s9/s10, which carry the processed camera input, and perform the displaced store of s9 at [x20 + 0x420].
static void camera_input(Cpu *c) {
    mouse_input_object = 0;
    uint32_t flags = (uint32_t)c->x[21];
    int accept = !(flags & 1) && !input_direct_mouse_selector_active();
    __atomic_store_n(&accepts_mouse, accept, __ATOMIC_RELAXED);
    __atomic_store_n(&last_update, plat_now_ns(), __ATOMIC_RELEASE);
    if (direct_mode() && __atomic_load_n(&shake_injected, __ATOMIC_RELAXED)) {
        c->v[9] = c->v[10] = 0;   // discrete shake pulses are for the game's gesture detector, not the camera
    }
    MouseSample next = {.object = c->x[20], .when = plat_now_ns(), .camera_enabled = accept};
    memcpy(&next.pad[0], &c->v[9], sizeof(float));
    memcpy(&next.pad[1], &c->v[10], sizeof(float));
    if (flags & 2) next.pad[0] = -next.pad[0];
    if (flags & 4) next.pad[1] = -next.pad[1];
    if (direct_mode()) {
        int32_t dx, dy;
        input_direct_mouse_sample(!input_direct_mouse_selector_active() && (accept || hand_active(plat_now_ns())), &dx, &dy);
        float sensitivity = (float)cfg_double("TOTK_MOUSE_SENS");
        sensitivity = fminf(10, fmaxf(0, sensitivity));
        // The game multiplies camera input by its 30 Hz frame ratio later. Divide by that exact ratio, rather
        // than wall-clock time: an equal distance of mouse motion then gives equal input at every frame rate.
        float ratio = 1;
        if (frame_step_realtime()) memcpy(&ratio, rt_patch_page(), sizeof ratio);
        else {
            double hz = frame_step_limit_hz();
            if (hz > 0) ratio = (float)(30 / hz);
        }
        if (isfinite(ratio) && ratio > 0 && sensitivity > 0 && (dx || dy)) {
            mouse_shake_feed(dx, dy, plat_now_ns());
            if (accept) mouse_input_object = c->x[20];
            float x, y;
            memcpy(&x, &c->v[9], sizeof x);
            memcpy(&y, &c->v[10], sizeof y);
            float scale = 0.01f * sensitivity / ratio;
            next.motion[0] = dx * scale * (flags & 2 ? -1 : 1);
            next.motion[1] = -dy * scale * (flags & 4 ? -1 : 1);
            next.angle[0] = -dx * 0.08f * sensitivity * (flags & 2 ? -1 : 1);
            next.angle[1] = dy * 0.08f * sensitivity * (flags & 4 ? -1 : 1);
            if (accept) { x += dx * scale; y -= dy * scale; }
            uint32_t xb, yb;
            memcpy(&xb, &x, sizeof xb);
            memcpy(&yb, &y, sizeof yb);
            c->v[9] = xb;
            c->v[10] = yb;
            static int logged;
            if (!logged) {
                logged = 1;
                rt_log("mouse: direct camera input after the stick dead zone, clamp and exponent (experimental)");
            }
        }
    }
    plat_mutex_lock(&sample_lock);
    next.generation = sample.generation + 1;
    sample = next;
    plat_mutex_unlock(&sample_lock);
    uint32_t original = (uint32_t)c->v[9];
    memcpy((void *)(uintptr_t)(c->x[20] + 0x420), &original, sizeof original);
}

// Free camera has another signed-square curve for its vertical axis, plus mode-dependent velocity filters.
// Remove only this mouse sample before that controller runs. At its final spherical-angle output, add degrees
// directly and move its persistent pitch/yaw by the same amounts so the next update starts at the new orientation.
// The controller still handles its gamepad input, following, collision and focus normally.
static void orbit_begin(Cpu *c) {
    orbit.object = 0;
    uint64_t object = c->x[0];
    if (direct_mode()) {
        MouseSample m = sample_read();
        uint64_t input = guest_pointer(object + 0x20);
        if (m.object == input && m.camera_enabled) {
            guest_float_store(input + 0x420, m.pad[0]);
            guest_float_store(input + 0x424, m.pad[1]);
            orbit.object = object;
            orbit.mouse = m;
            uint64_t held = guest_pointer(input + 0x148);
            uint8_t override;
            memcpy(&override, (void *)(uintptr_t)(input + 0x15c), 1);
            plat_mutex_lock(&sample_lock);
            OrbitTrack *track = orbit_track(object);
            if (track->input != input) track->hold_pitch = track->target_saved = 0;
            if (m.pad[1] != 0 || (held & 0x20) || (override & 0x40)) orbit_pitch_release(track);
            track->input = input;
            if (track->hold_pitch) orbit_pitch_target(object, track->pitch);
            plat_mutex_unlock(&sample_lock);
        }
    } else {
        plat_mutex_lock(&sample_lock);
        orbit_pitch_release(orbit_track(object));
        plat_mutex_unlock(&sample_lock);
    }
    c->x[8] = guest_pointer(object + 8);   // displaced ldr x8, [x0, #8]
}

static void orbit_angles(Cpu *c) {
    uint64_t object = c->x[20];
    if (orbit.object == object) {
        float yaw_delta = orbit.mouse.angle[0], pitch_delta = orbit.mouse.angle[1];
        int fresh = plat_now_ns() - orbit.mouse.when < 150000000ull &&
                    orbit_sample_once(object, orbit.mouse.generation);
        if (!fresh) yaw_delta = pitch_delta = 0;
        plat_mutex_lock(&sample_lock);
        OrbitTrack *track = orbit_track(object);
        if (track->hold_pitch || yaw_delta || pitch_delta) {
            float pitch = guest_float(c->sp + 0x44), yaw = guest_float(c->sp + 0x48);
            float low = guest_float(object + 0xf8), high = guest_float(object + 0xfc);
            if (!isfinite(low) || !isfinite(high) || low >= high) low = -89.9f, high = 89.9f;
            low = fmaxf(-89.9f, low); high = fminf(89.9f, high);
            // The game can recalculate its pitch after orbit_begin (following, mode changes and another filter).
            // Integrate from our last requested angle, then correct the final output even on stationary frames.
            float base = track->hold_pitch ? track->pitch : pitch;
            float new_pitch = fminf(high, fmaxf(low, base + pitch_delta));
            guest_float_store(c->sp + 0x44, new_pitch);
            guest_float_store(c->sp + 0x48, wrap_degrees(yaw + yaw_delta));
            guest_float_store(object + 0x108, wrap_degrees(new_pitch - orbit_pitch_offset(object)));
            guest_float_store(object + 0x120, wrap_degrees(guest_float(object + 0x120) + yaw_delta));
            if (!track->target_saved) track->original_target = guest_float(object + 0x15c), track->target_saved = 1;
            track->pitch = new_pitch; track->input = orbit.mouse.object; track->hold_pitch = 1;
            orbit_pitch_target(object, new_pitch);
            static int logged;
            if (!logged) {
                logged = 1;
                rt_log("mouse: free-camera angles directly, 0.08 degrees per count at sensitivity 1, equal X/Y");
            }
        }
        plat_mutex_unlock(&sample_lock);
        orbit.object = 0;
    }
    uint32_t bits;
    memcpy(&bits, (const void *)(uintptr_t)(c->sp + 0x44), sizeof bits);
    c->v[12] = bits;   // displaced ldr s12, [sp, #0x44]
}

// Ultrahand reads the player's manipulation input rather than the camera's processed stick. Insert the mouse
// after that controller's per-axis power curves (main+0x1d59460), preserving the keyboard/controller contribution.
static void hand_input(Cpu *c) {
    __atomic_store_n(&hand_update, plat_now_ns(), __ATOMIC_RELEASE);
    static _Thread_local uint64_t object, generation;
    if (direct_mode()) {
        MouseSample m = sample_read();
        if (m.generation != generation || object != c->x[19]) {
            float x = guest_float(c->sp + 0x60), y;
            memcpy(&y, &c->v[15], sizeof y);
            if (__atomic_load_n(&shake_injected, __ATOMIC_RELAXED)) x = y = 0;
            x += m.motion[0];
            y += m.motion[1];
            guest_float_store(c->sp + 0x60, x);
            uint32_t bits;
            memcpy(&bits, &y, sizeof bits);
            c->v[15] = bits;
            object = c->x[19]; generation = m.generation;
            static int logged;
            if (!logged && (m.motion[0] || m.motion[1])) {
                logged = 1;
                rt_log("mouse: held-object input after the manipulation controller's power curves (experimental)");
            }
        }
    }
    uint32_t original = (uint32_t)c->x[8];
    memcpy((void *)(uintptr_t)(c->sp + 0x48), &original, sizeof original);
}

// The game also uses a 10% activity threshold after processing the stick. A one-count mouse movement must count
// as active, even when its displacement is smaller than that threshold. Do this after the game computed the bits,
// then execute the displaced ldp d9, d8, [sp, #8]. No flag is changed for controller-only input.
static void camera_activity(Cpu *c) {
    if (mouse_input_object && mouse_input_object == c->x[20]) {
        uint32_t flags;
        void *at = (void *)(uintptr_t)(mouse_input_object + 0x418);
        memcpy(&flags, at, sizeof flags);
        flags = (flags | 0x11u) & ~0x28u;
        memcpy(at, &flags, sizeof flags);
        mouse_input_object = 0;
    }
    uint64_t d9, d8;
    memcpy(&d9, (const void *)(uintptr_t)(c->sp + 8), sizeof d9);
    memcpy(&d8, (const void *)(uintptr_t)(c->sp + 16), sizeof d8);
    c->v[9] = d9;
    c->v[8] = d8;
}

__attribute__((constructor)) static void mouse_direct_register(void) {
    rt_register_host(HOST_MOUSE_DIRECT, camera_input);
    rt_register_host(HOST_MOUSE_ACTIVITY, camera_activity);
    rt_register_host(HOST_MOUSE_ORBIT_BEGIN, orbit_begin);
    rt_register_host(HOST_MOUSE_ORBIT_ANGLES, orbit_angles);
    rt_register_host(HOST_MOUSE_HAND, hand_input);
}
#endif
