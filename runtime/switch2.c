// Ported from adriadam10/mkw-recomp (MIT, Copyright (c) 2026 Adrià), runtime/input/switch2.c at eb8fe198, through
// dkb-pc-recomp (a fork of this port), which changed it: rumble plays the game's HD values (switch2_rumble_hd: both bands' amplitude and frequency per side, 5 ms samples
// through switch2_proto.h's rumble log) instead of two motors at fixed frequencies, which stay as the fallback for a plain
// SDL_RumbleGamepad; switch2_owns; a switch2_start stub where there is no transport (macOS).
//
// Switch 2 controllers over Bluetooth LE (switch2.h): the protocol on top of a transport, and each pad (a Pro
// Controller 2, or one or two Joy-Con 2) shown as a virtual SDL gamepad. The flow follows mkw-neo's CoreBluetooth
// session (seedds/mkw-neo dm/host/pad_switch2_apple.m, MIT): identity, pairing when the controller is in Sync,
// stick calibration, gyro offset, features, then reports; the player lights are written once a second, since the link
// drops after about ten seconds without a write from the host. Everything here runs on the transport's thread, but
// for the gamepad's Rumble callback, which only stores the motors' values.
// The protocol is checked without a controller by tests/switch2_rumble_test.c (the HD rumble encoding and sample scheduling).
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "switch2.h"
#include "switch2_proto.h"

enum { S2_REPLY_MAX = 0x50 + S2_REPLY_DATA, S2_TIMEOUT_MS = 2000, S2_LED_NS = 1000000000 };
// HD rumble: a pad with no value from the game for this long is silent (ov_input.c RUMBLE_STALE_NS); a packet's three
// 5 ms samples end this long before the tick that sends it, so every value set within them is known by then (a tick
// comes every ~15 ms, later when the transport is busy)
#define S2_HD_STALE_NS 250000000ull
#define S2_HD_LAG_NS 20000000ull

typedef struct S2Pad {
    s2_pad_state st;              // its controllers' last reports and calibration (switch2_proto.h)
    S2Session *pro, *left, *right;
    SDL_JoystickID id;
    SDL_Joystick *js;
    _Atomic uint32_t rumble;      // low << 16 | high, as the game last set them (Rumble callback, any thread)
    int index;                    // for the player lights
    int band;                     // battery band last logged (S2_BATTERY_*)
    struct S2Pad *next;
} S2Pad;

// The pads' batteries for other threads (switch2_battery): SDL id << 32 | band << 16 | mV, by pad index; 0 empty
enum { S2_BATTERY_SLOTS = 8 };
static _Atomic uint64_t battery[S2_BATTERY_SLOTS];

// The game's HD values by pad index (the same slots), written from the game's vibration thread (switch2_rumble_hd) and
// read by the transport's (rumble): the SDL id the slot belongs to (0 free), the values with their times, the last time
static PlatMutex hd_lock = PLAT_MUTEX_INIT;
static struct { uint32_t id; uint64_t last; s2_rumble_log log; } hd[S2_BATTERY_SLOTS];   // hd_lock

struct S2Session {
    S2Link *link;
    const S2LinkOps *ops;
    uint16_t pid;
    int use;
    uint8_t host[6];
    S2Pad *pad;
    s2_cal cal[2];
    s2_rest rest;                 // the gyro offset learned while it lies still
    float factory[3];             // its factory gyro offset in counts, when usable (s2_gyro_cal_usable)
    int have_factory;
    s2_clock clk;
    uint8_t want;                 // the command a reply is awaited for, 0 none
    uint8_t reply[S2_REPLY_MAX];
    size_t reply_n;
    int got;
    uint8_t vseq;
    int quiet;                    // silent rumble packets still to send after the motors stopped
    s2_rumble_cursor cur;         // its place in its pad's HD rumble log
    uint64_t rate_t0;             // the first report's arrival, for the reports-a-second line 5 s later (#113's lag)
    uint32_t rate_c0, rate_n;     // its controller clock, and the reports since
    int rate_done;
    uint64_t next_led;
};

static S2Pad *pads;

uint64_t switch2_now_ns(void) { return SDL_GetTicksNS(); }

static const char *kind(uint16_t pid) {
    return pid == S2_PID_PRO ? "Pro Controller 2" : pid == S2_PID_JOYCON_L ? "Joy-Con 2 (L)" : "Joy-Con 2 (R)";
}

// ---- commands: one in flight (the controller has one reply channel); the transport pumps until the reply

static int command(S2Session *s, uint8_t cmd, uint8_t sub, const uint8_t *payload, size_t n) {
    uint8_t f[8 + 0xFF];
    size_t len = s2_command(f, sizeof f, cmd, sub, payload, n);
    s->want = cmd, s->got = 0;
    if (!len || !s->ops->write_cmd(s->link, f, len)) return s->want = 0, 0;
    for (uint64_t end = switch2_now_ns() + S2_TIMEOUT_MS * 1000000ull; !s->got && switch2_now_ns() < end;)
        s->ops->pump(10);
    if (!s->got) rt_log("input: %s: command 0x%02x timed out", kind(s->pid), cmd);
    s->want = 0;
    return s->got;
}

void switch2_reply(S2Session *s, const uint8_t *d, size_t n) {
    if (!s->want || !s2_reply_is(d, n, s->want)) return;
    s->reply_n = n < sizeof s->reply ? n : sizeof s->reply;
    memcpy(s->reply, d, s->reply_n);
    s->got = 1;
}

// len bytes of the controller's memory at addr into out; 0 when it did not answer them
static int read_memory(S2Session *s, uint8_t len, uint32_t addr, uint8_t *out) {
    uint8_t p[8];
    s2_memory_payload(p, len, addr);
    if (!command(s, S2_CMD_MEMORY, S2_SUB_MEMORY_READ, p, 8)) return 0;
    if (s->reply_n < (size_t)S2_REPLY_DATA + len || s->reply[8] != len) return 0;
    memcpy(out, s->reply + S2_REPLY_DATA, len);
    return 1;
}

static void read_cal(S2Session *s, int slot) {   // the player's own calibration when set, else the factory's
    uint8_t d[0x0B];
    if (read_memory(s, sizeof d, slot ? S2_ADDR_USER_STICK2 : S2_ADDR_USER_STICK1, d) && s2_cal_parse(d, sizeof d, &s->cal[slot]))
        return;
    if (read_memory(s, sizeof d, slot ? S2_ADDR_FACTORY_STICK2 : S2_ADDR_FACTORY_STICK1, d)) s2_cal_parse(d, sizeof d, &s->cal[slot]);
}

// The console's pairing: this host's address and two keys of the host's choosing (switch2_proto.h, Bonding). From then
// on a button press wakes the controller toward this host. A failed step leaves it playing, but needing Sync again.
static void bond(S2Session *s) {
    uint8_t host[S2_PAIR_HOST_LEN], k[16], key[S2_PAIR_KEY_LEN], zero = 0;
    s2_pair_host_payload(host, s->host);
    int ok = command(s, S2_CMD_PAIR, S2_SUB_PAIR_HOST, host, sizeof host);
    for (int i = 0; ok && i < 2; i++) {
        for (int j = 0; j < 16; j++) k[j] = (uint8_t)SDL_rand_bits();
        s2_pair_key_payload(key, k);
        ok = command(s, S2_CMD_PAIR, i ? S2_SUB_PAIR_KEY2 : S2_SUB_PAIR_KEY1, key, sizeof key);
    }
    ok = ok && command(s, S2_CMD_PAIR, S2_SUB_PAIR_DONE, &zero, 1);
    rt_log("input: %s %s", kind(s->pid), ok ? "paired with this PC: from now on a press of any of its buttons connects it"
                                            : "not paired (no answer): it plays now, but needs Sync again next time");
}

// ---- the gamepad

static bool SDLCALL pad_rumble(void *userdata, Uint16 low, Uint16 high) {
    atomic_store(&((S2Pad *)userdata)->rumble, (uint32_t)low << 16 | high);
    return true;
}

static int attach(S2Pad *p) {
    static const SDL_VirtualJoystickSensorDesc sensors[2] = {{SDL_SENSOR_ACCEL, 33.3f}, {SDL_SENSOR_GYRO, 33.3f}};
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    // Nintendo's ids: SDL then labels it as a Switch Pro Controller (A on the east button), as the input's layout expects
    d.vendor_id = S2_VENDOR_ID;
    d.product_id = S2_PID_PRO;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.nsensors = 2;
    d.sensors = sensors;
    d.name = p->pro ? "Pro Controller 2 (Bluetooth)" : "Joy-Con 2 (Bluetooth)";
    d.userdata = p;
    d.Rumble = pad_rumble;
    p->id = SDL_AttachVirtualJoystick(&d);
    p->js = p->id ? SDL_OpenJoystick(p->id) : NULL;
    if (!p->js) rt_log("input: Switch 2 controller: no virtual gamepad (%s)", SDL_GetError());
    if (p->js && p->index < S2_BATTERY_SLOTS) {
        plat_mutex_lock(&hd_lock);
        memset(&hd[p->index], 0, sizeof hd[p->index]);
        hd[p->index].id = p->id;
        plat_mutex_unlock(&hd_lock);
    }
    return p->js != NULL;
}

static void detach(S2Pad *p) {
    if (p->index < S2_BATTERY_SLOTS) {
        atomic_store(&battery[p->index], 0);
        plat_mutex_lock(&hd_lock);
        memset(&hd[p->index], 0, sizeof hd[p->index]);
        plat_mutex_unlock(&hd_lock);
    }
    if (p->js) SDL_CloseJoystick(p->js);
    if (p->id) SDL_DetachVirtualJoystick(p->id);
    p->js = NULL, p->id = 0;
}

// Its buttons (HidNpadButton bits, s2_pad_input) as an SDL gamepad's: A east, as on a Switch Pro Controller
static void push_buttons(S2Pad *p, uint64_t b, const int32_t st[4]) {
    static const struct { int bit; SDL_GamepadButton sdl; } map[] = {
        {B_A, SDL_GAMEPAD_BUTTON_EAST}, {B_B, SDL_GAMEPAD_BUTTON_SOUTH}, {B_X, SDL_GAMEPAD_BUTTON_NORTH},
        {B_Y, SDL_GAMEPAD_BUTTON_WEST}, {B_STICK_L, SDL_GAMEPAD_BUTTON_LEFT_STICK},
        {B_STICK_R, SDL_GAMEPAD_BUTTON_RIGHT_STICK}, {B_L, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {B_R, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {B_PLUS, SDL_GAMEPAD_BUTTON_START}, {B_MINUS, SDL_GAMEPAD_BUTTON_BACK},
        {B_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {B_UP, SDL_GAMEPAD_BUTTON_DPAD_UP},
        {B_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT}, {B_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) SDL_SetJoystickVirtualButton(p->js, map[i].sdl, (b >> map[i].bit) & 1);
    SDL_SetJoystickVirtualButton(p->js, SDL_GAMEPAD_BUTTON_GUIDE, s2_pad_home(&p->st));
    // A virtual gamepad's trigger axes run over the whole axis, released at SDL_JOYSTICK_AXIS_MIN: SDL turns them into
    // 0..32767 as (value + 32768) / 2, so released at 0 a trigger read as half pressed (input.c TRIGGER_ON)
    SDL_SetJoystickVirtualAxis(p->js, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, (b >> B_ZL) & 1 ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
    SDL_SetJoystickVirtualAxis(p->js, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, (b >> B_ZR) & 1 ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
    // y up here, down in SDL
    SDL_SetJoystickVirtualAxis(p->js, SDL_GAMEPAD_AXIS_LEFTX, (Sint16)st[0]);
    SDL_SetJoystickVirtualAxis(p->js, SDL_GAMEPAD_AXIS_LEFTY, (Sint16)-st[1]);
    SDL_SetJoystickVirtualAxis(p->js, SDL_GAMEPAD_AXIS_RIGHTX, (Sint16)st[2]);
    SDL_SetJoystickVirtualAxis(p->js, SDL_GAMEPAD_AXIS_RIGHTY, (Sint16)-st[3]);
}

// One controller's motion in the Switch frame (s2_motion: G, rad/s), handed to SDL in its own axes, the inverse of
// input.c's input_motion_axes: SDL x = Switch x, SDL y = Switch z, SDL z = -Switch y; acceleration as a push, in m/s^2
static void push_motion(S2Session *s, const s2_dev *dev, uint64_t at) {
    float acc[3], rot[3];
    const float *bias = s->rest.have ? s->rest.bias : s->have_factory ? s->factory : NULL;
    // TOTK_SWITCH2_MAP: the six signed channels instead of the controller's own map (switch2_proto.h s2_map_parse)
    static int8_t user_map[6];
    static int user = -1;
    if (user < 0) {
        user = s2_map_parse(getenv("TOTK_SWITCH2_MAP"), user_map);
        if (user) rt_log("input: Switch 2 controllers: motion axes from TOTK_SWITCH2_MAP");
    }
    s2_motion(dev->r.raw, user ? user_map : s2_pad_map(&s->pad->st, dev), bias, S2_GYRO_RAD, acc, rot);
    float a[3] = {-acc[0] * SDL_STANDARD_GRAVITY, -acc[2] * SDL_STANDARD_GRAVITY, acc[1] * SDL_STANDARD_GRAVITY};
    float g[3] = {rot[0], rot[2], -rot[1]};
    SDL_SendJoystickVirtualSensorData(s->pad->js, SDL_SENSOR_ACCEL, at, a, 3);
    SDL_SendJoystickVirtualSensorData(s->pad->js, SDL_SENSOR_GYRO, at, g, 3);
}

// After a report: the pad's battery for switch2_battery, and a log line with the voltage when its band changes (the
// bands are unmeasured: players' logs tell the voltages a controller really goes through)
static void battery_update(S2Pad *p) {
    uint16_t mv = s2_pad_battery_mv(&p->st);
    int band = s2_battery_band_next(p->band, mv);
    if (band != p->band && band != S2_BATTERY_UNKNOWN) {
        static const char *const names[] = {"unknown", "low", "medium", "high"};
        rt_log("input: %s battery %u mV (%s)", p->pro ? "Pro Controller 2" : "Joy-Con 2", mv, names[band]);
    }
    p->band = band;
    if (p->index < S2_BATTERY_SLOTS)
        atomic_store(&battery[p->index], (uint64_t)p->id << 32 | (uint64_t)band << 16 | mv);
}

int switch2_battery(uint32_t sdl_id, int *mv) {
    for (int i = 0; sdl_id && i < S2_BATTERY_SLOTS; i++) {
        uint64_t v = atomic_load(&battery[i]);
        if ((uint32_t)(v >> 32) != sdl_id) continue;
        if (mv) *mv = (int)(v & 0xFFFF);
        return (int)(v >> 16 & 0xFFFF);
    }
    return S2_BATTERY_UNKNOWN;
}

int switch2_owns(uint32_t sdl_id) {
    int ours = 0;
    plat_mutex_lock(&hd_lock);
    for (int i = 0; sdl_id && i < S2_BATTERY_SLOTS && !ours; i++) ours = hd[i].id == sdl_id;
    plat_mutex_unlock(&hd_lock);
    return ours;
}

int switch2_rumble_hd(uint32_t sdl_id, const float v[2][4]) {
    s2_haptic m[2];
    for (int side = 0; side < 2; side++) {
        float lo = v[side][0], hi = v[side][2];
        s2_rumble_cap(&lo, &hi);   // the console's limit on the two bands together
        m[side] = s2_haptic_from(lo, v[side][1], hi, v[side][3]);
    }
    uint64_t now = switch2_now_ns();
    int ours = 0;
    plat_mutex_lock(&hd_lock);
    for (int i = 0; sdl_id && i < S2_BATTERY_SLOTS && !ours; i++)
        if (hd[i].id == sdl_id) {
            s2_rumble_log_add(&hd[i].log, now, m);
            hd[i].last = now;
            ours = 1;
        }
    plat_mutex_unlock(&hd_lock);
    return ours;
}

static s2_dev *dev_of(S2Pad *p, uint16_t pid) {
    return pid == S2_PID_PRO ? &p->st.pro : pid == S2_PID_JOYCON_L ? &p->st.left : &p->st.right;
}

// A Joy-Con joins a pad that holds the other one alone; anything else starts a pad (mkw-neo's s2_pad_for)
static S2Pad *pad_for(uint16_t pid, int *fresh) {
    *fresh = 0;
    if (pid != S2_PID_PRO)
        for (S2Pad *p = pads; p; p = p->next)
            if (!p->pro && ((pid == S2_PID_JOYCON_L && p->right && !p->left) || (pid == S2_PID_JOYCON_R && p->left && !p->right)))
                return p;
    S2Pad *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    int used = 0;
    for (S2Pad *q = pads; q; q = q->next) used |= 1 << q->index;
    while (used & 1 << p->index) p->index++;
    p->next = pads, pads = p, *fresh = 1;
    return p;
}

static void unlink_pad(S2Pad *p) {
    S2Pad **l = &pads;
    while (*l && *l != p) l = &(*l)->next;
    if (*l) *l = p->next;
}

static void set_leds(S2Session *s) {
    uint8_t pl[4] = {s2_led_pattern[s->pad->index & 7], 0, 0, 0}, f[12];
    size_t n = s2_command(f, sizeof f, S2_CMD_LEDS, S2_SUB_LEDS_SET, pl, 4);
    if (n) s->ops->write_cmd(s->link, f, n);
}

// ---- a session

S2Session *switch2_new(S2Link *l, const S2LinkOps *ops, uint16_t pid, int use, const uint8_t host[6]) {
    S2Session *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->link = l, s->ops = ops, s->pid = pid, s->use = use;
    if (host) memcpy(s->host, host, 6);
    s2_cal_default(&s->cal[0]);
    s2_cal_default(&s->cal[1]);
    return s;
}

int switch2_handshake(S2Session *s) {
    const uint16_t pid = s->pid;
    uint8_t info[0x40], gyro[16];
    uint16_t vid, real;
    char serial[15];
    if (!read_memory(s, sizeof info, S2_ADDR_INFO, info) || !s2_info_parse(info, sizeof info, &vid, &real, serial) ||
        vid != S2_VENDOR_ID) {
        rt_log("input: %s: it did not answer the identity read", kind(pid));
        return 0;
    }
    s->pid = real;
    if (s->use == S2_ADV_PAIR) bond(s);
    if (read_memory(s, sizeof gyro, S2_ADDR_GYRO_CAL, gyro) && s2_gyro_cal_usable(s->pid))
        s->have_factory = s2_gyro_cal_parse(gyro, sizeof gyro, s->factory);
    read_cal(s, 0);
    if (s->pid == S2_PID_PRO) read_cal(s, 1);
    uint8_t feat[4] = {S2_FEATURES, 0, 0, 0};
    command(s, S2_CMD_FEATURE, S2_SUB_FEATURE_SET, feat, 4);
    if (!command(s, S2_CMD_FEATURE, S2_SUB_FEATURE_ENABLE, feat, 4)) {
        rt_log("input: %s: enabling its reports was refused", kind(s->pid));
        return 0;
    }
    int fresh;
    S2Pad *p = pad_for(s->pid, &fresh);
    if (!p || (fresh && !attach(p))) {
        if (p && fresh) unlink_pad(p), free(p);
        return 0;
    }
    *(s->pid == S2_PID_PRO ? &p->pro : s->pid == S2_PID_JOYCON_L ? &p->left : &p->right) = s;
    s2_dev *d = dev_of(p, s->pid);
    memset(d, 0, sizeof *d);
    d->have = 1, d->pid = s->pid, d->cal[0] = s->cal[0], d->cal[1] = s->cal[1];
    s->pad = p;
    set_leds(s);
    s->next_led = switch2_now_ns() + S2_LED_NS;
    if (!s->ops->start_input(s->link)) rt_log("input: %s: its reports could not be subscribed", kind(s->pid));
    rt_log("input: %s %.14s connected over Bluetooth%s", kind(s->pid), serial, fresh ? "" : ", with the Joy-Con already there");
    return 1;
}

void switch2_report(S2Session *s, const uint8_t *d, size_t n, uint64_t at) {
    S2Pad *p = s->pad;
    s2_dev *dev = p ? dev_of(p, s->pid) : NULL;
    if (!dev || !s2_report_parse(d, n, &dev->r)) return;
    if (!s->rate_t0) s->rate_t0 = at, s->rate_c0 = dev->r.clock_ms;
    else if (!s->rate_done && at - s->rate_t0 >= 5000000000ull) {
        s->rate_done = 1;
        rt_log("input: %s: %.1f reports a second over its first 5 s, %.1f ms apart on its own clock", kind(s->pid),
               s->rate_n / ((at - s->rate_t0) / 1e9), s->rate_n ? (double)(dev->r.clock_ms - s->rate_c0) / s->rate_n : 0.0);
        if (s->ops->log_link) s->ops->log_link(s->link);
    }
    s->rate_n++;
    uint64_t t = s2_clock_sample(&s->clk, dev->r.clock_ms, at, NULL);
    s2_rest_update(&s->rest, dev->r.raw);
    uint64_t b;
    int32_t st[4];
    if (!p->js || !s2_pad_input(&p->st, &b, st)) return;
    push_buttons(p, b, st);
    battery_update(p);
    // a pair's motion is its right Joy-Con's, as SDL gives one gyro per gamepad
    if (s2_pad_motion_dev(&p->st, 1) == dev) push_motion(s, dev, t);
}

// One packet every ~15 ms: three 5 ms samples a block, the first block for the left grip (or a Joy-Con's own actuator),
// the second for the right grip. While the game streams HD values (switch2_rumble_hd, within S2_HD_STALE_NS) the
// samples are its values in time order (s2_rumble_slot: each value plays once, in the first slot that ends after it was
// set), each side to its own grip on a Pro Controller 2, a Joy-Con of a pair its own side, a Joy-Con alone the stronger
// of the two (s2_rumble_pick). Otherwise the pad's two SDL motors play as the low band at 160 Hz and the high band at
// 320 Hz. After the last non-silent packet three silent ones go out (one write without response can be lost).
static void rumble(S2Session *s) {
    if (!s->ops->write_vib) return;
    S2Pad *p = s->pad;
    s2_dev *dev = dev_of(p, s->pid);
    s2_haptic a[3], b[3];
    int on = 0, streamed = 0;
    uint64_t now = switch2_now_ns();
    if (p->index < S2_BATTERY_SLOTS) {
        plat_mutex_lock(&hd_lock);
        if (hd[p->index].id == (uint32_t)p->id && hd[p->index].last && now - hd[p->index].last < S2_HD_STALE_NS) {
            streamed = 1;
            unsigned motors = s2_rumble_motors(&p->st, dev);
            s2_rumble_resync(&s->cur, now, S2_HD_LAG_NS);
            for (int i = 0; i < 3; i++) {
                s2_rumble_slot(&hd[p->index].log, &s->cur, motors);
                s2_rumble_pick(&p->st, dev, s->cur.now, &a[i], &b[i]);
                on |= s2_haptic_on(&a[i]) || s2_haptic_on(&b[i]);
            }
        } else if (hd[p->index].id == (uint32_t)p->id) {   // the next stream starts after what is logged now
            memset(&s->cur, 0, sizeof s->cur);
            s->cur.next = hd[p->index].log.n;
        }
        plat_mutex_unlock(&hd_lock);
    }
    if (!streamed) {
        uint32_t v = atomic_load(&p->rumble);
        float low = (float)(v >> 16) / 65535.f, high = (float)(v & 0xFFFF) / 65535.f;
        on = low > 0 || high > 0;
        s2_rumble_cap(&low, &high);
        s2_haptic h = s2_haptic_from(low, 160.f, high, 320.f);
        for (int i = 0; i < 3; i++) a[i] = b[i] = h;
    }
    if (!on) {
        if (!s->quiet) return;
        s->quiet--;
        static const s2_haptic silent = {0};   // amplitude 0 in both bands: the frequencies do not matter
        for (int i = 0; i < 3; i++) a[i] = b[i] = silent;
    } else s->quiet = 3;
    uint8_t out[33];
    size_t n = s2_rumble_packet3(out, a, b, s->vseq++);
    s->ops->write_vib(s->link, out, n);
}

void switch2_tick(void) {
    uint64_t now = switch2_now_ns();
    for (S2Pad *p = pads; p; p = p->next)
        for (int i = 0; i < 3; i++) {
            S2Session *s = i == 0 ? p->pro : i == 1 ? p->left : p->right;
            if (!s) continue;
            rumble(s);
            if (now >= s->next_led) set_leds(s), s->next_led = now + S2_LED_NS;
        }
}

void switch2_close(S2Session *s) {
    S2Pad *p = s->pad;
    if (p) rt_log("input: %s disconnected", kind(s->pid));
    if (p) {
        memset(dev_of(p, s->pid), 0, sizeof(s2_dev));
        *(s->pid == S2_PID_PRO ? &p->pro : s->pid == S2_PID_JOYCON_L ? &p->left : &p->right) = NULL;
        if (!p->pro && !p->left && !p->right) {
            detach(p);
            unlink_pad(p);
            free(p);
        }
    }
    free(s);
}

#ifndef _WIN32
// No Bluetooth LE transport on this OS here (dkb-pc-recomp's BlueZ one for Linux is not carried over; macOS:
// CoreBluetooth is not ported): the setting does nothing
void switch2_start(void) {
    if (cfg_int("TOTK_SWITCH2")) rt_log("input: Switch 2 controllers over Bluetooth are not available on this OS");
}
#endif
