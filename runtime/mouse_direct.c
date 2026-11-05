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
#include "input_map.h"
#include "input_wsys.h"
#include "layout.h"
#include "platform/platform.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

// One turn rate for every camera, as a PC game has: degrees per raw count at sensitivity 1. The on-foot camera turns
// 70 degrees a second at a full stick, so this is a full stick's turn for 1500 counts a second. The stick rates and the
// ability camera's scales below are those Weeping of the Empire's engine/runtime/pc_mouse.cpp works with.
#define DEG_PER_COUNT (70.0f / 1500.0f)
// The stick turns yaw 70 x / 30 degrees per step at the 30 Hz frame ratio, and pitch 51 y |y| / 30 (a signed square):
// a processed stick value of STICK_PER_COUNT a count turns yaw DEG_PER_COUNT, and the mouse's part of y is the square
// root of PITCH_GAIN |v| so pitch turns as many degrees a count as yaw, at any speed.
#define STICK_PER_COUNT (DEG_PER_COUNT * 30.0f / 70.0f)
#define PITCH_GAIN (70.0f / 51.0f)
// The ability camera (ExecutePlayerCameraSpecialPower) turns the stored stick its own way: yaw x * 0.8 of on foot's
// rate and pitch y * 1.5 * 51, linear (no signed square). The mouse's part is scaled back to on foot's degrees a count:
// x / 0.8, and pitch linear at 70 / (1.5 * 51).
#define ABILITY_GAIN_X (1.0f / 0.8f)
#define ABILITY_GAIN_Y (70.0f / (1.5f * 51.0f))
// Ultrahand's held object moves by its post-curve input times its speed at a full tilt, as the camera turns by the
// stick: the mouse's part is the camera's stick value a count, so the object follows the hand as the camera would,
// at most HAND_MAX full tilts a step
#define HAND_PER_COUNT STICK_PER_COUNT
#define HAND_MAX 3.0f

static int direct_mode(void) {
    // cfg_value: unset is the settings table's default (direct), as the menu shows it
    return !strcmp(cfg_value("TOTK_MOUSE_MODE"), "direct") && rt_patch_enabled(rt_patch_set("mouse_direct"));
}

static uint64_t last_update;
static uint64_t hand_update;
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
    uint64_t object, generation, input, seen;
    float pitch, original_target;
    int hold_pitch, target_saved, resume;
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

// ---- the player's state: using an ability, the bow in hand. The player's action verbs, read as Weeping of the
// Empire's engine/runtime/totk_player_state.cpp reads them, with its 1.4.3 placement (games/totk/patches/
// pins_v143.json, fields_v143.json): the player root's GOT slot main+0x3969918 (1.0.0 +0x4562308), its link +0x20
// (id +0x10, unit +8; the unit's id +0x48 and proc +0x40, the proc's state +0x24 not 7..11 while it is deleted), the
// actor's component array +0x228 (1.0.0 +0x220), its AS component +0x158, whose 5 slots (+0x1f0) hold the verb at +0x28
// (+0x4c when +0x28 is Invalid). Every pointer is checked before it is followed; any miss reads as "nothing".
#define PLAYER_ROOT_SLOT 0x3969918ull
#define MAIN_IMAGE_SIZE 0x3adf000ull
enum { VERB_USE_BOW = 59, VERB_USE_SPECIAL_POWER = 61, VERB_DRAWN_BOW = 205, VERB_INVALID = 284 };
enum { PLAYER_FOUND = 1, PLAYER_ABILITY = 2, PLAYER_BOW = 4 };

static int readable(uint64_t p, uint64_t n) {
    return p >= 0x10000 && p < (1ull << 47) && !(p & 7) && plat_mem_readable(p, n);
}
static int32_t guest_s32(uint64_t at) { int32_t v; memcpy(&v, (void *)(uintptr_t)at, sizeof v); return v; }
static int game_object(uint64_t p, uint64_t n) {   // readable, its vtable in main's image
    if (!readable(p, n)) return 0;
    uint64_t vt = guest_pointer(p);
    return vt >= MOD_MAIN_BASE && vt < MOD_MAIN_BASE + MAIN_IMAGE_SIZE;
}

static uint32_t player_state(void) {
    uint64_t slot = MOD_MAIN_BASE + PLAYER_ROOT_SLOT;
    if (!readable(slot, 8)) return 0;
    uint64_t var = guest_pointer(slot);
    if (!readable(var, 8)) return 0;
    uint64_t root = guest_pointer(var);
    if (!readable(root, 0x38)) return 0;
    uint64_t link = root + 0x20, unit = guest_pointer(link + 8);
    int32_t id = guest_s32(link + 0x10);
    if (id == -1 || !readable(unit, 0x50) || guest_s32(unit + 0x48) != id) return 0;
    uint64_t proc = guest_pointer(unit + 0x40);
    if (!game_object(proc, 0x230) || (uint32_t)(guest_s32(proc + 0x24) - 7) < 5) return 0;
    uint64_t comps = guest_pointer(proc + 0x228);
    if (!readable(comps, 0x160)) return 0;
    uint64_t as = guest_pointer(comps + 0x158);
    if (!game_object(as, 0x1f0 + 5 * 8)) return 0;
    uint32_t f = PLAYER_FOUND;
    for (int i = 0; i < 5; i++) {
        uint64_t s = guest_pointer(as + 0x1f0 + 8ull * i);
        if (!readable(s, 0x50)) continue;
        int32_t v = guest_s32(s + 0x28);
        if (v == VERB_INVALID) v = guest_s32(s + 0x4c);
        if (v == VERB_USE_SPECIAL_POWER) f |= PLAYER_ABILITY;
        if (v == VERB_USE_BOW || v == VERB_DRAWN_BOW) f |= PLAYER_BOW;
    }
    return f;
}

// The player's state for this camera step, read again every 50 ms (each read costs memory queries), and its changes in
// the log (the first reads, then each change, 40 lines)
static uint32_t player_state_logged(void) {
    static uint32_t last = ~0u, f;
    static uint64_t at;
    static int lines;
    uint64_t now = plat_now_ns();
    if (at && now - at < 50000000ull) return f;
    at = now;
    f = player_state();
    if (f != last && lines < 40) {
        lines++;
        rt_log("mouse: player %s%s%s", f & PLAYER_FOUND ? "found" : "not found",
               f & PLAYER_ABILITY ? ", using an ability (ability camera gains)" : "",
               f & PLAYER_BOW ? ", bow in hand" : "");
    }
    last = f;
    return f;
}

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
    track->hold_pitch = track->target_saved = track->resume = 0;
}

// The pitch the camera has now (the filtered pitch plus its vertical offset), as orbit_angles leaves it
static float orbit_pitch_now(uint64_t object) {
    return wrap_degrees(guest_float(object + 0x108) + orbit_pitch_offset(object));
}

// The camera stays where it is now: the mouse holds the pitch the camera has (orbit_begin writes its target), so
// neither the game's default target nor the mouse's target from before an aim or a lock-on pulls it back.
static void orbit_pitch_settle(OrbitTrack *track) {
    track->pitch = orbit_pitch_now(track->object);
    track->hold_pitch = 1, track->resume = 0;
}

// The mouse's part of the processed stick for dx / dy counts at k stick units a count. On foot x is linear and y the
// signed square root that makes the camera's signed-square pitch linear again; the ability camera's linear scales are
// undone instead.
static void mouse_stick_add(int32_t dx, int32_t dy, float k, int ability, float *x, float *y) {
    float v = -dy * k;
    if (ability) {
        *x += dx * k * ABILITY_GAIN_X;
        *y += v * ABILITY_GAIN_Y;
    } else {
        *x += dx * k;
        *y += copysignf(sqrtf(PITCH_GAIN * fabsf(v)), v);
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
        uint32_t player = (dx || dy) ? player_state_logged() : 0;
        int ability = (player & PLAYER_ABILITY) != 0;
        // The bow drawn (in hand with ZR held): mouse_aim_scale times the mouse's motion on every path
        if ((player & PLAYER_BOW) && (input_buttons_last() & (1ull << B_ZR)))
            sensitivity *= (float)fminf(3, fmaxf(0.1, cfg_double("TOTK_MOUSE_AIM_SCALE")));
        // The game multiplies camera input by its 30 Hz frame ratio later. Divide by that exact ratio, rather
        // than wall-clock time: an equal distance of mouse motion then gives equal input at every frame rate.
        float ratio = 1;
        if (frame_step_realtime()) memcpy(&ratio, rt_patch_page(), sizeof ratio);
        else {
            double hz = frame_step_limit_hz();
            if (hz > 0) ratio = (float)(30 / hz);
        }
        if (isfinite(ratio) && ratio > 0 && sensitivity > 0 && (dx || dy)) {
            if (accept) mouse_input_object = c->x[20];
            float x, y;
            memcpy(&x, &c->v[9], sizeof x);
            memcpy(&y, &c->v[10], sizeof y);
            float hand = HAND_PER_COUNT * sensitivity / ratio;
            next.motion[0] = fminf(HAND_MAX, fmaxf(-HAND_MAX, dx * hand * (flags & 2 ? -1 : 1)));
            next.motion[1] = fminf(HAND_MAX, fmaxf(-HAND_MAX, -dy * hand * (flags & 4 ? -1 : 1)));
            next.angle[0] = -dx * DEG_PER_COUNT * sensitivity * (flags & 2 ? -1 : 1);
            next.angle[1] = dy * DEG_PER_COUNT * sensitivity * (flags & 4 ? -1 : 1);
            // stick_sens multiplies the game's stick turn rates (camera_speed.c): the mouse divides it out
            if (accept) mouse_stick_add(dx, dy, STICK_PER_COUNT * sensitivity / (ratio * cam_speed_applied()), ability,
                                        &x, &y);
            uint32_t xb, yb;
            memcpy(&xb, &x, sizeof xb);
            memcpy(&yb, &y, sizeof yb);
            c->v[9] = xb;
            c->v[10] = yb;
            static int logged;
            if (!logged) {
                logged = 1;
                rt_log("mouse: direct camera input after the stick dead zone, clamp and exponent");
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
            // The hooks take a step when the mouse moved in it, or while the mouse holds the pitch. A step of the
            // gamepad alone (no mouse motion, no hold: a player who never moved the mouse) keeps the game's input.
            int moved = m.motion[0] != 0 || m.motion[1] != 0, take;
            uint64_t held = guest_pointer(input + 0x148);
            uint8_t override;
            memcpy(&override, (void *)(uintptr_t)(input + 0x15c), 1);
            plat_mutex_lock(&sample_lock);
            OrbitTrack *track = orbit_track(object);
            if (track->input != input) track->hold_pitch = track->target_saved = track->resume = 0;
            uint64_t now = plat_now_ns();
            // A gamepad's vertical input gives the pitch back to the game's own target. The game's lock-on or override
            // (aiming) drives the camera itself: the mouse lets go without restoring the target it had before, and
            // once that ends (or after this camera went 100 ms without a step) settles on the pitch the camera has
            if (m.pad[1] != 0) orbit_pitch_release(track);
            else if ((held & 0x20) || (override & 0x40)) {
                if (track->hold_pitch) track->hold_pitch = 0, track->resume = 1;
            } else if (track->resume || (track->hold_pitch && now - track->seen > 100000000ull))
                orbit_pitch_settle(track);
            track->seen = now;
            take = moved || track->hold_pitch;
            if (take) track->input = input;
            if (track->hold_pitch) orbit_pitch_target(object, track->pitch);
            plat_mutex_unlock(&sample_lock);
            if (moved) {   // the mouse's part of this step's stick goes to the final angles instead
                guest_float_store(input + 0x420, m.pad[0]);
                guest_float_store(input + 0x424, m.pad[1]);
            }
            if (take) orbit.object = object, orbit.mouse = m;
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
                rt_log("mouse: free-camera angles directly, %.4f degrees per count at sensitivity 1, equal X/Y",
                       DEG_PER_COUNT);
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
                rt_log("mouse: held-object input after the manipulation controller's power curves");
            }
        }
    }
    uint32_t original = (uint32_t)c->x[8];
    memcpy((void *)(uintptr_t)(c->sp + 0x48), &original, sizeof original);
}

// The game also uses a 10% activity threshold after processing the stick. A one-count mouse movement must count
// as active, even when its displacement is smaller than that threshold. The site is the displaced ldp d9, d8, [sp, #8]
// just before the update's last `str w8, [x20, #0x418]`: w8 holds the flags then (bit 0 raw input, bit 4 any input,
// bits 3 and 5 processed input below the threshold), so they are changed in w8; a write to +0x418 would be overwritten
// by that store. No flag is changed for controller-only input.
static void camera_activity(Cpu *c) {
    if (mouse_input_object && mouse_input_object == c->x[20]) {
        uint32_t flags = (uint32_t)c->x[8];
        c->x[8] = (flags | 0x11u) & ~0x28u;
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
