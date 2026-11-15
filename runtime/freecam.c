// Free camera for TotK 1.4.3: the view leaves Link and flies with the gamepad while the game goes on (docs/controls.md,
// "Free camera").
//
// The camera component update (main+0x628460, called with the component as x0 = x19 from main+0x627dc8, the camera
// actor's component array +0x228, slot +0x270) copies the component's pose into its sead camera and builds the view
// matrix from it. Component fields (doubles unless noted): eye +0xe8, target +0x110, up +0x128 (floats), then near,
// far, vertical view angle and aspect +0x134..+0x140 (the fov and aspect sets of recomp/patches-v143.txt load the last
// two). Two one-shot overrides come after: a non-zero eye at +0x180 or target at +0x198 replaces the pose for that
// update and is cleared (main+0x628fdc, main+0x629004). Additive offsets follow at +0x1b0 (flag) / +0x1c0 (eye),
// +0x1d8 / +0x1e8 (target) and +0x200 (up, three floats, applied when any is non-zero and never cleared by the game).
//
// The freecam set's one site (main+0x6286d4, `ldr x8, [x19, #0x18]`, the first instruction every path to the pose
// copy runs) calls freecam_hook, which performs that load and, while the free camera is on, writes the overrides.
// The game's own eye and target stay as they were, so the game camera takes over the update after the hook stops.
// The up offset is the game's state: the hook keeps the component's value and puts it back when the free camera ends,
// and makes the up vector the world's +Y meanwhile, so the horizon stays level.
//
// Which components: a 1.4.3 frame updates three. Two share the player camera's pose (both must move, or the scene
// and what follows the second disagree); the third rests at the default pose, eye (0, 0, 10) towards the origin. The
// first component seen away from that default anchors the pick, and every component whose eye is within MATCH_M of
// the anchor is driven. When no driven component has been updated for LOST_NS (a load replaced them) the next one
// away from the default anchors again; the free camera keeps its own position.
//
// Input (runtime/input.c freecam_gate): TOTK_FREECAM_PAD switches it, ZL+ZR+L3 by default. While it is on the game
// gets no gamepad input; the left stick flies along the view and to the side, the triggers straight up and down, the
// right stick turns, and each press of L or R halves or doubles the speed. Every driven component steps the flight by
// the wall-clock time since the last step, so the speed is the same at any frame rate and game speed.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "platform/platform.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

#define SPEED_START 8.0      // metres a second at full deflection
#define SPEED_MIN 0.25       // 8 m/s halved five times
#define SPEED_MAX 512.0      // 8 m/s doubled six times
#define TURN_DEG_S 110.0     // the right stick's turn rate at full deflection
#define PITCH_MAX_DEG 85.0   // short of straight up or down, where the view's up vector would fail
#define TARGET_M 10.0        // the target's distance in front of the eye
#define MATCH_M 2.0
#define LOST_NS 500000000ull
#define STEP_MAX_S 0.1       // a longer gap (a load, a hitch) moves the camera no further than this
#define MAX_DRIVEN 8

static PlatMutex pad_lock = PLAT_MUTEX_INIT;
static FreecamPad pad_now;        // the last poll's input (input thread), under pad_lock
static int speed_clicks;          // R presses minus L presses not yet applied (input thread adds, the hook takes)
// + 1 at every switch (input thread); odd while the free camera is on. One value, so the hook never sees a switch
// without the state it switched to.
static unsigned generation;

int freecam_on(void) { return __atomic_load_n(&generation, __ATOMIC_ACQUIRE) & 1; }

void freecam_pad(const FreecamPad *p) {
    plat_mutex_lock(&pad_lock);
    pad_now = *p;
    plat_mutex_unlock(&pad_lock);
}

void freecam_speed_click(int faster) { __atomic_add_fetch(&speed_clicks, faster ? 1 : -1, __ATOMIC_RELAXED); }

#ifdef RT_PATCH_PAGE
#define HOST_FREECAM (RT_PATCH_PAGE + 0xf50)

enum {
    OFF_LINK = 0x18, OFF_EYE = 0xe8, OFF_TARGET = 0x110, OFF_UP = 0x128,
    OFF_EYE_OVERRIDE = 0x180, OFF_TARGET_OVERRIDE = 0x198, OFF_UP_OFFSET = 0x200,
    SPAN = OFF_UP_OFFSET + 12 - OFF_EYE,   // the fields the hook reads and writes, from OFF_EYE
};

int freecam_available(void) { return rt_game_version == 143 && rt_patch_enabled(rt_patch_set("freecam")); }

void freecam_toggle(void) {
    unsigned gen = __atomic_add_fetch(&generation, 1, __ATOMIC_ACQ_REL);
    rt_log("freecam: %s", gen & 1 ? "on" : "off");
}

// Game thread only (the camera update runs on one thread). A component stays listed from its first drive until its
// up offset is put back (the first update after the switch goes off) or, while off, until it has gone LOST_NS
// without an update (freed by a load).
static struct {
    unsigned generation;
    int started;                       // pos, yaw and pitch hold a pose
    double pos[3], yaw, pitch, speed;  // radians; yaw 0 looks along +Z, pitch > 0 up
    uint64_t last_step, anchor_seen;
    int anchored;                      // anchor holds the eye of the components being driven
    double anchor[3];
    int n;
    struct { uint64_t cam, seen; float up_offset[3]; } driven[MAX_DRIVEN];
} fc;

static void read3(uint64_t at, double v[3]) { memcpy(v, (const void *)(uintptr_t)at, 3 * sizeof(double)); }
static void write3(uint64_t at, const double v[3]) { memcpy((void *)(uintptr_t)at, v, 3 * sizeof(double)); }

static int at_default(const double eye[3], const double target[3]) {
    return eye[0] == 0 && eye[1] == 0 && eye[2] == 10 && target[0] == 0 && target[1] == 0 && target[2] == 0;
}

static void direction(double d[3]) {
    d[0] = sin(fc.yaw) * cos(fc.pitch);
    d[1] = sin(fc.pitch);
    d[2] = cos(fc.yaw) * cos(fc.pitch);
}

static int driven_index(uint64_t cam) {
    for (int i = 0; i < fc.n; i++)
        if (fc.driven[i].cam == cam) return i;
    return -1;
}

static void drop(int i) { fc.driven[i] = fc.driven[--fc.n]; }

// The free camera starts where the game camera is: its eye, looking at its target.
static void start(const double eye[3], const double target[3], uint64_t now) {
    double d[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    memcpy(fc.pos, eye, sizeof fc.pos);
    fc.yaw = len > 1e-6 ? atan2(d[0], d[2]) : 0;
    fc.pitch = len > 1e-6 ? asin(fmax(-1.0, fmin(1.0, d[1] / len))) : 0;
    fc.started = 1;
    fc.last_step = now;
    rt_log("freecam: starting at (%.1f %.1f %.1f)", eye[0], eye[1], eye[2]);
}

// A new component joins while it shares the anchor's eye; the first one away from the default pose sets the anchor.
static int pick(uint64_t cam, const double eye[3], const double target[3]) {
    if (at_default(eye, target)) return -1;
    if (fc.anchored) {
        double dx = eye[0] - fc.anchor[0], dy = eye[1] - fc.anchor[1], dz = eye[2] - fc.anchor[2];
        if (dx * dx + dy * dy + dz * dz > MATCH_M * MATCH_M) return -1;
    } else {
        memcpy(fc.anchor, eye, sizeof fc.anchor);
        fc.anchored = 1;
    }
    if (fc.n == MAX_DRIVEN) {   // full after several loads: the one unseen longest was freed by a load
        int oldest = 0;
        for (int k = 1; k < fc.n; k++)
            if (fc.driven[k].seen < fc.driven[oldest].seen) oldest = k;
        drop(oldest);
    }
    int i = fc.n++;
    fc.driven[i].cam = cam;
    memcpy(fc.driven[i].up_offset, (const void *)(uintptr_t)(cam + OFF_UP_OFFSET), sizeof fc.driven[i].up_offset);
    rt_log("freecam: driving camera component %#llx", (unsigned long long)cam);
    return i;
}

static double axis(float v) { return v > 1 ? 1 : v < -1 ? -1 : v; }

// Flight since the last step: L and R presses scale the speed, the right stick turns, the left stick moves along the
// view and to the side, the triggers along the world's up.
// Every driven component steps; the second one of a frame comes microseconds after the first and moves that little.
static void step(uint64_t now) {
    double dt = (now - fc.last_step) / 1e9;
    fc.last_step = now;
    if (dt <= 0) return;
    if (dt > STEP_MAX_S) dt = STEP_MAX_S;
    FreecamPad p;
    plat_mutex_lock(&pad_lock);
    p = pad_now;
    plat_mutex_unlock(&pad_lock);
    int clicks = __atomic_exchange_n(&speed_clicks, 0, __ATOMIC_RELAXED);
    if (clicks) {
        fc.speed *= exp2(clicks);
        fc.speed = fc.speed < SPEED_MIN ? SPEED_MIN : fc.speed > SPEED_MAX ? SPEED_MAX : fc.speed;
    }
    double turn = TURN_DEG_S * M_PI / 180 * dt, limit = PITCH_MAX_DEG * M_PI / 180;
    fc.yaw = remainder(fc.yaw - axis(p.rx) * turn, 2 * M_PI);   // right turns towards -yaw (see right[] below)
    fc.pitch += axis(p.ry) * turn;
    fc.pitch = fc.pitch > limit ? limit : fc.pitch < -limit ? -limit : fc.pitch;
    double d[3], right[3];
    direction(d);
    // world +Y up, right-handed: right = d x Y, level
    double rl = sqrt(d[0] * d[0] + d[2] * d[2]);
    right[0] = -d[2] / rl, right[1] = 0, right[2] = d[0] / rl;
    double fwd = axis(p.ly), side = axis(p.lx), lift = axis(p.rt) - axis(p.lt), dist = fc.speed * dt;
    for (int k = 0; k < 3; k++) fc.pos[k] += (d[k] * fwd + right[k] * side) * dist;
    fc.pos[1] += lift * dist;
}

static void drive(uint64_t cam) {
    double d[3], target[3];
    direction(d);
    for (int k = 0; k < 3; k++) target[k] = fc.pos[k] + d[k] * TARGET_M;
    write3(cam + OFF_EYE_OVERRIDE, fc.pos);
    write3(cam + OFF_TARGET_OVERRIDE, target);
    // up + offset = +Y: the game adds the offset to its up vector before it builds the view
    float up[3], offset[3];
    memcpy(up, (const void *)(uintptr_t)(cam + OFF_UP), sizeof up);
    offset[0] = -up[0], offset[1] = 1.0f - up[1], offset[2] = -up[2];
    if (offset[0] == 0 && offset[1] == 0 && offset[2] == 0) offset[1] = 1e-6f;   // all zero would skip the offset
    memcpy((void *)(uintptr_t)(cam + OFF_UP_OFFSET), offset, sizeof offset);
}

// The site's own instruction first: x8 = [x19 + 0x18]. x19 is the camera component (read only: the lifter keeps
// X19-X29 in locals across a host call).
static void freecam_hook(Cpu *c) {
    uint64_t cam = c->x[19];
    memcpy(&c->x[8], (const void *)(uintptr_t)(cam + OFF_LINK), sizeof c->x[8]);
    unsigned gen = __atomic_load_n(&generation, __ATOMIC_ACQUIRE);
    int active = gen & 1;
    if (!active && !fc.n) return;   // the common case: one load and two compares
    if (gen != fc.generation) {     // a switch: a new flight starts at the game camera, at the start speed
        fc.generation = gen;
        fc.started = 0, fc.anchored = 0, fc.speed = SPEED_START;
    }
    uint64_t now = plat_now_ns();
    int i = driven_index(cam);
    if (!active) {
        if (i >= 0) {
            memcpy((void *)(uintptr_t)(cam + OFF_UP_OFFSET), fc.driven[i].up_offset, sizeof fc.driven[i].up_offset);
            drop(i);
        }
        for (int k = fc.n - 1; k >= 0; k--)
            if (now - fc.driven[k].seen > LOST_NS) drop(k);   // freed by a load while it waited
        return;
    }
    if (!mem_guest_readable(cam + OFF_EYE, SPAN)) return;
    if (fc.anchored && now - fc.anchor_seen > LOST_NS) fc.anchored = 0;   // a load replaced the components
    double eye[3], target[3];
    read3(cam + OFF_EYE, eye);
    read3(cam + OFF_TARGET, target);
    if (i < 0 && (i = pick(cam, eye, target)) < 0) return;
    if (!fc.started) start(eye, target, now);
    fc.driven[i].seen = fc.anchor_seen = now;
    step(now);
    drive(cam);
}

__attribute__((constructor)) static void freecam_register(void) {
    if (rt_game_version == 143) rt_register_host(HOST_FREECAM, freecam_hook);
}
#else
int freecam_available(void) { return 0; }
void freecam_toggle(void) {}
#endif
