// Amiibo / NFC menu page (Dear ImGui): browser and scan controls.
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <filesystem>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <vector>

#include "imgui.h"
#include "ui/amiibo_page.h"
#include "ui/menu.h"
#include "ui/option_rows.h"

extern "C" {
#include "amiibo.h"
#include "cfg.h"
#include "folder_pick.h"
#include "platform/platform.h"
#include "ui_text.h"
}

namespace fs = std::filesystem;

struct FileEntry {
    std::string name;
    bool is_dir;
};

static const char kDefaultAmiiboDir[] = "./amiibo";
static std::string g_current_dir = kDefaultAmiiboDir;
static std::vector<FileEntry> g_entries;
static int g_selected_entry = -1;

static std::vector<std::string> g_all_files;
static int g_all_index = -1;
static PlatMutex g_files_lock = PLAT_MUTEX_INIT;

// Toast notification
static PlatMutex g_toast_lock = PLAT_MUTEX_INIT;
static std::string g_toast_title;
static std::string g_toast_sub;
static uint64_t g_toast_start_ns = 0;

static void set_toast(const std::string &title, const std::string &sub) {
    plat_mutex_lock(&g_toast_lock);
    g_toast_title = title;
    g_toast_sub = sub;
    g_toast_start_ns = plat_now_ns();
    plat_mutex_unlock(&g_toast_lock);
}
// The last action: a catalog id and the file or folder it names, formatted when drawn (the menu language can change)
static UiTextId g_status_id = UI_MENU_AMIIBO_READY;
static std::string g_status_arg;

static void set_status(UiTextId id, const std::string &arg = std::string()) {
    g_status_id = id;
    g_status_arg = arg;
}

// Loads a dump and puts it on the reader; the status names the file.
static void tap(const std::string &dir, const std::string &name) {
    std::string full_path = (fs::path(dir) / name).string();
    set_status(amiibo_scan_and_tap(full_path.c_str()) ? UI_MENU_AMIIBO_TAPPED : UI_MENU_AMIIBO_LOAD_FAILED, name);
}

static bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

static void set_current_dir(const std::string &path) {
    g_current_dir = path;
    cfg_set("TOTK_AMIIBO_DIR", g_current_dir.c_str());
}

// The host's folder dialog (folder_pick.h) answers on its own thread; the page takes the answer when it draws.
static PlatMutex g_pick_lock = PLAT_MUTEX_INIT;
static bool g_pick_open;
static bool g_pick_ready;
static std::string g_pick_path;

static void on_folder_picked(const char *path, void *ctx) {
    (void)ctx;
    plat_mutex_lock(&g_pick_lock);
    g_pick_ready = path != NULL;
    if (path) g_pick_path = path;
    g_pick_open = false;
    plat_mutex_unlock(&g_pick_lock);
}

static void refresh_dir(void) {
    g_entries.clear();
    g_selected_entry = -1;

    std::error_code ec;
    fs::path dir_path(g_current_dir);

    // A missing folder lists nothing and says so; it is not replaced by another one, and opening the tab writes no
    // setting (the folder is saved only when the player sets one).
    if (!fs::exists(dir_path, ec) || !fs::is_directory(dir_path, ec)) {
        set_status(UI_MENU_AMIIBO_NO_DIR, g_current_dir);
        return;
    }
    if (g_status_id == UI_MENU_AMIIBO_NO_DIR) set_status(UI_MENU_AMIIBO_READY);

    // Add parent navigation if not at root
    if (dir_path.has_parent_path() && dir_path != dir_path.parent_path()) {
        g_entries.push_back({"..", true});
    }

    for (const auto &entry : fs::directory_iterator(dir_path, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        std::error_code status_ec;
        bool is_dir = entry.is_directory(status_ec);
        if (status_ec) continue;

        std::string filename = entry.path().filename().string();
        if (filename.empty() || filename == "." || filename == "..") continue;

        if (!is_dir) {
            std::string ext = entry.path().extension().string();
            if (!iequals(ext, ".bin") && !iequals(ext, ".nfc")) {
                continue;
            }
        }

        g_entries.push_back({std::move(filename), is_dir});
    }

    std::sort(g_entries.begin(), g_entries.end(), [](const FileEntry &a, const FileEntry &b) {
        if (a.name == "..") return true;
        if (b.name == "..") return false;
        if (a.is_dir != b.is_dir) return a.is_dir;
        return a.name < b.name;
    });
}

static void scan_all_files(void) {
    plat_mutex_lock(&g_files_lock);
    g_all_files.clear();
    std::error_code ec;

    std::vector<std::string> search_roots = { g_current_dir };
    if (g_current_dir != kDefaultAmiiboDir) {
        search_roots.push_back(kDefaultAmiiboDir);
    }

    for (const auto &root : search_roots) {
        fs::path root_p(root);
        if (!fs::exists(root_p, ec) || !fs::is_directory(root_p, ec)) continue;
        for (const auto &entry : fs::recursive_directory_iterator(root_p, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            std::error_code status_ec;
            if (!entry.is_regular_file(status_ec)) continue;
            std::string fn = entry.path().filename().string();
            if (iequals(fn, "key_retail.bin")) continue;
            std::string ext = entry.path().extension().string();
            if (iequals(ext, ".bin") || iequals(ext, ".nfc")) {
                g_all_files.push_back(entry.path().string());
            }
        }
    }

    std::sort(g_all_files.begin(), g_all_files.end());
    g_all_files.erase(std::unique(g_all_files.begin(), g_all_files.end()), g_all_files.end());

    const AmiiboInfo *cur = amiibo_get_current_info();
    g_all_index = -1;
    if (cur && cur->filename[0]) {
        for (size_t i = 0; i < g_all_files.size(); ++i) {
            if (fs::path(g_all_files[i]).filename().string() == cur->filename) {
                g_all_index = (int)i;
                break;
            }
        }
    }
    if (g_all_index < 0 && !g_all_files.empty()) {
        g_all_index = 0;
    }
    plat_mutex_unlock(&g_files_lock);
}

extern "C" void amiibo_quick_tap(void) {
    if (!amiibo_has_tag() && amiibo_get_current_info()->filename[0] == '\0') {
        scan_all_files();
        plat_mutex_lock(&g_files_lock);
        if (!g_all_files.empty()) {
            if (g_all_index < 0 || g_all_index >= (int)g_all_files.size()) g_all_index = 0;
            std::string p = g_all_files[g_all_index];
            plat_mutex_unlock(&g_files_lock);
            amiibo_load_file(p.c_str());
        } else {
            plat_mutex_unlock(&g_files_lock);
            set_toast("No Amiibo Found", "Put .bin dumps in amiibo folder");
            return;
        }
    }
    const AmiiboInfo *info = amiibo_get_current_info();
    std::string name = info->filename[0] ? info->filename : "Amiibo";
    amiibo_tap_tag();
    set_status(UI_MENU_AMIIBO_TAPPED, name);
    set_toast(fs::path(name).stem().string(), "Tapped • Scanning into game");
}

extern "C" void amiibo_quick_next(void) {
    scan_all_files();
    plat_mutex_lock(&g_files_lock);
    if (g_all_files.empty()) {
        plat_mutex_unlock(&g_files_lock);
        set_toast("No Amiibo Found", "Put .bin dumps in amiibo folder");
        return;
    }
    g_all_index = (g_all_index + 1) % (int)g_all_files.size();
    std::string path = g_all_files[g_all_index];
    int idx = g_all_index + 1;
    int total = (int)g_all_files.size();
    plat_mutex_unlock(&g_files_lock);

    std::string stem = fs::path(path).stem().string();
    std::string filename = fs::path(path).filename().string();
    amiibo_load_file(path.c_str());
    set_status(UI_MENU_AMIIBO_READY, filename);

    char sub[128];
    snprintf(sub, sizeof sub, "Selected (%d/%d) • R3+Up to Tap", idx, total);
    set_toast(stem, sub);
}

extern "C" void amiibo_quick_prev(void) {
    scan_all_files();
    plat_mutex_lock(&g_files_lock);
    if (g_all_files.empty()) {
        plat_mutex_unlock(&g_files_lock);
        set_toast("No Amiibo Found", "Put .bin dumps in amiibo folder");
        return;
    }
    g_all_index = (g_all_index - 1 + (int)g_all_files.size()) % (int)g_all_files.size();
    std::string path = g_all_files[g_all_index];
    int idx = g_all_index + 1;
    int total = (int)g_all_files.size();
    plat_mutex_unlock(&g_files_lock);

    std::string stem = fs::path(path).stem().string();
    std::string filename = fs::path(path).filename().string();
    amiibo_load_file(path.c_str());
    set_status(UI_MENU_AMIIBO_READY, filename);

    char sub[128];
    snprintf(sub, sizeof sub, "Selected (%d/%d) • R3+Up to Tap", idx, total);
    set_toast(stem, sub);
}

extern "C" void amiibo_quick_menu(void) {
    menu_open_tab(TAB_AMIIBO);
}

extern "C" void amiibo_toast_draw(float screen_w, float screen_h, float scale) {
    (void)screen_h;
    uint64_t now = plat_now_ns();
    plat_mutex_lock(&g_toast_lock);
    uint64_t elapsed = now - g_toast_start_ns;
    if (elapsed > 3000000000ull || g_toast_title.empty()) {
        plat_mutex_unlock(&g_toast_lock);
        return;
    }
    std::string title = g_toast_title;
    std::string sub = g_toast_sub;
    plat_mutex_unlock(&g_toast_lock);

    float alpha = 1.0f;
    if (elapsed > 2200000000ull) {
        alpha = 1.0f - (float)(elapsed - 2200000000ull) / 800000000.0f;
        if (alpha < 0.0f) alpha = 0.0f;
    }
    int a = (int)(alpha * 255.0f);

    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const char badge[] = "AMIIBO";
    ImVec2 badge_sz = ImGui::CalcTextSize(badge);
    ImVec2 title_sz = ImGui::CalcTextSize(title.c_str());
    ImVec2 sub_sz = ImGui::CalcTextSize(sub.c_str());

    float text_w = std::max(title_sz.x, sub_sz.x);
    float box_w = badge_sz.x + text_w + 40.0f * scale;
    if (box_w < 280.0f * scale) box_w = 280.0f * scale;
    float box_h = 44.0f * scale;
    float box_x = (screen_w - box_w) * 0.5f;
    float box_y = 28.0f * scale;

    ImVec2 p0(box_x, box_y);
    ImVec2 p1(box_x + box_w, box_y + box_h);

    dl->AddRectFilled(p0, p1, IM_COL32(18, 20, 26, (int)(230 * alpha)), 8.0f * scale);
    dl->AddRect(p0, p1, IM_COL32(237, 194, 84, (int)(180 * alpha)), 8.0f * scale, 0, 1.5f * scale);

    float badge_pad_x = 8.0f * scale;
    float badge_w = badge_sz.x + badge_pad_x * 2;
    float badge_h = 24.0f * scale;
    ImVec2 bp0(box_x + 10.0f * scale, box_y + (box_h - badge_h) * 0.5f);
    ImVec2 bp1(bp0.x + badge_w, bp0.y + badge_h);
    dl->AddRectFilled(bp0, bp1, IM_COL32(237, 194, 84, (int)(220 * alpha)), 4.0f * scale);
    dl->AddText(ImVec2(bp0.x + badge_pad_x, bp0.y + (badge_h - badge_sz.y) * 0.5f),
                IM_COL32(20, 20, 20, a), badge);

    float text_x = bp1.x + 12.0f * scale;
    float text_y = box_y + 6.0f * scale;
    dl->AddText(ImVec2(text_x, text_y), IM_COL32(255, 255, 255, a), title.c_str());
    if (!sub.empty()) {
        dl->AddText(ImVec2(text_x, text_y + title_sz.y + 1.0f * scale),
                    IM_COL32(237, 194, 84, (int)(210 * alpha)), sub.c_str());
    }
}

void tab_amiibo(float scale) {
    (void)scale;   // the rows take ImGui's own sizes, already scaled with the font
    static bool s_inited = false;
    if (!s_inited) {
        const char *cfg_dir = cfg_get("TOTK_AMIIBO_DIR");
        if (cfg_dir && *cfg_dir) {
            g_current_dir = cfg_dir;
        } else {
            g_current_dir = kDefaultAmiiboDir;
        }
        refresh_dir();
        s_inited = true;
    }

    plat_mutex_lock(&g_pick_lock);
    bool picking = g_pick_open;
    std::string picked;
    if (g_pick_ready) picked.swap(g_pick_path);
    g_pick_ready = false;
    plat_mutex_unlock(&g_pick_lock);
    if (!picked.empty()) {
        set_current_dir(picked);
        refresh_dir();
    }

    // Two panes: Left (Directory Browser & Tag List), Right (Amiibo Tag Details & Controls)
    const float content_h = ImGui::GetContentRegionAvail().y - opt_bottom();   // Continue and Quit stay visible
    const float left_w = ImGui::GetContentRegionAvail().x * 0.58f;
    const float right_w = ImGui::GetContentRegionAvail().x - left_w - ImGui::GetStyle().ItemSpacing.x;

    // ---- Left Pane: Explorer & List ----
    ImGui::BeginChild("##amiibo_explorer", ImVec2(left_w, content_h), true);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1.0f), "%s", menu_text(UI_MENU_AMIIBO_DIR));

    ImGui::TextWrapped("%s", g_current_dir.c_str());

    ImGui::Spacing();
    ImGui::BeginDisabled(picking);
    if (ImGui::Button(menu_text(UI_MENU_AMIIBO_OPEN))) {
        plat_mutex_lock(&g_pick_lock);
        g_pick_open = true;
        plat_mutex_unlock(&g_pick_lock);
        folder_pick(g_current_dir.c_str(), on_folder_picked, NULL);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(menu_text(UI_MENU_AMIIBO_DEFAULT_DIR))) {
        set_current_dir(kDefaultAmiiboDir);
        refresh_dir();
    }
    ImGui::SameLine();
    if (ImGui::Button(menu_text(UI_MENU_AMIIBO_REFRESH))) {
        refresh_dir();
    }

    ImGui::Separator();
    ImGui::TextDisabled("%s", menu_text(UI_MENU_AMIIBO_PICK));

    ImGui::BeginChild("##file_list", ImVec2(0, 0), true);
    if (g_status_id == UI_MENU_AMIIBO_NO_DIR) {
        char missing[512];
        snprintf(missing, sizeof missing, menu_text(UI_MENU_AMIIBO_NO_DIR), g_current_dir.c_str());
        ImGui::TextWrapped("%s", missing);
    }
    for (size_t i = 0; i < g_entries.size(); i++) {
        const auto &e = g_entries[i];
        std::string label = e.is_dir ? e.name + "/" : e.name;
        bool selected = ((int)i == g_selected_entry);

        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
            g_selected_entry = (int)i;
            if (e.is_dir) {
                if (ImGui::IsMouseDoubleClicked(0) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown)) {
                    if (e.name == "..") {
                        fs::path cur(g_current_dir);
                        if (cur.has_parent_path() && cur != cur.parent_path()) {
                            set_current_dir(cur.parent_path().string());
                        } else {
                            set_current_dir(".");
                        }
                    } else {
                        set_current_dir((fs::path(g_current_dir) / e.name).string());
                    }
                    refresh_dir();
                    break;
                }
            } else {
                if (ImGui::IsMouseDoubleClicked(0) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown) ||
                    ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp)) {
                    tap(g_current_dir, e.name);
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::SameLine();

    // ---- Right Pane: Controls & Status ----
    ImGui::BeginChild("##amiibo_controls", ImVec2(right_w, content_h), true);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1.0f), "%s", menu_text(UI_MENU_AMIIBO_READER));
    ImGui::Separator();

    AmiiboDeviceState state = amiibo_get_reader_state();
    UiTextId state_id = UI_MENU_AMIIBO_STATE_IDLE;
    ImVec4 state_col = ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
    switch (state) {
    case AMIIBO_DEV_STATE_INITIALIZED: state_id = UI_MENU_AMIIBO_STATE_IDLE; break;
    case AMIIBO_DEV_STATE_SEARCHING: state_id = UI_MENU_AMIIBO_STATE_SEARCHING; state_col = ImVec4(1.0f, 0.8f, 0.2f, 1.0f); break;
    case AMIIBO_DEV_STATE_TAG_FOUND: state_id = UI_MENU_AMIIBO_STATE_FOUND; state_col = ImVec4(0.3f, 0.9f, 0.3f, 1.0f); break;
    case AMIIBO_DEV_STATE_TAG_MOUNTED: state_id = UI_MENU_AMIIBO_STATE_MOUNTED; state_col = ImVec4(0.2f, 0.9f, 1.0f, 1.0f); break;
    case AMIIBO_DEV_STATE_UNAVAILABLE: state_id = UI_MENU_AMIIBO_STATE_OFF; state_col = ImVec4(0.9f, 0.3f, 0.3f, 1.0f); break;
    default: break;
    }
    ImGui::TextUnformatted(menu_text(UI_MENU_AMIIBO_STATE));
    ImGui::SameLine();
    ImGui::TextColored(state_col, "%s", menu_text(state_id));

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(menu_text(UI_MENU_AMIIBO_FILE));
    if (g_selected_entry >= 0 && g_selected_entry < (int)g_entries.size() && !g_entries[g_selected_entry].is_dir) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s", g_entries[g_selected_entry].name.c_str());

        if (ImGui::Button(menu_text(UI_MENU_AMIIBO_TAP), ImVec2(ImGui::GetContentRegionAvail().x, 0)))
            tap(g_current_dir, g_entries[g_selected_entry].name);
    } else {
        ImGui::TextDisabled("%s", menu_text(UI_MENU_AMIIBO_NO_FILE));
        ImGui::BeginDisabled();
        ImGui::Button(menu_text(UI_MENU_AMIIBO_TAP), ImVec2(ImGui::GetContentRegionAvail().x, 0));
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    if (ImGui::Button(menu_text(UI_MENU_AMIIBO_REMOVE), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
        amiibo_remove_tag();
        set_status(UI_MENU_AMIIBO_REMOVED);
    }

    ImGui::Spacing();
    ImGui::Separator();
    bool auto_remove = amiibo_get_auto_remove();
    if (ImGui::Checkbox(menu_text(UI_MENU_AMIIBO_AUTO_REMOVE), &auto_remove)) {
        amiibo_set_auto_remove(auto_remove);
    }
    if (auto_remove) {
        float delay = amiibo_get_auto_remove_delay_s();
        ImGui::TextUnformatted(menu_text(UI_MENU_AMIIBO_HOLD));   // above the slider: beside it the text is cut off
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##amiibo_hold", &delay, 1.0f, 6.0f, menu_text(UI_MENU_AMIIBO_SECONDS))) {
            amiibo_set_auto_remove_delay_s(delay);
        }
    }

    bool random_uid = amiibo_get_random_uid();
    if (ImGui::Checkbox(menu_text(UI_MENU_AMIIBO_RANDOM_UID), &random_uid)) {
        amiibo_set_random_uid(random_uid);
    }

    bool repeat_orig = amiibo_get_repeat_orig_uid();
    if (ImGui::Checkbox(menu_text(UI_MENU_AMIIBO_REPEAT_ORIG_UID), &repeat_orig)) {
        amiibo_set_repeat_orig_uid(repeat_orig);
    }
    if (repeat_orig && amiibo_get_clock_offset() > 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton(menu_text(UI_MENU_AMIIBO_RESET_CLOCK))) {
            amiibo_reset_clock_offset();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted(menu_text(UI_MENU_AMIIBO_TAG));
    if (amiibo_has_tag()) {
        const AmiiboInfo *info = amiibo_get_current_info();
        char character[8], uid[24];
        snprintf(character, sizeof character, "%02x%02x%02x", info->character_id[0], info->character_id[1],
                 info->character_id[2]);
        snprintf(uid, sizeof uid, "%02x:%02x:%02x:%02x:%02x:%02x:%02x", info->uuid[0], info->uuid[1], info->uuid[2],
                 info->uuid[3], info->uuid[4], info->uuid[5], info->uuid[6]);
        ImGui::Text(menu_text(UI_MENU_AMIIBO_TAG_FILE), info->filename);
        ImGui::Text(menu_text(UI_MENU_AMIIBO_TAG_ID), character, info->series_id, info->numbering_id, info->nfp_type);
        ImGui::Text(menu_text(UI_MENU_AMIIBO_TAG_UID), uid);
    } else {
        ImGui::TextDisabled("%s", menu_text(UI_MENU_AMIIBO_NO_TAG));
    }

    ImGui::Spacing();
    ImGui::Separator();
    char status[512];
    snprintf(status, sizeof status, menu_text(g_status_id), g_status_arg.c_str());
    ImGui::TextDisabled(menu_text(UI_MENU_AMIIBO_STATUS), status);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1.0f), "Shortcuts");
    ImGui::TextDisabled("Controller in-game:");
    ImGui::BulletText("R3 + D-Pad Up: Tap / Scan Amiibo");
    ImGui::BulletText("R3 + D-Pad Right: Next Amiibo");
    ImGui::BulletText("R3 + D-Pad Left: Previous Amiibo");
    ImGui::BulletText("R3 + D-Pad Down: Open Amiibo Menu");
    ImGui::TextDisabled("Keyboard in-game:");
    ImGui::BulletText("F3: Tap | [ / ]: Prev / Next | F2: Menu");
    ImGui::TextDisabled("In list: Press A or X to Tap directly");

    ImGui::EndChild();
}
