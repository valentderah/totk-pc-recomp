// kbm_scheme.c: the engine behind kbm_scheme.h. The rows live in kbm_scheme_ui.inc and
// kbm_scheme_gameplay.inc, unchanged from Weeping of the Empire (MIT), so an upstream change to the scheme is a
// copy of those two files. Everything here is the part upstream spreads over pc_input.cpp (the routing and the
// per-context resolution), pc_key_hold.h (the hold gates) and the state machine of pc_scheme.inc's kinds.
//
// The frame is a pure function of (keys held, context, clock) plus the press and pulse state the timed kinds keep:
//   H  held:     the row's bits while its key is held (upstream's 'G' virtual-key rows too, stage 2a: the pad bits)
//   P  press:    the bits for KBM_PULSE_MS once per press
//   T  tap:      a release within KBM_TAP_MS is a tap; held longer, the next row with the same scope, action and
//                keys (the pair, upstream's "the key becomes the NEXT row's") is held until the release
//   M  macro:    a macro the pad layer cannot run: the hold macros hold their bits, the rest pulse them on a press
//   O  op:       a menu operation: the bits the op means (confirm, back, tab, scroll), else the row's bits
//   E  external: another part claims the key (a click, the map's pointer): nothing here
//   X  none:     the action has no key in this scope: nothing, and no row of a group applies either
//   K  keys:     as E for the router; the part that acts reads the key itself
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kbm_scheme.h"
#include "cfg.h"
#include "input_map.h"

// ---- the vocabulary of the tables -------------------------------------------------------------

// Menu operations (upstream pc_input.h PcMenuOp). TAB_GOTO and MENU_GOTO walk a menu's tabs, which belongs to a
// screen hook the port does not have: they fall back to the row's bits.
enum {
    PCOP_NONE, PCOP_CONFIRM, PCOP_BACK, PCOP_TAB_PREV, PCOP_TAB_NEXT, PCOP_TAB_GOTO, PCOP_CHOICE_PICK,
    PCOP_SCROLL, PCOP_MENU_GOTO
};

// Macros (upstream PcMacroId). A macro is a sequence the game's own virtual keys need; without that hook a row
// holds its bits for the hold macros (attack, sprint, the draw, the arms) and pulses them for the rest (the
// ability picks, the strips, the holster), which is upstream's stage 2a.
enum {
    MAC_NONE, MAC_ABILITY_1, MAC_ABILITY_2, MAC_ABILITY_3, MAC_ABILITY_4, MAC_ABILITY_5, MAC_ABILITY_6,
    MAC_ARROW_NEXT, MAC_ARROW_PREV, MAC_ATTACK, MAC_BOW_DRAW, MAC_BOW_OUT, MAC_BOW_STRIP, MAC_BOW_TOGGLE,
    MAC_CONSTRUCT_ATTACH, MAC_CONSTRUCT_LEAVE, MAC_HOLSTER, MAC_LOWER, MAC_NEXT, MAC_PREV, MAC_PRIMARY,
    MAC_PULL, MAC_PUSH, MAC_RAISE, MAC_RECIPE_HISTORY, MAC_RECIPE_OPEN, MAC_ROTATE, MAC_SECONDARY, MAC_SPRINT,
    MAC_UNSTICK, MAC_VEHICLE_LEAVE, MAC_WEAPON_STRIP, MAC_COUNT
};

// The macros whose key is held while it acts. The others act once, on the press.
// The ability picks and the two strips belong here too: their pad bits open a wheel or a strip that the left stick
// and the d-pad then walk, and the release picks -- pulsed, they would open and close in one frame.
static const uint8_t g_macro_hold[MAC_COUNT] = {
    [MAC_ABILITY_1] = 1, [MAC_ABILITY_2] = 1, [MAC_ABILITY_3] = 1, [MAC_ABILITY_4] = 1, [MAC_ABILITY_5] = 1,
    [MAC_ABILITY_6] = 1, [MAC_WEAPON_STRIP] = 1, [MAC_BOW_STRIP] = 1,
    [MAC_ATTACK] = 1, [MAC_SPRINT] = 1, [MAC_BOW_DRAW] = 1, [MAC_PRIMARY] = 1, [MAC_SECONDARY] = 1,
    [MAC_RAISE] = 1, [MAC_LOWER] = 1, [MAC_PUSH] = 1, [MAC_PULL] = 1, [MAC_ROTATE] = 1,
};

// The tables name the pad bits as masks (upstream's B_A is 1 << 0); input_map.h names them by their position in
// the Npad word. K + the upstream name is the mask, so a row still reads as upstream wrote it.
#define KB_A (1u << B_A)
#define KB_B (1u << B_B)
#define KB_X (1u << B_X)
#define KB_Y (1u << B_Y)
#define KB_L (1u << B_L)
#define KB_R (1u << B_R)
#define KB_ZL (1u << B_ZL)
#define KB_ZR (1u << B_ZR)
#define KB_PLUS (1u << B_PLUS)
#define KB_MINUS (1u << B_MINUS)
#define KB_DU (1u << B_UP)
#define KB_DD (1u << B_DOWN)
#define KB_DL (1u << B_LEFT)
#define KB_DR (1u << B_RIGHT)
#define KB_LS (1u << B_STICK_L)
#define KB_RS (1u << B_STICK_R)
#define KM_U (1u << B_SL_UP)
#define KM_D (1u << B_SL_DOWN)
#define KM_L (1u << B_SL_LEFT)
#define KM_R (1u << B_SL_RIGHT)
#define KR_U (1u << B_SR_UP)
#define KR_D (1u << B_SR_DOWN)
#define KR_L (1u << B_SR_LEFT)
#define KR_R (1u << B_SR_RIGHT)
#define KM_WALK KBM_OUT_WALK

// PCOP_MENU_GOTO's argument (upstream pc_input.h): the page a key walks to. Without the menu cursor hook the op
// falls back to the row's pad bits, which open the screen the pad's own key does.
#define PCMG_PAD 0x10
#define PCMG_ITEM 0x20
#define PCMG_LAST 0x30
#define PCMG_CANCEL (-1)

// PCVK is the gameplay table's own (it defines and undefines it around its rows).

typedef struct {
    const char *action;
    const char *keys;
    uint32_t out;      // pad bits (0..15) and stick directions (16..23), or KBM_OUT_WALK
    uint64_t vk;       // PC_VROW's virtual-key mask: stage 2b, unused
    int16_t arg;       // the op's or the macro's argument
    int16_t pair;      // a 'T' row's held row, -1 without one
    uint8_t scope;
    uint8_t kind;
    uint8_t op;
    uint8_t nkeys;
    uint8_t key[4];
} KbmRow;

#define PC_ROW(scope, action, kind, op, arg, out, keys)                  \
    { action, keys, (uint32_t)(out), 0, (int16_t)(arg), -1, KBM_SC_##scope, kind, op, 0, {0, 0, 0, 0} },
#define PC_VROW(scope, action, vk, glyph, keys)                          \
    { action, keys, (uint32_t)(glyph), (uint64_t)(vk), 0, -1, KBM_SC_##scope, 'G', PCOP_NONE, 0, {0, 0, 0, 0} },
// Upstream marks a scope whose keys no longer fall back to the console layout. This port has no console-layout
// fallback under the scheme, so the marker carries nothing.
#define PC_NOLAYOUT(scope)

// The feature gates the gameplay table uses (upstream's pc_input.h): this port carries the rows for all of them,
// so the camera's and the Scope's rows are in.
#define PC_HAS_CAMERA 1
#define PC_HAS_SCOPE 1

static KbmRow g_rows[] = {
#include "kbm_scheme_ui.inc"
#include "kbm_scheme_gameplay.inc"
};

#define KBM_ROWS_MAX 320
#define KBM_ACTIONS_MAX 128
_Static_assert(sizeof g_rows / sizeof g_rows[0] <= KBM_ROWS_MAX, "raise KBM_ROWS_MAX");

static const int g_nrows = (int)(sizeof g_rows / sizeof g_rows[0]);

// scope -> the scope its rows are inherited from (upstream: ROOT > GAMEPLAY, and ROOT > UI > MENUS > the menus).
static const int8_t g_parent[KBM_SC_COUNT] = {
    [KBM_SC_ROOT] = -1,
    [KBM_SC_GAMEPLAY] = KBM_SC_ROOT,
    [KBM_SC_UI] = KBM_SC_ROOT,
    [KBM_SC_MENUS] = KBM_SC_UI,
    [KBM_SC_WORLD] = KBM_SC_GAMEPLAY, [KBM_SC_CUTSCENE] = KBM_SC_GAMEPLAY, [KBM_SC_TRANSITION] = KBM_SC_GAMEPLAY,
    [KBM_SC_CLIMB] = KBM_SC_GAMEPLAY, [KBM_SC_AIR] = KBM_SC_GAMEPLAY, [KBM_SC_GLIDE] = KBM_SC_GAMEPLAY,
    [KBM_SC_SWIM] = KBM_SC_GAMEPLAY, [KBM_SC_AIM] = KBM_SC_GAMEPLAY, [KBM_SC_ABILITY] = KBM_SC_GAMEPLAY,
    [KBM_SC_HORSE] = KBM_SC_GAMEPLAY, [KBM_SC_VEHICLE] = KBM_SC_GAMEPLAY, [KBM_SC_LOADING] = KBM_SC_GAMEPLAY,
    [KBM_SC_QUICKMENU] = KBM_SC_GAMEPLAY, [KBM_SC_CAMERA] = KBM_SC_GAMEPLAY, [KBM_SC_SCOPE] = KBM_SC_GAMEPLAY,
    [KBM_SC_CONSTRUCT] = KBM_SC_GAMEPLAY,
    [KBM_SC_OVERLAY] = KBM_SC_UI, [KBM_SC_TITLE] = KBM_SC_UI, [KBM_SC_DIALOGUE] = KBM_SC_UI,
    [KBM_SC_CHOICE] = KBM_SC_UI,
    [KBM_SC_SAVES] = KBM_SC_MENUS, [KBM_SC_PAUSE] = KBM_SC_MENUS, [KBM_SC_POPUP] = KBM_SC_MENUS,
    [KBM_SC_PAD] = KBM_SC_MENUS, [KBM_SC_MAP] = KBM_SC_MENUS, [KBM_SC_OPTIONS] = KBM_SC_MENUS,
    [KBM_SC_SHOP] = KBM_SC_MENUS, [KBM_SC_MENU] = KBM_SC_MENUS, [KBM_SC_PADLOG] = KBM_SC_MENUS,
    [KBM_SC_RECIPE] = KBM_SC_MENUS,
};

static const char *const g_scope_names[KBM_SC_COUNT] = {
    [KBM_SC_ROOT] = "root", [KBM_SC_GAMEPLAY] = "gameplay", [KBM_SC_UI] = "ui", [KBM_SC_MENUS] = "menus",
    [KBM_SC_WORLD] = "world", [KBM_SC_OVERLAY] = "overlay", [KBM_SC_TITLE] = "title", [KBM_SC_SAVES] = "saves",
    [KBM_SC_PAUSE] = "pause", [KBM_SC_POPUP] = "popup", [KBM_SC_PAD] = "pad", [KBM_SC_MAP] = "map",
    [KBM_SC_OPTIONS] = "options", [KBM_SC_SHOP] = "shop", [KBM_SC_MENU] = "menu", [KBM_SC_DIALOGUE] = "dialogue",
    [KBM_SC_CHOICE] = "choice", [KBM_SC_CUTSCENE] = "cutscene", [KBM_SC_TRANSITION] = "transition",
    [KBM_SC_CLIMB] = "climb", [KBM_SC_AIR] = "air", [KBM_SC_GLIDE] = "glide", [KBM_SC_SWIM] = "swim",
    [KBM_SC_AIM] = "aim", [KBM_SC_ABILITY] = "ability", [KBM_SC_HORSE] = "horse", [KBM_SC_VEHICLE] = "vehicle",
    [KBM_SC_LOADING] = "loading", [KBM_SC_QUICKMENU] = "quickmenu", [KBM_SC_PADLOG] = "padlog",
    [KBM_SC_CAMERA] = "camera", [KBM_SC_SCOPE] = "scope", [KBM_SC_CONSTRUCT] = "construct",
    [KBM_SC_RECIPE] = "recipe",
};

// ---- state ------------------------------------------------------------------------------------

static const char *g_action[KBM_ACTIONS_MAX];
static int g_nactions;
static int16_t g_active[KBM_SC_COUNT][KBM_ACTIONS_MAX];   // (scope, action) -> row, -1 none

static uint64_t g_pulse[KBM_ROWS_MAX];         // a P / O / M / tap row's bits run until this ms
static uint32_t g_pulse_bits[KBM_ROWS_MAX];

// TOTK_KBM_PULSE_MS: how long those bits stay pressed; 0 keeps them for the frame of the press only.
static uint32_t pulse_ms(void) {
    int ms = cfg_int("TOTK_KBM_PULSE_MS");
    return ms < 0 ? 0 : ms > 200 ? 200 : (uint32_t)ms;
}

static void pulse_row(int r, uint32_t bits, uint64_t now_ms, uint32_t *out) {
    uint32_t ms = pulse_ms();
    if (ms) g_pulse[r] = now_ms + ms, g_pulse_bits[r] = bits;
    else *out |= bits;
}
static uint64_t g_press[KBM_ROWS_MAX];         // a 'T' row's press
static uint8_t g_hold[KBM_ROWS_MAX];           // ... and its held row is in effect
static uint8_t g_was[KBM_ROWS_MAX];
static uint8_t g_held[KBM_ROWS_MAX];

// MAC_NEXT / MAC_PREV and MAC_ARROW_NEXT / MAC_ARROW_PREV: the game's quick select is "hold the d-pad and walk the
// strip with the right stick" (D-pad right for the weapons, ZR + D-pad up for the arrows; docs/controls.md). The pad
// layer has no next-weapon action here, so a notch plays upstream's macro itself (pc_gameplay.cpp, MAC_NEXT): d-right
// held until the strip is in front, one flick of the right stick, two frames more on the d-pad, the release that
// equips. One step at a time, because one strip is open at a time; the notches that arrive during a step wait.
enum { KBM_STEP_OPEN_FRAMES = 6, KBM_STEP_FLICK_FRAMES = 2, KBM_STEP_REST_FRAMES = 2, KBM_STEP_WAIT_MAX = 8 };
static struct {
    int on;                     // a step is running
    int dpad, flick;            // the strip's key and the stick direction this step walks it
    uint64_t at;                // when the step began
    uint32_t frame;             // the frame length it is timed with, ms
    int pending;                // notches waiting: + next, - previous
    int p_dpad, p_next, p_prev;   // the strip and its two stick directions, from the last notch
} g_step;

// One presented frame's length (kbm_scheme_set_frame_ms). The step counts frames, as upstream's macro does: d-right
// until the strip is in front (6 frames where the screen cannot be read), a 2-frame flick, 2 frames more on the d-pad,
// then the release that equips. Counting milliseconds instead was too short at 30 fps and too long at 120.
static uint32_t g_frame_ms = 16;

void kbm_scheme_set_frame_ms(unsigned ms) {
    g_frame_ms = !ms ? 16 : ms < 8 ? 8 : ms > 100 ? 100 : ms;
}

// The strip a macro walks: its d-pad key and the two stick directions, +1 the next row, -1 the previous one.
static int step_family(int arg, int *dpad, int *next, int *prev) {
    switch (arg) {
    case MAC_NEXT: case MAC_PREV: *dpad = B_RIGHT; break;
    case MAC_ARROW_NEXT: case MAC_ARROW_PREV: *dpad = B_UP; break;   // ZR + D-pad up: the bow is in hand
    default: return 0;
    }
    *next = B_SR_RIGHT;
    *prev = B_SR_LEFT;
    return arg == MAC_NEXT || arg == MAC_ARROW_NEXT ? 1 : -1;
}

static int g_ctx = KBM_SC_WORLD;
// G and the wheel: the material page, from screenshots taken frame by frame. With the throw key held, D-pad up opens
// the page and the page stays only while up is still held; the right stick walks it; letting up go closes it with the
// highlighted material in hand, and letting the throw key go is what throws it. So the first notch takes UP and keeps
// it, every later notch only flicks the stick inside that hold, and releasing the throw key drops both. Nothing here
// presses A or R on its own, so the player's release is the confirm.
enum { MAT_FLICK_FRAMES = 3 };
static struct { int open; uint64_t flick_until; int flick; } g_mat;
static int g_ready;
static int g_unknown_keys;

// TOTK_KBM_BINDS: "action=key,key|context.action=key" overrides a scheme action's keys, in every context or in one.
// The rows themselves stay the upstream table; an empty spec keeps them all.
enum { KBM_BIND_MAX = 128 };
static struct { char context[24]; char action[32]; char keys[64]; } g_binds[KBM_BIND_MAX];
static int g_nbinds;

// A scope name as the spec and the tab write it, lower-case; -1 for another name.
static int scope_from_name(const char *s, size_t n) {
    char buf[24];
    if (!n || n >= sizeof buf) return -1;
    for (size_t i = 0; i < n; i++) buf[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] - 'A' + 'a' : s[i]);
    buf[n] = 0;
    for (int i = 0; i < KBM_SC_COUNT; i++)
        if (g_scope_names[i] && !strcmp(g_scope_names[i], buf)) return i;
    return -1;
}

static const char *keys_for(const char *action, uint8_t scope) {
    const char *ctx = scope < KBM_SC_COUNT ? g_scope_names[scope] : NULL;
    for (int i = 0; i < g_nbinds; i++)
        if (g_binds[i].context[0] && ctx && !strcmp(g_binds[i].context, ctx) && !strcmp(g_binds[i].action, action))
            return g_binds[i].keys;
    for (int i = 0; i < g_nbinds; i++)
        if (!g_binds[i].context[0] && !strcmp(g_binds[i].action, action)) return g_binds[i].keys;
    return NULL;
}

void kbm_scheme_set_binds(const char *spec) {
    g_nbinds = 0;
    g_ready = 0;   // rows parse their keys again on the next init
    const char *p = spec;
    while (p && *p && g_nbinds < KBM_BIND_MAX) {
        while (*p == ' ' || *p == ',' || *p == '|') p++;
        const char *eq = strchr(p, '=');
        if (!eq) break;
        const char *name = p;
        size_t an = (size_t)(eq - p);
        while (an && name[an - 1] == ' ') an--;
        size_t cn = 0;
        const char *dot = memchr(name, '.', an);
        int scope = -1;
        if (dot) {
            cn = (size_t)(dot - name);
            scope = scope_from_name(name, cn);
        }
        const char *action = scope >= 0 ? dot + 1 : name;
        size_t action_n = scope >= 0 ? (size_t)(eq - action) : an;
        while (action_n && action[action_n - 1] == ' ') action_n--;
        while (action_n && *action == ' ') action++, action_n--;
        const char *end = eq + 1;
        while (*end && *end != '|') end++;
        size_t kn = (size_t)(end - (eq + 1));
        while (kn && (eq[1 + kn - 1] == ' ' || eq[1 + kn - 1] == '\r')) kn--;
        if (action_n && action_n < sizeof g_binds[0].action && kn && kn < sizeof g_binds[0].keys) {
            if (scope >= 0) {
                snprintf(g_binds[g_nbinds].context, sizeof g_binds[0].context, "%s", g_scope_names[scope]);
            } else {
                g_binds[g_nbinds].context[0] = 0;
            }
            memcpy(g_binds[g_nbinds].action, action, action_n);
            g_binds[g_nbinds].action[action_n] = 0;
            memcpy(g_binds[g_nbinds].keys, eq + 1, kn);
            g_binds[g_nbinds].keys[kn] = 0;
            g_nbinds++;
        }
        p = end;
        if (*p == '|') p++;
    }
}

// ---- keys -------------------------------------------------------------------------------------

// KEY_NONE for a name the table has and this port does not know.
static int key_from_name(const char *s, size_t n) {
    static const struct { const char *name; int key; } table[] = {
        {"space", KEY_SPACE}, {"tab", KEY_TAB}, {"shift", KEY_LSHIFT}, {"lshift", KEY_LSHIFT}, {"rshift", KEY_RSHIFT},
        {"ctrl", KEY_LCTRL}, {"lctrl", KEY_LCTRL}, {"rctrl", KEY_RCTRL}, {"alt", KEY_LALT}, {"capslock", KEY_CAPSLOCK},
        {"esc", KEY_ESC}, {"enter", KEY_ENTER}, {"up", KEY_UP}, {"down", KEY_DOWN}, {"left", KEY_LEFT},
        {"right", KEY_RIGHT}, {"mouse left", KEY_MOUSE_L}, {"mouse right", KEY_MOUSE_R}, {"mouse middle", KEY_MOUSE_M},
        {"mouse x1", KEY_MOUSE_X1}, {"mouse x2", KEY_MOUSE_X2},
        {"wheel up", KEY_WHEEL_UP}, {"wheel down", KEY_WHEEL_DOWN},
        {"backspace", KEY_BACKSPACE}, {"insert", KEY_INSERT}, {"delete", KEY_DELETE}, {"home", KEY_HOME},
        {"end", KEY_END}, {"pageup", KEY_PAGEUP}, {"pagedown", KEY_PAGEDOWN},
        {"f2", KEY_F2}, {"f3", KEY_F3}, {"f4", KEY_F4}, {"f5", KEY_F5}, {"f6", KEY_F6}, {"f7", KEY_F7},
        {"f9", KEY_F9}, {"f12", KEY_F12},
        {"num0", KEY_NUM0}, {"num1", KEY_NUM1}, {"num2", KEY_NUM2}, {"num3", KEY_NUM3}, {"num4", KEY_NUM4},
        {"num5", KEY_NUM5}, {"num6", KEY_NUM6}, {"num7", KEY_NUM7}, {"num8", KEY_NUM8}, {"num9", KEY_NUM9},
        {"num add", KEY_NUM_ADD}, {"num sub", KEY_NUM_SUB}, {"num mul", KEY_NUM_MUL}, {"num div", KEY_NUM_DIV},
        {"num decimal", KEY_NUM_DECIMAL},
        {"minus", KEY_MINUS}, {"equals", KEY_EQUALS}, {"lbracket", KEY_LBRACKET}, {"rbracket", KEY_RBRACKET},
        {"semicolon", KEY_SEMICOLON}, {"apostrophe", KEY_APOSTROPHE}, {"comma", KEY_COMMA}, {"period", KEY_PERIOD},
        {"slash", KEY_SLASH}, {"backslash", KEY_BACKSLASH}, {"grave", KEY_GRAVE},
    };
    char buf[24];
    if (n == 0 || n >= sizeof buf) return KEY_NONE;
    for (size_t i = 0; i < n; i++) buf[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] - 'A' + 'a' : s[i]);
    buf[n] = 0;
    if (n == 1) {
        char c = buf[0];
        if (c >= 'a' && c <= 'z') return KEY_A + (c - 'a');
        if (c >= '0' && c <= '9') return KEY_0 + (c - '0');
    }
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
        if (!strcmp(buf, table[i].name)) return table[i].key;
    return KEY_NONE;
}

// "Esc, Mouse Right" -> the keys the game sees, in order; the first is the prompt's glyph.
static void parse_keys(KbmRow *row) {
    const char *override = keys_for(row->action, row->scope);
    // TOTK_KBM_MATERIALS_KEY: the material action's key, for G + a key that is easier to reach than C.
    if (!override && !strcmp(row->action, "materials")) override = cfg_value("TOTK_KBM_MATERIALS_KEY");
    const char *p = override ? override : row->keys;
    row->nkeys = 0;
    while (*p && row->nkeys < 4) {
        while (*p == ' ' || *p == ',') p++;
        const char *start = p;
        while (*p && *p != ',') p++;
        size_t n = (size_t)(p - start);
        while (n && start[n - 1] == ' ') n--;
        if (!n) break;
        int k = key_from_name(start, n);
        if (k == KEY_NONE) { g_unknown_keys++; continue; }
        row->key[row->nkeys++] = (uint8_t)k;
    }
}

static int row_held(const KbmRow *row, const uint8_t down[KEY_COUNT]) {
    for (int i = 0; i < row->nkeys; i++)
        if (down[row->key[i]]) return 1;
    return 0;
}

// Whether the key of action in the context in effect is held (the throw key routes the wheel to materials).
static int action_key_held(const char *action, const uint8_t down[KEY_COUNT]) {
    for (int a = 0; a < g_nactions; a++) {
        if (strcmp(g_action[a], action)) continue;
        int r = g_active[g_ctx][a];
        return r >= 0 && row_held(&g_rows[r], down);
    }
    return 0;
}

// ---- the table --------------------------------------------------------------------------------

static int action_id(const char *name) {
    for (int i = 0; i < g_nactions; i++)
        if (!strcmp(g_action[i], name)) return i;
    if (g_nactions == KBM_ACTIONS_MAX) return -1;
    g_action[g_nactions] = name;
    return g_nactions++;
}

void kbm_scheme_init(void) {
    if (g_ready) return;
    g_nactions = 0;
    g_unknown_keys = 0;
    for (int s = 0; s < KBM_SC_COUNT; s++)
        for (int a = 0; a < KBM_ACTIONS_MAX; a++) g_active[s][a] = -1;
    // Actions first: a row's id must not depend on the order the scopes are walked.
    for (int r = 0; r < g_nrows; r++) {
        parse_keys(&g_rows[r]);
        action_id(g_rows[r].action);
    }
    // A 'T' row's pair: the next row with the same scope, action and keys that holds (upstream's held kinds). The
    // pair is one action -- "one binding moves both" -- so the table is resolved before the lookup is built.
    for (int r = 0; r < g_nrows; r++) {
        if (g_rows[r].kind != 'T') continue;
        for (int q = r + 1; q < g_nrows; q++) {
            if (strcmp(g_rows[q].action, g_rows[r].action)) continue;
            if (g_rows[q].scope != g_rows[r].scope) continue;
            if (strcmp(g_rows[q].keys, g_rows[r].keys)) continue;
            char k = (char)g_rows[q].kind;
            if (k == 'H' || k == 'M' || k == 'G' || k == 'P') { g_rows[r].pair = (int16_t)q; break; }
        }
    }
    // The contexts in enumeration order, parents before children: inherit the parent's row for every action, then
    // the scope's own rows replace it (upstream: "a row applies to its scope's contexts that have no row of their
    // own for the action"). A 'T' row keeps the slot its held twin would take: the twin is the pair, not a row a
    // key can match on its own.
    for (int s = 0; s < KBM_SC_COUNT; s++) {
        int p = g_parent[s];
        if (p >= 0) memcpy(g_active[s], g_active[p], sizeof g_active[s]);
        for (int r = 0; r < g_nrows; r++) {
            if (g_rows[r].scope != s) continue;
            int a = action_id(g_rows[r].action);
            if (a < 0) continue;
            int cur = g_active[s][a];
            if (cur >= 0 && g_rows[cur].kind == 'T' && g_rows[cur].pair == r) continue;
            g_active[s][a] = (int16_t)r;
        }
    }
    memset(g_pulse, 0, sizeof g_pulse);
    memset(g_press, 0, sizeof g_press);
    memset(g_hold, 0, sizeof g_hold);
    memset(g_was, 0, sizeof g_was);
    memset(&g_step, 0, sizeof g_step);
    g_ctx = KBM_SC_WORLD;
    g_ready = 1;
}

int kbm_scheme_row_count(void) { kbm_scheme_init(); return g_nrows; }
int kbm_scheme_unknown_key_count(void) { kbm_scheme_init(); return g_unknown_keys; }

void kbm_scheme_set_ctx(int scope) {
    kbm_scheme_init();
    if (scope < 0 || scope >= KBM_SC_COUNT) scope = KBM_SC_WORLD;
    if (scope != g_ctx) {
        // A key that is already held when the context changes must not read as a fresh press in the new one: the Esc
        // that opened the pause menu is the pause's "back", and firing it here would close the menu the same frame it
        // opened. g_held still holds the previous frame's rows, so carrying it into g_was lets the key end its hold
        // where it started and only a new press acts. Upstream's 'T' rows still start over (g_hold, g_press below).
        for (int r = 0; r < g_nrows; r++) g_was[r] = g_held[r];
        memset(g_hold, 0, sizeof g_hold);
        g_step.on = 0, g_step.pending = 0;   // a strip left open by the old context's step closes with it
        g_ctx = scope;
    }
}

int kbm_scheme_ctx(void) { return g_ctx; }

const char *kbm_scheme_scope_name(int scope) {
    if (scope < 0 || scope >= KBM_SC_COUNT) return "?";
    return g_scope_names[scope] ? g_scope_names[scope] : "?";
}

// ---- the frame --------------------------------------------------------------------------------

static uint32_t op_bits(const KbmRow *row) {
    switch (row->op) {
    case PCOP_CONFIRM: case PCOP_CHOICE_PICK: return 1u << B_A;
    case PCOP_BACK: return 1u << B_B;
    case PCOP_TAB_PREV: return 1u << B_L;
    case PCOP_TAB_NEXT: return 1u << B_R;
    case PCOP_SCROLL: return 1u << (row->arg < 0 ? B_UP : B_DOWN);
    default: return row->out;
    }
}

// A row's out as a stick: the four directions of `first` (left, up, right, down), a diagonal on the unit circle.
static void dirs_to_stick(uint32_t out, int first, int32_t v[2]) {
    int l = (int)((out >> (first + 0)) & 1), u = (int)((out >> (first + 1)) & 1);
    int r = (int)((out >> (first + 2)) & 1), d = (int)((out >> (first + 3)) & 1);
    int x = r - l, y = u - d;
    if (x && y) { v[0] = x * 23170; v[1] = y * 23170; }
    else { v[0] = x * 32767; v[1] = y * 32767; }
}

void kbm_scheme_frame(const uint8_t down[KEY_COUNT], uint64_t now_ms, uint64_t *buttons, int32_t left[2],
                      int32_t right[2]) {
    kbm_scheme_init();
    *buttons = 0;
    left[0] = left[1] = right[0] = right[1] = 0;
    for (int r = 0; r < g_nrows; r++) g_held[r] = (uint8_t)row_held(&g_rows[r], down);

    uint32_t out = 0;
    for (int a = 0; a < g_nactions; a++) {
        int r = g_active[g_ctx][a];
        if (r < 0) continue;
        KbmRow *row = &g_rows[r];
        int held = g_held[r], was = g_was[r];
        // While the throw key is held the left button belongs to the material page, and to nothing else: it closes the
        // page (the material stays in hand, so aiming continues) and it never reaches the attack row. The gate is the
        // throw key, not the page's state, so the rest of a real 50 to 150 ms click cannot leak into the world either --
        // the button comes back when the key comes up. The page's sort is on the middle button while it is up.
        if (!strcmp(row->action, "attack") && action_key_held("throw", down)) {
            if (held && !was && g_mat.open) g_mat.open = 0;
            continue;
        }
        if (!strcmp(row->action, "scope") && g_mat.open) {
            if (held && !was) pulse_row(r, 1u << B_Y, now_ms, &out);
            continue;
        }
        switch ((char)row->kind) {
        case 'H': case 'G': case 'K':
            if (held) out |= row->out;
            break;
        case 'M': {
            int arg = row->arg;
            // The throw key turns the wheel into the material page's own walk; see g_mat above.
            int mat = (arg == MAC_NEXT || arg == MAC_PREV) && action_key_held("throw", down);
            if (mat) {
                if (held && !was) {
                    if (!g_mat.open) g_mat.open = 1;   // the first notch only opens the page: UP goes out from here on
                    else {
                        g_mat.flick = arg == MAC_NEXT ? B_SR_RIGHT : B_SR_LEFT;
                        g_mat.flick_until = now_ms + MAT_FLICK_FRAMES * g_frame_ms;
                    }
                }
                break;
            }
            int dpad, next, prev;
            int step = step_family(arg, &dpad, &next, &prev);
            if (step) {
                // The strip's step (g_step): queue the notch, it runs below.
                if (held && !was && g_step.pending > -KBM_STEP_WAIT_MAX && g_step.pending < KBM_STEP_WAIT_MAX) {
                    g_step.pending += step;
                    g_step.p_dpad = dpad, g_step.p_next = next, g_step.p_prev = prev;
                }
                break;
            }
            if (row->arg > 0 && row->arg < MAC_COUNT && g_macro_hold[row->arg]) {
                if (held) out |= row->out;
            } else if (held && !was && row->out) {
                pulse_row(r, row->out, now_ms, &out);
            }
            break;
        }
        case 'P':
            if (held && !was && row->out) pulse_row(r, row->out, now_ms, &out);
            break;
        case 'O': {
            if (!held || was) break;
            uint32_t bits = op_bits(row);
            if (bits) pulse_row(r, bits, now_ms, &out);
            break;
        }
        case 'T':
            if (held && !was) { g_press[r] = now_ms; g_hold[r] = 0; }
            if (held) {
                if (!g_hold[r] && row->pair >= 0 && now_ms - g_press[r] >= KBM_TAP_MS) g_hold[r] = 1;
                if (g_hold[r] && row->pair >= 0) out |= g_rows[row->pair].out;
            } else if (was) {
                // A tap: this row's bits, or the pair's when the tap is a macro the pad layer cannot run (the bow's
                // take-out and put-away), so the key still does its nearest pad action once. A release that ends a
                // hold is not a tap.
                if (!g_hold[r] && now_ms - g_press[r] < KBM_TAP_MS) {
                    uint32_t bits = row->out;
                    if (!bits && row->pair >= 0) bits = g_rows[row->pair].out;
                    if (bits) pulse_row(r, bits, now_ms, &out);
                }
                g_hold[r] = 0;
            }
            break;
        case 'E': case 'X': default:
            break;
        }
    }
    for (int r = 0; r < g_nrows; r++) {
        if (g_pulse[r] > now_ms) out |= g_pulse_bits[r];
        else g_pulse[r] = 0;
    }
    // The strip step (MAC_NEXT / MAC_PREV): the d-pad held for the whole step, the stick's flick inside it, then the
    // d-pad alone again before the release that equips. The flick's bits are the right stick's directions (16..23),
    // which dirs_to_stick turns into a full deflection below.
    if (g_step.on && now_ms - g_step.at >=
                         (uint64_t)(KBM_STEP_OPEN_FRAMES + KBM_STEP_FLICK_FRAMES + KBM_STEP_REST_FRAMES) * g_step.frame)
        g_step.on = 0;
    if (!g_step.on && g_step.pending) {
        g_step.on = 1;
        g_step.dpad = g_step.p_dpad;
        g_step.flick = g_step.pending > 0 ? g_step.p_next : g_step.p_prev;
        g_step.pending -= g_step.pending > 0 ? 1 : -1;
        g_step.at = now_ms;
        g_step.frame = g_frame_ms;
    }
    if (g_step.on) {
        uint64_t el = now_ms - g_step.at, open = (uint64_t)KBM_STEP_OPEN_FRAMES * g_step.frame;
        out |= 1u << g_step.dpad;
        if (el >= open && el < open + (uint64_t)KBM_STEP_FLICK_FRAMES * g_step.frame) out |= 1u << g_step.flick;
    }
    // The material page's press, held for its own few frames. The page state ends with the throw key: the next time it
    // is held, the first notch opens the page again rather than stepping a list that is not on screen.
    // The material page: UP is held for as long as the page is open (the game keeps the page only while it is held --
    // screenshots: G + up opens it, both held 1.5 s later it is still there, releasing up closes it), and each notch
    // after the first flicks the right stick inside that hold. The throw key owns the page, and the release is checked
    // before the bits go out so the frame it comes up lets UP go with it: that is the player's confirm.
    if (!action_key_held("throw", down)) g_mat.open = 0;   // the throw key owns the page: up and the mouse gate
    if (g_mat.open) {
        out |= 1u << B_UP;
        if (now_ms < g_mat.flick_until) out |= 1u << g_mat.flick;
    }
    for (int r = 0; r < g_nrows; r++) g_was[r] = g_held[r];

    int walk = (out & KBM_OUT_WALK) != 0;
    out &= ~(uint32_t)KBM_OUT_WALK;
    dirs_to_stick(out, B_SL_LEFT, left);
    dirs_to_stick(out, B_SR_LEFT, right);
    if (walk) {
        left[0] = (int32_t)(left[0] * KBM_WALK_SCALE);
        left[1] = (int32_t)(left[1] * KBM_WALK_SCALE);
    }
    // Only the buttons here: input.c derives the stick directions (16..23) from the sticks it ends up with, so
    // setting them twice would keep a direction alive for the frame after a key is released.
    *buttons = out & 0xFFFFu;
}

int kbm_scheme_keys(int scope, const char *action, int *keys, int max) {
    kbm_scheme_init();
    if (scope < 0 || scope >= KBM_SC_COUNT || !action) return 0;
    for (int a = 0; a < g_nactions; a++) {
        if (strcmp(g_action[a], action)) continue;
        int r = g_active[scope][a];
        if (r < 0) return 0;
        int n = g_rows[r].nkeys < max ? g_rows[r].nkeys : max;
        for (int i = 0; i < n; i++) keys[i] = g_rows[r].key[i];
        return n;
    }
    return 0;
}

// ---- the F1 "Keyboard & mouse" tab's view of the table -----------------------------------------

int kbm_scheme_action_count(void) {
    kbm_scheme_init();
    return g_nactions;
}

const char *kbm_scheme_action_name(int index) {
    kbm_scheme_init();
    return index >= 0 && index < g_nactions ? g_action[index] : NULL;
}

int kbm_scheme_action_keys(const char *action, int *keys, int max) {
    kbm_scheme_init();
    if (!action) return 0;
    for (int r = 0; r < g_nrows; r++) {
        if (strcmp(g_rows[r].action, action)) continue;
        int n = g_rows[r].nkeys < max ? g_rows[r].nkeys : max;
        for (int i = 0; i < n; i++) keys[i] = g_rows[r].key[i];
        return n;
    }
    return 0;
}

int kbm_scheme_action_default_keys(const char *action, int *keys, int max) {
    kbm_scheme_init();
    if (!action) return 0;
    for (int r = 0; r < g_nrows; r++) {
        if (strcmp(g_rows[r].action, action)) continue;
        const char *p = g_rows[r].keys;
        int n = 0;
        while (*p && n < max) {
            while (*p == ' ' || *p == ',') p++;
            const char *start = p;
            while (*p && *p != ',') p++;
            size_t len = (size_t)(p - start);
            while (len && start[len - 1] == ' ') len--;
            if (!len) break;
            int k = key_from_name(start, len);
            if (k == KEY_NONE) continue;
            keys[n++] = k;
        }
        return n;
    }
    return 0;
}

static int scope_action_seen(int scope, int row) {
    for (int r = 0; r < row; r++)
        if (g_rows[r].scope == scope && !strcmp(g_rows[r].action, g_rows[row].action)) return 1;
    return 0;
}

int kbm_scheme_scope_action_count(int scope) {
    kbm_scheme_init();
    if (scope < 0 || scope >= KBM_SC_COUNT) return 0;
    int n = 0;
    for (int r = 0; r < g_nrows; r++)
        if (g_rows[r].scope == scope && !scope_action_seen(scope, r)) n++;
    return n;
}

const char *kbm_scheme_scope_action(int scope, int index) {
    kbm_scheme_init();
    if (scope < 0 || scope >= KBM_SC_COUNT || index < 0) return NULL;
    int n = 0;
    for (int r = 0; r < g_nrows; r++) {
        if (g_rows[r].scope != scope || scope_action_seen(scope, r)) continue;
        if (n++ == index) return g_rows[r].action;
    }
    return NULL;
}

int kbm_scheme_bind_kind(int scope, const char *action) {
    if (!action) return 0;
    const char *ctx = scope >= 0 && scope < KBM_SC_COUNT ? g_scope_names[scope] : NULL;
    for (int i = 0; i < g_nbinds; i++)
        if (g_binds[i].context[0] && ctx && !strcmp(g_binds[i].context, ctx) && !strcmp(g_binds[i].action, action))
            return 2;
    for (int i = 0; i < g_nbinds; i++)
        if (!g_binds[i].context[0] && !strcmp(g_binds[i].action, action)) return 1;
    return 0;
}

const char *kbm_scheme_bind(int scope, const char *action) {
    int kind = kbm_scheme_bind_kind(scope, action);
    if (!kind) return NULL;
    const char *ctx = scope >= 0 && scope < KBM_SC_COUNT ? g_scope_names[scope] : NULL;
    for (int i = 0; i < g_nbinds; i++)
        if (kind == 1 ? !g_binds[i].context[0] : (ctx && !strcmp(g_binds[i].context, ctx)))
            if (!strcmp(g_binds[i].action, action)) return g_binds[i].keys;
    return NULL;
}
