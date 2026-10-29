// Binding tables (keyboard/mouse and gamepad), their totk.ini overrides, the pure mapping functions and the Buttons
// tab's edits (input_map.h). No Win32 or SDL here.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input_map.h"

#define STICK_MAX 32767
#define DIAGONAL 23170   // STICK_MAX / sqrt(2)
#define MOUSE_FULL_CPS 3000.0

#define DEFAULT_KEYS \
    {KEY_W, B_SL_UP}, {KEY_A, B_SL_LEFT}, {KEY_S, B_SL_DOWN}, {KEY_D, B_SL_RIGHT},                  /* move */ \
    {KEY_UP, B_SR_UP}, {KEY_LEFT, B_SR_LEFT}, {KEY_DOWN, B_SR_DOWN}, {KEY_RIGHT, B_SR_RIGHT},       /* camera, second source */ \
    {KEY_MOUSE_L, B_Y}, {KEY_MOUSE_R, B_ZL}, {KEY_Z, B_ZL}, {KEY_F, B_ZR}, {KEY_X, B_ZR}, \
    {KEY_SPACE, B_X}, {KEY_LSHIFT, B_B}, {KEY_E, B_A}, {KEY_Q, B_L}, {KEY_R, B_R}, \
    {KEY_LCTRL, B_STICK_L}, {KEY_C, B_STICK_R}, \
    {KEY_1, B_LEFT}, {KEY_2, B_UP}, {KEY_3, B_RIGHT}, {KEY_4, B_DOWN}, \
    {KEY_TAB, B_PLUS}, {KEY_M, B_MINUS}

// By position: south is Switch B, east A, west Y, north X (Nintendo layout); the Xbox layout swaps A with B and X with Y
// (south A, as the Xbox labels; cross A on a PlayStation pad). The triggers are ZL and ZR.
#define DEFAULT_PADS(south, east, west, north) \
    {PAD_SOUTH, south}, {PAD_EAST, east}, {PAD_WEST, west}, {PAD_NORTH, north}, {PAD_LSTICK, B_STICK_L}, \
    {PAD_RSTICK, B_STICK_R}, {PAD_L, B_L}, {PAD_R, B_R}, {PAD_START, B_PLUS}, {PAD_BACK, B_MINUS}, \
    {PAD_DPAD_LEFT, B_LEFT}, {PAD_DPAD_UP, B_UP}, {PAD_DPAD_RIGHT, B_RIGHT}, {PAD_DPAD_DOWN, B_DOWN}, {PAD_ZL, B_ZL}, \
    {PAD_ZR, B_ZR}
#define NINTENDO_PADS DEFAULT_PADS(B_B, B_A, B_Y, B_X)
#define XBOX_PADS DEFAULT_PADS(B_A, B_B, B_X, B_Y)
enum { DEFAULT_KEY_COUNT = 26, DEFAULT_PAD_COUNT = 16 };
_Static_assert(sizeof (Binding[]){DEFAULT_KEYS} / sizeof (Binding) == DEFAULT_KEY_COUNT, "DEFAULT_KEY_COUNT");

static const InputMap defaults = {{DEFAULT_KEYS}, DEFAULT_KEY_COUNT, {{NINTENDO_PADS}, {XBOX_PADS}},
                                  {DEFAULT_PAD_COUNT, DEFAULT_PAD_COUNT}};
InputMap input_map = {{DEFAULT_KEYS}, DEFAULT_KEY_COUNT, {{NINTENDO_PADS}, {XBOX_PADS}},
                      {DEFAULT_PAD_COUNT, DEFAULT_PAD_COUNT}};

static const char *const action_names[ACTION_COUNT] = {
    "A", "B", "X", "Y", "LCLICK", "RCLICK", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN",
    "LSTICK_LEFT", "LSTICK_UP", "LSTICK_RIGHT", "LSTICK_DOWN", "RSTICK_LEFT", "RSTICK_UP", "RSTICK_RIGHT", "RSTICK_DOWN"};
static const char *const pad_names[PAD_COUNT] = {
    NULL, "south", "east", "west", "north", "lstick", "rstick", "l", "r", "start", "back",
    "dpad_left", "dpad_up", "dpad_right", "dpad_down", "zl", "zr"};
static const char *const key_names[KEY_COUNT] = {   // each group starts at its id; the ids of a group are consecutive
    [KEY_A] = "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u",
    "v", "w", "x", "y", "z",
    [KEY_0] = "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    [KEY_SPACE] = "space", "tab", "lshift", "lctrl", "up", "down", "left", "right",
    [KEY_MOUSE_L] = "mouse_l", "mouse_r", "mouse_m", "mouse_x1", "mouse_x2",
    [KEY_ENTER] = "enter", "backspace", "rshift", "rctrl", "lalt", "capslock", "insert", "delete", "home", "end",
    "pageup", "pagedown",
    [KEY_F2] = "f2", "f3", "f4", "f5", "f6", "f7", "f9", "f12",
    [KEY_NUM0] = "num0", "num1", "num2", "num3", "num4", "num5", "num6", "num7", "num8", "num9",
    [KEY_NUM_ADD] = "num_add", "num_sub", "num_mul", "num_div", "num_decimal",
    [KEY_MINUS] = "minus", "equals", "lbracket", "rbracket", "semicolon", "apostrophe", "comma", "period", "slash",
    "backslash", "grave"};

const char *input_action_name(int action) { return action >= 0 && action < ACTION_COUNT ? action_names[action] : NULL; }
const char *input_key_name(int key) { return key > KEY_NONE && key < KEY_COUNT ? key_names[key] : NULL; }
const char *input_pad_name(int pad) { return pad > PAD_NONE && pad < PAD_COUNT ? pad_names[pad] : NULL; }

void input_map_reset(void) { input_map = defaults; }

static int key_id(const char *name) {
    for (int k = KEY_A; k < KEY_COUNT; k++)
        if (key_names[k] && !_stricmp(key_names[k], name)) return k;
    return KEY_NONE;
}

static int pad_id(const char *name) {
    for (int p = PAD_SOUTH; p < PAD_COUNT; p++)
        if (!_stricmp(pad_names[p], name)) return p;
    return PAD_NONE;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

int input_map_keys_of(const InputMap *m, int action, uint8_t *out, int max) {
    int n = 0;
    for (int i = 0; i < m->nkeys; i++)
        if (m->keys[i].action == action && n < max) out[n++] = m->keys[i].key;
    return n;
}

int input_map_pads_of(const InputMap *m, int action, int layout, uint8_t *out, int max) {
    int n = 0;
    for (int i = 0; i < m->npads[layout]; i++)
        if (m->pads[layout][i].action == action && n < max) out[n++] = m->pads[layout][i].pad;
    return n;
}

// action's keys become list[0..n): its old ones go, the new ones are appended (the table keeps its order otherwise).
static void keys_put(InputMap *m, int action, const uint8_t *list, int n) {
    int w = 0;
    for (int i = 0; i < m->nkeys; i++)
        if (m->keys[i].action != action) m->keys[w++] = m->keys[i];
    m->nkeys = w;
    for (int i = 0; i < n && m->nkeys < INPUT_BINDING_MAX; i++) m->keys[m->nkeys++] = (Binding){list[i], (uint8_t)action};
}

static void pads_put(InputMap *m, int layout, int action, const uint8_t *list, int n) {
    PadBinding *t = m->pads[layout];
    int w = 0;
    for (int i = 0; i < m->npads[layout]; i++)
        if (t[i].action != action) t[w++] = t[i];
    m->npads[layout] = w;
    for (int i = 0; i < n && m->npads[layout] < INPUT_PAD_BINDING_MAX; i++)
        t[m->npads[layout]++] = (PadBinding){list[i], (uint8_t)action};
}

int input_map_bind(const char *button, const char *list) {
    int action = -1;
    for (int a = 0; a < ACTION_COUNT; a++)
        if (!_stricmp(action_names[a], button)) action = a;
    if (action < 0) { rt_log("input: bind: unknown button '%s'", button); return 0; }
    uint8_t keys[INPUT_BINDING_MAX], pads[INPUT_PAD_BINDING_MAX];
    int nk = 0, np = 0, none = 0, key_def = 0, pad_def = 0, bad = 0;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *save = NULL, *tok = strtok_s(buf, ",", &save); tok; tok = strtok_s(NULL, ",", &save)) {
        tok = trim(tok);
        if (!_stricmp(tok, "none")) { none = 1; continue; }
        int ok = 0;
        if (!_strnicmp(tok, "pad:", 4) && action < B_SL_LEFT) {
            const char *name = trim(tok + 4);
            int p = pad_id(name);
            if (!_stricmp(name, "default")) pad_def = ok = 1;
            else if (p && np < INPUT_PAD_BINDING_MAX) pads[np++] = (uint8_t)p, ok = 1;
        } else if (!_strnicmp(tok, "key:", 4)) {
            const char *name = trim(tok + 4);
            int k = key_id(name);
            if (!_stricmp(name, "default")) key_def = ok = 1;
            else if (k && nk < INPUT_BINDING_MAX) keys[nk++] = (uint8_t)k, ok = 1;
        }
        if (!ok) {
            bad++;
            rt_log("input: bind %s: ignoring '%s' (expected pad:<name> or key:<name>; stick directions take keys only)",
                   button, tok);
        }
    }
    if (!nk && !np && !none && !key_def && !pad_def) {
        if (bad) rt_log("input: bind %s: no valid entry, keeping the defaults", button);
        return 0;
    }
    uint8_t list_k[INPUT_BINDING_MAX];
    int n = key_def ? input_map_keys_of(&defaults, action, list_k, INPUT_BINDING_MAX) : 0;
    for (int i = 0; i < nk && n < INPUT_BINDING_MAX; i++) list_k[n++] = keys[i];
    keys_put(&input_map, action, list_k, n);
    for (int l = 0; l < PAD_LAYOUT_COUNT; l++) {   // a bind line names physical buttons: the same in every layout
        uint8_t list_p[INPUT_PAD_BINDING_MAX];
        int c = pad_def ? input_map_pads_of(&defaults, action, l, list_p, INPUT_PAD_BINDING_MAX) : 0;
        for (int i = 0; i < np && c < INPUT_PAD_BINDING_MAX; i++) list_p[c++] = pads[i];
        pads_put(&input_map, l, action, list_p, c);
    }
    return 1;
}

void input_map_configure(void) {
    input_map_reset();
    for (int a = 0; a < ACTION_COUNT; a++) {
        char var[32];
        snprintf(var, sizeof var, "TOTK_BIND_%s", action_names[a]);
        const char *v = cfg_get(var);
        if (v && *v && input_map_bind(action_names[a], v)) rt_log("input: %s = %s", var + 5, v);
    }
}

int input_map_set_key(InputMap *m, int action, int slot, int key) {
    uint8_t list[INPUT_BINDING_MAX];
    int n = input_map_keys_of(m, action, list, INPUT_BINDING_MAX), lost = -1;
    if (key != KEY_NONE) {
        for (int i = 0; i < m->nkeys; i++)
            if (m->keys[i].key == key && m->keys[i].action != action) lost = m->keys[i].action;
        if (lost >= 0) {
            uint8_t other[INPUT_BINDING_MAX];
            int c = 0;
            for (int i = 0; i < m->nkeys; i++)
                if (m->keys[i].action == lost && m->keys[i].key != key) other[c++] = m->keys[i].key;
            keys_put(m, lost, other, c);
        }
        int w = 0;   // the key once in this action's list too
        for (int i = 0; i < n; i++)
            if (list[i] != key) list[w++] = list[i];
        n = w;
        if (slot < n) list[slot] = (uint8_t)key;
        else if (n < INPUT_BINDING_MAX) list[n++] = (uint8_t)key;
    } else if (slot < n) {
        memmove(list + slot, list + slot + 1, (size_t)(n - slot - 1));
        n--;
    }
    keys_put(m, action, list, n);
    return lost;
}

static int same_set(const uint8_t *a, int na, const uint8_t *b, int nb);

// Every action has its default gamepad buttons in layout.
static int pads_at_defaults(const InputMap *m, int layout) {
    for (int a = 0; a < B_SL_LEFT; a++) {
        uint8_t p[INPUT_PAD_BINDING_MAX], dp[INPUT_PAD_BINDING_MAX];
        int np = input_map_pads_of(m, a, layout, p, INPUT_PAD_BINDING_MAX);
        if (!same_set(p, np, dp, input_map_pads_of(&defaults, a, layout, dp, INPUT_PAD_BINDING_MAX))) return 0;
    }
    return 1;
}

int input_map_set_pad(InputMap *m, int action, int layout, int pad) {
    uint8_t mine[INPUT_PAD_BINDING_MAX];
    int n = input_map_pads_of(m, action, layout, mine, INPUT_PAD_BINDING_MAX), other = -1;
    if (pad != PAD_NONE)
        for (int i = 0; i < m->npads[layout]; i++)
            if (m->pads[layout][i].pad == pad && m->pads[layout][i].action != action) other = m->pads[layout][i].action;
    if (other >= 0) {   // the other action gives up pad and takes this action's old button
        uint8_t list[INPUT_PAD_BINDING_MAX];
        int c = 0;
        for (int i = 0; i < m->npads[layout]; i++)
            if (m->pads[layout][i].action == other && m->pads[layout][i].pad != pad) list[c++] = m->pads[layout][i].pad;
        if (n && mine[0] != pad && c < INPUT_PAD_BINDING_MAX) list[c++] = mine[0];
        pads_put(m, layout, other, list, c);
    }
    uint8_t p = (uint8_t)pad;
    pads_put(m, layout, action, &p, pad != PAD_NONE);
    if (pads_at_defaults(m, layout)) {   // put back as it was: the layout setting moves the face buttons again
        memcpy(m->pads, defaults.pads, sizeof m->pads);
        memcpy(m->npads, defaults.npads, sizeof m->npads);
        return other;
    }
    int pin[6] = {B_A, B_B, B_X, B_Y, action, other}, npin = other >= 0 ? 6 : 5;
    for (int i = 0; i < npin; i++) {
        uint8_t list[INPUT_PAD_BINDING_MAX];
        int c = input_map_pads_of(m, pin[i], layout, list, INPUT_PAD_BINDING_MAX);
        for (int l = 0; l < PAD_LAYOUT_COUNT; l++)
            if (l != layout) pads_put(m, l, pin[i], list, c);
    }
    return other;
}

// The same bindings in any order (the menu appends; a line lists them its own way).
static int same_set(const uint8_t *a, int na, const uint8_t *b, int nb) {
    if (na != nb) return 0;
    for (int i = 0; i < na; i++) {
        int ca = 0, cb = 0;
        for (int j = 0; j < na; j++) ca += a[j] == a[i], cb += b[j] == a[i];
        if (ca != cb) return 0;
    }
    return 1;
}

void input_map_line(const InputMap *m, int action, char *out, size_t n) {
    uint8_t k[INPUT_BINDING_MAX], dk[INPUT_BINDING_MAX];
    int nk = input_map_keys_of(m, action, k, INPUT_BINDING_MAX);
    int key_def = same_set(k, nk, dk, input_map_keys_of(&defaults, action, dk, INPUT_BINDING_MAX));
    int pad_def = 1;
    for (int l = 0; l < PAD_LAYOUT_COUNT; l++) {
        uint8_t p[INPUT_PAD_BINDING_MAX], dp[INPUT_PAD_BINDING_MAX];
        int np = input_map_pads_of(m, action, l, p, INPUT_PAD_BINDING_MAX);
        pad_def &= same_set(p, np, dp, input_map_pads_of(&defaults, action, l, dp, INPUT_PAD_BINDING_MAX));
    }
    out[0] = 0;
    if (key_def && pad_def) return;
    size_t len = 0;
#define PUT(...) (len += (size_t)snprintf(out + len, len < n ? n - len : 0, __VA_ARGS__))
    if (key_def) PUT("key:default");
    else
        for (int i = 0; i < nk; i++) PUT("%skey:%s", len ? ", " : "", key_names[k[i]]);
    if (pad_def && action < B_SL_LEFT) PUT("%spad:default", len ? ", " : "");
    else if (!pad_def) {
        uint8_t p[INPUT_PAD_BINDING_MAX];
        int np = input_map_pads_of(m, action, PAD_LAYOUT_NINTENDO, p, INPUT_PAD_BINDING_MAX);
        for (int i = 0; i < np; i++) PUT("%spad:%s", len ? ", " : "", pad_names[p[i]]);
    }
    if (!len) PUT("none");
#undef PUT
}

int input_pad_layout_pick(const char *setting, int east_labelled_a) {
    if (setting && !_stricmp(setting, "nintendo")) return PAD_LAYOUT_NINTENDO;
    if (setting && !_stricmp(setting, "xbox")) return PAD_LAYOUT_XBOX;
    return east_labelled_a ? PAD_LAYOUT_NINTENDO : PAD_LAYOUT_XBOX;
}

uint64_t input_map_pad_buttons(const uint8_t down[PAD_COUNT], int layout) {
    const PadBinding *t = input_map.pads[layout];
    uint64_t b = 0;
    for (int i = 0; i < input_map.npads[layout]; i++)
        if (down[t[i].pad]) b |= 1ull << t[i].action;
    return b;
}

uint64_t input_map_buttons(const uint8_t down[KEY_COUNT]) {
    uint64_t b = 0;
    for (int i = 0; i < input_map.nkeys; i++)
        if (input_map.keys[i].action < B_SL_LEFT && down[input_map.keys[i].key]) b |= 1ull << input_map.keys[i].action;
    return b;
}

void input_map_stick(const uint8_t down[KEY_COUNT], int stick, int32_t out[2]) {
    int first = stick ? B_SR_LEFT : B_SL_LEFT, dir[4] = {0, 0, 0, 0};   // left, up, right, down
    for (int i = 0; i < input_map.nkeys; i++) {
        int a = input_map.keys[i].action - first;
        if (a >= 0 && a < 4 && down[input_map.keys[i].key]) dir[a] = 1;
    }
    int dx = dir[2] - dir[0], dy = dir[1] - dir[3];
    int32_t m = dx && dy ? DIAGONAL : STICK_MAX;
    out[0] = dx * m;
    out[1] = dy * m;
}

void input_mouse_stick(int32_t dx, int32_t dy, double dt_s, float sens, int32_t out[2]) {
    out[0] = out[1] = 0;
    if (!(dt_s > 0) || !(sens > 0)) return;
    double k = STICK_MAX * (double)sens / (MOUSE_FULL_CPS * dt_s);
    double x = dx * k, y = -dy * k, len = sqrt(x * x + y * y);
    if (len > STICK_MAX) { x *= STICK_MAX / len; y *= STICK_MAX / len; }
    out[0] = (int32_t)lrint(x);
    out[1] = (int32_t)lrint(y);
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

#define MOUSE_DEG_PER_COUNT 0.022   // at sensitivity 1
#define MOUSE_PITCH_RATE 0.7237     // the pitch rate at full deflection over the yaw rate (the sibling game's 61.8 / 85.39)
#define MOUSE_MAX_CARRY_DEG 12.0

void input_mouse_camera(MouseCamState *st, int32_t dx, int32_t dy, double dt_s, const MouseCamCfg *c, int32_t out[2]) {
    out[0] = out[1] = 0;
    if (!st || !c) return;
    if (!(dt_s > 0) || !(c->sens > 0)) {
        st->carry[0] = st->carry[1] = 0;
        return;
    }
    const double sens = c->sens, curve = clampd(c->curve, 1.0, 3.0), dz = clampd(c->deadzone, 0.0, 0.5);
    const double rate = c->rate >= 1 ? c->rate : 85.0;
    const double yaw = dx * MOUSE_DEG_PER_COUNT * sens + st->carry[0];                                  // right +
    const double pitch = -dy * MOUSE_DEG_PER_COUNT * sens * clampd(c->y_ratio, 0.25, 4.0) + st->carry[1];   // up +
    // the post-curve amount of full rate that turns that angle in dt_s; past full deflection the direction is kept
    double ux = yaw / dt_s / rate, uy = pitch / dt_s / (rate * MOUSE_PITCH_RATE);
    double m = sqrt(ux * ux + uy * uy);
    if (m > 1.0) {
        ux /= m;
        uy /= m;
        m = 1.0;
    }
    st->carry[0] = clampd(yaw - ux * rate * dt_s, -MOUSE_MAX_CARRY_DEG, MOUSE_MAX_CARRY_DEG);
    st->carry[1] = clampd(pitch - uy * rate * MOUSE_PITCH_RATE * dt_s, -MOUSE_MAX_CARRY_DEG, MOUSE_MAX_CARRY_DEG);
    if (fabs(st->carry[0]) < 1e-6) st->carry[0] = 0;
    if (fabs(st->carry[1]) < 1e-6) st->carry[1] = 0;
    if (m < 1e-9) return;
    // the inverse of the game's response: raw = dead zone + amount^(1/curve) * (1 - dead zone), along the same direction
    const double raw = dz + pow(m, 1.0 / curve) * (1.0 - dz);
    out[0] = (int32_t)lrint(ux / m * raw * STICK_MAX);
    out[1] = (int32_t)lrint(uy / m * raw * STICK_MAX);
}

// Minus and Plus held together open or close the menu. *held: latched from the poll where both go down until neither is
// down (the caller keeps it). Every poll of the latch reports neither to the game, so releasing one button first does not
// leak the other; Minus alone before Plus reaches the game unchanged. With the menu open the game sees a neutral pad.
// Returns 1 in the poll where both go down: the caller toggles the menu once.
int input_menu_gate(int *held, int menu_open, uint64_t *buttons, int32_t sticks[4]) {
    const uint64_t combo = 1ull << B_MINUS | 1ull << B_PLUS;
    uint64_t raw = *buttons & combo;
    int edge = 0;
    if (*held) {
        *held = raw != 0;
    } else if (raw == combo) {
        *held = 1;
        edge = 1;
    }
    if (*held) *buttons &= ~combo;
    if (menu_open) {
        *buttons = 0;
        sticks[0] = sticks[1] = sticks[2] = sticks[3] = 0;
    }
    return edge;
}
