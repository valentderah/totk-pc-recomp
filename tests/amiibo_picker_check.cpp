// Real Dear ImGui keyboard/gamepad navigation, with a synthetic file list and stub reader.
#include <cassert>
#include "imgui_internal.h"
#include "../runtime/ui/amiibo_page.cpp"

static int taps;
static bool detecting;
static std::string tapped_path;
static const char *test_dir = "build/picker-test-files";

// Report test failures to the console without opening the CRT's assertion dialog.
#undef assert
#define assert(condition) do { \
    if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } \
} while (0)

extern "C" {
void plat_mutex_lock(PlatMutex *) {}
void plat_mutex_unlock(PlatMutex *) {}
void folder_pick(const char *, FolderPickDone done, void *ctx) { done(nullptr, ctx); }
AmiiboDeviceState amiibo_get_reader_state(void) { return detecting ? AMIIBO_DEV_STATE_SEARCHING : AMIIBO_DEV_STATE_INITIALIZED; }
const char *cfg_get(const char *) { return test_dir; }
void cfg_set(const char *, const char *) {}
const char *menu_text(UiTextId id) {
    if (id == UI_MENU_CONTINUE) return "Continue";
    if (id == UI_MENU_AMIIBO_PICK) return "Amiibo files";
    return "%s";
}
bool amiibo_scan_and_tap(const char *path) { ++taps; tapped_path = path; return true; }
bool amiibo_is_detecting(void) { return detecting; }
AmiiboDeviceState amiibo_get_device_state(void) { return AMIIBO_DEV_STATE_INITIALIZED; }
void amiibo_remove_tag(void) {}
bool amiibo_get_auto_remove(void) { return true; }
void amiibo_set_auto_remove(bool) {}
float amiibo_get_auto_remove_delay_s(void) { return 2.5f; }
void amiibo_set_auto_remove_delay_s(float) {}
bool amiibo_has_tag(void) { return false; }
const AmiiboInfo *amiibo_get_current_info(void) { static AmiiboInfo info; return &info; }
}
float opt_bottom(void) { return 0; }
void opt_save_soon(void) {}

static bool frame(bool opening = false) {
    ImGui::NewFrame();
    bool done = amiibo_picker(opening);
    ImGui::Render();
    return done;
}

static bool press(ImGuiKey key) {
    ImGuiIO &io = ImGui::GetIO();
    io.AddKeyEvent(key, true);
    bool done = frame();
    io.AddKeyEvent(key, false);
    done |= frame();
    return done;
}

int main() {
    fs::create_directories(test_dir);
    for (const char *name : {"First.bin", "Second.bin"}) {
        FILE *f = fopen((fs::path(test_dir) / name).string().c_str(), "wb");
        assert(f); fclose(f);
    }
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60;
    unsigned char *pixels; int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    detecting = true;
    assert(!frame(true));
    for (int i = 0; i < 30; ++i) {
        detecting = i % 2;
        assert(!frame()); // Neither side of a detection retry closes the picker.
    }
    assert(!press(ImGuiKey_GamepadDpadDown));
    assert(taps == 0);
    assert(press(ImGuiKey_GamepadFaceDown));
    assert(taps == 1 && fs::path(tapped_path).filename() == "Second.bin");
    // Opening with no active detection still shows a picker and explicit cancel scans nothing.
    detecting = false;
    assert(!frame(true));
    assert(!frame());
    assert(press(ImGuiKey_GamepadFaceRight));
    assert(taps == 1);
    ImGui::DestroyContext();
    puts("amiibo picker: detection retries, gamepad navigation, confirm and cancel passed");
}
