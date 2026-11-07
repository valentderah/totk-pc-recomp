// Player-owned item catalog and one-shot actions. Game calls run on the guest frame-end context, never in ImGui.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#include "imgui.h"
#include "ui/items_page.h"
extern "C" {
#include "items.h"
#include "platform/platform.h"
}
struct Item { std::string id, name, category, description; bool pack; };
static std::vector<Item> items;
static std::vector<std::string> categories;
static bool loaded;
static char search[128];
static int selected = -1, category, count = 1;
static std::string preferences;
static std::set<std::string> favorites;
static std::vector<std::string> recent;
static void save_preferences() {
    std::ofstream out(preferences);
    for (const auto &id : favorites) out << "F\t" << id << '\n';
    for (const auto &id : recent) out << "R\t" << id << '\n';
}
static bool contains(const std::string &s, const char *needle) {
    std::string text = s, find = needle;
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)tolower(c); });
    std::transform(find.begin(), find.end(), find.begin(), [](unsigned char c) { return (char)tolower(c); });
    return text.find(find) != std::string::npos;
}
static void load() {
    char path[2048];
    if (!plat_exe_path(path, sizeof path)) return;
    std::string file = path;
    file.erase(file.find_last_of("/\\") + 1);
    preferences = file + "items-preferences.txt";
    std::ifstream prefs(preferences);
    std::string pref;
    while (std::getline(prefs, pref)) {
        if (pref.rfind("F\t", 0) == 0) favorites.insert(pref.substr(2));
        if (pref.rfind("R\t", 0) == 0 && recent.size() < 20) recent.push_back(pref.substr(2));
    }
    std::ifstream input(file + "items.tsv");
    std::string line;
    categories.push_back("All items");
    categories.push_back("Favorites");
    categories.push_back("Recent");
    while (std::getline(input, line)) {
        std::vector<std::string> fields;
        size_t start = 0, end;
        while ((end = line.find('\t', start)) != std::string::npos) { fields.push_back(line.substr(start, end - start)); start = end + 1; }
        fields.push_back(line.substr(start));
        if (fields.size() < 5) continue;
        items.push_back({fields[0], fields[1], fields[2], fields[4], fields[3] == "1"});
        if (std::find(categories.begin(), categories.end(), fields[2]) == categories.end()) categories.push_back(fields[2]);
    }
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.name == b.name ? a.id < b.id : a.name < b.name; });
    if (const char *test = std::getenv("TOTK_ITEM_TEST"))
        for (int i = 0; i < (int)items.size(); ++i) if (items[i].id == test) selected = i;
}
void tab_items(float scale, float bottom) {
    if (!items_supported()) {
        ImGui::TextWrapped("Items are available with game version 1.4.3.");
        return;
    }
    if (!loaded) { loaded = true; load(); }
    ImGui::TextUnformatted("Items");
    if (items.empty()) { ImGui::TextWrapped("Item catalog not found next to the executable (items.tsv)."); return; }
    ImGui::SetNextItemWidth(320 * scale);
    ImGui::InputTextWithHint("##item_search", "Search item name or ID", search, sizeof search);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(250 * scale);
    if (ImGui::BeginCombo("##item_category", categories[category].c_str())) {
        for (int i = 0; i < (int)categories.size(); ++i)
            if (ImGui::Selectable(categories[i].c_str(), category == i)) category = i;
        ImGui::EndCombo();
    }
    float height = std::max(100.f, ImGui::GetContentRegionAvail().y - bottom);
    float width = ImGui::GetContentRegionAvail().x * 0.47f;
    ImGui::BeginChild("##item_results", ImVec2(width, height), ImGuiChildFlags_Borders);
    for (int i = 0; i < (int)items.size(); ++i) {
        const Item &item = items[i];
        if (category == 1 && !favorites.count(item.id)) continue;
        if (category == 2 && std::find(recent.begin(), recent.end(), item.id) == recent.end()) continue;
        if (category >= 3 && item.category != categories[category]) continue;
        if (!contains(item.name, search) && !contains(item.id, search)) continue;
        ImGui::PushID(i);
        if (ImGui::Selectable(item.name.c_str(), selected == i)) selected = i;
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##item_details", ImVec2(0, height), ImGuiChildFlags_Borders);
    if (selected >= 0) {
        const Item &item = items[selected];
        ImGui::TextWrapped("%s", item.name.c_str());
        ImGui::TextDisabled("%s", item.id.c_str());
        bool favorite = favorites.count(item.id) != 0;
        if (ImGui::Checkbox("Favorite", &favorite)) {
            if (favorite) favorites.insert(item.id); else favorites.erase(item.id);
            save_preferences();
        }
        ImGui::Separator();
        ImGui::TextWrapped("%s", item.description.c_str());
        ImGui::Spacing();
        ImGui::SetNextItemWidth(180 * scale);
        ImGui::InputInt("Spawn quantity (1-10)", &count);
        count = std::clamp(count, 1, 10);
        bool special = item.category == "Special and key items" || item.category == "Paraglider fabrics" ||
                       item.category == "Horse equipment" || item.category == "Armor";
        ImGui::BeginDisabled(!item.pack || special || items_busy());
        if (ImGui::Button("Spawn near Link") && items_request(item.id.c_str(), count)) {
            recent.erase(std::remove(recent.begin(), recent.end(), item.id), recent.end());
            recent.insert(recent.begin(), item.id);
            if (recent.size() > 20) recent.resize(20);
            save_preferences();
        }
        ImGui::EndDisabled();
        if (!item.pack || special) ImGui::TextWrapped("World spawn is unavailable for this item.");
        ImGui::Spacing();
        bool grant_type = item.category == "Materials and ingredients" ||
                          item.category.rfind("Weapons /", 0) == 0 ||
                          item.category == "Bows" ||
                          item.category == "Shields";
        ImGui::BeginDisabled(!item.pack || !grant_type || !items_inventory_ready() || items_busy());
        if (ImGui::Button("Add to inventory") && items_inventory_request(item.id.c_str())) {
            recent.erase(std::remove(recent.begin(), recent.end(), item.id), recent.end());
            recent.insert(recent.begin(), item.id);
            if (recent.size() > 20) recent.resize(20);
            save_preferences();
        }
        ImGui::EndDisabled();
        if (!items_inventory_ready()) ImGui::TextWrapped("Pick up any item once after loading to enable inventory adds.");
        else if (!grant_type) ImGui::TextWrapped("Inventory add is unavailable for this item.");
        else ImGui::TextWrapped("Adds one item. Resume play to complete the request.");
    } else ImGui::TextWrapped("Choose an item from the list.");
    ImGui::Separator();
    char status[256]; items_status(status, sizeof status); ImGui::TextWrapped("%s", status);
    ImGui::EndChild();
}
