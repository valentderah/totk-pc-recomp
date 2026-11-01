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
#include "ui/option_rows.h"

extern "C" {
#include "amiibo.h"
#include "cfg.h"
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
static char g_dir_input[512] = "./amiibo";
static std::vector<FileEntry> g_entries;
static int g_selected_entry = -1;
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
    snprintf(g_dir_input, sizeof(g_dir_input), "%s", g_current_dir.c_str());
    cfg_set("TOTK_AMIIBO_DIR", g_current_dir.c_str());
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
        snprintf(g_dir_input, sizeof(g_dir_input), "%s", g_current_dir.c_str());
        refresh_dir();
        s_inited = true;
    }

    // Two panes: Left (Directory Browser & Tag List), Right (Amiibo Tag Details & Controls)
    const float content_h = ImGui::GetContentRegionAvail().y - opt_bottom();   // Continue and Quit stay visible
    const float left_w = ImGui::GetContentRegionAvail().x * 0.58f;
    const float right_w = ImGui::GetContentRegionAvail().x - left_w - ImGui::GetStyle().ItemSpacing.x;

    // ---- Left Pane: Explorer & List ----
    ImGui::BeginChild("##amiibo_explorer", ImVec2(left_w, content_h), true);
    ImGui::TextColored(ImVec4(0.93f, 0.76f, 0.33f, 1.0f), "%s", menu_text(UI_MENU_AMIIBO_DIR));

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::InputText("##amiibo_dir_input", g_dir_input, sizeof(g_dir_input), ImGuiInputTextFlags_EnterReturnsTrue)) {
        set_current_dir(g_dir_input);
        refresh_dir();
    }

    ImGui::Spacing();
    if (ImGui::Button(menu_text(UI_MENU_AMIIBO_OPEN))) {
        set_current_dir(g_dir_input);
        refresh_dir();
    }
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
                if (ImGui::IsMouseDoubleClicked(0)) tap(g_current_dir, e.name);
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

    AmiiboDeviceState state = amiibo_get_device_state();
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

    ImGui::EndChild();
}
