// Direct mouse input for TotK 1.4.3 and 1.2.1. The camera controller's input update (1.4.3 main+0x874c1c, 1.2.1
// main+0xd18088) turns HID stick floats into a radial dead zone, clamps the radius to one and applies StickExponent.
// The mouse enters after all three, at the store of x (1.4.3 main+0x874cb4), before the game's X/Y inversion and
// input-activity flags. It never becomes a HID stick value. Where each version keeps the values the hooks read is in
// its MouseLayout (layout_143, layout_121), chosen by the running game version.
// Free orbit removes that sample again before the camera's additional vertical square/velocity filters and adds
// raw-count angular deltas at its spherical-angle output instead. The aiming, scope, ability, horse and Purah Pad
// cameras do the same at the angle each integrates from (camera_load). Ultrahand has a separate post-curve hook.
// The other camera modes still take the mouse as stick, linear on both axes (mouse_stick_add).
// The ordinary stick's result stays, so a controller still uses the game's own response.
//
// This is deliberately not a render-camera override: the game remains responsible for aiming, camera collision,
// lock-on and scripted cameras.
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

// One turn rate for every camera, as a PC game has: degrees per raw count at sensitivity 1. It is the free camera's rate
// since the direct mode came (0.08, about 14.3 cm a turn at 800 DPI), so a mouse_sens set for it keeps its meaning. The
// stick rates and the ability camera's scales below are those Weeping of the Empire's engine/runtime/pc_mouse.cpp
// works with.
#define DEG_PER_COUNT 0.08f
// The stick turns yaw 70 x / 30 degrees per step at the 30 Hz frame ratio: a processed stick value of STICK_PER_COUNT a
// count turns yaw DEG_PER_COUNT in the cameras that still take the mouse as stick (mouse_stick_add).
#define STICK_PER_COUNT (DEG_PER_COUNT * 30.0f / 70.0f)
// Pitch turns 51 degrees a second a unit of y against yaw's 70 a unit of x (linearly in every camera but the free
// orbit, which squares y and takes degrees instead): the mouse's y is PITCH_GAIN times its x a count.
#define PITCH_GAIN (70.0f / 51.0f)
// The ability camera (ExecutePlayerCameraSpecialPower) turns the stored stick its own way: yaw x * 0.8 of on foot's
// rate and pitch y * 1.5 * 51, linear (no signed square). The mouse's part is scaled back to on foot's degrees a count:
// x / 0.8, and pitch linear at 70 / (1.5 * 51).
#define ABILITY_GAIN_X (1.0f / 0.8f)
#define ABILITY_GAIN_Y (70.0f / (1.5f * 51.0f))
// Ultrahand's held object moves by its post-curve input times its speed at a full tilt: the mouse's part is
// HAND_PER_COUNT a count over the frame ratio, the rate the direct mode has had since it came, at most HAND_MAX full
// tilts a step
#define HAND_PER_COUNT 0.01f
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

// A shake of the mouse while Ultrahand holds an object (camera_input, mouse_shake_step): the right stick's side that
// follows it, -1 / +1, 0 while the mouse does not shake (input.c puts it on the right stick's x)
static volatile int32_t shake_side;
int input_direct_mouse_shake(void) { return __atomic_load_n(&shake_side, __ATOMIC_RELAXED); }

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

// Where a game version keeps what the hooks read and write (recomp/patches-v143.txt, patches-v121.txt). The camera
// input update keeps its flags in w21, its input object in x20 and y in s10 in both; the free orbit's object is x0 at
// its begin and x20 at its angles; Ultrahand's object is x19 and its y s15.
struct LoadSite;
typedef struct {
    uint8_t input_x;                // camera_input: the s register with x, which the displaced store puts at +0x420
    uint8_t activity_sp;            // camera_activity: the displaced ldp d9, d8, [sp, #activity_sp]
    uint8_t orbit_pitch, orbit_yaw; // orbit_angles: the final pitch and yaw on the stack, sp + these
    uint8_t orbit_reg;              // and the displaced ldr s<orbit_reg>, [sp, #orbit_pitch]
    uint8_t hand_x_sp, hand_x_reg;  // hand_input: x at sp + hand_x_sp, or in s<hand_x_reg> when that is 0
    uint8_t hand_store_reg, hand_store_sp;   // the displaced str w<hand_store_reg>, [sp, #hand_store_sp]
    uint64_t player_root_slot, main_size;    // player_state: 0 when not known for this version
    const struct LoadSite *loads;   // camera_load's sites
    size_t load_count;
} MouseLayout;
static const MouseLayout *mouse_layout(void);

typedef struct {
    uint64_t object, generation, when;
    float pad[2], motion[2], angle[2];   // post-inversion controller input, mouse input, mouse yaw/pitch in degrees
    int camera_enabled;
    int swing;         // a mouse shake swings the right stick in this step or did in the last (pad[0] is 0 then)
    uint32_t player;   // player_state_logged's flags for this step (0 without mouse motion)
} MouseSample;
static MouseSample sample;
static PlatMutex sample_lock = PLAT_MUTEX_INIT;
// The generation of the last sample a direct hook turned a camera by (orbit_angles, load_stick); sample_lock. A sample
// with mouse motion that none took went to its camera as stick input only (mouse_stick_add): camera_input logs that.
static uint64_t sample_taken;
// The last sample with mouse motion a watched camera mode loaded as stick (ROLE_WATCH): its generation and mode
static uint64_t sample_watched;
static int sample_watched_mode = -1;
static const char *mode_name(int mode);   // the load sites' camera modes, below
static void sample_mark_taken(uint64_t generation) {
    plat_mutex_lock(&sample_lock);
    sample_taken = generation;
    plat_mutex_unlock(&sample_lock);
}
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
// 1.2.1's slot, component offset and verb numbers are not known (MouseLayout.player_root_slot 0): there the player
// always reads as not found, so the ability camera's stick fallback keeps on-foot gains and mouse_aim_scale stays off.
enum { VERB_USE_BOW = 59, VERB_USE_SPECIAL_POWER = 61, VERB_DRAWN_BOW = 205, VERB_INVALID = 284 };
enum { PLAYER_FOUND = 1, PLAYER_ABILITY = 2, PLAYER_BOW = 4 };

static int readable(uint64_t p, uint64_t n) {
    return p >= 0x10000 && p < (1ull << 47) && !(p & 7) && mem_guest_readable(p, n);
}
static int32_t guest_s32(uint64_t at) { int32_t v; memcpy(&v, (void *)(uintptr_t)at, sizeof v); return v; }
static int game_object(uint64_t p, uint64_t n) {   // readable, its vtable in main's image
    if (!readable(p, n)) return 0;
    uint64_t vt = guest_pointer(p);
    return vt >= MOD_MAIN_BASE && vt < MOD_MAIN_BASE + mouse_layout()->main_size;
}

static uint32_t player_state(void) {
    if (!mouse_layout()->player_root_slot) return 0;
    uint64_t slot = MOD_MAIN_BASE + mouse_layout()->player_root_slot;
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

// The mouse's part of the processed stick for dx / dy counts at k stick units a count. Only the free orbit turns its
// pitch by the signed square of y, and it never takes this value (orbit_begin removes it, orbit_angles adds degrees).
// The other cameras that read the stick turn both axes linearly, yaw at the orbit's rate and pitch at 51 / 70 of it
// (Horse, ClimbObj, WaterfallClimb: PlayerCamera +0xc9c for pitch against +0xc9c times the yaw factor for yaw), so y
// is linear with PITCH_GAIN. The cameras with load sites below take degrees instead; this is the fallback for the
// rest. The ability camera's linear scales are undone in its offset mode, which the load sites leave to the stick.
static void mouse_stick_add(int32_t dx, int32_t dy, float k, int ability, float *x, float *y) {
    float v = -dy * k;
    if (ability) {
        *x += dx * k * ABILITY_GAIN_X;
        *y += v * ABILITY_GAIN_Y;
    } else {
        *x += dx * k;
        *y += v * PITCH_GAIN;
    }
}

// The game detaches a fused part when the right stick swings left and right a few times; in direct mode the mouse
// never reaches that stick. Once the mouse shakes (horizontal swings of at least SHAKE_SWING_DEG of camera turn each,
// changing direction SHAKE_FOLLOW_TURNS times within SHAKE_WINDOW_NS of the first change), the right stick follows
// it: full tilt the way the mouse moves, each side held at least SHAKE_SIDE_NS (two frames at the game's 30 fps) so
// every side is read, neutral as soon as the mouse rests. Only the player's own swings reach the stick, nothing plays
// on after them; SHAKE_IDLE_NS without motion ends the shake. Turning a held object back and forth at an ordinary pace
// takes longer than the window, and a reversal after a shorter swing starts the count again.
#define SHAKE_SWING_DEG 10.0f
#define SHAKE_FOLLOW_TURNS 2
#define SHAKE_WINDOW_NS 350000000ull
#define SHAKE_SIDE_NS 66666667ull
#define SHAKE_IDLE_NS 150000000ull
typedef struct { int dir, turns, side; float swing; uint64_t first, side_from, moved; } MouseShake;
// deg: this step's horizontal motion in degrees of camera turn. Returns the right stick's side for this step: -1 / +1
// while it follows the mouse's shake, 0 otherwise.
static int mouse_shake_step(MouseShake *s, float deg, uint64_t now) {
    int following = s->turns >= SHAKE_FOLLOW_TURNS;
    if (!following && s->turns && now - s->first > SHAKE_WINDOW_NS) s->turns = 0;
    if (deg != 0) {
        int d = deg > 0 ? 1 : -1;
        s->moved = now;
        if (d == s->dir) {
            s->swing += fabsf(deg);
        } else {
            if (following || (s->dir && s->swing >= SHAKE_SWING_DEG)) {
                if (!s->turns) s->first = now;
                s->turns++;
            } else {
                s->turns = 0;
            }
            s->dir = d, s->swing = fabsf(deg);
        }
        following = s->turns >= SHAKE_FOLLOW_TURNS;
    } else if (following && now - s->moved >= SHAKE_IDLE_NS) {   // the shake is over
        *s = (MouseShake){0};
        return 0;
    }
    if (!following) return 0;
    if (s->side && now - s->side_from < SHAKE_SIDE_NS) return s->side;   // a side is read before the next
    int want = deg != 0 ? s->dir : 0;                                     // neutral while the mouse rests
    if (want != s->side) s->side = want, s->side_from = now;
    return s->side;
}

// A host call at an instruction site, not an AAPCS64 function replacement. Preserve every guest register except
// x and s10, which carry the processed camera input (x is s9 in 1.4.3, s11 in 1.2.1: MouseLayout.input_x), and
// perform the displaced store of x at [x20 + 0x420].
static void camera_input(Cpu *c) {
    const unsigned rx = mouse_layout()->input_x;
    mouse_input_object = 0;
    uint32_t flags = (uint32_t)c->x[21];
    int accept = !(flags & 1) && !input_direct_mouse_selector_active();
    __atomic_store_n(&accepts_mouse, accept, __ATOMIC_RELAXED);
    __atomic_store_n(&last_update, plat_now_ns(), __ATOMIC_RELEASE);
    MouseSample next = {.object = c->x[20], .when = plat_now_ns(), .camera_enabled = accept};
    memcpy(&next.pad[0], &c->v[rx], sizeof(float));
    memcpy(&next.pad[1], &c->v[10], sizeof(float));
    if (flags & 2) next.pad[0] = -next.pad[0];
    if (flags & 4) next.pad[1] = -next.pad[1];
    if (direct_mode()) {
        int32_t dx, dy;
        input_direct_mouse_sample(!input_direct_mouse_selector_active() && (accept || hand_active(plat_now_ns())), &dx, &dy);
        float sensitivity = (float)cfg_double("TOTK_MOUSE_SENS");
        sensitivity = fminf(10, fmaxf(0, sensitivity));
        uint32_t player = (dx || dy) ? player_state_logged() : 0;
        next.player = player;
        int ability = (player & PLAYER_ABILITY) != 0;
        // The bow drawn (in hand with ZR held): mouse_aim_scale times the mouse's motion on every path
        if ((player & PLAYER_BOW) && (input_buttons_last() & (1ull << B_ZR)))
            sensitivity *= (float)fminf(3, fmaxf(0.1, cfg_double("TOTK_MOUSE_AIM_SCALE")));
        // Ultrahand holding an object: a shake of the mouse swings the right stick with it (mouse_shake_step)
        static MouseShake shake;
        static int last_side;
        uint64_t step_now = plat_now_ns();
        int side = 0;
        if (hand_active(step_now)) side = mouse_shake_step(&shake, dx * DEG_PER_COUNT * sensitivity, step_now);
        else shake = (MouseShake){0};
        __atomic_store_n(&shake_side, side, __ATOMIC_RELAXED);
        if (side) {
            static int logged;
            if (!logged) logged = 1, rt_log("mouse: a shake while Ultrahand holds an object swings the right stick with it");
        }
        // That swing is the mouse's own motion, which turns the camera in degrees already: the camera hooks take the
        // right stick's x as 0 while it swings (the stick read of this step may still carry the last step's side)
        if (side || last_side) next.pad[0] = 0, next.swing = 1;
        last_side = side;
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
            memcpy(&x, &c->v[rx], sizeof x);
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
            c->v[rx] = xb;
            c->v[10] = yb;
            static int logged;
            if (!logged) {
                logged = 1;
                rt_log("mouse: direct camera input after the stick dead zone, clamp and exponent");
            }
        }
    }
    plat_mutex_lock(&sample_lock);
    MouseSample last = sample;
    int untaken = last.camera_enabled && (last.angle[0] || last.angle[1]) && sample_taken != last.generation;
    int watched = sample_watched == last.generation ? sample_watched_mode : -1;
    next.generation = sample.generation + 1;
    sample = next;
    plat_mutex_unlock(&sample_lock);
    // The camera modes still on stick input, as play reaches them: the first 30 steps, then one in 600
    static unsigned untaken_count;
    if (untaken && (++untaken_count <= 30 || untaken_count % 600 == 0)) {
        uint32_t state = 0;
        if (readable(last.object, 0x160)) memcpy(&state, (void *)(uintptr_t)(last.object + 0x15c), sizeof state);
        rt_log("mouse: step %u with mouse motion took the stick path (no direct camera hook): camera %s, player "
               "%s%s%s, input +0x15c %#x", untaken_count, watched >= 0 ? mode_name(watched) : "not a watched one",
               last.player & PLAYER_FOUND ? "found" : "not found", last.player & PLAYER_ABILITY ? ", using an ability" : "",
               last.player & PLAYER_BOW ? ", bow in hand" : "", state);
    }
    uint32_t original = (uint32_t)c->v[rx];
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

// The final pitch and yaw are on the stack (1.4.3 sp + 0x44 / 0x48, 1.2.1 sp + 0x3c / 0x40)
static void orbit_angles(Cpu *c) {
    const MouseLayout *ml = mouse_layout();
    const uint64_t pitch_at = c->sp + ml->orbit_pitch, yaw_at = c->sp + ml->orbit_yaw;
    uint64_t object = c->x[20];
    if (orbit.object == object) {
        float yaw_delta = orbit.mouse.angle[0], pitch_delta = orbit.mouse.angle[1];
        int fresh = plat_now_ns() - orbit.mouse.when < 150000000ull &&
                    orbit_sample_once(object, orbit.mouse.generation);
        if (!fresh) yaw_delta = pitch_delta = 0;
        if (yaw_delta || pitch_delta) sample_mark_taken(orbit.mouse.generation);
        plat_mutex_lock(&sample_lock);
        OrbitTrack *track = orbit_track(object);
        if (track->hold_pitch || yaw_delta || pitch_delta) {
            float pitch = guest_float(pitch_at), yaw = guest_float(yaw_at);
            float low = guest_float(object + 0xf8), high = guest_float(object + 0xfc);
            if (!isfinite(low) || !isfinite(high) || low >= high) low = -89.9f, high = 89.9f;
            low = fmaxf(-89.9f, low); high = fminf(89.9f, high);
            // The game can recalculate its pitch after orbit_begin (following, mode changes and another filter).
            // Integrate from our last requested angle, then correct the final output even on stationary frames.
            float base = track->hold_pitch ? track->pitch : pitch;
            float new_pitch = fminf(high, fmaxf(low, base + pitch_delta));
            guest_float_store(pitch_at, new_pitch);
            guest_float_store(yaw_at, wrap_degrees(yaw + yaw_delta));
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
    memcpy(&bits, (const void *)(uintptr_t)pitch_at, sizeof bits);
    c->v[ml->orbit_reg] = bits;   // displaced ldr s12, [sp, #0x44] (1.2.1 ldr s13, [sp, #0x3c])
}

// Ultrahand reads the player's manipulation input rather than the camera's processed stick. Insert the mouse
// after that controller's per-axis power curves (1.4.3 main+0x1d59460, 1.2.1 main+0x176949c), preserving the
// keyboard/controller contribution. x is at [sp + 0x60] in 1.4.3 and in s9 in 1.2.1, y in s15 in both.
static void hand_input(Cpu *c) {
    const MouseLayout *ml = mouse_layout();
    __atomic_store_n(&hand_update, plat_now_ns(), __ATOMIC_RELEASE);
    static _Thread_local uint64_t object, generation;
    if (direct_mode()) {
        MouseSample m = sample_read();
        if (m.generation != generation || object != c->x[19]) {
            float x, y;
            if (ml->hand_x_sp) x = guest_float(c->sp + ml->hand_x_sp);
            else memcpy(&x, &c->v[ml->hand_x_reg], sizeof x);
            memcpy(&y, &c->v[15], sizeof y);
            x += m.motion[0];
            y += m.motion[1];
            uint32_t bits;
            if (ml->hand_x_sp) guest_float_store(c->sp + ml->hand_x_sp, x);
            else memcpy(&bits, &x, sizeof bits), c->v[ml->hand_x_reg] = bits;
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
    uint32_t original = (uint32_t)c->x[ml->hand_store_reg];   // displaced str w8, [sp, #0x48] (1.2.1 w9, #0x18)
    memcpy((void *)(uintptr_t)(c->sp + ml->hand_store_sp), &original, sizeof original);
}

// ---- The other player cameras that turn by the stick: degrees at the angle they integrate from.
// Each of these camera modes (vtable slot 36 of its ExecutePlayerCamera* class, the per-step update) turns by the
// processed stick at input +0x420 (x) / +0x424 (y) times its own speeds and the frame time, and adds that to a base
// angle in degrees. Two loads per axis are replaced by a host call (the `mouse_direct` load sites of
// recomp/patches-v143.txt and patches-v121.txt, all at page +0xf80; the return address tells them apart; the main
// addresses below are 1.4.3's, load_sites_121 has 1.2.1's):
//   stick: the load the turn is computed from gets the gamepad's part only (the sample's pad, post-inversion as the
//          game stored it), so the mode's speeds, time step and slow motion never touch the mouse;
//   base:  the load of the angle the turn is added to gets the mouse's degrees added, DEG_PER_COUNT a count as the
//          free orbit, once per sample. The game's clamps, collision and blends run on the result as usual.
// The stick loads that only test whether there is input (before the gyro branch) keep the mouse, so a mouse move
// still counts as stick input there. The stick load always runs before its base load; it leaves the degrees for that
// base (pending), so the two never disagree about a step.
//   Aiming (bow, throwing; main+0x23be978): yaw = input +0x414 + turn ([x29-0x48]), pitch target = +0x15c + turn, the
//     pitch follows its target at once (main+0x191970 with rate 1).
//   AimingTelescope (the scope; main+0x23c08ac) and SpecialPower (abilities; main+0x23d2d40): yaw as
//     Aiming ([sp+0x58], [x29-0x50]); pitch is +0x160 in degrees, taken to a position along an eased curve
//     (main+0x23c11d8 / 0x23d38e0), moved by the stick and taken back (main+0x23c14dc / 0x23d3be4). Degrees added to
//     +0x160 before that come back unchanged, so the curve no longer shapes the mouse. Beyond the pitch limits
//     (+0x1ac / +0x1b0) the stick moves a second camera parameter instead; the mouse's part past the limit stays in
//     the stick for that (LIMIT_160). That parameter is the camera's distance (the first of the (distance, pitch, yaw)
//     main+0x2e2468 takes from the camera's look vector), not a turn.
//   SpecialPower's offset mode (input +0x15c bit 8; SKIP_OFFSET on the sites above, ONLY_OFFSET below) is Recall's
//     cursor: the camera keeps a direction (+0x54 yaw) and the stick moves the cursor, +0x1d8 += x *
//     ReverseRecorderCursorSpeed * frame ratio and +0x1dc likewise, clamped to ReverseRecorderCursorRange; the camera
//     follows it, yaw = +0x54 - +0x1d8 * P.x and pitch = ... - +0x1dc * P.y with P = ReverseRecorderCameraRange (the
//     getters main+0x18a82a0, 0x18a824c, 0x18a8348; the step publishes the cursor at input +0x6c0). The cursor's base
//     loads get the change that moves the aim, the camera's turn plus the cursor's angle from the screen's centre
//     (o * P + atan(o * tan(half view angle)), the offset taken as the screen's half width / height), by the mouse's
//     degrees: the world under the cursor moves DEG_PER_COUNT a count, as the camera turns elsewhere (recall_offset).
//     P is read where the step loads P.y (ROLE_SCALE, both floats); until it was read once the mouse stays in the stick.
//   CameraFinder (the Purah Pad camera; main+0x23c26c8): pitch = +0x50 - PlayerCamera +0xc9c * y * frame ratio,
//     clamped; yaw = input +0x414 ([sp+0x48]) - the same with x.
//   LockOn / Target (lock-on; main+0x23d7140): the game points the camera at the target and the stick adds an offset,
//     yaw +0xbc and pitch +0xa4 += the step's turn ([sp+0x20], -PlayerCamera +0xc9c * stick * frame time), which the
//     update then limits and lets return; the step's turn is read twice (ROLE_BASE_KEEP, then ROLE_BASE) and also
//     decides that return, so it is the turn that gets the mouse's degrees. Where the camera points stays the game's.
//   LockOnAimingAt (the bow drawn while locked on, BowLockOn / BowLockOn_Horse; its stick step main+0x23cec74):
//     pitch offset +0x198 and yaw offset +0x19c += -PlayerCamera +0xc9c * stick * frame time, then clamped. The
//     other bow cameras (Bow, Bow_Crouch, Bow_Zoom, Bow_Horse, Bow_Zoom_Horse) are Aiming.
//   Tail (wall climbing, parameter set Climb; main+0x23d4950): the stick as angle and length; in its manual state
//     (+0x16c == 4) yaw +0xc0 and pitch +0xb4 -= x / y * speed * frame ratio. ClimbObj (main+0x23c3a98): pitch +0x88
//     (through x22, set by a patched mov) and yaw +0x90 likewise. WaterfallClimb (main+0x23dce00): yaw = the camera's
//     yaw - x * speed * frame ratio, kept at [x29-0x34] and loaded into w8 (the base); its vertical axis moves the
//     camera's height (ManualHeightMin..Max), not an angle, and stays stick input (watched).
//   EventMultiTalk (main+0x23c531c) and EventTalkManual (main+0x23c8ffc): pitch / yaw fields -= stick * speed *
//     frame ratio; MultiTalk's x load is also its input test (EPSILON).
//   ShieldSurfing (main+0x23d0088) and Wakeboard (main+0x23da61c), one template: the stick as angle and length; yaw
//     in the steering state = the camera's yaw ([sp+0x3c]) - x * speed * frame time; pitch = +0xdc / +0xd8 taken to an
//     eased curve's position, moved by K * -y|y| ([sp+0x10] / [sp+0x14]) and taken back, and the same turn moves a
//     second parameter. Stick y keeps the mouse (KEEP_STICK), the pitch base gets the degrees, the pitch's turn is cut
//     to the pad's share (ROLE_SHARE); stick x is EPSILON, its length also choosing the steering state.
//   HorseLockOnEmpty (ZL on a horse with no target) maps the stick's position to an offset that returns with it: no
//     degrees a count to give, so it stays stick input (watched).
//   Horse (main+0x23ca518, its helpers main+0x23cb1d4 and 0x23cb398): yaw = input +0x414 + turn (+0x68), pitch =
//     input +0x410 + turn (+0x64), clamped to +0x5c..+0x60.
// Every one of them turns by -K x and -K y for a stick (x, y) and a positive K, as the free orbit does: the sample's
// angle[] (-x and -y in counts, times DEG_PER_COUNT) is added as it is.
#define HOST_MOUSE_LOAD (RT_PATCH_PAGE + 0xf80)
// ROLE_BASE_KEEP: a base load another base load follows. ROLE_SHARE: a turn the game computed from a stick load with
// KEEP_STICK, cut to the gamepad's share of it. ROLE_BASE_STORE: a store (str s<t>, [x<n>, #imm]) of base + turn that a
// shared helper computed, where the base load itself is in the helper: the stored value and s<t> get the degrees.
enum { ROLE_STICK, ROLE_BASE, ROLE_SCALE, ROLE_WATCH, ROLE_BASE_KEEP, ROLE_SHARE, ROLE_BASE_STORE };
// Stick flags. EPSILON: the load is also the game's test for input, so with the gamepad's axis at rest and the mouse
// moving it gives +-1e-12 in the mouse's direction (input, no turn) instead of 0. KEEP_STICK: the value stays the
// game's, mouse and all (other parameters follow it); the turn it makes is cut to the pad's share at ROLE_SHARE.
// REPEAT: another read of the axis for the same turn before its stick load (1.2.1 reads the stick once for its length
// and again for its angle): the same value, and it leaves the degrees to that stick load.
enum { LIMIT_160 = 1, SKIP_OFFSET = 2, ONLY_OFFSET = 4, EPSILON = 8, KEEP_STICK = 16, REPEAT = 32 };
enum {
    MODE_AIM, MODE_SCOPE, MODE_ABILITY, MODE_HORSE, MODE_ABILITY_OFFSET, MODE_FINDER,
    MODE_CLIMB, MODE_WAKEBOARD, MODE_WATERFALL, MODE_TAIL, MODE_LOCKON, MODE_LOCKON_AIM, MODE_HORSE_LOCKON, MODE_TALK,
    MODE_TALK_MANUAL, MODE_SHIELD,
    MODE_COUNT
};
static const char *const mode_names[MODE_COUNT] = {
    "aiming (bow, throw)", "scope", "ability", "horse", "Recall cursor (ability offset mode)", "Purah Pad camera",
    "climbing an object (ClimbObj)", "Wakeboard", "waterfall climbing (WaterfallClimb)", "climbing (Tail)",
    "LockOn / Target", "bow lock-on (LockOnAimingAt)", "HorseLockOnEmpty", "dialogue (EventMultiTalk)",
    "dialogue (EventTalkManual)", "shield surfing"};
static const char *mode_name(int mode) { return mode >= 0 && mode < MODE_COUNT ? mode_names[mode] : "?"; }
typedef struct LoadSite {
    uint32_t offset, insn;   // main offset, the original load (ldr / ldur s<t> or w<t>, [x<n>, #imm]) or store
    uint8_t mode, axis, role, input_reg, flags;
} LoadSite;
#define PAIR(m, a) ((m) * 2 + (a))
static const LoadSite load_sites_143[] = {
    {0x23beb94, 0xbd4426c1, MODE_AIM, 1, ROLE_STICK, 22, 0},             // ldr s1, [x22, #0x424]
    {0x23bebc8, 0xbd415e61, MODE_AIM, 1, ROLE_BASE, 0, 0},               // ldr s1, [x19, #0x15c]
    {0x23bee2c, 0xbd4422c1, MODE_AIM, 0, ROLE_STICK, 22, 0},             // ldr s1, [x22, #0x420]
    {0x23beeec, 0xbc5b83a1, MODE_AIM, 0, ROLE_BASE, 0, 0},               // ldur s1, [x29, #-0x48]
    {0x23c0a10, 0xbd4422e0, MODE_SCOPE, 0, ROLE_STICK, 23, 0},           // ldr s0, [x23, #0x420]
    {0x23c0a1c, 0xbd4426eb, MODE_SCOPE, 1, ROLE_STICK, 23, LIMIT_160},   // ldr s11, [x23, #0x424]
    {0x23c0b00, 0xbd416260, MODE_SCOPE, 1, ROLE_BASE, 0, 0},             // ldr s0, [x19, #0x160]
    {0x23c0e80, 0xbd405be1, MODE_SCOPE, 0, ROLE_BASE, 0, 0},             // ldr s1, [sp, #0x58]
    {0x23d2eec, 0xbd4422e8, MODE_ABILITY, 0, ROLE_STICK, 23, SKIP_OFFSET},               // ldr s8, [x23, #0x420]
    {0x23d2ef4, 0xbd4426ee, MODE_ABILITY, 1, ROLE_STICK, 23, SKIP_OFFSET | LIMIT_160},   // ldr s14, [x23, #0x424]
    {0x23d3158, 0xbd416260, MODE_ABILITY, 1, ROLE_BASE, 0, 0},           // ldr s0, [x19, #0x160]
    {0x23d34cc, 0xbc5b03a1, MODE_ABILITY, 0, ROLE_BASE, 0, 0},           // ldur s1, [x29, #-0x50]
    {0x23cb274, 0xbd442288, MODE_HORSE, 0, ROLE_STICK, 20, 0},           // ldr s8, [x20, #0x420]
    {0x23cb2a0, 0xbd406a61, MODE_HORSE, 0, ROLE_BASE, 0, 0},             // ldr s1, [x19, #0x68]
    {0x23cb3f8, 0xbd4426a8, MODE_HORSE, 1, ROLE_STICK, 21, 0},           // ldr s8, [x21, #0x424]
    {0x23cb424, 0xbd406661, MODE_HORSE, 1, ROLE_BASE, 0, 0},             // ldr s1, [x19, #0x64]
    {0x23d2f30, 0xbd4422e8, MODE_ABILITY_OFFSET, 0, ROLE_STICK, 23, ONLY_OFFSET},   // ldr s8, [x23, #0x420]
    {0x23d2f5c, 0xbd41da62, MODE_ABILITY_OFFSET, 0, ROLE_BASE, 0, 0},               // ldr s2, [x19, #0x1d8]
    {0x23d2f6c, 0xbd4426e8, MODE_ABILITY_OFFSET, 1, ROLE_STICK, 23, ONLY_OFFSET},   // ldr s8, [x23, #0x424]
    {0x23d2f98, 0xbd41de62, MODE_ABILITY_OFFSET, 1, ROLE_BASE, 0, 0},               // ldr s2, [x19, #0x1dc]
    {0x23d3210, 0xbd400400, MODE_ABILITY_OFFSET, 1, ROLE_SCALE, 0, 0},              // ldr s0, [x0, #0x4] (P.y)
    {0x23c2890, 0xbd4426ad, MODE_FINDER, 1, ROLE_STICK, 21, 0},          // ldr s13, [x21, #0x424]
    {0x23c28b4, 0xbd405261, MODE_FINDER, 1, ROLE_BASE, 0, 0},            // ldr s1, [x19, #0x50]
    {0x23c2934, 0xbd4422ad, MODE_FINDER, 0, ROLE_STICK, 21, 0},          // ldr s13, [x21, #0x420]
    {0x23c2964, 0xbd404be1, MODE_FINDER, 0, ROLE_BASE, 0, 0},            // ldr s1, [sp, #0x48]
    {0x23c3dd0, 0xbd442301, MODE_CLIMB, 0, ROLE_STICK, 24, 0},           // ldr s1, [x24, #0x420]
    {0x23c3dd8, 0xbd409261, MODE_CLIMB, 0, ROLE_BASE, 0, 0},             // ldr s1, [x19, #0x90] (yaw)
    {0x23c3d64, 0xbd442701, MODE_CLIMB, 1, ROLE_STICK, 24, 0},           // ldr s1, [x24, #0x424]
    // The game's pitch load is ldr s1, [x22, #0x88]! (bc488ec1). recomp/patches-v143.txt makes the mov before it
    // set x22 = x19 + 0x88, so the hook performs it as the plain load below and writes no register but s1.
    {0x23c3d70, 0xbd4002c1, MODE_CLIMB, 1, ROLE_BASE, 0, 0},             // ldr s1, [x22] (pitch +0x88)
    {0x23da694, 0xbd4422c1, MODE_WAKEBOARD, 0, ROLE_STICK, 22, EPSILON},    // ldr s1, [x22, #0x420]
    {0x23da698, 0xbd4426c0, MODE_WAKEBOARD, 1, ROLE_STICK, 22, KEEP_STICK}, // ldr s0, [x22, #0x424]
    {0x23da840, 0xbd40da60, MODE_WAKEBOARD, 1, ROLE_BASE, 0, 0},         // ldr s0, [x19, #0xd8] (pitch)
    {0x23da894, 0xbd4017e0, MODE_WAKEBOARD, 1, ROLE_SHARE, 0, 0},        // ldr s0, [sp, #0x14] (pitch turn)
    {0x23daed8, 0xbd403fe9, MODE_WAKEBOARD, 0, ROLE_BASE, 0, 0},         // ldr s9, [sp, #0x3c] (yaw)
    {0x23d0100, 0xbd4422c1, MODE_SHIELD, 0, ROLE_STICK, 22, EPSILON},    // ldr s1, [x22, #0x420]
    {0x23d0104, 0xbd4426c0, MODE_SHIELD, 1, ROLE_STICK, 22, KEEP_STICK}, // ldr s0, [x22, #0x424]
    {0x23d02ac, 0xbd40de60, MODE_SHIELD, 1, ROLE_BASE, 0, 0},            // ldr s0, [x19, #0xdc] (pitch)
    {0x23d0a34, 0xbd4013e0, MODE_SHIELD, 1, ROLE_SHARE, 0, 0},           // ldr s0, [sp, #0x10] (pitch turn)
    {0x23d087c, 0xbd403fe9, MODE_SHIELD, 0, ROLE_BASE, 0, 0},            // ldr s9, [sp, #0x3c] (yaw)
    {0x23dced8, 0xbd4422cd, MODE_WATERFALL, 0, ROLE_STICK, 22, 0},       // ldr s13, [x22, #0x420]
    {0x23dd070, 0xb85cc3a8, MODE_WATERFALL, 0, ROLE_BASE, 0, 0},         // ldur w8, [x29, #-0x34] (yaw, as bits)
    {0x23dd008, 0xbd4426c9, MODE_WATERFALL, 1, ROLE_WATCH, 22, 0},       // ldr s9, [x22, #0x424] (height, stick)
    {0x23d4b14, 0xbd44228c, MODE_TAIL, 0, ROLE_STICK, 20, 0},            // ldr s12, [x20, #0x420]
    {0x23d4b1c, 0xbd44268d, MODE_TAIL, 1, ROLE_STICK, 20, 0},            // ldr s13, [x20, #0x424]
    {0x23d4f34, 0xbd40c260, MODE_TAIL, 0, ROLE_BASE, 0, 0},              // ldr s0, [x19, #0xc0] (yaw)
    {0x23d5028, 0xbd40b661, MODE_TAIL, 1, ROLE_BASE, 0, 0},              // ldr s1, [x19, #0xb4] (pitch)
    {0x23d756c, 0xbd442300, MODE_LOCKON, 0, ROLE_WATCH, 24, 0},          // ldr s0, [x24, #0x420] (the input test)
    {0x23d7594, 0xbd442301, MODE_LOCKON, 0, ROLE_STICK, 24, 0},          // ldr s1, [x24, #0x420]
    {0x23d75b4, 0xbd4023e0, MODE_LOCKON, 0, ROLE_BASE_KEEP, 0, 0},       // ldr s0, [sp, #0x20] (the yaw turn)
    {0x23d75c4, 0xbd4023e9, MODE_LOCKON, 0, ROLE_BASE, 0, 0},            // ldr s9, [sp, #0x20]
    {0x23d7998, 0xbd442701, MODE_LOCKON, 1, ROLE_STICK, 24, 0},          // ldr s1, [x24, #0x424]
    {0x23d79b8, 0xbd4023e0, MODE_LOCKON, 1, ROLE_BASE_KEEP, 0, 0},       // ldr s0, [sp, #0x20] (the pitch turn)
    {0x23d79c8, 0xbd4023ea, MODE_LOCKON, 1, ROLE_BASE, 0, 0},            // ldr s10, [sp, #0x20]
    {0x23cecb4, 0xbd4426a9, MODE_LOCKON_AIM, 1, ROLE_STICK, 21, 0},      // ldr s9, [x21, #0x424]
    {0x23cecf8, 0xbd419a61, MODE_LOCKON_AIM, 1, ROLE_BASE, 0, 0},        // ldr s1, [x19, #0x198] (pitch offset)
    {0x23ced0c, 0xbd4422a9, MODE_LOCKON_AIM, 0, ROLE_STICK, 21, 0},      // ldr s9, [x21, #0x420]
    {0x23ced4c, 0xbd419e61, MODE_LOCKON_AIM, 0, ROLE_BASE, 0, 0},        // ldr s1, [x19, #0x19c] (yaw offset)
    {0x23cc4b4, 0xbd44232b, MODE_HORSE_LOCKON, 0, ROLE_WATCH, 25, 0},    // ldr s11, [x25, #0x420]
    {0x23c542c, 0xbd4426eb, MODE_TALK, 1, ROLE_STICK, 23, 0},            // ldr s11, [x23, #0x424]
    {0x23c546c, 0xbd405e61, MODE_TALK, 1, ROLE_BASE, 0, 0},              // ldr s1, [x19, #0x5c] (pitch)
    {0x23c54d4, 0xbd4422eb, MODE_TALK, 0, ROLE_STICK, 23, EPSILON},      // ldr s11, [x23, #0x420] (also the test)
    {0x23c5554, 0xbd406661, MODE_TALK, 0, ROLE_BASE, 0, 0},              // ldr s1, [x19, #0x64] (yaw)
    {0x23c90ac, 0xbd442709, MODE_TALK_MANUAL, 1, ROLE_STICK, 24, 0},     // ldr s9, [x24, #0x424]
    {0x23c90d8, 0xbd407661, MODE_TALK_MANUAL, 1, ROLE_BASE, 0, 0},       // ldr s1, [x19, #0x74] (pitch)
    {0x23c9140, 0xbd442309, MODE_TALK_MANUAL, 0, ROLE_STICK, 24, 0},     // ldr s9, [x24, #0x420]
    {0x23c9170, 0xbd407e61, MODE_TALK_MANUAL, 0, ROLE_BASE, 0, 0},       // ldr s1, [x19, #0x7c] (yaw)
};
// The same camera modes in 1.2.1 (recomp/patches-v121.txt), each site matched to its 1.4.3 one by enclosing function.
// The base (the angle a turn is added to) differs where 1.2.1 adds the turn in a shared helper (*p = wrap(*p + s0)):
// Aiming's pitch target takes the degrees at the store of the helper's result, Horse's yaw at the turn the helper is
// given. Wakeboard, ShieldSurfing and Tail read each stick axis twice (REPEAT on the first read).
static const LoadSite load_sites_121[] = {
    {0x1d34338, 0xbd4426c1, MODE_AIM, 1, ROLE_STICK, 22, 0},             // ldr s1, [x22, #0x424]
    {0x1d34374, 0xbd015660, MODE_AIM, 1, ROLE_BASE_STORE, 0, 0},         // str s0, [x19, #0x154] (+0x15c + turn)
    {0x1d34584, 0xbd4422c1, MODE_AIM, 0, ROLE_STICK, 22, 0},             // ldr s1, [x22, #0x420]
    {0x1d345d0, 0xbc5b83a1, MODE_AIM, 0, ROLE_BASE, 0, 0},               // ldur s1, [x29, #-0x48]
    {0x1d35fd4, 0xbd4422e0, MODE_SCOPE, 0, ROLE_STICK, 23, 0},           // ldr s0, [x23, #0x420]
    {0x1d35fe0, 0xbd4426e9, MODE_SCOPE, 1, ROLE_STICK, 23, LIMIT_160},   // ldr s9, [x23, #0x424]
    {0x1d36280, 0xbd416260, MODE_SCOPE, 1, ROLE_BASE, 0, 0},             // ldr s0, [x19, #0x160]
    {0x1d36224, 0xbd405be1, MODE_SCOPE, 0, ROLE_BASE, 0, 0},             // ldr s1, [sp, #0x58]
    {0x1d48478, 0xbd4422cf, MODE_ABILITY, 0, ROLE_STICK, 22, SKIP_OFFSET},               // ldr s15, [x22, #0x420]
    {0x1d48480, 0xbd4426cc, MODE_ABILITY, 1, ROLE_STICK, 22, SKIP_OFFSET | LIMIT_160},   // ldr s12, [x22, #0x424]
    {0x1d48bb4, 0xbd416262, MODE_ABILITY, 1, ROLE_BASE, 0, 0},           // ldr s2, [x19, #0x160]
    {0x1d48b38, 0xbc5a83a1, MODE_ABILITY, 0, ROLE_BASE, 0, 0},           // ldur s1, [x29, #-0x58]
    // Horse's x19 is the yaw's address (+0x68) here, after a pre-indexed store: a key for the pair, as an object is
    {0x1d4070c, 0xbd442288, MODE_HORSE, 0, ROLE_STICK, 20, 0},           // ldr s8, [x20, #0x420]
    {0x1d40740, 0xbd401be0, MODE_HORSE, 0, ROLE_BASE, 0, 0},             // ldr s0, [sp, #0x18] (the yaw turn)
    {0x1d408f8, 0xbd4426a8, MODE_HORSE, 1, ROLE_STICK, 21, 0},           // ldr s8, [x21, #0x424]
    {0x1d409ac, 0xbd406661, MODE_HORSE, 1, ROLE_BASE, 0, 0},             // ldr s1, [x19, #0x64]
    {0x1d48ccc, 0xbd4422c9, MODE_ABILITY_OFFSET, 0, ROLE_STICK, 22, ONLY_OFFSET},   // ldr s9, [x22, #0x420]
    {0x1d48cf0, 0xbd41da61, MODE_ABILITY_OFFSET, 0, ROLE_BASE, 0, 0},               // ldr s1, [x19, #0x1d8]
    {0x1d48cfc, 0xbd4426c9, MODE_ABILITY_OFFSET, 1, ROLE_STICK, 22, ONLY_OFFSET},   // ldr s9, [x22, #0x424]
    {0x1d48d18, 0xbd41de61, MODE_ABILITY_OFFSET, 1, ROLE_BASE, 0, 0},               // ldr s1, [x19, #0x1dc]
    {0x1d485f8, 0xbd400400, MODE_ABILITY_OFFSET, 1, ROLE_SCALE, 0, 0},              // ldr s0, [x0, #0x4] (P.y)
    {0x1d37b44, 0xbd4426ad, MODE_FINDER, 1, ROLE_STICK, 21, 0},          // ldr s13, [x21, #0x424]
    {0x1d37b68, 0xbd405261, MODE_FINDER, 1, ROLE_BASE, 0, 0},            // ldr s1, [x19, #0x50]
    {0x1d37bcc, 0xbd4422ad, MODE_FINDER, 0, ROLE_STICK, 21, 0},          // ldr s13, [x21, #0x420]
    {0x1d37bf4, 0xbd405be1, MODE_FINDER, 0, ROLE_BASE, 0, 0},            // ldr s1, [sp, #0x58]
    {0x1d38e08, 0xbd442301, MODE_CLIMB, 0, ROLE_STICK, 24, 0},           // ldr s1, [x24, #0x420]
    {0x1d38e10, 0xbd409261, MODE_CLIMB, 0, ROLE_BASE, 0, 0},             // ldr s1, [x19, #0x90] (yaw)
    {0x1d38dac, 0xbd442701, MODE_CLIMB, 1, ROLE_STICK, 24, 0},           // ldr s1, [x24, #0x424]
    {0x1d38db8, 0xbd4002c1, MODE_CLIMB, 1, ROLE_BASE, 0, 0},             // ldr s1, [x22] (pitch +0x88, as 1.4.3)
    {0x1d50738, 0xbd4422c0, MODE_WAKEBOARD, 0, ROLE_STICK, 22, EPSILON | REPEAT},       // ldr s0, [x22, #0x420]
    {0x1d50740, 0xbd4426c1, MODE_WAKEBOARD, 1, ROLE_STICK, 22, KEEP_STICK | REPEAT},    // ldr s1, [x22, #0x424]
    {0x1d50768, 0xbd4426c0, MODE_WAKEBOARD, 1, ROLE_STICK, 22, KEEP_STICK}, // ldr s0, [x22, #0x424]
    {0x1d5076c, 0xbd4422c1, MODE_WAKEBOARD, 0, ROLE_STICK, 22, EPSILON},    // ldr s1, [x22, #0x420]
    {0x1d50e5c, 0xbd40da60, MODE_WAKEBOARD, 1, ROLE_BASE, 0, 0},         // ldr s0, [x19, #0xd8] (pitch)
    {0x1d508bc, 0xbd401fa0, MODE_WAKEBOARD, 1, ROLE_SHARE, 0, 0},        // ldr s0, [x29, #0x1c] (pitch turn)
    {0x1d50f20, 0xbd403bec, MODE_WAKEBOARD, 0, ROLE_BASE, 0, 0},         // ldr s12, [sp, #0x38] (yaw)
    {0x1d46108, 0xbd4422e0, MODE_SHIELD, 0, ROLE_STICK, 23, EPSILON | REPEAT},          // ldr s0, [x23, #0x420]
    {0x1d46110, 0xbd4426e1, MODE_SHIELD, 1, ROLE_STICK, 23, KEEP_STICK | REPEAT},       // ldr s1, [x23, #0x424]
    {0x1d46138, 0xbd4426e0, MODE_SHIELD, 1, ROLE_STICK, 23, KEEP_STICK}, // ldr s0, [x23, #0x424]
    {0x1d4613c, 0xbd4422e1, MODE_SHIELD, 0, ROLE_STICK, 23, EPSILON},    // ldr s1, [x23, #0x420]
    {0x1d46788, 0xbd40de60, MODE_SHIELD, 1, ROLE_BASE, 0, 0},            // ldr s0, [x19, #0xdc] (pitch)
    {0x1d46910, 0xbd401fa0, MODE_SHIELD, 1, ROLE_SHARE, 0, 0},           // ldr s0, [x29, #0x1c] (pitch turn)
    {0x1d467b4, 0xbd403bec, MODE_SHIELD, 0, ROLE_BASE, 0, 0},            // ldr s12, [sp, #0x38] (yaw)
    {0x1d51e10, 0xbd4422cd, MODE_WATERFALL, 0, ROLE_STICK, 22, 0},       // ldr s13, [x22, #0x420]
    {0x1d51fb8, 0xb9403be8, MODE_WATERFALL, 0, ROLE_BASE, 0, 0},         // ldr w8, [sp, #0x38] (yaw, as bits)
    {0x1d51f48, 0xbd4426c9, MODE_WATERFALL, 1, ROLE_WATCH, 22, 0},       // ldr s9, [x22, #0x424] (height, stick)
    {0x1d4a11c, 0xbd442280, MODE_TAIL, 0, ROLE_STICK, 20, REPEAT},       // ldr s0, [x20, #0x420]
    {0x1d4a124, 0xbd442681, MODE_TAIL, 1, ROLE_STICK, 20, REPEAT},       // ldr s1, [x20, #0x424]
    {0x1d4a14c, 0xbd442680, MODE_TAIL, 1, ROLE_STICK, 20, 0},            // ldr s0, [x20, #0x424]
    {0x1d4a150, 0xbd442281, MODE_TAIL, 0, ROLE_STICK, 20, 0},            // ldr s1, [x20, #0x420]
    {0x1d4a4a0, 0xbd40c260, MODE_TAIL, 0, ROLE_BASE, 0, 0},              // ldr s0, [x19, #0xc0] (yaw)
    {0x1d4a514, 0xbd40b661, MODE_TAIL, 1, ROLE_BASE, 0, 0},              // ldr s1, [x19, #0xb4] (pitch)
    {0x1d4c930, 0xbd442320, MODE_LOCKON, 0, ROLE_WATCH, 25, 0},          // ldr s0, [x25, #0x420] (the input test)
    {0x1d4c958, 0xbd442321, MODE_LOCKON, 0, ROLE_STICK, 25, 0},          // ldr s1, [x25, #0x420]
    {0x1d4c97c, 0xbd402be0, MODE_LOCKON, 0, ROLE_BASE_KEEP, 0, 0},       // ldr s0, [sp, #0x28] (the yaw turn)
    {0x1d4c98c, 0xbd402be9, MODE_LOCKON, 0, ROLE_BASE, 0, 0},            // ldr s9, [sp, #0x28]
    {0x1d4cd5c, 0xbd442721, MODE_LOCKON, 1, ROLE_STICK, 25, 0},          // ldr s1, [x25, #0x424]
    {0x1d4cd80, 0xbd402be0, MODE_LOCKON, 1, ROLE_BASE_KEEP, 0, 0},       // ldr s0, [sp, #0x28] (the pitch turn)
    {0x1d4cd90, 0xbd402beb, MODE_LOCKON, 1, ROLE_BASE, 0, 0},            // ldr s11, [sp, #0x28]
    {0x1d443e8, 0xbd4426a9, MODE_LOCKON_AIM, 1, ROLE_STICK, 21, 0},      // ldr s9, [x21, #0x424]
    {0x1d44430, 0xbd419a61, MODE_LOCKON_AIM, 1, ROLE_BASE, 0, 0},        // ldr s1, [x19, #0x198] (pitch offset)
    {0x1d44444, 0xbd4422a9, MODE_LOCKON_AIM, 0, ROLE_STICK, 21, 0},      // ldr s9, [x21, #0x420]
    {0x1d44490, 0xbd419e61, MODE_LOCKON_AIM, 0, ROLE_BASE, 0, 0},        // ldr s1, [x19, #0x19c] (yaw offset)
    {0x1d418dc, 0xbd44234b, MODE_HORSE_LOCKON, 0, ROLE_WATCH, 26, 0},    // ldr s11, [x26, #0x420]
    // EventMultiTalk and EventTalkManual: pre-indexed angle loads, made plain loads by the patched mov before each
    {0x1d3a3f8, 0xbd44270b, MODE_TALK, 1, ROLE_STICK, 24, 0},            // ldr s11, [x24, #0x424]
    {0x1d3a420, 0xbd4002e1, MODE_TALK, 1, ROLE_BASE, 0, 0},              // ldr s1, [x23] (pitch +0x5c)
    {0x1d3a484, 0xbd44230b, MODE_TALK, 0, ROLE_STICK, 24, EPSILON},      // ldr s11, [x24, #0x420] (also the test)
    {0x1d3a4e4, 0xbd4002c1, MODE_TALK, 0, ROLE_BASE, 0, 0},              // ldr s1, [x22] (yaw +0x64)
    {0x1d3e2b0, 0xbd442729, MODE_TALK_MANUAL, 1, ROLE_STICK, 25, 0},     // ldr s9, [x25, #0x424]
    {0x1d3e2d8, 0xbd400301, MODE_TALK_MANUAL, 1, ROLE_BASE, 0, 0},       // ldr s1, [x24] (pitch +0x74)
    {0x1d3e33c, 0xbd442329, MODE_TALK_MANUAL, 0, ROLE_STICK, 25, 0},     // ldr s9, [x25, #0x420]
    {0x1d3e364, 0xbd4002c1, MODE_TALK_MANUAL, 0, ROLE_BASE, 0, 0},       // ldr s1, [x22] (yaw +0x7c)
};

static const MouseLayout layout_143 = {
    .input_x = 9, .activity_sp = 8, .orbit_pitch = 0x44, .orbit_yaw = 0x48, .orbit_reg = 12,
    .hand_x_sp = 0x60, .hand_store_reg = 8, .hand_store_sp = 0x48,
    .player_root_slot = 0x3969918, .main_size = 0x3adf000,
    .loads = load_sites_143, .load_count = sizeof load_sites_143 / sizeof load_sites_143[0]};
static const MouseLayout layout_121 = {
    .input_x = 11, .activity_sp = 0x10, .orbit_pitch = 0x3c, .orbit_yaw = 0x40, .orbit_reg = 13,
    .hand_x_reg = 9, .hand_store_reg = 9, .hand_store_sp = 0x18,
    .player_root_slot = 0, .main_size = 0x472b000,
    .loads = load_sites_121, .load_count = sizeof load_sites_121 / sizeof load_sites_121[0]};
// Only 1.4.3 and 1.2.1 have the mouse_direct set (1.1.0's table has none, so direct_mode() is off and no hook runs)
static const MouseLayout *mouse_layout(void) { return RT_V143 ? &layout_143 : &layout_121; }

// The original single-register 32-bit loads: LDR S / W (unsigned offset), LDUR S and LDUR W (a float's bits in a
// general register), and for ROLE_BASE_STORE the store STR S (unsigned offset). Never a load with write-back: the hook
// is a call, and the lifted caller keeps X19-X29 in locals that a call does not reload (recomp/regs.py), so a base
// register written here would be lost. Their address:
static uint64_t load_address(const Cpu *c, uint32_t insn) {
    unsigned n = (insn >> 5) & 31;
    uint64_t base = n == 31 ? c->sp : c->x[n];
    if ((insn & 0xfb800000u) == 0xb9000000u) return base + (((insn >> 10) & 0xfff) << 2);   // LDR / STR, S or W
    int32_t imm9 = (int32_t)((insn >> 12) & 0x1ff);
    if (imm9 & 0x100) imm9 -= 0x200;
    return base + (int64_t)imm9;
}
static int load_to_general(uint32_t insn) {
    return (insn & 0xffe00c00u) == 0xb8400000u || (insn & 0xffc00000u) == 0xb9400000u;   // LDUR W, LDR W
}

// Degrees a stick load left for its base load, on this thread: per mode and axis, for one camera object
static _Thread_local struct { uint64_t object, generation; float degrees; } load_pending[MODE_COUNT * 2];
// A KEEP_STICK load's gamepad part and total, for its ROLE_SHARE load
static _Thread_local struct { uint64_t object; float pad, total; } load_share[MODE_COUNT * 2];
// The sample each camera object and axis took its degrees from: a sample turns a camera once
static struct { uint64_t object, generation[2]; } load_taken[16];
static int load_take_once(uint64_t object, int axis, uint64_t generation) {
    plat_mutex_lock(&sample_lock);
    int slot = 0;
    for (int i = 0; i < 16; i++) {
        if (load_taken[i].object == object) { slot = i; goto found; }
        if (load_taken[i].generation[0] + load_taken[i].generation[1] <
            load_taken[slot].generation[0] + load_taken[slot].generation[1]) slot = i;
    }
    load_taken[slot].object = object;
    load_taken[slot].generation[0] = load_taken[slot].generation[1] = 0;
found:;
    int fresh = load_taken[slot].generation[axis] != generation;
    load_taken[slot].generation[axis] = generation;
    plat_mutex_unlock(&sample_lock);
    return fresh;
}

// The ability camera's offset scale P (degrees of turn per unit of offset, x and y), as its last step read it
static struct { uint64_t object; float scale[2]; } offset_scale;

// Recall's view: SpecialPower's Fovy (50 degrees, vertical; PlayerCameraAIParam SpecialPower) times the field-of-view
// factor and the camera's aspect ratio when those patch sets are on (resolution.c: page +60, +56), else 16:9. The tangent
// of the half view angle along an axis (0 across, 1 up).
#define RECALL_FOVY 50.0f
#define RECALL_PI 3.14159265f
static float recall_tan_half(int axis) {
    static int fov_set = -2, aspect_set = -2;
    if (fov_set == -2) fov_set = rt_patch_set("fov"), aspect_set = rt_patch_set("aspect");
    float fovy = RECALL_FOVY, aspect = 16.0f / 9.0f, v;
    const uint8_t *page = rt_patch_page();
    if (page && fov_set >= 0 && rt_patch_enabled(fov_set)) {
        memcpy(&v, page + 60, sizeof v);
        if (v > 0.5f && v < 2) fovy *= v;
    }
    if (page && aspect_set >= 0 && rt_patch_enabled(aspect_set)) {
        memcpy(&v, page + 56, sizeof v);
        if (v > 0.5f && v < 5) aspect = v;
    }
    float t = tanf(fovy * 0.5f * RECALL_PI / 180);
    return axis ? t : t * aspect;
}

// The change of Recall's cursor offset o that moves its aim by `degrees`. The aim is the camera's follow, o * range
// degrees, plus the cursor's angle from the centre, atan(o * t); it grows with o, so Newton's method converges from o.
static float recall_offset(float o, float range, float t, float degrees) {
    const float k = 180 / RECALL_PI;
    float target = o * range + atanf(o * t) * k + degrees, n = o;
    for (int i = 0; i < 5; i++) {
        float d = range + k * t / (1 + n * n * t * t);
        n -= (n * range + atanf(n * t) * k - target) / d;
    }
    return isfinite(n) ? n - o : 0;
}

// The stick load: the gamepad's part of the axis, and the degrees for the base load. f is what the game stored.
static float load_stick(const LoadSite *s, Cpu *c, float f) {
    uint64_t object = c->x[19], input = c->x[s->input_reg];
    int pair = PAIR(s->mode, s->axis);
    if (!(s->flags & REPEAT)) load_pending[pair].object = 0;
    MouseSample m = sample_read();
    if (m.object != input || !m.camera_enabled || plat_now_ns() - m.when >= 150000000ull) return f;
    // A step without the mouse in it keeps the game's value as it is: the gamepad's stick and anything else the game
    // put there after the input update (the sample's pad is only the stick the update stored)
    if (!m.angle[0] && !m.angle[1] && !m.swing) return f;
    float scale = 1;
    if (s->flags & (SKIP_OFFSET | ONLY_OFFSET)) {
        uint32_t state;
        memcpy(&state, (void *)(uintptr_t)(input + 0x15c), sizeof state);
        int offset = (state & 0x100) != 0;
        if ((s->flags & SKIP_OFFSET) && offset) return f;
        if (s->flags & ONLY_OFFSET) {
            if (!offset) return f;
            plat_mutex_lock(&sample_lock);
            scale = offset_scale.object == object ? offset_scale.scale[s->axis] : 0;
            plat_mutex_unlock(&sample_lock);
            if (!isfinite(scale) || fabsf(scale) < 1e-4f) {   // not read yet: the stick, as the game turns it
                static int logged;
                if (!logged) logged = 1, rt_log("mouse: ability offset mode before its scale was read, stick input");
                return f;
            }
        }
    }
    float mouse = f - m.pad[s->axis], degrees = m.angle[s->axis];
    float out = m.pad[s->axis];
    if (s->flags & KEEP_STICK) {
        out = f;
        load_share[pair].object = object, load_share[pair].pad = m.pad[s->axis], load_share[pair].total = f;
    } else if ((s->flags & EPSILON) && out == 0 && mouse != 0) {
        out = copysignf(1e-12f, mouse);
    }
    if (s->flags & REPEAT) return out;
    if (!degrees || !load_take_once(object, s->axis, m.generation)) degrees = 0;
    // The offset turns the camera by -offset * P: an aim that turns as the sample's angle is -degrees of o's growth
    if (s->flags & ONLY_OFFSET && degrees)
        degrees = recall_offset(guest_float(object + (s->axis ? 0x1dc : 0x1d8)), scale, recall_tan_half(s->axis),
                                -degrees);
    if ((s->flags & LIMIT_160) && degrees) {
        float pitch = guest_float(object + 0x160), lo = guest_float(object + 0x1ac), hi = guest_float(object + 0x1b0);
        if (isfinite(pitch) && isfinite(lo) && isfinite(hi)) {
            if (lo > hi) { float t = lo; lo = hi; hi = t; }
            float want = pitch + degrees, kept = fminf(hi, fmaxf(lo, want));
            if (kept != want) {   // past the limit: that share of the mouse's stick moves the camera's other parameter
                out += mouse * ((want - kept) / degrees);
                degrees = kept - pitch;
            }
        }
    }
    load_pending[pair].object = object;
    load_pending[pair].generation = m.generation;
    load_pending[pair].degrees = degrees;
    return out;
}

// The base load: the angle plus the degrees its stick load left (ROLE_BASE_KEEP leaves them for the next base load)
static float load_base(const LoadSite *s, Cpu *c, float f) {
    uint64_t object = c->x[19];
    int pair = PAIR(s->mode, s->axis);
    if (load_pending[pair].object != object) return f;
    if (s->role != ROLE_BASE_KEEP) load_pending[pair].object = 0;
    float degrees = load_pending[pair].degrees;
    if (degrees) {
        sample_mark_taken(load_pending[pair].generation);
        static int logged[MODE_COUNT];
        if (!logged[s->mode]) {
            logged[s->mode] = 1;
            rt_log("mouse: %s camera angles directly, %.4f degrees per count at sensitivity 1",
                   mode_names[s->mode], DEG_PER_COUNT);
        }
    }
    return degrees ? f + degrees : f;   // f + 0 would turn a -0 the game loads into +0
}

// A turn the game made from a KEEP_STICK load's total t, as K * -t|t| (shield surfing, wakeboard: their pitch squares
// the stick), cut to the gamepad's p: K * -p|p|, the mouse's part being degrees at the base load
static float load_share_cut(const LoadSite *s, Cpu *c, float f) {
    int pair = PAIR(s->mode, s->axis);
    if (load_share[pair].object != c->x[19]) return f;
    load_share[pair].object = 0;
    float p = load_share[pair].pad, t = load_share[pair].total;
    return t != 0 && isfinite(f) ? f * (p * fabsf(p)) / (t * fabsf(t)) : f;
}

// A host call in place of one float load (MouseLayout.loads): performs the load, then gives the register the value
static void camera_load(Cpu *c) {
    uint64_t offset = c->x[30] - 4 - MOD_MAIN_BASE;
    const MouseLayout *ml = mouse_layout();
    const LoadSite *s = NULL;
    for (size_t i = 0; i < ml->load_count; i++)
        if (ml->loads[i].offset == offset) { s = &ml->loads[i]; break; }
    if (!s) {
        rt_log("mouse: load hook called from main+%#llx, no such site", (unsigned long long)offset);
        abort();
    }
    uint64_t at = load_address(c, s->insn);
    if (s->role == ROLE_BASE_STORE) {   // performs the store, of the value plus the degrees when there are any
        uint32_t bits = (uint32_t)c->v[s->insn & 31];
        float f, g;
        memcpy(&f, &bits, sizeof f);
        if (direct_mode() && (g = load_base(s, c, f)) != f) {
            f = wrap_degrees(g);
            memcpy(&bits, &f, sizeof bits);
            c->v[s->insn & 31] = bits;   // the code after the store goes on with the value in the register
        }
        memcpy((void *)(uintptr_t)at, &bits, sizeof bits);
        return;
    }
    float f = guest_float(at);
    if (s->role == ROLE_SCALE) {   // the ability camera's offset scale: P.y is this load, P.x the float before it
        plat_mutex_lock(&sample_lock);
        offset_scale.object = c->x[19];
        offset_scale.scale[0] = guest_float(at - 4);
        offset_scale.scale[1] = f;
        plat_mutex_unlock(&sample_lock);
    } else if (s->role == ROLE_WATCH) {   // the value stays the game's; a load with the mouse in it names its mode
        if (direct_mode()) {
            plat_mutex_lock(&sample_lock);
            if (sample.object == c->x[(s->insn >> 5) & 31] && sample.camera_enabled &&
                (sample.angle[0] || sample.angle[1]))
                sample_watched = sample.generation, sample_watched_mode = s->mode;
            plat_mutex_unlock(&sample_lock);
        }
    } else if (direct_mode()) {
        f = s->role == ROLE_STICK ? load_stick(s, c, f) : s->role == ROLE_SHARE ? load_share_cut(s, c, f)
                                                                                   : load_base(s, c, f);
    }
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    if (load_to_general(s->insn)) c->x[s->insn & 31] = bits;   // a W load clears the upper half
    else c->v[s->insn & 31] = bits;   // a 32-bit load clears the rest of the vector register
}

// The game also uses a 10% activity threshold after processing the stick. A one-count mouse movement must count
// as active, even when its displacement is smaller than that threshold. The site is the displaced ldp d9, d8, [sp, #8]
// (1.2.1 [sp, #0x10]) just before the update's last `str w8, [x20, #0x418]`: w8 holds the flags then (bit 0 raw
// input, bit 4 any input, bits 3 and 5 processed input below the threshold), so they are changed in w8; a write to
// +0x418 would be overwritten by that store. No flag is changed for controller-only input.
static void camera_activity(Cpu *c) {
    if (mouse_input_object && mouse_input_object == c->x[20]) {
        uint32_t flags = (uint32_t)c->x[8];
        c->x[8] = (flags | 0x11u) & ~0x28u;
        mouse_input_object = 0;
    }
    uint64_t d9, d8;
    memcpy(&d9, (const void *)(uintptr_t)(c->sp + mouse_layout()->activity_sp), sizeof d9);
    memcpy(&d8, (const void *)(uintptr_t)(c->sp + mouse_layout()->activity_sp + 8), sizeof d8);
    c->v[9] = d9;
    c->v[8] = d8;
}

__attribute__((constructor)) static void mouse_direct_register(void) {
    rt_register_host(HOST_MOUSE_DIRECT, camera_input);
    rt_register_host(HOST_MOUSE_ACTIVITY, camera_activity);
    rt_register_host(HOST_MOUSE_ORBIT_BEGIN, orbit_begin);
    rt_register_host(HOST_MOUSE_ORBIT_ANGLES, orbit_angles);
    rt_register_host(HOST_MOUSE_HAND, hand_input);
    rt_register_host(HOST_MOUSE_LOAD, camera_load);
}
#endif
