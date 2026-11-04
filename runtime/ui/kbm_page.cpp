// The Keyboard & mouse tab: one row per scheme action, for the whole scheme or for one context. A row captures a key
// through input_capture and writes TOTK_KBM_BINDS: "action=key" applies to every context, "context.action=key" only to
// the rows that context writes itself (a context entry wins over a global one). Delete clears that entry.
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "imgui.h"
#include "ui/kbm_page.h"
#include "ui/option_rows.h"
#include "ui/binds_page.h"

extern "C" {
#include "cfg.h"
#include "input.h"
#include "input_map.h"
#include "kbm_scheme.h"
#include "platform/platform.h"
#include "ui_text.h"
}

enum { KBM_CAPTURE_SECONDS = 5 };
static const uint64_t kDoneNs = 350000000ull;

// The contexts worth a page: the ones that write rows of their own. The rest inherit and are edited in their parent.
static const int kScopes[] = {
    -1, KBM_SC_WORLD, KBM_SC_AIM, KBM_SC_CLIMB, KBM_SC_AIR, KBM_SC_GLIDE, KBM_SC_SWIM, KBM_SC_HORSE,
    KBM_SC_QUICKMENU, KBM_SC_ABILITY, KBM_SC_VEHICLE, KBM_SC_MENUS, KBM_SC_PAUSE, KBM_SC_MAP, KBM_SC_CHOICE,
    KBM_SC_CAMERA, KBM_SC_SCOPE, KBM_SC_CONSTRUCT, KBM_SC_RECIPE,
};
static int g_sel;   // index into kScopes

static int zh_menu(void) { return !strncmp(menu_language(), "zh", 2); }

// A short Chinese name of an action, else the action itself. The scheme's names are English; the tab shows the Chinese
// alone on a Chinese menu. These strings are not catalog rows, so kbm_labels_visit hands them to the font atlas: a
// glyph that no ui_text string uses would otherwise show as a box.
static const struct { const char *action, *zh; } kActionZh[] = {
    {"jump", "跳跃"}, {"sprint", "冲刺"}, {"interact", "交互"}, {"attack", "攻击"}, {"lock_on", "锁定"},
    {"holster", "收放武器"}, {"bow_toggle", "弓"}, {"throw", "投掷"}, {"ability_wheel", "能力轮盘"},
    {"ultrahand", "究极手"}, {"fuse", "余料建造"}, {"ascend", "通天术"}, {"recall", "倒转乾坤"},
    {"autobuild", "蓝图"}, {"camera", "相机"}, {"weapons", "武器栏"}, {"shields", "盾牌栏"},
    {"materials", "材料"}, {"bows", "弓栏"}, {"weapon_next", "下一个武器"}, {"weapon_prev", "上一个武器"},
    {"scope", "望远镜"}, {"whistle", "口哨"}, {"crouch", "蹲下"}, {"dive", "俯冲"}, {"swim_dash", "冲刺游泳"},
    {"guard", "防御"}, {"parry", "弹反"}, {"soothe", "安抚"}, {"spur", "加速"}, {"dismount", "下马"},
    {"get_off", "下车"}, {"unstick", "分离"}, {"rotate", "旋转"}, {"rotate_left", "左转"},
    {"rotate_right", "右转"}, {"push", "推"}, {"pull", "拉"}, {"raise", "抬起"}, {"lower", "放下"},
    {"place_pin", "放置图钉"}, {"map", "地图"}, {"pause", "暂停"}, {"inventory", "背包"},
    {"adventure_log", "冒险记录"}, {"recipe_book", "食谱"}, {"sort", "排序"}, {"drop", "丢弃"},
    {"confirm", "确认"}, {"back", "返回"}, {"close", "关闭"}, {"next_tip", "下一条提示"},
    {"scroll_up", "向上滚"}, {"scroll_down", "向下滚"}, {"tab_prev", "上一页"}, {"tab_next", "下一页"},
    {"move_up", "上移"}, {"move_down", "下移"}, {"move_left", "左移"}, {"move_right", "右移"},
    {"focus_up", "焦点上"}, {"focus_down", "焦点下"}, {"focus_left", "焦点左"}, {"focus_right", "焦点右"},
    {"zoom_in", "放大"}, {"zoom_out", "缩小"}, {"self_portrait", "自拍"}, {"capture", "拍照"},
    {"arrow_next", "下一支箭"}, {"arrow_prev", "上一支箭"},
    {"attach", "附着"}, {"back_part", "返回上一部分"}, {"click", "点击"}, {"close_book", "关闭书"},
    {"close_glider", "收起滑翔伞"}, {"dpad_down", "十字键下"}, {"dpad_left", "十字键左"},
    {"dpad_right", "十字键右"}, {"dpad_up", "十字键上"}, {"history", "历史"}, {"l_stick_click", "左摇杆按下"},
    {"leave", "离开"}, {"left_arm", "左臂"}, {"let_go", "松手"}, {"log", "记录"}, {"quit", "退出"},
    {"r_stick_click", "右摇杆按下"}, {"reset_camera", "重置视角"}, {"reset_rotation", "重置旋转"},
    {"right_arm", "右臂"}, {"secondary", "次要"}, {"skip", "跳过"}, {"skip_x", "跳过 X"},
    {"use", "使用"}, {"walk", "走"}, {"x_prompt", "X 提示"}, {"y_prompt", "Y 提示"}, {"zl", "ZL"}, {"zr", "ZR"},
    {"choice_1", "选项 1"}, {"choice_2", "选项 2"}, {"choice_3", "选项 3"}, {"choice_4", "选项 4"},
};

static const struct { int scope; const char *zh; } kScopeZh[] = {
    {KBM_SC_WORLD, "世界"}, {KBM_SC_AIM, "拉弓"}, {KBM_SC_CLIMB, "攀爬"}, {KBM_SC_AIR, "空中"},
    {KBM_SC_GLIDE, "滑翔"}, {KBM_SC_SWIM, "游泳"}, {KBM_SC_HORSE, "骑乘"}, {KBM_SC_QUICKMENU, "快捷栏"},
    {KBM_SC_ABILITY, "能力"}, {KBM_SC_VEHICLE, "载具"}, {KBM_SC_MENUS, "菜单"}, {KBM_SC_PAUSE, "暂停"},
    {KBM_SC_MAP, "地图"}, {KBM_SC_CHOICE, "选项"}, {KBM_SC_CAMERA, "相机"}, {KBM_SC_SCOPE, "望远镜"},
    {KBM_SC_CONSTRUCT, "建造"}, {KBM_SC_RECIPE, "食谱"},
};

void kbm_labels_visit(void (*visit)(void *ctx, const char *text), void *ctx) {
    for (size_t i = 0; i < sizeof kActionZh / sizeof *kActionZh; i++) visit(ctx, kActionZh[i].zh);
    for (size_t i = 0; i < sizeof kScopeZh / sizeof *kScopeZh; i++) visit(ctx, kScopeZh[i].zh);
}

static const char *zh_action(const char *a) {
    for (size_t i = 0; i < sizeof kActionZh / sizeof *kActionZh; i++)
        if (!strcmp(kActionZh[i].action, a)) return kActionZh[i].zh;
    return NULL;
}

static const char *zh_scope(int scope) {
    for (size_t i = 0; i < sizeof kScopeZh / sizeof *kScopeZh; i++)
        if (kScopeZh[i].scope == scope) return kScopeZh[i].zh;
    return NULL;
}

static const char *scope_label(int scope, char *buf, size_t n) {
    (void)buf, (void)n;
    if (scope < 0) return menu_text(UI_MENU_KBM_GLOBAL);
    if (zh_menu()) {
        const char *zh = zh_scope(scope);
        if (zh) return zh;
    }
    return kbm_scheme_scope_name(scope);
}

static const char *action_label(const char *action, char *buf, size_t n) {
    (void)buf, (void)n;
    if (zh_menu()) {
        const char *zh = zh_action(action);
        if (zh) return zh;
    }
    return action;
}

// The row's keys as the player sees them: "1", "Mouse Left", "F / Shift".
static void keys_text(const int *keys, int n, char *out, size_t cap) {
    size_t at = 0;
    if (n <= 0) {
        snprintf(out, cap, "-");
        return;
    }
    out[0] = 0;
    for (int i = 0; i < n && at + 1 < cap; i++) {
        char tmp[32];
        const char *l = binds_key_label(keys[i], tmp, sizeof tmp);
        int w = snprintf(out + at, cap - at, "%s%s", i ? " / " : "", l);
        if (w < 0) break;
        at += (size_t)w;
    }
}

// The name TOTK_KBM_BINDS takes for a captured key id.
static const char *key_spec(int k) {
    switch (k) {
    case KEY_SPACE: return "Space";
    case KEY_TAB: return "Tab";
    case KEY_LSHIFT: return "Shift";
    case KEY_RSHIFT: return "RShift";
    case KEY_LCTRL: return "Ctrl";
    case KEY_RCTRL: return "RCtrl";
    case KEY_LALT: return "Alt";
    case KEY_CAPSLOCK: return "CapsLock";
    case KEY_ESC: return "Esc";
    case KEY_ENTER: return "Enter";
    case KEY_BACKSPACE: return "Backspace";
    case KEY_INSERT: return "Insert";
    case KEY_DELETE: return "Delete";
    case KEY_HOME: return "Home";
    case KEY_END: return "End";
    case KEY_PAGEUP: return "PageUp";
    case KEY_PAGEDOWN: return "PageDown";
    case KEY_UP: return "Up";
    case KEY_DOWN: return "Down";
    case KEY_LEFT: return "Left";
    case KEY_RIGHT: return "Right";
    case KEY_MOUSE_L: return "Mouse Left";
    case KEY_MOUSE_R: return "Mouse Right";
    case KEY_MOUSE_M: return "Mouse Middle";
    case KEY_MOUSE_X1: return "Mouse X1";
    case KEY_MOUSE_X2: return "Mouse X2";
    case KEY_WHEEL_UP: return "Wheel Up";
    case KEY_WHEEL_DOWN: return "Wheel Down";
    case KEY_F2: return "F2";
    case KEY_F3: return "F3";
    case KEY_F4: return "F4";
    case KEY_F5: return "F5";
    case KEY_F6: return "F6";
    case KEY_F7: return "F7";
    case KEY_F9: return "F9";
    case KEY_F12: return "F12";
    case KEY_NUM_ADD: return "Num Add";
    case KEY_NUM_SUB: return "Num Sub";
    case KEY_NUM_MUL: return "Num Mul";
    case KEY_NUM_DIV: return "Num Div";
    case KEY_NUM_DECIMAL: return "Num Decimal";
    default:
        if (k >= KEY_NUM0 && k <= KEY_NUM9) {
            static char num[8];
            snprintf(num, sizeof num, "Num%d", k - KEY_NUM0);
            return num;
        }
        return input_key_name(k);
    }
}

static void bind_target(int scope, const char *action, char *out, size_t n) {
    if (scope < 0) snprintf(out, n, "%s", action);
    else snprintf(out, n, "%s.%s", kbm_scheme_scope_name(scope), action);
}

// Replace (or with key NULL remove) one entry of TOTK_KBM_BINDS, keeping every other entry as it was. A context view
// touches only that context's entry; the global view only the bare action.
static void bind_change(int scope, const char *action, const char *key) {
    const char *spec = cfg_get("TOTK_KBM_BINDS");
    char out[8192];
    size_t at = 0;
    out[0] = 0;
    const char *p = spec ? spec : "";
    while (*p) {
        while (*p == ' ' || *p == '|' || *p == ',') p++;
        if (!*p) break;
        const char *end = p;
        while (*end && *end != '|') end++;
        const char *eq = (const char *)memchr(p, '=', (size_t)(end - p));
        const char *token_end = end;
        while (token_end > p && (token_end[-1] == ' ' || token_end[-1] == '\r')) token_end--;
        int keep = 1;
        if (eq && eq < end) {
            const char *name = p;
            const char *name_end = eq;
            while (name_end > name && name_end[-1] == ' ') name_end--;
            const char *dot = (const char *)memchr(name, '.', (size_t)(name_end - name));
            int token_scope = -1;
            const char *token_action = name;
            size_t action_n = (size_t)(name_end - name);
            if (dot) {
                char ctx[24];
                size_t cn = (size_t)(dot - name);
                if (cn < sizeof ctx) {
                    for (size_t i = 0; i < cn; i++)
                        ctx[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] - 'A' + 'a' : name[i]);
                    ctx[cn] = 0;
                    for (int i = 0; i < KBM_SC_COUNT; i++)
                        if (kbm_scheme_scope_name(i) && !strcmp(kbm_scheme_scope_name(i), ctx)) token_scope = i;
                }
                if (token_scope >= 0) {
                    token_action = dot + 1;
                    action_n = (size_t)(name_end - token_action);
                }
            }
            if (scope < 0) keep = !(token_scope < 0 && action_n == strlen(action) && !strncmp(token_action, action, action_n));
            else keep = !(token_scope == scope && action_n == strlen(action) && !strncmp(token_action, action, action_n));
            if (!keep) {
                p = end;
                if (*p == '|') p++;
                continue;
            }
        }
        if (at && at + 1 < sizeof out) out[at++] = '|';
        size_t len = (size_t)(token_end - p);
        if (len && at + len < sizeof out) {
            memcpy(out + at, p, len);
            at += len;
            out[at] = 0;
        }
        p = end;
        if (*p == '|') p++;
    }
    if (key && *key) {
        char target[80];
        bind_target(scope, action, target, sizeof target);
        if (at && at + 1 < sizeof out) out[at++] = '|';
        snprintf(out + at, sizeof out - at, "%s=%s", target, key);
    }
    cfg_set("TOTK_KBM_BINDS", out);
    opt_save_soon();
}

// ---- the press window ----------------------------------------------------------------------------
static struct {
    int scope;
    char action[32];
    char id[96];
    int reset;
    uint64_t start, done;
} g_cap;
static int g_cap_active;

static void start_capture(int scope, const char *action, const char *id) {
    g_cap.scope = scope;
    snprintf(g_cap.action, sizeof g_cap.action, "%s", action);
    snprintf(g_cap.id, sizeof g_cap.id, "%s", id);
    g_cap.reset = 1;
    g_cap.start = plat_now_ns();
    g_cap.done = 0;
    g_cap_active = 1;
}

static void capture_window(void) {
    if (!g_cap_active) return;
    if (!ImGui::IsPopupOpen("##kbm_capture")) ImGui::OpenPopup("##kbm_capture");
    uint64_t now = plat_now_ns();
    if (!g_cap.done) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
            bind_change(g_cap.scope, g_cap.action, NULL);
            g_cap.done = now;
        } else {
            int id = 0, got = input_capture(g_cap.reset, &id);
            g_cap.reset = 0;
            if (got == INPUT_CAPTURE_KEY) {
                bind_change(g_cap.scope, g_cap.action, key_spec(id));
                g_cap.done = now;
            } else if (got == INPUT_CAPTURE_CANCEL || got == INPUT_CAPTURE_PAD) {
                g_cap.done = now;
            } else if (now - g_cap.start > KBM_CAPTURE_SECONDS * 1000000000ull) {
                g_cap.done = now;
            }
        }
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("##kbm_capture", nullptr,
                               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        char label[96], text[256];
        snprintf(text, sizeof text, menu_text(UI_MENU_KBM_PRESS), action_label(g_cap.action, label, sizeof label));
        ImGui::TextUnformatted(text);
        int left = KBM_CAPTURE_SECONDS - (int)((now - g_cap.start) / 1000000000ull);
        snprintf(text, sizeof text, menu_text(UI_MENU_KBM_CANCEL_HINT), left < 1 ? 1 : left);
        ImGui::TextDisabled("%s", text);
        ImGui::TextDisabled("%s", menu_text(UI_MENU_KBM_CLEAR_HINT));
        if (g_cap.done && now - g_cap.done > kDoneNs) {
            ImGui::CloseCurrentPopup();
            opt_focus_key(g_cap.id);
            g_cap_active = 0;
        }
        ImGui::EndPopup();
    }
}

bool kbm_modal(void) { return g_cap_active; }

// The global list: the gameplay contexts first (world, aim, ...), then the UI actions; a table's action repeated in
// two contexts is listed once.
static const char *g_order[256];
static int g_order_n;

static int order_seen(const char *a) {
    for (int i = 0; i < g_order_n; i++)
        if (!strcmp(g_order[i], a)) return 1;
    return 0;
}

static void build_global_order(void) {
    if (g_order_n) return;
    for (size_t s = 0; s < sizeof kScopes / sizeof *kScopes; s++) {
        int sc = kScopes[s];
        if (sc < 0) continue;
        int n = kbm_scheme_scope_action_count(sc);
        for (int i = 0; i < n && g_order_n < (int)(sizeof g_order / sizeof *g_order); i++) {
            const char *a = kbm_scheme_scope_action(sc, i);
            if (a && !order_seen(a)) g_order[g_order_n++] = a;
        }
    }
    int n = kbm_scheme_action_count();
    for (int i = 0; i < n && g_order_n < (int)(sizeof g_order / sizeof *g_order); i++) {
        const char *a = kbm_scheme_action_name(i);
        if (a && !order_seen(a)) g_order[g_order_n++] = a;
    }
}

// ---- the page -------------------------------------------------------------------------------------
void kbm_rows(float scale) {
    (void)scale;
    opt_text(UI_MENU_KBM_HELP);
    char ctx[64];
    int step = opt_text_row("##kbm_scope", menu_text(UI_MENU_KBM_CONTEXT), scope_label(kScopes[g_sel], ctx, sizeof ctx),
                            nullptr, menu_text(UI_MENU_KBM_CONTEXT_DESC), false);
    if (step) {
        int n = (int)(sizeof kScopes / sizeof *kScopes);
        g_sel = (g_sel + (step == 2 ? 1 : step) + n) % n;
    }
    int scope = kScopes[g_sel];
    opt_group(UI_MENU_KBM_ACTIONS);
    if (scope < 0) build_global_order();
    int count = scope < 0 ? g_order_n : kbm_scheme_scope_action_count(scope);
    if (count <= 0) {
        opt_text(UI_MENU_KBM_NO_ROWS);
        return;
    }
    for (int i = 0; i < count; i++) {
        const char *action = scope < 0 ? g_order[i] : kbm_scheme_scope_action(scope, i);
        if (!action) continue;
        int keys[4], n = 0;
        if (scope < 0) {
            // A global override is what every row shows; without one the table's own keys, not a context override's.
            if (kbm_scheme_bind_kind(-1, action) == 1) n = kbm_scheme_action_keys(action, keys, 4);
            else n = kbm_scheme_action_default_keys(action, keys, 4);
        } else {
            n = kbm_scheme_keys(scope, action, keys, 4);
        }
        char kbuf[96], lbuf[96];
        keys_text(keys, n, kbuf, sizeof kbuf);
        const char *label = action_label(action, lbuf, sizeof lbuf);
        int kind = kbm_scheme_bind_kind(scope, action);
        const char *nt = nullptr;
        if (kind == 1) nt = menu_text(UI_MENU_KBM_OVERRIDE_GLOBAL);
        else if (kind == 2) nt = menu_text(UI_MENU_KBM_OVERRIDE_CONTEXT);
        char id[96];
        snprintf(id, sizeof id, "##kbm_%d_%s", scope, action);
        int row = opt_text_row(id, label, kbuf, nt, menu_text(UI_MENU_KBM_ACTION_DESC), false);
        if (row) start_capture(scope, action, id);
    }
}

void kbm_after_page(void) { capture_window(); }
