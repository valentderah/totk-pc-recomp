// The remapping rows of the Controls tab (binds_page.h): edits a copy of the input tables (input_map.h), writes the TOTK_BIND_* lines that
// give the copy, and input_win.c applies them at its next poll; totk.ini follows a moment later (opt_save_soon). The
// gamepad cells show and change the table of the button layout in use (TOTK_PAD_LAYOUT).
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "ui/binds_page.h"
#include "ui/option_rows.h"

extern "C" {
#include "cfg.h"
#include "input.h"
#include "input_map.h"
#include "platform/platform.h"
#include "ui_text.h"
// host.h pulls in cpu.h, which is C only: the one function needed from it, as declared there.
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

static_assert(UI_BIND_RSTICK_DOWN - UI_BIND_A == ACTION_COUNT - 1, "UI_BIND_* follow the actions");
static_assert(UI_BIND_DESC_MINUS - UI_BIND_DESC_A == B_MINUS, "UI_BIND_DESC_* follow the actions");
static_assert(UI_KEY_PAGEDOWN - UI_KEY_SPACE == KEY_PAGEDOWN - KEY_SPACE, "UI_KEY_* follow the key ids");
static_assert(UI_PAD_RPADDLE2 - UI_PAD_SOUTH == PAD_RPADDLE2 - PAD_SOUTH && PAD_RPADDLE2 == PAD_COUNT - 1,
              "UI_PAD_* follow the pad ids");

enum { CAPTURE_SECONDS = 5 };
// The capture window stays this long after the press: the press and its click reach ImGui a frame or two later and must
// land on the window, not on a cell (a new capture), on Continue or as Esc closing the menu.
static const uint64_t kDoneNs = 350000000ull;

static const int kButtons[] = {B_A, B_B, B_X, B_Y, B_L, B_R, B_ZL, B_ZR, B_STICK_L, B_STICK_R, B_PLUS, B_MINUS,
                               B_UP, B_DOWN, B_LEFT, B_RIGHT};
static const int kMove[] = {B_SL_UP, B_SL_DOWN, B_SL_LEFT, B_SL_RIGHT};
static const int kCamera[] = {B_SR_UP, B_SR_DOWN, B_SR_LEFT, B_SR_RIGHT};

static struct {
    int action = -1, cell;   // the cell waiting for a press (0, 1 keys, 2 the controller); action -1 none
    bool reset;              // the next capture poll takes what is held as the start
    uint64_t start, done;    // plat_now_ns of the start and of the press, timeout or Esc (0 still waiting)
} g_cap;
static char g_note[160];     // what the last change took or swapped, after its row's description
static int g_note_action = -1;
static uint64_t g_note_until;

static const char *action_label(int a) { return menu_text((UiTextId)(UI_BIND_A + a)); }

static UiTextId action_desc(int a) {
    if (a <= B_MINUS) return (UiTextId)(UI_BIND_DESC_A + a);
    if (a <= B_DOWN) return UI_BIND_DESC_DPAD;
    return a <= B_SL_DOWN ? UI_BIND_DESC_MOVE : UI_BIND_DESC_CAMERA;
}

static void utf8(uint32_t c, char *out, size_t n) {
    if (n < 5) { out[0] = 0; return; }
    if (c < 0x80) { out[0] = (char)c; out[1] = 0; }
    else if (c < 0x800) { out[0] = (char)(0xC0 | c >> 6); out[1] = (char)(0x80 | (c & 0x3F)); out[2] = 0; }
    else {
        out[0] = (char)(0xE0 | c >> 12); out[1] = (char)(0x80 | (c >> 6 & 0x3F)); out[2] = (char)(0x80 | (c & 0x3F));
        out[3] = 0;
    }
}

// A key as the player sees it: its catalog name, the numpad format, the character on a punctuation key in the keyboard
// layout in use, else its totk.ini name in capitals (letters, digits, F keys).
static const char *key_label(int k, char *buf, size_t n) {
    if (k == KEY_NONE) return "";
    if (k >= KEY_SPACE && k <= KEY_PAGEDOWN) return menu_text((UiTextId)(UI_KEY_SPACE + (k - KEY_SPACE)));
    if (k >= KEY_NUM0 && k <= KEY_NUM_DECIMAL) {
        static const char *const ops[] = {"+", "-", "*", "/", "."};
        char d[4];
        if (k <= KEY_NUM9) snprintf(d, sizeof d, "%d", k - KEY_NUM0);
        else snprintf(d, sizeof d, "%s", ops[k - KEY_NUM_ADD]);
        snprintf(buf, n, menu_text(UI_KEY_NUM_FMT), d);
        return buf;
    }
    if (uint32_t c = input_key_char(k)) {
        utf8(c, buf, n);
        return buf;
    }
    snprintf(buf, n, "%s", input_key_name(k));
    for (char *p = buf; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
    return buf;
}

// A gamepad button: the label the open pad has on a face button, else its place on the pad.
static const char *pad_label(int p) {
    if (p == PAD_NONE) return "";
    switch (input_pad_label(p)) {
    case PAD_LABEL_A: return "A";
    case PAD_LABEL_B: return "B";
    case PAD_LABEL_X: return "X";
    case PAD_LABEL_Y: return "Y";
    case PAD_LABEL_CROSS: return menu_text(UI_PAD_CROSS);
    case PAD_LABEL_CIRCLE: return menu_text(UI_PAD_CIRCLE);
    case PAD_LABEL_SQUARE: return menu_text(UI_PAD_SQUARE);
    case PAD_LABEL_TRIANGLE: return menu_text(UI_PAD_TRIANGLE);
    }
    return menu_text((UiTextId)(UI_PAD_SOUTH + (p - PAD_SOUTH)));
}

// The layout the gamepad cells show: the one in effect, or without a gamepad the one the setting names (Xbox for auto).
static int shown_layout(void) {
    int l = input_pad_layout();
    return l >= 0 ? l : input_pad_layout_pick(cfg_value("TOTK_PAD_LAYOUT"), 0);
}

// Writes the bind_ line of every action whose line for m differs from the one set; a line at the defaults goes away.
static void write_lines(const InputMap *m) {
    for (int a = 0; a < ACTION_COUNT; a++) {
        char var[40], line[256];
        snprintf(var, sizeof var, "TOTK_BIND_%s", input_action_name(a));
        input_map_line(m, a, line, sizeof line);
        const char *now = cfg_get(var);
        if (!strcmp(line, now ? now : "")) continue;
        if (line[0]) {
            cfg_set(var, line);
        } else {
            const char *k = var + 5;
            cfg_reset(&k, 1);
        }
        rt_log("menu: bind_%s = %s", input_action_name(a), line[0] ? line : "(default)");
    }
    opt_save_soon();
}

static void set_note(int action, UiTextId fmt, const char *a, const char *b) {
    snprintf(g_note, sizeof g_note, menu_text(fmt), a, b);
    g_note_action = action;
    g_note_until = plat_now_ns() + 5000000000ull;
}

// cell 0, 1: a key (KEY_NONE clears it); 2: a gamepad button (PAD_NONE clears it).
static void change(int action, int cell, int id) {
    InputMap m;
    input_map_snapshot(&m);
    char buf[48];
    if (cell < 2) {
        int lost = input_map_set_key(&m, action, cell, id);
        if (lost >= 0) set_note(action, UI_BINDS_TAKEN, key_label(id, buf, sizeof buf), action_label(lost));
    } else {
        int other = input_map_set_pad(&m, action, shown_layout(), id);
        if (other >= 0) set_note(action, UI_BINDS_SWAPPED, action_label(action), action_label(other));
    }
    write_lines(&m);
}

static void reset_binds(void) {
    char names[ACTION_COUNT][32];
    const char *keys[ACTION_COUNT];
    for (int a = 0; a < ACTION_COUNT; a++) {
        snprintf(names[a], sizeof names[a], "BIND_%s", input_action_name(a));
        keys[a] = names[a];
    }
    int n = cfg_reset(keys, ACTION_COUNT);
    opt_save_soon();
    g_note_action = -1;
    rt_log("menu: reset %d button bindings", n);
}

static void row_action(int a, const InputMap &m, int layout) {
    uint8_t k[INPUT_BINDING_MAX], p[INPUT_PAD_BINDING_MAX];
    int nk = input_map_keys_of(&m, a, k, INPUT_BINDING_MAX);
    int np = a < B_SL_LEFT ? input_map_pads_of(&m, a, layout, p, INPUT_PAD_BINDING_MAX) : 0;
    char b0[48], b1[48];
    const char *cells[3] = {nk > 0 ? key_label(k[0], b0, sizeof b0) : "", nk > 1 ? key_label(k[1], b1, sizeof b1) : "",
                            a >= B_SL_LEFT ? menu_text(a <= B_SL_DOWN ? UI_PAD_LSTICK : UI_PAD_RSTICK)
                            : np           ? pad_label(p[0])
                                           : ""};
    const bool enabled[3] = {true, true, a < B_SL_LEFT};   // the sticks stay sticks
    char key[32];
    snprintf(key, sizeof key, "BIND_%s", input_action_name(a));
    const char *note = g_note_action == a && plat_now_ns() < g_note_until ? g_note : nullptr;
    int r = opt_bind_row(key, action_label(a), action_desc(a), cells, enabled, note);
    if (g_cap.action >= 0 || r < 0) return;
    if (r < 3) {
        g_cap.action = a;
        g_cap.cell = r;
        g_cap.reset = true;
        g_cap.start = plat_now_ns();
        g_cap.done = 0;
    } else {
        change(a, r - 10, 0);   // KEY_NONE / PAD_NONE
    }
}

// The window over the page while a cell waits: the press is polled from the devices (input_capture), so the menu's own
// keys (Enter, arrows, Q / E) bind like any other. Esc or the timeout cancels; a controller button cancels a key cell.
static void capture_window(void) {
    if (g_cap.action < 0) return;
    if (!ImGui::IsPopupOpen("##bind_capture")) ImGui::OpenPopup("##bind_capture");
    uint64_t now = plat_now_ns();
    bool pad_cell = g_cap.cell == 2;
    if (!g_cap.done) {
        int id = 0, got = input_capture(g_cap.reset, &id);
        g_cap.reset = false;
        if (got == INPUT_CAPTURE_KEY && !pad_cell) change(g_cap.action, g_cap.cell, id);
        if (got == INPUT_CAPTURE_PAD && pad_cell) change(g_cap.action, g_cap.cell, id);
        if (got != INPUT_CAPTURE_NONE && !(got == INPUT_CAPTURE_KEY && pad_cell)) g_cap.done = now;
        if (now - g_cap.start > CAPTURE_SECONDS * 1000000000ull) g_cap.done = now;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("##bind_capture", nullptr,
                               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        char t[256];
        snprintf(t, sizeof t, menu_text(pad_cell ? UI_BINDS_PRESS_PAD : UI_BINDS_PRESS_KEY), action_label(g_cap.action));
        ImGui::TextUnformatted(t);
        int left = CAPTURE_SECONDS - (int)((now - g_cap.start) / 1000000000ull);
        snprintf(t, sizeof t, menu_text(UI_BINDS_CANCEL_HINT), left < 1 ? 1 : left);
        ImGui::TextDisabled("%s", t);
        if (g_cap.done && now - g_cap.done > kDoneNs) {
            ImGui::CloseCurrentPopup();
            char id[48];
            snprintf(id, sizeof id, "BIND_%s#%d", input_action_name(g_cap.action), g_cap.cell);
            opt_focus_key(id);   // back on the cell it came from
            g_cap.action = -1;
        }
        ImGui::EndPopup();
    }
}

bool binds_modal(void) { return g_cap.action >= 0; }

static bool g_reset_asked;   // the reset row was chosen this frame (acted on after the page)

void binds_rows(float scale) {
    InputMap m;
    input_map_snapshot(&m);
    int layout = shown_layout();
    opt_group(UI_BINDS_GROUP_BUTTONS);
    opt_text(UI_BINDS_HELP);
    opt_bind_header(UI_BINDS_COL_KEY1, UI_BINDS_COL_KEY2, UI_BINDS_COL_PAD);
    for (int a : kButtons) row_action(a, m, layout);
    opt_group(UI_BINDS_GROUP_MOVE);
    for (int a : kMove) row_action(a, m, layout);
    opt_group(UI_BINDS_GROUP_CAMERA);
    for (int a : kCamera) row_action(a, m, layout);
    ImGui::Dummy(ImVec2(0, 6 * scale));
    g_reset_asked = opt_button("##binds_reset", UI_BINDS_RESET, UI_BINDS_RESET_DESC);
}

void binds_after_page(void) {
    if (g_reset_asked && g_cap.action < 0) reset_binds();
    g_reset_asked = false;
    capture_window();
}
