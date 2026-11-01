// Adapted from MacNativeGames games/totk/runtime/input.c and hid_feedback.c @ b75be8cf (MIT, Copyright (c) 2026
// MacNativeGames contributors).
// nn::hid overrides: one connected controller fed by input_poll (gamepad and keyboard,
// runtime/input_win.c; nothing pressed with TOTK_INPUT=none). Docked (default) it is a full-key controller as player 1 (id 0, style bit 0); with
// TOTK_OPERATION_MODE=handheld it is the handheld controller (id 0x20, style bit 1) and player 1 is not connected,
// which is the controller the game reads in that mode. Mouse and keyboard report no input. The controller has two
// vibration devices (left, right) played on the gamepad's motors (TOTK_RUMBLE=0: none) and a six-axis sensor fed by
// the gamepad's gyro and accelerometer (TOTK_MOTION=0, live: reads at rest). State layouts follow libnx HID.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "ov.h"
#include "platform/platform.h"

typedef struct { int64_t sampling; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes, pad; } NpadState;
typedef struct { int64_t sampling; int32_t x, y, dx, dy, wx, wy; uint32_t buttons, attributes; } MouseState;
typedef struct { int64_t sampling; uint32_t modifiers, attributes; uint64_t keys[4]; } KeyboardState;
typedef struct { int8_t players; uint8_t pad[3]; uint32_t selected; } ControllerSupportResult;
_Static_assert(sizeof(NpadState) == 40 && sizeof(MouseState) == 40 && sizeof(KeyboardState) == 48, "hid state ABI");
_Static_assert(sizeof(ControllerSupportResult) == 8, "controller support result ABI");

enum { STYLE_FULL_KEY = 1, STYLE_HANDHELD = 2, ATTR_CONNECTED = 1 };

static PlatMutex lock = PLAT_MUTEX_INIT;
static uint32_t styles = STYLE_FULL_KEY;
static int enabled, allowed = 1, mouse_ready, keyboard_ready;
static int64_t sample;

static uint32_t pad_id(void) { return ov_handheld() ? 0x20u : 0u; }
static uint32_t pad_style(void) { return ov_handheld() ? STYLE_HANDHELD : STYLE_FULL_KEY; }

static const uint32_t *need_id(Cpu *c, int i, const char *fn) {
    const uint32_t *id = arg_p(c, i);
    if (!id) rt_fatal("nn::hid::%s: null controller id", fn);
    return id;
}

// void InitializeNpad()
static void ov_initialize_npad(Cpu *c) {
    (void)c;
    plat_mutex_lock(&lock);
    enabled = 1;
    plat_mutex_unlock(&lock);
    rt_log("hid: controllers initialized (one connected %s controller)",
           ov_handheld() ? "handheld" : "full-key");
}

// void SetSupportedNpadIdType(const NpadIdType *ids, size_t count)
static void ov_supported_ids(Cpu *c) {
    const uint32_t *ids = arg_p(c, 0);
    uint64_t n = arg_x(c, 1);
    if (n > 10 || (!ids && n)) rt_fatal("nn::hid::SetSupportedNpadIdType: invalid id list (%llu ids)", (unsigned long long)n);
    int ok = 0;
    for (uint64_t i = 0; i < n; i++) ok |= ids[i] == pad_id();
    plat_mutex_lock(&lock);
    allowed = ok;
    plat_mutex_unlock(&lock);
}

// void SetSupportedNpadStyleSet(NpadStyleSet styles)
static void ov_supported_styles(Cpu *c) {
    plat_mutex_lock(&lock);
    styles = (uint32_t)arg_x(c, 0);
    plat_mutex_unlock(&lock);
}

// NpadStyleSet GetNpadStyleSet(const NpadIdType &id)
static void ov_style_set(Cpu *c) {
    uint32_t id = *need_id(c, 0, "GetNpadStyleSet");
    plat_mutex_lock_shared(&lock);
    uint32_t v = enabled && allowed && id == pad_id() ? styles & pad_style() : 0;
    plat_mutex_unlock_shared(&lock);
    ret_x(c, v);
}

// int GetNpadStates(<State> *out, int count, const NpadIdType &id): the newest state only. want: the style of the
// overload, 0 for the Joy-Con overloads (never connected).
static void npad_states(Cpu *c, uint32_t want) {
    NpadState *out = arg_p(c, 0);
    int max = (int)arg_x(c, 1);
    uint32_t id = *need_id(c, 2, "GetNpadStates");
    if (max <= 0) { ret_x(c, 0); return; }
    if (!out) rt_fatal("nn::hid::GetNpadStates: null output");
    plat_mutex_lock(&lock);
    NpadState s = {.sampling = ++sample};
    int live = want && enabled && allowed && id == pad_id() && want == pad_style() && (styles & want);
    plat_mutex_unlock(&lock);
    if (live) {
        s.attributes = ATTR_CONNECTED;
        input_poll(&s.buttons, &s.lx, &s.ly, &s.rx, &s.ry);
    }
    *out = s;
    ret_x(c, 1);
}
static void ov_states_full_key(Cpu *c) { npad_states(c, STYLE_FULL_KEY); }
static void ov_states_handheld(Cpu *c) { npad_states(c, STYLE_HANDHELD); }
static void ov_states_joycon(Cpu *c) { npad_states(c, 0); }

// void SetNpadJoyHoldType(NpadJoyHoldType type)
static void ov_hold_type(Cpu *c) {
    int64_t t = (int32_t)arg_x(c, 0);
    if (t < 0 || t > 1) rt_fatal("nn::hid::SetNpadJoyHoldType: invalid hold type %lld", (long long)t);
}

// void DisconnectNpad(const NpadIdType &id): only the controller in use (handheld mode disconnects player 1)
static void ov_disconnect(Cpu *c) {
    uint32_t id = *need_id(c, 0, "DisconnectNpad");
    if (id != pad_id()) return;
    plat_mutex_lock(&lock);
    allowed = 0;
    plat_mutex_unlock(&lock);
}

// Result ShowControllerSupport(ControllerSupportResultInfo *out, const ControllerSupportArg &arg): the one controller
// is already connected, so the applet returns at once with one player on it.
static void ov_controller_support(Cpu *c) {
    ControllerSupportResult *out = arg_p(c, 0);
    if (out) *out = (ControllerSupportResult){.players = 1, .selected = pad_id()};
    ret_x(c, 0);
}

static void ov_initialize_mouse(Cpu *c) {
    (void)c;
    plat_mutex_lock(&lock);
    mouse_ready = 1;
    plat_mutex_unlock(&lock);
}

static void ov_initialize_keyboard(Cpu *c) {
    (void)c;
    plat_mutex_lock(&lock);
    keyboard_ready = 1;
    plat_mutex_unlock(&lock);
}

// int GetMouseStates(MouseState *out, int count): a mouse that is not connected
static void ov_mouse_states(Cpu *c) {
    MouseState *out = arg_p(c, 0);
    int max = (int)arg_x(c, 1);
    if (max <= 0) { ret_x(c, 0); return; }
    if (!out) rt_fatal("nn::hid::GetMouseStates: null output");
    plat_mutex_lock(&lock);
    if (!mouse_ready) rt_fatal("nn::hid::GetMouseStates before InitializeMouse");
    *out = (MouseState){.sampling = ++sample};
    plat_mutex_unlock(&lock);
    ret_x(c, 1);
}

// void GetKeyboardState(KeyboardState *out): a keyboard with no key down
static void ov_keyboard_state(Cpu *c) {
    KeyboardState *out = arg_p(c, 0);
    if (!out) rt_fatal("nn::hid::GetKeyboardState: null output");
    plat_mutex_lock(&lock);
    if (!keyboard_ready) rt_fatal("nn::hid::GetKeyboardState before InitializeKeyboard");
    *out = (KeyboardState){.sampling = ++sample};
    plat_mutex_unlock(&lock);
}

// ---- vibration and six-axis sensor (MacNativeGames hid_feedback.c)
// Handles are the SDK's (nn::hid::utility, sdk+0x4a6e00 and sdk+0x4a6ab0): byte 0 the style index (3 full-key,
// 4 handheld), byte 1 the controller id, byte 2 the device (vibration 0 left / 1 right, six-axis 2). Every call under
// the public functions goes through nn::hid::detail (sdk's PLT), answered here instead of by the hid service, including
// the batches VibrationTarget queues through detail::EnqueueVibrationValue into detail::SendVibrationValues.
// VibrationValue {amplitude low, frequency low, amplitude high, frequency high}: 0..1 and Hz, neutral 0/160/0/320.
typedef struct { float amp_low, freq_low, amp_high, freq_high; } VibrationValue;
typedef struct { uint32_t type, position; } VibrationDeviceInfo;   // type 1 linear resonant actuator; position 1 left, 2 right
_Static_assert(sizeof(VibrationValue) == 16 && sizeof(VibrationDeviceInfo) == 8, "vibration ABI");

enum { STYLE_INDEX_FULL_KEY = 3, STYLE_INDEX_HANDHELD = 4, SIX_AXIS_DEVICE = 2, RUMBLE_MS = 250 };
#define RUMBLE_STALE_NS 250000000ull     // a side the game has not updated for 250 ms is silent
#define RUMBLE_REFRESH_NS 50000000ull    // an unchanged value is sent again every 50 ms (its 250 ms start over)
#define RUMBLE_MIN_GAP_NS 10000000ull    // a changed value at most every 10 ms (the game streams every few ms)
static const VibrationValue vib_neutral = {0, 160, 0, 320};

static int env_zero(const char *name) {
    const char *e = cfg_get(name);
    return e && !strcmp(e, "0");
}

static void once(volatile int32_t *flag, const char *fmt, uint32_t v) {
    if (!plat_xchg32(flag, 1)) rt_log(fmt, v);
}

// TOTK_RUMBLE_STRENGTH (percent, 0..300, default 130; 0 = off) and TOTK_RUMBLE_CURVE (exponent on the amplitude, 0.2..1.5,
// default 0.6). HD rumble is strong at low amplitude, an ERM motor hardly turns there, so weak values are lifted by the
// curve (0.1 -> 0.25) and the strength scales the result (saturating at 65535; 0.66 and up reach full drive at 130 %).
// Both are live settings (the menu): read again whenever cfg_version changes.
static float rumble_strength = 1.3f, rumble_curve = 0.6f;
static volatile int32_t rumble_cfg_logged;
static unsigned rumble_cfg_seen = ~0u;
static float cfg_float(const char *name, float lo, float hi) {
    double v = cfg_double(name);
    return (float)(v < lo ? lo : v > hi ? hi : v);
}
static void rumble_cfg(void) {
    unsigned v = cfg_version();
    if (v == rumble_cfg_seen) return;
    rumble_cfg_seen = v;
    rumble_strength = cfg_float("TOTK_RUMBLE_STRENGTH", 0, 300) / 100;
    rumble_curve = cfg_float("TOTK_RUMBLE_CURVE", 0.2f, 1.5f);
    int trig = input_rumble_triggers_enabled();
    if (!plat_xchg32(&rumble_cfg_logged, 1))
        rt_log("hid: rumble strength %.0f%%, curve %.2f, triggers %s", rumble_strength * 100, rumble_curve, trig ? "on" : "off");
}
static uint16_t motor(float a) { return rumble_motor(a, rumble_curve, rumble_strength); }

static uint32_t pad_style_index(void) { return ov_handheld() ? STYLE_INDEX_HANDHELD : STYLE_INDEX_FULL_KEY; }

// The SDK's id check (utility functions): players 1-8, other (0x10) and handheld (0x20); the public functions abort
// on another id.
static uint32_t handle_id(Cpu *c, const char *fn) {
    uint32_t id = *need_id(c, 2, fn);
    if (id >= 8 && id != 0x10 && id != 0x20) rt_fatal("nn::hid::%s: invalid controller id %#x", fn, id);
    return id;
}

// int GetVibrationDeviceHandles(VibrationDeviceHandle *out, int count, const NpadIdType &id, NpadStyleSet style):
// utility::GetVibrationDeviceHandles' table, the first style bit in its order; none with TOTK_RUMBLE=0.
static void ov_vibration_handles(Cpu *c) {
    uint32_t *out = arg_p(c, 0), style = (uint32_t)arg_x(c, 3);
    int max = (int)arg_x(c, 1);
    uint32_t id = handle_id(c, "GetVibrationDeviceHandles");
    if (!out || max <= 0 || __builtin_popcount(style) != 1)
        rt_fatal("nn::hid::GetVibrationDeviceHandles: invalid arguments (count %d, styles %#x)", max, style);
    static volatile int32_t asked;
    rumble_cfg();
    // strength 0 keeps the devices (silent), so the menu can turn rumble up again
    if (env_zero("TOTK_RUMBLE")) { once(&asked, "hid: no vibration devices (TOTK_RUMBLE=%u)", 0); ret_x(c, 0); return; }
    static const struct { uint8_t bit, n; uint32_t base; } t[] = {
        {0, 2, 0x3}, {1, 2, 0x4}, {2, 2, 0x5}, {3, 1, 0x6}, {4, 1, 0x10007}, {5, 1, 0x8}, {10, 1, 0xd}, {8, 2, 0x4},
        {30, 2, 0x21}, {29, 2, 0x20}};
    int n = 0;
    for (size_t i = 0; i < sizeof t / sizeof *t && !n; i++) {
        if (!(style >> t[i].bit & 1)) continue;
        n = t[i].n < max ? t[i].n : max;
        for (int k = 0; k < n; k++) out[k] = (t[i].base | id << 8) + ((uint32_t)k << 16);
    }
    once(&asked, "hid: the game asked for its vibration devices (first handle %#x)", n ? out[0] : 0);
    ret_x(c, (uint64_t)n);
}

// int GetSixAxisSensorHandles(SixAxisSensorHandle *out, int count, const NpadIdType &id, NpadStyleSet style):
// utility::GetSixAxisSensorHandle's order (one sensor per style, two for the Joy-Con pair). Also with TOTK_MOTION=0: the
// sensor then reads at rest (six_fill), so the menu can turn motion on while the game runs.
static void ov_six_axis_handles(Cpu *c) {
    uint32_t *out = arg_p(c, 0), style = (uint32_t)arg_x(c, 3);
    int max = (int)arg_x(c, 1);
    uint32_t id = handle_id(c, "GetSixAxisSensorHandles");
    if (!out || max <= 0) rt_fatal("nn::hid::GetSixAxisSensorHandles: invalid arguments (count %d)", max);
    static volatile int32_t asked;
    uint32_t base = 0, n = 1;
    if (style & 1) base = 0x20003;
    else if (style & 2) base = 0x20004;
    else if (style & 4) base = 0x5, n = 2;
    else if (style & 8) base = 0x6;
    else if (style & 0x10) base = 0x10007;
    else if (style & 0x20) base = 0x20003;
    else if (style & 0xe80) n = 0;
    else if (style & 0x100) base = 0x20004;
    else if ((style & 0x60000040) == 0x40) base = 0x20003;
    else n = 0;
    if (n > (uint32_t)max) n = (uint32_t)max;
    for (uint32_t k = 0; k < n; k++) out[k] = (base | id << 8) + (k << 16);
    once(&asked, "hid: the game asked for its six-axis sensor (first handle %#x)", n ? out[0] : 0);
    ret_x(c, n);
}

static const uint32_t *need_handle(const void *p, const char *fn) {
    if (!p) rt_fatal("nn::hid::detail::%s: null handle", fn);
    return p;
}

// The vibration device (0 left, 1 right) a handle names on the connected controller, or -1 (logged once).
static int vibration_side(const uint32_t *h, const char *fn) {
    uint32_t v = *need_handle(h, fn), dev = v >> 16 & 0xff;
    if ((v & 0xff) == pad_style_index() && (v >> 8 & 0xff) == pad_id() && dev < 2) return (int)dev;
    static volatile int32_t warned;
    once(&warned, "hid: vibration call for handle %#x, not a device of the connected controller: ignored", v);
    return -1;
}

static PlatMutex vib_lock = PLAT_MUTEX_INIT;
static VibrationValue vib_last[2] = {{0, 160, 0, 320}, {0, 160, 0, 320}};   // vib_lock
static uint64_t vib_at[2], sent_at;                                          // vib_lock
static uint16_t sent_low, sent_high;                                         // vib_lock
static int vib_allowed = 1;                                                  // vib_lock: PermitVibration

static float clamp01(float v) { return v > 0 ? (v < 1 ? v : 1) : 0; }   // NaN -> 0
// Share of a band at f Hz that goes to the light (high-frequency, right) motor: none at 160 Hz and below (the low band's
// neutral frequency), all at 320 Hz and above (the high band's), log scale between.
static float light_share(float f) { return f > 160 ? (f < 320 ? log2f(f / 160) : 1) : 0; }

// Switch HD rumble -> SDL_RumbleGamepad. Each side's two bands split by frequency between the heavy (low) and the light
// (high) motor (light_share) and add up as energy per motor (root of the summed squares); the stronger side drives each
// motor. A side the game has not updated for 250 ms is silent. Each send lasts 250 ms and is renewed every 50 ms while
// the game streams, so the pad stops by itself when the game stops sending (pause, freeze). Under vib_lock.
static void rumble_update(uint64_t now) {
    float heavy = 0, light = 0;
    rumble_cfg();
    for (int s = 0; s < 2 && vib_allowed; s++) {
        if (!vib_at[s] || now - vib_at[s] > RUMBLE_STALE_NS) continue;
        const VibrationValue *v = &vib_last[s];
        float al = clamp01(v->amp_low), ah = clamp01(v->amp_high), sl = light_share(v->freq_low), sh = light_share(v->freq_high);
        float h = sqrtf(al * (1 - sl) * al * (1 - sl) + ah * (1 - sh) * ah * (1 - sh));
        float l = sqrtf(al * sl * al * sl + ah * sh * ah * sh);
        if (h > heavy) heavy = h;
        if (l > light) light = l;
    }
    uint16_t lo = motor(clamp01(heavy)), hi = motor(clamp01(light));
    int changed = lo != sent_low || hi != sent_high, on = lo || hi;
    if (changed ? on && now - sent_at < RUMBLE_MIN_GAP_NS : !on || now - sent_at < RUMBLE_REFRESH_NS) return;
    input_rumble(lo, hi, on ? RUMBLE_MS : 0);
    sent_low = lo, sent_high = hi, sent_at = now;
}

static void play(int side, const VibrationValue *v) {
    if (!v) rt_fatal("nn::hid::detail::SendVibrationValue: null value");
    if (side < 0) return;
    VibrationValue c = *v;
    if (!isfinite(c.amp_low) || !isfinite(c.amp_high) || !isfinite(c.freq_low) || !isfinite(c.freq_high)) c = vib_neutral;
    static volatile int32_t first, first_on;
    if (!plat_xchg32(&first, 1))
        rt_log("hid: first vibration value from the game: %s %.2f@%.0f Hz %.2f@%.0f Hz", side ? "right" : "left",
               (double)c.amp_low, (double)c.freq_low, (double)c.amp_high, (double)c.freq_high);
    if ((c.amp_low > 0.01f || c.amp_high > 0.01f) && !plat_xchg32(&first_on, 1))
        rt_log("hid: first non-zero vibration from the game: %s %.2f@%.0f Hz %.2f@%.0f Hz", side ? "right" : "left",
               (double)c.amp_low, (double)c.freq_low, (double)c.amp_high, (double)c.freq_high);
    uint64_t now = kern_now_ns();
    plat_mutex_lock(&vib_lock);
    vib_last[side] = c;
    vib_at[side] = now;
    rumble_update(now);
    plat_mutex_unlock(&vib_lock);
}

// Result detail::InitializeVibrationDevice(const VibrationDeviceHandle &)
static void ov_vib_init(Cpu *c) {
    static volatile int32_t logged;
    const uint32_t *h = arg_p(c, 0);
    if (vibration_side(h, "InitializeVibrationDevice") >= 0)
        once(&logged, "hid: the game initialized its vibration devices (handle %#x)", *h);
    ret_x(c, 0);
}

// Result detail::SendVibrationValue(const VibrationDeviceHandle &, const VibrationValue &)
static void ov_vib_send(Cpu *c) {
    play(vibration_side(arg_p(c, 0), "SendVibrationValue"), arg_p(c, 1));
    ret_x(c, 0);
}

// Result detail::SendVibrationValues(const VibrationDeviceHandle *, const VibrationValue *, int count)
static void ov_vib_send_many(Cpu *c) {
    const uint32_t *h = arg_p(c, 0);
    const VibrationValue *v = arg_p(c, 1);
    int n = (int)arg_x(c, 2);
    if (n < 0 || (n && (!h || !v))) rt_fatal("nn::hid::detail::SendVibrationValues: invalid batch (%d values)", n);
    for (int i = 0; i < n; i++) play(vibration_side(&h[i], "SendVibrationValues"), &v[i]);
    ret_x(c, 0);
}

// Result detail::GetActualVibrationValue(VibrationValue *out, const VibrationDeviceHandle &): the last value sent
static void ov_vib_actual(Cpu *c) {
    VibrationValue *out = arg_p(c, 0);
    if (!out) rt_fatal("nn::hid::detail::GetActualVibrationValue: null output");
    int side = vibration_side(arg_p(c, 1), "GetActualVibrationValue");
    VibrationValue v = vib_neutral;
    plat_mutex_lock_shared(&vib_lock);
    if (side >= 0) v = vib_last[side];
    plat_mutex_unlock_shared(&vib_lock);
    *out = v;
    ret_x(c, 0);
}

// Result detail::GetVibrationDeviceInfo(VibrationDeviceInfo *out, const VibrationDeviceHandle &)
static void ov_vib_info(Cpu *c) {
    VibrationDeviceInfo *out = arg_p(c, 0);
    if (!out) rt_fatal("nn::hid::detail::GetVibrationDeviceInfo: null output");
    int side = vibration_side(arg_p(c, 1), "GetVibrationDeviceInfo");
    *out = side >= 0 ? (VibrationDeviceInfo){1, (uint32_t)side + 1} : (VibrationDeviceInfo){0, 0};
    ret_x(c, 0);
}

// Result detail::IsVibrationDeviceMounted(bool *out, const VibrationDeviceHandle &)
static void ov_vib_mounted(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    if (!out) rt_fatal("nn::hid::detail::IsVibrationDeviceMounted: null output");
    *out = vibration_side(arg_p(c, 1), "IsVibrationDeviceMounted") >= 0;
    ret_x(c, 0);
}

// Result detail::IsVibrationPermitted(bool *out)
static void ov_vib_permitted(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    if (!out) rt_fatal("nn::hid::detail::IsVibrationPermitted: null output");
    plat_mutex_lock_shared(&vib_lock);
    *out = (uint8_t)vib_allowed;
    plat_mutex_unlock_shared(&vib_lock);
    ret_x(c, 0);
}

// Result detail::PermitVibration(bool): off silences the gamepad at once
static void ov_vib_permit(Cpu *c) {
    int on = (arg_x(c, 0) & 0xff) != 0;
    plat_mutex_lock(&vib_lock);
    vib_allowed = on;
    rumble_update(kern_now_ns());
    plat_mutex_unlock(&vib_lock);
    ret_x(c, 0);
}

static volatile int32_t six_running;   // StartSixAxisSensor .. StopSixAxisSensor

static int six_axis_ours(const uint32_t *h, const char *fn) {
    uint32_t v = *need_handle(h, fn);
    if ((v & 0xff) == pad_style_index() && (v >> 8 & 0xff) == pad_id() && (v >> 16 & 0xff) == SIX_AXIS_DEVICE) return 1;
    static volatile int32_t warned;
    once(&warned, "hid: six-axis call for handle %#x, not the connected controller's sensor: reads at rest", v);
    return 0;
}

// Result detail::StartSixAxisSensor(const SixAxisSensorHandle &): angle and direction start over
static void ov_six_start(Cpu *c) {
    const uint32_t *h = arg_p(c, 0);
    if (six_axis_ours(h, "StartSixAxisSensor")) {
        static volatile int32_t logged;
        once(&logged, "hid: the game started the six-axis sensor (handle %#x)", *h);
        motion_restart(motion_global());
        input_motion_start();
        plat_xchg32(&six_running, 1);
    }
    ret_x(c, 0);
}

// Result detail::StopSixAxisSensor(const SixAxisSensorHandle &)
static void ov_six_stop(Cpu *c) {
    if (six_axis_ours(arg_p(c, 0), "StopSixAxisSensor")) plat_xchg32(&six_running, 0);
    ret_x(c, 0);
}

// All max entries, newest first: the game reads all 16 it asks for and ignores the count (main+0x2a268e0). A short
// history repeats its oldest sample. No motion (sensor not started, no gamepad, one without a gyro, TOTK_MOTION=0):
// a controller lying still, not marked connected.
static int six_fill(MotionState *s, int max, const uint32_t *h, const char *fn) {
    if (max <= 0) return 0;
    if (!s) rt_fatal("nn::hid::detail::%s: null output", fn);
    int live = six_axis_ours(h, fn) && six_running && input_motion_enabled();
    int n = live ? motion_read(motion_global(), s, max, kern_now_ns()) : 0;
    if (!n) {
        static const MotionState rest = {.acc = {0, 0, -1}, .dir = {1, 0, 0, 0, 1, 0, 0, 0, 1}};
        for (int i = 0; i < max; i++) s[i] = rest;
    } else
        for (int i = n; i < max; i++) s[i] = s[n - 1];
    static volatile int32_t first_rest, first_live;
    if (!n && six_running) once(&first_rest, "hid: six-axis read with no motion samples: at rest (%u states)", (uint32_t)max);
    if (n) once(&first_live, "hid: first six-axis read with live motion (%u samples)", (uint32_t)n);
    return max;
}

// Result detail::GetSixAxisSensorStates(int *count, SixAxisSensorState *out, int max, const SixAxisSensorHandle &)
static void ov_six_states(Cpu *c) {
    int32_t *count = arg_p(c, 0);
    if (!count) rt_fatal("nn::hid::detail::GetSixAxisSensorStates: null count");
    *count = six_fill(arg_p(c, 1), (int)arg_x(c, 2), arg_p(c, 3), "GetSixAxisSensorStates");
    ret_x(c, 0);
}

// Result detail::GetSixAxisSensorState(SixAxisSensorState *out, const SixAxisSensorHandle &)
static void ov_six_state(Cpu *c) {
    six_fill(arg_p(c, 0), 1, arg_p(c, 1), "GetSixAxisSensorState");
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn3hid14InitializeNpadEv", ov_initialize_npad);
HOST_OVERRIDE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", ov_supported_ids);
HOST_OVERRIDE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", ov_supported_styles);
HOST_OVERRIDE("_ZN2nn3hid15GetNpadStyleSetERKj", ov_style_set);
HOST_OVERRIDE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", ov_states_full_key);
HOST_OVERRIDE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", ov_states_joycon);
HOST_OVERRIDE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", ov_states_joycon);
HOST_OVERRIDE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", ov_states_joycon);
HOST_OVERRIDE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", ov_states_handheld);
HOST_OVERRIDE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", ov_hold_type);
HOST_OVERRIDE("_ZN2nn3hid14DisconnectNpadERKj", ov_disconnect);
HOST_OVERRIDE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",
              ov_controller_support);
HOST_OVERRIDE("_ZN2nn3hid15InitializeMouseEv", ov_initialize_mouse);
HOST_OVERRIDE("_ZN2nn3hid18InitializeKeyboardEv", ov_initialize_keyboard);
HOST_OVERRIDE("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", ov_mouse_states);
HOST_OVERRIDE("_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", ov_keyboard_state);
HOST_OVERRIDE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_"
              "12NpadStyleTagEEE", ov_vibration_handles);
HOST_OVERRIDE("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_"
              "12NpadStyleTagEEE", ov_six_axis_handles);
HOST_OVERRIDE("_ZN2nn3hid6detail25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", ov_vib_init);
HOST_OVERRIDE("_ZN2nn3hid6detail18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", ov_vib_send);
HOST_OVERRIDE("_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi",
              ov_vib_send_many);
HOST_OVERRIDE("_ZN2nn3hid6detail23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE",
              ov_vib_actual);
HOST_OVERRIDE("_ZN2nn3hid6detail22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE",
              ov_vib_info);
HOST_OVERRIDE("_ZN2nn3hid6detail24IsVibrationDeviceMountedEPbRKNS0_21VibrationDeviceHandleE", ov_vib_mounted);
HOST_OVERRIDE("_ZN2nn3hid6detail20IsVibrationPermittedEPb", ov_vib_permitted);
HOST_OVERRIDE("_ZN2nn3hid6detail15PermitVibrationEb", ov_vib_permit);
HOST_OVERRIDE("_ZN2nn3hid6detail18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", ov_six_start);
HOST_OVERRIDE("_ZN2nn3hid6detail17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", ov_six_stop);
HOST_OVERRIDE("_ZN2nn3hid6detail22GetSixAxisSensorStatesEPiPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE",
              ov_six_states);
HOST_OVERRIDE("_ZN2nn3hid6detail21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE",
              ov_six_state);
