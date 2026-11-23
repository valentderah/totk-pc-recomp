// Ported from adriadam10/mkw-recomp (MIT, Copyright (c) 2026 Adrià), runtime/input/switch2_proto.h at eb8fe198 through dkb-pc-recomp; TOTK_SWITCH2 for MKW_SWITCH2.
// Nintendo Switch 2 controllers over Bluetooth LE (Pro Controller 2, Joy-Con 2 left and right): the protocol and the state a pad is made of. Pure C, no platform headers, so
// tests/t_switch2.c checks it on any machine; pad_switch2_apple.m is the CoreBluetooth transport the Mac and iOS front ends share.
//
// Where it comes from: the GATT identifiers, command framing and report layout are the community's reverse engineering of the controllers (Peter Sharma's "Finally the Controller
// Works", ndeadly/switch2_controller_research, trevlars/switch2-controllers-linux, Nadeflore/switch2-controllers and others; the same protocol seedds/nxnative PR 645 uses on iOS).
// This code was written from those notes; none of it is copied from them.
// Observed here on real hardware (a left and a right Joy-Con 2, macOS 27): the Joy-Con 2s advertise product ids 0x2067 (left) and 0x2066 (right) with Nintendo's company id
// 0x0553; they use the same three characteristics as the Pro Controller 2 (input, command, command reply); after the same handshake they send the same 63-byte input report; a
// Joy-Con's one stick sits in the stick slot of its side (left: first pair, right: second pair) but its calibration is stored in the first stick slot (memory 0x130A8).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
// Ported from seedds/mkw-neo (MIT, Copyright (c) seedds), dm/host/switch2_proto.h at cfa62ddc: unchanged but for this block,
// which gives its DM_* names our values (libnx HidNpadButton bits, as input_map.h's B_* and dm_input.h both use).
#include "input_map.h"
enum {
    DM_BTN_A = 1u << B_A, DM_BTN_B = 1u << B_B, DM_BTN_X = 1u << B_X, DM_BTN_Y = 1u << B_Y,
    DM_BTN_LS = 1u << B_STICK_L, DM_BTN_RS = 1u << B_STICK_R, DM_BTN_L = 1u << B_L, DM_BTN_R = 1u << B_R,
    DM_BTN_ZL = 1u << B_ZL, DM_BTN_ZR = 1u << B_ZR, DM_BTN_PLUS = 1u << B_PLUS, DM_BTN_MINUS = 1u << B_MINUS,
    DM_BTN_LEFT = 1u << B_LEFT, DM_BTN_UP = 1u << B_UP, DM_BTN_RIGHT = 1u << B_RIGHT, DM_BTN_DOWN = 1u << B_DOWN,
    DM_BTN_LSL = 1u << 24, DM_BTN_LSR = 1u << 25, DM_BTN_RSL = 1u << 26, DM_BTN_RSR = 1u << 27,   // HidNpadButton_LeftSL..
};
#define DM_STICK_MAX 32767

#define S2_COMPANY_ID 0x0553
#define S2_VENDOR_ID  0x057E
enum { S2_PID_JOYCON_R = 0x2066, S2_PID_JOYCON_L = 0x2067, S2_PID_PRO = 0x2069 };

// GATT characteristics of the proprietary service. Every write is Write Without Response.
#define S2_UUID_INPUT   "AB7DE9BE-89FE-49AD-828F-118F09DF7FD2"   // notifications: 63-byte input reports
// A vendor descriptor on the input characteristic: the official software writes 85 00 to it before turning the
// reports on, which moves the controller off its idle report cadence (FlexInput, x-iso, MIT:
// crates/joycon2/src/protocol.rs:126-131, from a capture; seen on Joy-Con 2, not checked on a Pro Controller 2)
#define S2_UUID_RATE_DESC "679D5510-5A24-4DEE-9557-95DF80486ECB"
#define S2_RATE_PAYLOAD 0x85, 0x00
#define S2_UUID_CMD_WR  "649D4AC9-8EB7-4E6C-AF44-1EA54FE5F005"   // commands
#define S2_UUID_CMD_RSP "C765A961-D9D8-4D36-A20A-5315B111836A"   // command replies (notifications)

enum { S2_CMD_MEMORY = 0x02, S2_CMD_LEDS = 0x09, S2_CMD_FEATURE = 0x0C, S2_CMD_PAIR = 0x15 };
enum { S2_SUB_MEMORY_READ = 0x04, S2_SUB_LEDS_SET = 0x07, S2_SUB_FEATURE_SET = 0x02, S2_SUB_FEATURE_ENABLE = 0x04,
       S2_SUB_PAIR_HOST = 0x01, S2_SUB_PAIR_KEY1 = 0x04, S2_SUB_PAIR_KEY2 = 0x02, S2_SUB_PAIR_DONE = 0x03 };
// Features to switch on: buttons, sticks, motion, battery current and the magnetometer (the mask the console uses; all bits makes a Joy-Con 2 report phantom ZL / ZR)
#define S2_FEATURES (0x01 | 0x02 | 0x04 | 0x20 | 0x80)
// The Joy-Con 2's mouse sensor (on its rail side): its own feature bit; the console sets it while a game reads the sensor. Measured on a right Joy-Con 2 slid on a desk: report 0x10 and
// 0x12 are int16 position counters that wrap (x right, y toward the player), 0x14 and 0x16 jump from about 1,600 and 150 on a surface to about 4,600 and 3,200 when lifted
#define S2_FEATURE_MOUSE 0x10
#define S2_MOUSE_SURFACE_MAX 1000   // 0x16 below this: on a surface
enum { S2_ADDR_INFO = 0x00013000u, S2_ADDR_FACTORY_STICK1 = 0x000130A8u, S2_ADDR_FACTORY_STICK2 = 0x000130E8u, S2_ADDR_USER_STICK1 = 0x001FC042u, S2_ADDR_USER_STICK2 = 0x001FC062u };

// Raw button bits (32-bit little endian at report offset 4; the top three bits are always set). The right Joy-Con's rail buttons are 0x10 / 0x20 and the left's 0x100000 / 0x200000
// (the Switch 1 order: SR before SL).
enum {
    S2_Y = 0x1, S2_X = 0x2, S2_B = 0x4, S2_A = 0x8, S2_SR_R = 0x10, S2_SL_R = 0x20, S2_R = 0x40, S2_ZR = 0x80, S2_MINUS = 0x100, S2_PLUS = 0x200,
    S2_RSTICK = 0x400, S2_LSTICK = 0x800, S2_HOME = 0x1000, S2_CAPTURE = 0x2000, S2_C = 0x4000,
    S2_DOWN = 0x10000, S2_UP = 0x20000, S2_RIGHT = 0x40000, S2_LEFT = 0x80000, S2_SR_L = 0x100000, S2_SL_L = 0x200000, S2_L = 0x400000, S2_ZL = 0x800000,
    S2_GR = 0x1000000, S2_GL = 0x2000000,
};

static inline uint16_t s2_u16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static inline uint32_t s2_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline int16_t s2_s16(const uint8_t *p) { return (int16_t)s2_u16(p); }

typedef struct { uint32_t clock_ms, buttons; uint16_t lx, ly, rx, ry; int16_t raw[6]; uint16_t battery_mv; int16_t mouse_x, mouse_y; uint16_t mouse_q, mouse_s; } s2_report;   // clock_ms: the controller's own clock; raw: accel xyz, gyro xyz in counts; mouse_*: S2_FEATURE_MOUSE

// A 63-byte input notification (the same characteristic also carries longer audio packets; the length tells them apart)
static inline int s2_report_parse(const uint8_t *d, size_t n, s2_report *r) {
    if (r) memset(r, 0, sizeof *r);
    if (!d || !r || n < 0x3C || n > 0x50) return 0;
    r->clock_ms = s2_u32(d); r->buttons = s2_u32(d + 4);
    uint32_t a = (uint32_t)d[10] | (uint32_t)d[11] << 8 | (uint32_t)d[12] << 16, b = (uint32_t)d[13] | (uint32_t)d[14] << 8 | (uint32_t)d[15] << 16;
    r->lx = (uint16_t)(a & 0xFFF); r->ly = (uint16_t)(a >> 12); r->rx = (uint16_t)(b & 0xFFF); r->ry = (uint16_t)(b >> 12);
    r->battery_mv = s2_u16(d + 0x1F);
    for (int i = 0; i < 6; i++) r->raw[i] = s2_s16(d + 0x30 + 2 * i);
    r->mouse_x = s2_s16(d + 0x10); r->mouse_y = s2_s16(d + 0x12); r->mouse_q = s2_u16(d + 0x14); r->mouse_s = s2_u16(d + 0x16);
    return 1;
}
// The movement between two counter readings (they wrap at 16 bits), and whether the sensor sees a surface
static inline int32_t s2_mouse_delta(int16_t now, int16_t before) { return (int16_t)(uint16_t)((uint16_t)now - (uint16_t)before); }
static inline int s2_mouse_on_surface(const s2_report *r) { return r->mouse_s < S2_MOUSE_SURFACE_MAX; }
// Whether the report carries the sensor at all: with S2_FEATURE_MOUSE off the four fields are zero (on a surface 0x14 reads about 1,600, lifted about 4,600)
static inline int s2_mouse_present(const s2_report *r) { return r->mouse_x || r->mouse_y || r->mouse_q || r->mouse_s; }

// Command frame: cmd, 0x91, transport 0x01 (Bluetooth), subcommand, 0, payload length, 0, 0, payload. Returns the frame length, 0 when it does not fit.
static inline size_t s2_command(uint8_t *out, size_t cap, uint8_t cmd, uint8_t sub, const uint8_t *payload, size_t n) {
    if (!out || n > 0xFF || cap < 8 + n) return 0;
    out[0] = cmd; out[1] = 0x91; out[2] = 0x01; out[3] = sub; out[4] = 0; out[5] = (uint8_t)n; out[6] = out[7] = 0;
    if (n) memcpy(out + 8, payload, n);
    return 8 + n;
}
// Memory read payload: length, 0x7E, 0, 0, address (little endian). The reply is the 8-byte frame header, the same 8 bytes echoed (length, 0x7E, 0, 0, address), then the data.
static inline void s2_memory_payload(uint8_t out[8], uint8_t len, uint32_t addr) {
    out[0] = len; out[1] = 0x7E; out[2] = out[3] = 0;
    out[4] = (uint8_t)addr; out[5] = (uint8_t)(addr >> 8); out[6] = (uint8_t)(addr >> 16); out[7] = (uint8_t)(addr >> 24);
}
#define S2_REPLY_DATA 16   // offset of the data in a memory-read reply
// A command reply is for command cmd when it starts with it and byte 1 is 1 (done) or 2 (status)
static inline int s2_reply_is(const uint8_t *d, size_t n, uint8_t cmd) { return d && n >= 8 && d[0] == cmd && (d[1] == 1 || d[1] == 2); }

// Advertisement (manufacturer data after the two company-id bytes): vendor id at 3, product id at 5; six bytes from offset 10 are the host the controller wakes toward (little
// endian), all zero in pairing mode (Sync held until the player lights sweep)
typedef struct { uint16_t vid, pid; int pairing; uint8_t host[6]; } s2_advert;
static inline int s2_advert_parse(const uint8_t *d, size_t n, s2_advert *a) {
    if (a) memset(a, 0, sizeof *a);
    if (!d || !a || n < 16) return 0;
    a->vid = s2_u16(d + 3); a->pid = s2_u16(d + 5);
    a->pairing = 1; for (int i = 10; i < 16; i++) { a->host[i - 10] = d[i]; if (d[i]) a->pairing = 0; }
    return a->vid == S2_VENDOR_ID && (a->pid == S2_PID_PRO || a->pid == S2_PID_JOYCON_L || a->pid == S2_PID_JOYCON_R);
}
// What the host does with an advertisement. host: this host's Bluetooth address (little endian), NULL when it cannot be known (iOS has no API for it).
// In pairing mode the controller is bonded to this host (S2_ADV_PAIR), or used without bonding when the address is unknown (S2_ADV_TAKE: it needs Sync again next time).
// A wake toward this host is connected (S2_ADV_WAKE). A wake toward another host (a nearby Switch 2) is left alone, and nothing is written to it.
enum { S2_ADV_IGNORE, S2_ADV_WAKE, S2_ADV_PAIR, S2_ADV_TAKE };
static inline int s2_advert_use(const s2_advert *a, const uint8_t *host) {
    if (a->pairing) return host ? S2_ADV_PAIR : S2_ADV_TAKE;
    return host && !memcmp(a->host, host, 6) ? S2_ADV_WAKE : S2_ADV_IGNORE;
}

// Bonding, the console's 0x15 commands, in order: the host's address (S2_SUB_PAIR_HOST: 0x00, 0x02, the address twice, little endian), two 16-byte keys after a 0x00
// (S2_SUB_PAIR_KEY1, S2_SUB_PAIR_KEY2), then S2_SUB_PAIR_DONE with 0x00. From then on a button press wakes the controller with an advertisement naming this host, and it connects
// without Sync. The keys are the host's choice: fresh random keys work (tested on a left and a right Joy-Con 2: each bonded, woke and reconnected). The reply to the first key was
// the same bytes on both Joy-Cons and with any key sent; the reply to the second changes with the key. Nothing here uses the link keys afterwards: the GATT link is not encrypted.
#define S2_PAIR_HOST_LEN 14
#define S2_PAIR_KEY_LEN 17
static inline void s2_pair_host_payload(uint8_t out[S2_PAIR_HOST_LEN], const uint8_t host[6]) { out[0] = 0x00; out[1] = 0x02; memcpy(out + 2, host, 6); memcpy(out + 8, host, 6); }
static inline void s2_pair_key_payload(uint8_t out[S2_PAIR_KEY_LEN], const uint8_t key[16]) { out[0] = 0x00; memcpy(out + 1, key, 16); }

// The controller info block (0x40 bytes at S2_ADDR_INFO): serial at 2 (14 characters), vendor id at 18, product id at 20
static inline int s2_info_parse(const uint8_t *d, size_t n, uint16_t *vid, uint16_t *pid, char serial[15]) {
    if (!d || n < 0x16) return 0;
    *vid = s2_u16(d + 18); *pid = s2_u16(d + 20);
    memcpy(serial, d + 2, 14); serial[14] = 0;
    for (int i = 0; i < 14; i++) if (serial[i] < 0x20 || serial[i] > 0x7e) serial[i] = '?';
    return 1;
}

// Stick calibration (0x0B bytes): centre, maximum and minimum travel, each a packed 12-bit pair
typedef struct { float cx, cy, maxx, maxy, minx, miny; int valid; } s2_cal;
static inline void s2_cal_default(s2_cal *c) { c->cx = c->cy = 2048; c->maxx = c->maxy = c->minx = c->miny = 1200; c->valid = 0; }
static inline int s2_cal_blank(const uint8_t *d, size_t n) { return !d || n < 3 || (d[0] == 0xFF && d[1] == 0xFF && d[2] == 0xFF); }
static inline int s2_cal_parse(const uint8_t *d, size_t n, s2_cal *c) {
    if (!d || !c || n < 9 || s2_cal_blank(d, n)) return 0;
    uint32_t p = (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16, q = (uint32_t)d[3] | (uint32_t)d[4] << 8 | (uint32_t)d[5] << 16, r = (uint32_t)d[6] | (uint32_t)d[7] << 8 | (uint32_t)d[8] << 16;
    c->cx = (float)(p & 0xFFF); c->cy = (float)(p >> 12);
    c->maxx = (float)(q & 0xFFF); c->maxy = (float)(q >> 12);   // travel above and below the centre, not absolute positions
    c->minx = (float)(r & 0xFFF); c->miny = (float)(r >> 12);
    c->valid = c->maxx > 100 && c->maxy > 100 && c->minx > 100 && c->miny > 100;
    return c->valid;
}
static inline float s2_cal_axis(float v, float centre, float up, float down, float dead) {
    float s = v - centre;
    if (s > dead) return up > 0 ? fminf(s / up, 1.f) : 0.f;
    if (s < -dead) return down > 0 ? -fminf(-s / down, 1.f) : 0.f;
    return 0.f;
}
// A stick in -1..1 (x right, y up), a deadzone of dead raw counts around the centre
static inline void s2_cal_apply(const s2_cal *c, uint16_t rawx, uint16_t rawy, float dead, float *x, float *y) {
    s2_cal d; if (!c || !c->valid) { s2_cal_default(&d); c = &d; }
    *x = s2_cal_axis((float)rawx, c->cx, c->maxx, c->minx, dead);
    *y = s2_cal_axis((float)rawy, c->cy, c->maxy, c->miny, dead);
}

// Player LED patterns of the console for players 1..8
static const uint8_t s2_led_pattern[8] = {0x01, 0x03, 0x07, 0x0F, 0x09, 0x05, 0x0D, 0x06};

// ---- One controller (one Bluetooth device) and the pad a player holds (one or two of them) ----

typedef struct { int have; uint16_t pid; s2_report r; s2_cal cal[2]; } s2_dev;   // cal[0]: the first stick slot (a Joy-Con's only stick), cal[1]: the second (Pro Controller 2's right stick)
typedef struct { s2_dev pro, left, right; } s2_pad_state;                       // a Pro Controller 2 in pro, or Joy-Con 2s in left / right

// A device's own buttons as NpadButtons. A Joy-Con held on its own (alone: 1) is used sideways, so its rail buttons are the shoulder buttons and its stick the left stick.
static inline uint64_t s2_buttons(uint32_t b, int joycon, int alone) {
    uint64_t m = 0;
    if (b & S2_A) m |= DM_BTN_A; if (b & S2_B) m |= DM_BTN_B; if (b & S2_X) m |= DM_BTN_X; if (b & S2_Y) m |= DM_BTN_Y;
    if (b & S2_L) m |= DM_BTN_L; if (b & S2_R) m |= DM_BTN_R; if (b & S2_ZL) m |= DM_BTN_ZL; if (b & S2_ZR) m |= DM_BTN_ZR;
    if (b & S2_PLUS) m |= DM_BTN_PLUS; if (b & S2_MINUS) m |= DM_BTN_MINUS; if (b & S2_LSTICK) m |= DM_BTN_LS; if (b & S2_RSTICK) m |= DM_BTN_RS;
    if (b & S2_LEFT) m |= DM_BTN_LEFT; if (b & S2_UP) m |= DM_BTN_UP; if (b & S2_RIGHT) m |= DM_BTN_RIGHT; if (b & S2_DOWN) m |= DM_BTN_DOWN;
    if (joycon && alone) {
        if (b & (S2_SL_L | S2_SL_R)) m |= DM_BTN_L;
        if (b & (S2_SR_L | S2_SR_R)) m |= DM_BTN_R;
    }
    return m;
}

#define S2_DEADZONE 40.f   // raw counts around the centre

// The pad's buttons and sticks (-32767..32767, y up): a Pro Controller 2's as they are; two Joy-Cons together are a Pro Controller (the left's stick is the left stick, the
// right's the right stick); one Joy-Con alone is used sideways. Returns 0 when the pad holds no controller.
static inline uint64_t s2_joy_buttons(uint32_t b, int right);   // (below)
static inline int s2_pad_input(const s2_pad_state *p, uint64_t *buttons, int32_t st[4]) {
    float x, y; uint64_t m = 0; st[0] = st[1] = st[2] = st[3] = 0;
#define STICK(i, dev, c, rx_, ry_) do { s2_cal_apply(&(dev)->cal[c], rx_, ry_, S2_DEADZONE, &x, &y); st[i] = (int32_t)lrintf(x * DM_STICK_MAX); st[(i) + 1] = (int32_t)lrintf(y * DM_STICK_MAX); } while (0)
    if (p->pro.have) {
        m = s2_buttons(p->pro.r.buttons, 0, 0);
        STICK(0, &p->pro, 0, p->pro.r.lx, p->pro.r.ly); STICK(2, &p->pro, 1, p->pro.r.rx, p->pro.r.ry);
    } else if (p->left.have && p->right.have) {
        // each Joy-Con's own side only: a Joy-Con's report also has bits in the other side's places, and taken from both
        // ZL / ZR read as held without a press on a pair
        m = s2_joy_buttons(p->left.r.buttons, 0) | s2_joy_buttons(p->right.r.buttons, 1);
        m &= (1u << 16) - 1;   // (the rail buttons, DM_BTN_LSL..: not a pair's)
        STICK(0, &p->left, 0, p->left.r.lx, p->left.r.ly); STICK(2, &p->right, 0, p->right.r.rx, p->right.r.ry);
    } else if (p->left.have) {
        m = s2_buttons(p->left.r.buttons, 1, 1); STICK(0, &p->left, 0, p->left.r.lx, p->left.r.ly);
    } else if (p->right.have) {
        m = s2_buttons(p->right.r.buttons, 1, 1); STICK(0, &p->right, 0, p->right.r.rx, p->right.r.ry);   // sideways, its stick is the left stick
    } else return 0;
#undef STICK
    *buttons = m;
    return 1;
}

// Battery: the report gives the cell's voltage in mV (battery_mv), not a level. No curve to a percentage is known (a
// straight line from 3300 to 4150 mV reads under half with a full Pro Controller 2: KW0j/connectyourpro issue 5), so it
// is shown as three bands; the edges are Switch2Connect's (TommyWabg, GPL-3.0: the idea only), unmeasured here. The
// recorded Joy-Con 2 reports above read 3464-3495 mV.
enum { S2_BATTERY_UNKNOWN, S2_BATTERY_LOW, S2_BATTERY_MEDIUM, S2_BATTERY_HIGH };
enum { S2_BATTERY_MEDIUM_MV = 3125, S2_BATTERY_HIGH_MV = 3250, S2_BATTERY_HYSTERESIS_MV = 25 };
static inline int s2_battery_band(uint16_t mv) {
    if (mv < 2500 || mv > 5000) return S2_BATTERY_UNKNOWN;   // 0 before the first report, or not a cell's voltage
    return mv > S2_BATTERY_HIGH_MV ? S2_BATTERY_HIGH : mv > S2_BATTERY_MEDIUM_MV ? S2_BATTERY_MEDIUM : S2_BATTERY_LOW;
}
// The band after was for a new reading: it moves only 25 mV past an edge, as the voltage jitters under rumble
static inline int s2_battery_band_next(int was, uint16_t mv) {
    int now = s2_battery_band(mv);
    if (was == S2_BATTERY_UNKNOWN || now == S2_BATTERY_UNKNOWN || now == was) return now;
    int held = s2_battery_band((uint16_t)(now > was ? mv - S2_BATTERY_HYSTERESIS_MV : mv + S2_BATTERY_HYSTERESIS_MV));
    return held == S2_BATTERY_UNKNOWN ? now : held;
}
// A pad's voltage: its lowest controller's (a pair of Joy-Con 2 stops with the first that runs out); 0 none reported
static inline uint16_t s2_pad_battery_mv(const s2_pad_state *p) {
    const s2_dev *d[3] = {&p->pro, &p->left, &p->right};
    uint16_t lo = 0;
    for (int i = 0; i < 3; i++)
        if (d[i]->have && s2_battery_band(d[i]->r.battery_mv) && (!lo || d[i]->r.battery_mv < lo)) lo = d[i]->r.battery_mv;
    return lo;
}

// The pad's Home, A and B as raw buttons of its controllers (the Home hold-to-quit prompt reads them, as it reads GameController's)
static inline int s2_pad_home(const s2_pad_state *p) { return ((p->pro.have ? p->pro.r.buttons : 0) | (p->right.have ? p->right.r.buttons : 0)) & S2_HOME ? 1 : 0; }   // (Home is on the right Joy-Con: the left one's bit there is not a press)
static inline void s2_pad_ab(const s2_pad_state *p, int *a, int *b) {
    uint32_t m = (p->pro.have ? p->pro.r.buttons : 0) | (p->right.have ? p->right.r.buttons : 0);   // A and B are on the right side (a left Joy-Con alone has none: the prompt also takes the keyboard)
    *a |= (m & S2_A) != 0; *b |= (m & S2_B) != 0;
}

// Rumble. Each controller lists its own vibration channel: the right Joy-Con 2's is S2_UUID_VIB_JOYCON_R, the left one's S2_UUID_VIB_JOYCON_L (it has no FA19B0FB; this one takes its
// place in its channel list), the Pro Controller 2's S2_UUID_VIB_PRO (from the notes, untested). The report is a zero byte, then a 16-byte block per actuator (a Joy-Con 2 has one; the
// Pro Controller 2's second block is its right grip): 0x40 | the number of samples (1 to 3) << 4 | a 4-bit sequence, then that many five-byte samples, played in turn (0x50 plays
// only the first of three: the Switch 2's own encoder sets the count). A sample is two bands, low then high, each a 10-bit frequency code
// and a 10-bit amplitude, 40 bits little endian. The frequency code is logarithmic, 96 steps an octave with code 333 at 110 Hz (about 9.9 Hz at 0, 16 kHz at 1023): the Switch 2's own
// conversion from a game's VibrationValue. The amplitude is linear, 1023 at full. (An earlier reading of the code as plain hertz played every effect 3 to 5 times too low.)
#define S2_UUID_VIB_JOYCON_R "FA19B0FB-CD1F-46A7-84A1-BBB09E00C149"
#define S2_UUID_VIB_JOYCON_L "289326CB-A471-485D-A8F4-240C14F18241"
#define S2_UUID_VIB_PRO      "CC483F51-9258-427D-A939-630C31F72B05"
typedef struct { uint16_t lf_hz, lf_amp, hf_hz, hf_amp; } s2_haptic;   // *_hz: the frequency code (s2_freq_code), *_amp: 0..1023
static inline void s2_haptic_pack(const s2_haptic *v, uint8_t out[5]) {
    uint64_t x = (uint64_t)(v->lf_hz & 0x3FF) | (uint64_t)(v->lf_amp & 0x3FF) << 10 | (uint64_t)(v->hf_hz & 0x3FF) << 20 | (uint64_t)(v->hf_amp & 0x3FF) << 30;
    for (int i = 0; i < 5; i++) out[i] = (uint8_t)(x >> (8 * i));
}
static inline float s2_freq_hz(uint16_t code) { return 110.f * exp2f(((float)code - 333.f) / 96.f); }
// Hz to code as the console rounds it (checked against its own encoder on 200,000 random values): the hertz clamped to 9.935..16034, the code truncated, then of it and the next one
// the one whose frequency is nearer
static inline uint16_t s2_freq_code(float hz) {
    if (!(hz >= 9.935f)) hz = 9.935f; if (hz > 16034.f) hz = 16034.f;
    int c = (int)(96.f * log2f(hz) - 318.0105285644531f);
    if (c > 0x3FE) return (uint16_t)(c > 0x3FF ? 0x3FF : c);
    if (c < 0) c = 0;
    return (uint16_t)(fabsf(s2_freq_hz((uint16_t)(c + 1)) - hz) < fabsf(s2_freq_hz((uint16_t)c) - hz) ? c + 1 : c);
}
// 0..1 to 0..1023 the same way: truncated, then the nearer of it and the next
static inline uint16_t s2_amp_code(float a) {
    if (!(a > 0.f)) a = 0.f; if (a > 1.f) a = 1.f;
    int c = (int)(a * 1023.f); if (c > 0x3FE) return 0x3FF;
    return (uint16_t)(fabsf((float)(c + 1) / 1023.f - a) < fabsf((float)c / 1023.f - a) ? c + 1 : c);
}
// The console's total-strength limit for one sample: both bands scaled by 0.98, or so that they add up to 0.98 when they add up to more than 1
static inline void s2_rumble_cap(float *low, float *high) { float sum = *low + *high, k = sum > 1.f ? 0.98f / sum : 0.98f; *low *= k; *high *= k; }
// A game's vibration (amplitudes 0..1, frequencies in Hz) as a sample, coded as the console codes it
static inline s2_haptic s2_haptic_from(float amp_low, float freq_low, float amp_high, float freq_high) {
    s2_haptic v = {s2_freq_code(freq_low), s2_amp_code(amp_low), s2_freq_code(freq_high), s2_amp_code(amp_high)};
    return v;
}
static inline int s2_haptic_on(const s2_haptic *v) { return v->lf_amp || v->hf_amp; }
// Three samples per block, each its own (HD rumble: the game's values at 5 ms steps instead of one value repeated); the header says three
static inline size_t s2_rumble_packet3(uint8_t out[33], const s2_haptic first[3], const s2_haptic second[3], uint8_t seq) {
    size_t o = 0; out[o++] = 0x00;
    for (int blk = 0; blk < 2; blk++) { out[o++] = (uint8_t)(0x40 | 3 << 4 | (seq & 0x0F)); for (int i = 0; i < 3; i++) { s2_haptic_pack(blk ? &second[i] : &first[i], out + o); o += 5; } }
    return o;
}
// What one controller of a pad plays from the player's two motors (m[0] left, m[1] right, the full-key handles): a Pro Controller 2 both, one per block (its two grips); a Joy-Con of
// a pair its own side's, in both blocks; a Joy-Con alone (the whole pad, held sideways) the stronger of the two in each band
static inline void s2_rumble_pick(const s2_pad_state *p, const s2_dev *dev, const s2_haptic m[2], s2_haptic *a, s2_haptic *b) {
    if (dev == &p->pro) { *a = m[0]; *b = m[1]; return; }
    int right = dev == &p->right;
    if ((right ? p->left.have : p->right.have)) { *a = *b = m[right]; return; }
    s2_haptic v = m[0];
    if (m[1].lf_amp > v.lf_amp) { v.lf_amp = m[1].lf_amp; v.lf_hz = m[1].lf_hz; }
    if (m[1].hf_amp > v.hf_amp) { v.hf_amp = m[1].hf_amp; v.hf_hz = m[1].hf_hz; }
    *a = *b = v;
}
// The motors s2_rumble_pick reads for that controller: bit 0 left, bit 1 right
static inline unsigned s2_rumble_motors(const s2_pad_state *p, const s2_dev *dev) {
    if (dev == &p->pro) return 3;
    int right = dev == &p->right;
    return (right ? p->left.have : p->right.have) ? 1u << right : 3;
}

// Rumble timing. A game sets its vibration about every 5 ms and holds each value until the next, so a value set for one step is a 5 ms pulse; a packet's three samples are three
// such 5 ms slots. Each value the game sets plays once, in the first slot that ends after it was set, so a one-step pulse stays one sample long and no value is skipped. Without it
// a pulse lasts until the next packet (one to three samples), which doubles the energy of a run of short pulses. A slot with no new value repeats the one before. Values that pile
// up (a game setting them faster than one per slot) wait their turn, but none waits more than S2_RUMBLE_BACKLOG slots: an older one is passed over for a newer one then.
// The host records the player's values with their times (s2_rumble_log_add); each controller keeps its own place (s2_rumble_cursor) and takes its slots in time order.
#define S2_RUMBLE_SLOT_NS 5000000ull
#define S2_RUMBLE_BACKLOG 5
#define S2_RUMBLE_LOG 64
typedef struct { uint64_t t[S2_RUMBLE_LOG]; s2_haptic m[S2_RUMBLE_LOG][2]; uint64_t n; } s2_rumble_log;   // a player's values (both motors) and when each was set, oldest first
typedef struct { uint64_t next, end; s2_haptic now[2]; } s2_rumble_cursor;   // the first value not yet played, the end of the last slot, what is playing
static inline void s2_rumble_log_add(s2_rumble_log *l, uint64_t t, const s2_haptic m[2]) {
    uint64_t i = l->n % S2_RUMBLE_LOG; l->t[i] = t; l->m[i][0] = m[0]; l->m[i][1] = m[1]; l->n++;
}
static inline int s2_haptic_same(const s2_haptic *a, const s2_haptic *b) { return a->lf_hz == b->lf_hz && a->lf_amp == b->lf_amp && a->hf_hz == b->hf_hz && a->hf_amp == b->hf_amp; }
// The next slot (ending at c->end + one slot): the oldest value not yet played that was set before its end becomes what plays. A value that changes none of the motors this controller
// plays (motors, s2_rumble_motors) takes no slot.
static inline void s2_rumble_slot(const s2_rumble_log *l, s2_rumble_cursor *c, unsigned motors) {
    uint64_t end = c->end += S2_RUMBLE_SLOT_NS, late = S2_RUMBLE_BACKLOG * S2_RUMBLE_SLOT_NS;
    if (c->next > l->n) c->next = l->n;   // another player's log (the pad changed player): from its next value
    else if (l->n - c->next > S2_RUMBLE_LOG) c->next = l->n - S2_RUMBLE_LOG;   // overwritten: start at the oldest kept
    for (; c->next < l->n; c->next++) {
        uint64_t i = c->next % S2_RUMBLE_LOG;
        if (l->t[i] >= end) return;   // not set yet at this slot's end
        if (c->next + 1 < l->n && l->t[(c->next + 1) % S2_RUMBLE_LOG] + late < end) continue;   // a newer one is also overdue: pass this one over
        int changes = 0;
        for (int k = 0; k < 2; k++) if (motors >> k & 1 && !s2_haptic_same(&l->m[i][k], &c->now[k])) changes = 1;
        if (!changes) continue;
        c->now[0] = l->m[i][0]; c->now[1] = l->m[i][1]; c->next++;
        return;
    }
}
// Where a controller's slots stand at time now (in the log's clock): its slots run on 5 ms at a time, the last one of a packet ending about lag before now, so every value set
// within a slot is known when it is played. A cursor more than a packet behind or ahead of that (the first packet, or a tick that came late or early) starts again there.
static inline void s2_rumble_resync(s2_rumble_cursor *c, uint64_t now, uint64_t lag) {
    uint64_t want = now > lag + 3 * S2_RUMBLE_SLOT_NS ? now - lag - 3 * S2_RUMBLE_SLOT_NS : 0;   // where the first of three slots starts
    if (c->end + 3 * S2_RUMBLE_SLOT_NS < want || c->end > want + 3 * S2_RUMBLE_SLOT_NS) c->end = want;
}

// A Joy-Con on its own (a Joy-Con style npad, dm_joy_set): its own buttons as NpadButtons, rail buttons included (DM_BTN_LSL..RSR), and its own stick, -32767..32767
static inline uint64_t s2_joy_buttons(uint32_t b, int right) {
    uint64_t m = 0;
    if (!right) {
        if (b & S2_LEFT) m |= DM_BTN_LEFT; if (b & S2_UP) m |= DM_BTN_UP; if (b & S2_RIGHT) m |= DM_BTN_RIGHT; if (b & S2_DOWN) m |= DM_BTN_DOWN;
        if (b & S2_L) m |= DM_BTN_L; if (b & S2_ZL) m |= DM_BTN_ZL; if (b & S2_MINUS) m |= DM_BTN_MINUS; if (b & S2_LSTICK) m |= DM_BTN_LS;
        if (b & S2_SL_L) m |= DM_BTN_LSL; if (b & S2_SR_L) m |= DM_BTN_LSR;
    } else {
        if (b & S2_A) m |= DM_BTN_A; if (b & S2_B) m |= DM_BTN_B; if (b & S2_X) m |= DM_BTN_X; if (b & S2_Y) m |= DM_BTN_Y;
        if (b & S2_R) m |= DM_BTN_R; if (b & S2_ZR) m |= DM_BTN_ZR; if (b & S2_PLUS) m |= DM_BTN_PLUS; if (b & S2_RSTICK) m |= DM_BTN_RS;
        if (b & S2_SL_R) m |= DM_BTN_RSL; if (b & S2_SR_R) m |= DM_BTN_RSR;
    }
    return m;
}
static inline void s2_joy_stick(const s2_dev *d, int right, int32_t out[2]) {
    float x, y; s2_cal_apply(&d->cal[0], right ? d->r.rx : d->r.lx, right ? d->r.ry : d->r.ly, S2_DEADZONE, &x, &y);
    out[0] = (int32_t)lrintf(x * DM_STICK_MAX); out[1] = (int32_t)lrintf(y * DM_STICK_MAX);
}

// How Joy-Cons play, chosen on the controllers as on a Switch's Change Grip/Order screen, but held for S2_GESTURE_NS so that play cannot do it by accident:
// SL and SR of a Joy-Con that is in a pair: it leaves the pair and plays alone, held sideways (S2_GESTURE_SPLIT); L of a left Joy-Con alone and R of a right one alone, both held:
// the two play as one pair (S2_GESTURE_JOIN, both must have held it for the whole time). The player lights show the new players.
#define S2_GESTURE_NS 3000000000ull
enum { S2_GESTURE_NONE, S2_GESTURE_SPLIT, S2_GESTURE_JOIN };
static inline int s2_gesture(uint32_t b, int right, int paired) {
    uint32_t rails = right ? (S2_SL_R | S2_SR_R) : (S2_SL_L | S2_SR_L);
    if (paired) return (b & rails) == rails ? S2_GESTURE_SPLIT : S2_GESTURE_NONE;
    return (b & (right ? S2_R : S2_L)) && !(b & rails) ? S2_GESTURE_JOIN : S2_GESTURE_NONE;
}
typedef struct { int kind, done; uint64_t since; } s2_hold;   // the gesture held now, since when, and whether it has acted
// Returns 1 while kind has been held S2_GESTURE_NS without a break
static inline int s2_hold_update(s2_hold *h, int kind, uint64_t now) {
    if (kind != h->kind) { h->kind = kind; h->since = now; h->done = 0; }
    return kind != S2_GESTURE_NONE && now - h->since >= S2_GESTURE_NS;
}

// The device whose motion sensor the pad uses: the Pro Controller 2; of two Joy-Cons the right one (want_right) or the left; of one, that one. NULL when none.
static inline const s2_dev *s2_pad_motion_dev(const s2_pad_state *p, int want_right) {
    if (p->pro.have) return &p->pro;
    if (p->left.have && p->right.have) return want_right ? &p->right : &p->left;
    return p->left.have ? &p->left : p->right.have ? &p->right : NULL;
}

// Raw sensor counts to the Switch frame (G, rad/s). map holds six signed channel numbers (1..6 = accel x y z, gyro x y z; negative flips the sign): the output acceleration
// x y z, then the output rotation x y z. The Switch frame is x right, y forward, z up, rotations right-handed about those, and acceleration is the direction of gravity
// (face up and still: 0 0 -1; motion_pro.h). Scales: 1/4096 G per count, and 0.0607 degrees per second per count for the gyro, measured on a left and a right Joy-Con 2:
// 14 tilts from lying on an edge to standing against a wall, 90.2..90.6 degrees by the accelerometer, integrate to 1.153 times as much at the Switch family's nominal
// 0.070 (1.13..1.17 each, both controllers alike), and four 360 degree turns by hand to 1.136 times. The Pro Controller 2 is not measured and takes the same scale.
#define S2_ACCEL_G (1.f / 4096.f)
#define S2_GYRO_DPS 0.0607f
#define S2_GYRO_RAD (S2_GYRO_DPS * 3.14159265358979323846f / 180.f)
// Measured on a right Joy-Con 2 held upright (stick on top, face toward the ceiling): raw x right, y forward, z up, and the gyro channels are right-handed rotations about the same
// axes (tilting the top away: gyro x -83 degrees; turning left: gyro z +115; leaning the right side down: gyro y +104). So the pair's map is the identity for rotation and
// the negated specific force for gravity. A Joy-Con held alone is turned a quarter turn about the up axis: the right one with its top to the player's left (the Pro Controller 1
// map of motion_pro.h), the left one with its top to the right.
// The Pro Controller 2 has the pair's frame, not the Pro Controller 1's. SDL's Switch 2 driver (SDL_hidapi_switch2.c, the same report over USB, one byte later) reads every Switch 2
// controller's sensors as SDL x = raw x, y = raw z, z = -raw y for both the accelerometer and the gyro, and sends the Pro Controller 2's and a joined right Joy-Con's without any
// further turn: that is exactly the pair map below once switch2.c push_motion turns the Switch frame into SDL's. The Pro Controller 1's map is a quarter turn off: tilting the
// controller forward rolled, rolling it pitched. TOTK_SWITCH2_MAP="a,b,c,d,e,f" overrides the map (s2_map_parse) for a controller that still turns the wrong way.
static const int8_t s2_map_pair[6] = {-1, -2, -3, 4, 5, 6};
static const int8_t s2_map_right_alone[6] = {2, -1, -3, -5, 4, 6};
static const int8_t s2_map_left_alone[6] = {-2, 1, -3, 5, -4, 6};
static const int8_t s2_map_pro[6] = {-1, -2, -3, 4, 5, 6};
static inline const int8_t *s2_pad_map(const s2_pad_state *p, const s2_dev *dev) {
    if (p->pro.have) return s2_map_pro;
    if (p->left.have && p->right.have) return s2_map_pair;
    return dev == &p->left ? s2_map_left_alone : s2_map_right_alone;
}
// Gyro rest offset: a Joy-Con 2 at rest reads about 1.3 degrees per second off. About a second of samples in which the gyro and accelerometer hardly vary is a controller lying or held
// still; its mean is the offset (blended with the one before). An offset past 4 degrees per second is not taken: that is a slow turn, not rest.
// Once an offset is known, a new one is only taken from a controller that is really lying still (its gyro within S2_REST_KEEP_G counts, about 1.5 degrees per second, its
// accelerometer within S2_REST_KEEP_A): a hand holding it steady while aiming slowly also passed the first test, and its slow turn was then learned as the offset, so slow aiming
// stopped moving and the view drifted back when the hand moved again.
#define S2_REST_N 33
#define S2_REST_KEEP_G 25
#define S2_REST_KEEP_A 80
typedef struct { int n, have; int32_t sum[3]; int16_t gmin[3], gmax[3], amin[3], amax[3]; float bias[3]; } s2_rest;
static inline void s2_rest_update(s2_rest *r, const int16_t raw[6]) {
    if (!r->n) for (int i = 0; i < 3; i++) { r->sum[i] = 0; r->gmin[i] = r->gmax[i] = raw[3 + i]; r->amin[i] = r->amax[i] = raw[i]; }
    for (int i = 0; i < 3; i++) {
        r->sum[i] += raw[3 + i];
        if (raw[3 + i] < r->gmin[i]) r->gmin[i] = raw[3 + i]; if (raw[3 + i] > r->gmax[i]) r->gmax[i] = raw[3 + i];
        if (raw[i] < r->amin[i]) r->amin[i] = raw[i]; if (raw[i] > r->amax[i]) r->amax[i] = raw[i];
        int gmax = r->have ? S2_REST_KEEP_G : 100, amax = r->have ? S2_REST_KEEP_A : 300;
        if (r->gmax[i] - r->gmin[i] > gmax || r->amax[i] - r->amin[i] > amax) { r->n = 0; return; }   // moving: start the second again
    }
    if (++r->n < S2_REST_N) return;
    r->n = 0;
    float m[3]; for (int i = 0; i < 3; i++) { m[i] = (float)r->sum[i] / S2_REST_N; if (fabsf(m[i]) > 60.f) return; }
    for (int i = 0; i < 3; i++) r->bias[i] = r->have ? 0.5f * r->bias[i] + 0.5f * m[i] : m[i];
    r->have = 1;
}
// The factory gyro offset (16 bytes at S2_ADDR_GYRO_CAL): four little-endian floats, a temperature in degrees Celsius and the offset about x, y and z in radians per second,
// in the report's own axes. Measured on a right Joy-Con 2, it is the offset at rest when radians are turned back into counts at 0.070 degrees per second per count
// (factory 1.5, -18.7, -6.3; at rest 0.6, -18.7, -6.3), so that is the block's own scale, whatever the true one. A left Joy-Con 2's block reads about the opposite of its
// rest offset and is not used (s2_gyro_cal_usable). Returns 1 with the offset in counts; 0 for a short block, a value that is not finite or an offset past 5 degrees per second
#define S2_ADDR_GYRO_CAL 0x00013040u
#define S2_GYRO_CAL_DPS 0.070f
static inline int s2_gyro_cal_parse(const uint8_t *d, size_t n, float off[3]) {
    if (!d || n < 16) return 0;
    float v[4]; memcpy(v, d, sizeof v);
    for (int i = 0; i < 4; i++) if (!isfinite(v[i])) return 0;
    for (int i = 0; i < 3; i++) { float dps = v[1 + i] * 57.29577951308232f; if (fabsf(dps) > 5.f) return 0; off[i] = dps / S2_GYRO_CAL_DPS; }
    return 1;
}
// Whether a product's factory offset is taken as is: the right Joy-Con 2's matches its rest offset; the left one's does not (it reads about the negative) and the Pro
// Controller 2's is not measured, so those wait for the rest offset (or the one remembered for the controller, pad_switch2_apple.m)
static inline int s2_gyro_cal_usable(uint16_t pid) { return pid == 0x2066; }

// Sample times from the controller's own clock (the report's first four bytes: milliseconds, +30 a report on a Mac's 30 ms connection interval, +60 or +90 when a report was
// lost). Arrival times come in bursts (a few reports of a connection event at once), and a lost report leaves a gap that only the clock shows, so the sample time is the
// previous one plus the clock's step, never later than the arrival and never more than S2_CLOCK_LAG_NS before it (the clocks drift apart slowly; a long silence or a clock
// that went back starts over at the arrival). *lost is the number of reports the step skipped.
#define S2_CLOCK_LAG_NS 60000000ull
#define S2_CLOCK_REPORT_MS 30u
typedef struct { int have; uint32_t ms; uint64_t ns; } s2_clock;
static inline uint64_t s2_clock_sample(s2_clock *c, uint32_t ms, uint64_t arrival, unsigned *lost) {
    uint32_t step = ms - c->ms; uint64_t t;
    if (lost) *lost = 0;
    if (!c->have || step > 1000u) t = arrival;   // the first report, a second or more of silence, or the clock went back (a huge unsigned step)
    else {
        t = c->ns + (uint64_t)step * 1000000ull;
        if (t <= c->ns) t = c->ns + 1000;   // the same millisecond as the last: still after it
        if (t > arrival) t = arrival;
        if (arrival - t > S2_CLOCK_LAG_NS) t = arrival - S2_CLOCK_LAG_NS;
        if (t <= c->ns) t = c->ns + 1000;
        if (lost && step > S2_CLOCK_REPORT_MS + S2_CLOCK_REPORT_MS / 2) *lost = (step + S2_CLOCK_REPORT_MS / 2) / S2_CLOCK_REPORT_MS - 1;
    }
    c->have = 1; c->ms = ms; c->ns = t;
    return t;
}
// bias: the gyro offset in counts (s2_rest.bias), NULL for none
static inline void s2_motion(const int16_t raw[6], const int8_t map[6], const float *bias, float gyro_scale, float acc[3], float rot[3]) {
    for (int i = 0; i < 6; i++) {
        int c = map[i] < 0 ? -map[i] : map[i]; float s = map[i] < 0 ? -1.f : 1.f; if (c < 1 || c > 6) { c = 1; s = 0; }
        float v = s * ((float)raw[c - 1] - (c > 3 && bias ? bias[c - 4] : 0.f));
        if (i < 3) acc[i] = v * S2_ACCEL_G; else rot[i - 3] = v * gyro_scale;
    }
}
// "2,-1,-3,-5,4,6" -> map; returns 0 and leaves map alone when the string is not six numbers of 1..6 with signs
static inline int s2_map_parse(const char *s, int8_t map[6]) {
    int8_t m[6]; if (!s) return 0;
    for (int i = 0; i < 6; i++) {
        char *e; long v = strtol(s, &e, 10); if (e == s || v == 0 || v < -6 || v > 6) return 0;
        m[i] = (int8_t)v; s = e; if (i < 5) { if (*s != ',') return 0; s++; }
    }
    if (*s) return 0;
    memcpy(map, m, 6);
    return 1;
}
