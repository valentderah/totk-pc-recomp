// The game launcher (docs/launcher.md): a card per game this repository builds, Breath of the Wild and Tears of the
// Kingdom, with the builds launcher/games.cpp finds on disk. A game with no build is shown greyed out and cannot start;
// its Install opens the setup window for it (installer/gui.py --game). Play starts the chosen build's botw.exe or
// totk.exe in its own folder and closes the launcher. Dear ImGui over SDL3's 2D renderer, the toolkit of the in-game
// menu (runtime/ui/menu.cpp), with its font.
//
//   launcher [--list] [--lang en|ru|pt-BR|zh-Hans] [--select totk|botw] [--shot FILE.png] [--size WxH]
//     --list     print the builds found and exit
//     --lang     window language (default: the one picked in the window's corner, else the system's)
//     --select   the card selected at start (default: the last game started)
//     --shot     draw the window, save it as PNG and exit (docs and tests)
//     --size     window size in points (default 960x760)
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"   // ImGui::ShadeVertsLinearColorGradientKeepAlpha
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma clang diagnostic pop

#include "games.h"

#ifndef LAUNCHER_VERSION
#define LAUNCHER_VERSION "unknown"
#endif

// Roboto Medium (Apache-2.0, runtime/ui/fonts/LICENSE-Roboto.txt), the in-game menu's font.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"   // #embed in C++
static const unsigned char roboto_ttf[] = {
#embed "../runtime/ui/fonts/Roboto-Medium.ttf"
};
#pragma clang diagnostic pop

struct Text {
    const char *heading, *subheading, *play, *install, *installing, *not_installed, *not_found, *version,
        *unknown_version, *locate, *other_folder, *start_failed, *no_setup, *not_a_game, *hint, *none;
};
static const Text kEnglish = {
    "Choose a game",
    "TotK Recomp",
    "Play",
    "Install",
    "Installing...",
    "Not installed",
    "Install it from your own game files",
    "Version %s",
    "Version unknown",
    "Locate the game...",
    "Other folder...",
    "Could not start it: %s",
    "No setup next to the launcher (installer/).",
    "No game data next to that file (romfs-index.bin).",
    "Arrows or D-pad: choose      Enter or A: play or install      Esc: quit",
    "No game is installed yet. Choose Install on a game's card and pick its files, or locate a game installed "
    "elsewhere.",
};
static const Text kRussian = {
    "Выберите игру",
    "TotK Recomp",
    "Играть",
    "Установить",
    "Устанавливается…",
    "Не установлена",
    "Установите её из своих файлов игры",
    "Версия %s",
    "Версия неизвестна",
    "Указать игру…",
    "Другая папка…",
    "Не удалось запустить: %s",
    "Рядом с лаунчером нет установщика (installer/).",
    "Рядом с этим файлом нет данных игры (romfs-index.bin).",
    "Стрелки или крестовина — выбор      Enter или A — играть или установить      Esc — выход",
    "Ни одна игра пока не установлена. Нажмите «Установить» на карточке игры и укажите её файлы, либо укажите "
    "игру, установленную в другом месте.",
};
static const Text kPortuguese = {
    "Escolha um jogo",
    "TotK Recomp",
    "Jogar",
    "Instalar",
    "Instalando…",
    "Não instalado",
    "Instale-o a partir dos seus próprios arquivos do jogo",
    "Versão %s",
    "Versão desconhecida",
    "Localizar o jogo…",
    "Outra pasta…",
    "Não foi possível iniciar: %s",
    "Não há instalador ao lado do launcher (installer/).",
    "Não há dados do jogo ao lado desse arquivo (romfs-index.bin).",
    "Setas ou direcional: escolher      Enter ou A: jogar ou instalar      Esc: sair",
    "Nenhum jogo instalado ainda. Escolha Instalar no cartão de um jogo e indique os arquivos dele, ou localize um "
    "jogo instalado em outro lugar.",
};
static const Text kChinese = {
    "选择游戏",
    "TotK Recomp",
    "开始游戏",
    "安装",
    "正在安装…",
    "未安装",
    "用你自己的游戏文件安装",
    "版本 %s",
    "版本未知",
    "指定游戏…",
    "其他文件夹…",
    "无法启动：%s",
    "启动器旁边没有安装程序（installer/）。",
    "该文件旁边没有游戏数据（romfs-index.bin）。",
    "方向键或十字键：选择      Enter 或 A：开始或安装      Esc：退出",
    "尚未安装任何游戏。在游戏卡片上选择“安装”并指定其文件，或指定安装在其他位置的游戏。",
};

// The window languages, those of the installer window (installer/guilogic.py UI_LANGUAGES), with their own names
struct Language {
    const char *code, *name;
    const Text *text;
};
static const Language kLanguages[] = {
    {"en", "English", &kEnglish},
    {"ru", "Русский", &kRussian},
    {"pt-BR", "Português (Brasil)", &kPortuguese},
    {"zh-Hans", "简体中文", &kChinese},
};
enum { LANGUAGE_COUNT = sizeof kLanguages / sizeof *kLanguages };
static int g_lang = 0;   // index into kLanguages
static const Text *T = &kEnglish;

static int language_index(const char *code) {
    for (int i = 0; code && i < LANGUAGE_COUNT; i++)
        if (!strcmp(code, kLanguages[i].code)) return i;
    return -1;
}

// The system's language as the installer window picks it (installer/guilogic.py ui_language_for_langid): Russian,
// Brazilian Portuguese, Simplified Chinese (zh-CN, zh-SG, zh-Hans), else English.
static int system_language() {
    int n = 0, out = 0;
    SDL_Locale **locales = SDL_GetPreferredLocales(&n);
    if (locales && n > 0 && locales[0]->language) {
        const char *lang = locales[0]->language, *country = locales[0]->country ? locales[0]->country : "";
        if (!strcmp(lang, "ru")) out = 1;
        else if (!strcmp(lang, "pt") && !strcmp(country, "BR")) out = 2;
        else if (!strcmp(lang, "zh") && (!strcmp(country, "CN") || !strcmp(country, "SG") || !*country)) out = 3;
    }
    SDL_free(locales);
    return out;
}

static void set_language(int i) {
    g_lang = i;
    T = kLanguages[i].text;
}

// Each game's colours: the accent of its button and glow, the gradient of its card when it has no icon.
struct Look {
    ImU32 accent, accent_text, art_top, art_bottom;
};
static const Look kLook[GAME_COUNT] = {   // GAME_BOTW: Sheikah blue; GAME_TOTK: gold over green
    {IM_COL32(88, 198, 238, 255), IM_COL32(6, 22, 32, 255), IM_COL32(36, 86, 118, 255), IM_COL32(10, 26, 40, 255)},
    {IM_COL32(226, 190, 96, 255), IM_COL32(28, 22, 8, 255), IM_COL32(58, 108, 78, 255), IM_COL32(16, 36, 26, 255)},
};
static const ImU32 kBgTop = IM_COL32(14, 17, 22, 255), kBgBottom = IM_COL32(8, 10, 13, 255);
static const ImU32 kCard = IM_COL32(24, 28, 35, 240), kTextHi = IM_COL32(240, 243, 247, 255),
                   kTextLo = IM_COL32(150, 158, 170, 255), kError = IM_COL32(255, 120, 110, 255);

static ImU32 with_alpha(ImU32 c, float a) {
    int alpha = (int)(((c >> IM_COL32_A_SHIFT) & 0xff) * (a < 0 ? 0 : a > 1 ? 1 : a));
    return (c & ~IM_COL32_A_MASK) | ((ImU32)alpha << IM_COL32_A_SHIFT);
}

static ImU32 lighten(ImU32 c, float k) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(v.x + (1 - v.x) * k, v.y + (1 - v.y) * k, v.z + (1 - v.z) * k, v.w));
}

// ---- images

struct Art {
    SDL_Texture *icon = nullptr;   // the game's icon
    SDL_Texture *blur = nullptr;   // the same at 8x8: drawn large with linear filtering, a soft backdrop
};

static SDL_Texture *texture(SDL_Renderer *r, const unsigned char *rgba, int w, int h) {
    SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return nullptr;
    SDL_UpdateTexture(t, nullptr, rgba, w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    return t;
}

static Art load_art(SDL_Renderer *r, const fs::path &file) {
    Art art;
    std::ifstream in(file, std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int w = 0, h = 0, n = 0;
    unsigned char *px = data.empty() ? nullptr : stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &n, 4);
    if (!px) return art;
    art.icon = texture(r, px, w, h);
    enum { B = 8 };
    unsigned char small[B * B * 4];
    for (int y = 0; y < B; y++)
        for (int x = 0; x < B; x++)
            for (int c = 0; c < 4; c++) {   // box filter over each 1/8 of the icon
                unsigned sum = 0, count = 0;
                for (int sy = y * h / B; sy < (y + 1) * h / B; sy++)
                    for (int sx = x * w / B; sx < (x + 1) * w / B; sx++, count++) sum += px[(sy * w + sx) * 4 + c];
                small[(y * B + x) * 4 + c] = (unsigned char)(count ? sum / count : 0);
            }
    art.blur = texture(r, small, B, B);
    stbi_image_free(px);
    return art;
}

// ---- fonts

struct Fonts {
    ImFont *body = nullptr, *small = nullptr, *title = nullptr, *heading = nullptr;
};
static Fonts F;
static float g_font_k = 1;   // the atlas is in pixels, the layout in points: 1 / the window's pixel density

static float size_of(ImFont *f) {
    return f->FontSize * g_font_k;
}

static ImVec2 measure(ImFont *f, const char *s) {
    return f->CalcTextSizeA(size_of(f), FLT_MAX, 0, s);
}

static void put(ImDrawList *dl, ImFont *f, ImVec2 pos, ImU32 col, const char *s) {
    dl->AddText(f, size_of(f), pos, col, s);
}

// A system font with Chinese glyphs (Roboto has none), merged into each size with only the glyphs the Chinese texts
// and the language's name use, as the in-game menu merges one (runtime/ui/menu.cpp fonts_up).
static const char *const kCjkFonts[] = {
    "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simsun.ttc", "C:/Windows/Fonts/simhei.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
};

static void build_fonts(float px_scale) {
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0400, 0x04FF, 0x2010, 0x2027, 0};
    static ImVector<ImWchar> cjk_ranges;
    if (cjk_ranges.empty()) {
        ImFontGlyphRangesBuilder b;
        const char *const *text = (const char *const *)&kChinese;
        for (size_t i = 0; i < sizeof(Text) / sizeof(char *); i++) b.AddText(text[i]);
        b.AddText(kLanguages[3].name);
        b.BuildRanges(&cjk_ranges);
    }
    const char *cjk = nullptr;
    for (const char *path : kCjkFonts) {
        std::error_code ec;
        if (fs::is_regular_file(fs::path(path), ec)) {
            cjk = path;
            break;
        }
    }
    ImFontAtlas *fa = ImGui::GetIO().Fonts;
    fa->Clear();
    ImFontConfig fc;
    fc.FontDataOwnedByAtlas = false;
    fc.OversampleH = 2;
    auto add = [&](float size) {
        ImFont *f = fa->AddFontFromMemoryTTF((void *)roboto_ttf, (int)sizeof roboto_ttf, size * px_scale, &fc, ranges);
        if (cjk) {
            ImFontConfig mc;
            mc.MergeMode = true;
            fa->AddFontFromFileTTF(cjk, size * px_scale, &mc, cjk_ranges.Data);
        }
        return f;
    };
    F.body = add(19);
    F.small = add(15);
    F.title = add(27);
    F.heading = add(40);
    ImGui_ImplSDLRenderer3_DestroyFontsTexture();   // NewFrame makes it again from the new atlas
}

// ---- state

struct Game {
    std::vector<int> builds;   // indices into g_builds, newest first
    int chosen = 0;            // index into builds
    Art art;                   // of the chosen build's icon
    fs::path art_file;
    float hover = 0, ambient = 0;   // animations, 0..1
    std::string error;
};

static std::vector<Build> g_builds;
static Game g_games[GAME_COUNT];
static Settings g_settings;
static fs::path g_settings_file;
static int g_sel = 0;
static float g_mouse_travel = 0;   // how far the mouse moved since the last key: hovering selects only after a real move

static void select_card(int g, const char *why) {
    if (g == g_sel) return;
    SDL_Log("launcher: %s selected (%s)", kGames[g].key, why);
    g_sel = g;
    if (strcmp(why, "mouse")) g_mouse_travel = 0;   // a resting mouse does not take a key's choice back
}

static std::mutex g_pick_lock;   // SDL may call the file dialog's callback on another thread
static std::string g_pick;
static int g_pick_game = -1;

static const Build *chosen_build(int g) {
    const Game &gm = g_games[g];
    return gm.builds.empty() ? nullptr : &g_builds[gm.builds[gm.chosen]];
}

static void index_builds() {
    for (int g = 0; g < GAME_COUNT; g++) {
        Game &gm = g_games[g];
        gm.builds.clear();
        for (int i = 0; i < (int)g_builds.size(); i++)
            if (g_builds[i].game == g) gm.builds.push_back(i);
        gm.chosen = 0;
        std::error_code ec;
        for (int k = 0; k < (int)gm.builds.size(); k++)   // the build this game was last started with
            if (!g_settings.exe[g].empty() && fs::equivalent(g_builds[gm.builds[k]].exe, g_settings.exe[g], ec))
                gm.chosen = k;
    }
}

static void update_art(SDL_Renderer *r) {
    for (int g = 0; g < GAME_COUNT; g++) {
        Game &gm = g_games[g];
        const Build *b = chosen_build(g);
        fs::path want = b ? b->icon : fs::path();
        if (want == gm.art_file) continue;
        if (gm.art.icon) SDL_DestroyTexture(gm.art.icon);
        if (gm.art.blur) SDL_DestroyTexture(gm.art.blur);
        gm.art = want.empty() ? Art() : load_art(r, want);
        gm.art_file = want;
    }
}

static void save() {
    if (!save_settings(g_settings_file, g_settings))
        SDL_Log("launcher: cannot write %s", to_utf8(g_settings_file).c_str());
}

static bool launch(int g) {
    const Build *b = chosen_build(g);
    if (!b) return false;
    std::string exe = to_utf8(b->exe), dir = to_utf8(b->exe.parent_path());
    const char *args[] = {exe.c_str(), nullptr};
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetStringProperty(p, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, dir.c_str());
    SDL_SetBooleanProperty(p, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process *proc = SDL_CreateProcessWithProperties(p);
    SDL_DestroyProperties(p);
    if (!proc) {
        char msg[512];
        snprintf(msg, sizeof msg, T->start_failed, SDL_GetError());
        g_games[g].error = msg;
        return false;
    }
    SDL_DestroyProcess(proc);   // frees the handle only: the game keeps running
    g_settings.last = kGames[g].key;
    g_settings.exe[g] = b->exe;
    save();
    return true;
}

// ---- installing

// The setup next to the launcher: the release folder it sits in (python/ beside installer/), else a repository
// checkout above it, run by the Python in TOTK_SETUP_PYTHON, else by .venv-py314's (docs/launcher.md).
struct Setup {
    fs::path python, root;
};

static bool find_setup(const fs::path &dir, Setup &out) {
    std::error_code ec;
    fs::path root;
    for (fs::path d = dir; !d.empty(); d = d.parent_path()) {
        if (fs::is_regular_file(d / "installer" / "gui.py", ec)) {
            root = d;
            break;
        }
        if (d == d.parent_path()) break;
    }
    if (root.empty()) return false;
#ifdef _WIN32
    fs::path release = root / "python" / "pythonw.exe", venv = root / ".venv-py314" / "Scripts" / "pythonw.exe";
#else
    fs::path release = root / "python" / "bin" / "python3", venv = root / ".venv-py314" / "bin" / "python3";
#endif
    const char *env = SDL_getenv("TOTK_SETUP_PYTHON");
    out.root = root;
    out.python = fs::is_regular_file(release, ec) ? release : env && *env ? from_utf8(env) : venv;
    return fs::is_regular_file(out.python, ec);
}

static SDL_Process *g_setup = nullptr;   // the installer window while it runs
static int g_setup_game = -1;

// Opens the installer window for game g with the folder next to the launcher and this launcher.ini; a finished
// install writes its executable there (installer/gui.py --launcher-ini), and the launcher reads it when the window
// closes (poll_setup).
static void install(int g) {
    Setup s;
    if (g_setup) return;
    if (!find_setup(g_settings_file.parent_path(), s)) {
        g_games[g].error = T->no_setup;
        return;
    }
    std::string py = to_utf8(s.python), root = to_utf8(s.root), ini = to_utf8(g_settings_file);
    std::string dest = to_utf8(g_settings_file.parent_path() / kGames[g].folder);
    // installer/steps.py REBUILD_BOOT: the folder goes on sys.path, as only the release Python's ._pth puts it there
    const char *boot = "import runpy, sys; sys.path.insert(0, sys.argv.pop(1)); "
                       "runpy.run_module('installer.gui', run_name='__main__', alter_sys=True)";
    const char *args[] = {py.c_str(), "-c", boot, root.c_str(), "--root", root.c_str(), "--game", kGames[g].key,
                          "--dest", dest.c_str(), "--launcher-ini", ini.c_str(), "--lang", kLanguages[g_lang].code,
                          nullptr};
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetStringProperty(p, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, root.c_str());
    g_setup = SDL_CreateProcessWithProperties(p);
    SDL_DestroyProperties(p);
    if (!g_setup) {
        char msg[512];
        snprintf(msg, sizeof msg, T->start_failed, SDL_GetError());
        g_games[g].error = msg;
        return;
    }
    SDL_Log("launcher: installer started for %s into %s", kGames[g].key, dest.c_str());
    g_setup_game = g;
    g_games[g].error.clear();
}

// When the installer window has closed: read launcher.ini again and look for the games anew.
static void poll_setup() {
    int code = 0;
    if (!g_setup || !SDL_WaitProcess(g_setup, false, &code)) return;
    SDL_DestroyProcess(g_setup);
    g_setup = nullptr;
    SDL_Log("launcher: installer closed (exit code %d)", code);
    g_settings = load_settings(g_settings_file);
    g_builds = find_builds(g_settings_file.parent_path(), g_settings);
    index_builds();
    if (chosen_build(g_setup_game)) select_card(g_setup_game, "installed");
    g_setup_game = -1;
}

static void SDLCALL picked(void *user, const char *const *files, int filter) {
    (void)filter;
    if (!files || !files[0]) return;
    std::lock_guard<std::mutex> lock(g_pick_lock);
    g_pick = files[0];
    g_pick_game = (int)(intptr_t)user;
}

static void locate(SDL_Window *w, int g) {
#ifdef _WIN32
    static const SDL_DialogFileFilter filter[] = {{"botw.exe, totk.exe", "exe"}};
#else
    static const SDL_DialogFileFilter filter[] = {{"botw, totk", "*"}};
#endif
    const Build *b = chosen_build(g);
    std::string start = b ? to_utf8(b->exe.parent_path()) : std::string();
    SDL_ShowOpenFileDialog(picked, (void *)(intptr_t)g, w, filter, 1, start.empty() ? nullptr : start.c_str(), false);
}

// A file the dialog returned: the game it is (or the card it was picked on) becomes that game's build.
static void take_pick() {
    std::string path;
    int g;
    {
        std::lock_guard<std::mutex> lock(g_pick_lock);
        if (g_pick_game < 0) return;
        path = g_pick;
        g = g_pick_game;
        g_pick_game = -1;
    }
    Build b;
    if (!identify(from_utf8(path), b, g)) {
        g_games[g].error = T->not_a_game;
        return;
    }
    g_settings.exe[b.game] = b.exe;
    save();
    g_builds = find_builds(g_settings_file.parent_path(), g_settings);
    index_builds();
    select_card(b.game, "located");
    g_games[b.game].error.clear();
}

// ---- drawing

static void text_centered(ImDrawList *dl, ImFont *f, ImVec2 center, ImU32 col, const char *s) {
    ImVec2 sz = measure(f, s);
    put(dl, f, ImVec2(floorf(center.x - sz.x / 2), floorf(center.y - sz.y / 2)), col, s);
}

// A rounded rectangle filled with a vertical gradient.
static void gradient_rect(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom, float rounding,
                          ImDrawFlags flags) {
    int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, IM_COL32_WHITE, rounding, flags);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, a, ImVec2(a.x, b.y), top, bottom);
}

// A soft glow: rounded outlines growing outwards and fading.
static void glow(ImDrawList *dl, ImVec2 a, ImVec2 b, float rounding, ImU32 col, float strength, float s) {
    for (int k = 1; k <= 8; k++) {
        float e = k * 2.5f * s;
        dl->AddRect(ImVec2(a.x - e, a.y - e), ImVec2(b.x + e, b.y + e), with_alpha(col, strength * 0.22f * (1 - k / 9.f)),
                    rounding + e, 0, 2.5f * s);
    }
}

// A button drawn by hand, filled, or outlined in fill when outline; true when pressed.
static bool pill(const char *id, ImVec2 pos, ImVec2 size, ImU32 fill, ImU32 text, const char *label,
                 bool enabled = true, bool outline = false) {
    ImGui::SetCursorScreenPos(pos);
    bool pressed = ImGui::InvisibleButton(id, size) && enabled;
    bool hovered = enabled && ImGui::IsItemHovered(), held = enabled && ImGui::IsItemActive();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 end(pos.x + size.x, pos.y + size.y);
    if (outline) {
        dl->AddRectFilled(pos, end, with_alpha(fill, held ? 0.28f : hovered ? 0.2f : 0.08f), size.y / 2);
        dl->AddRect(pos, end, fill, size.y / 2, 0, 2);
    } else {
        dl->AddRectFilled(pos, end, held ? lighten(fill, 0.05f) : hovered ? lighten(fill, 0.18f) : fill, size.y / 2);
    }
    text_centered(dl, F.body, ImVec2(pos.x + size.x / 2, pos.y + size.y / 2), text, label);
    return pressed;
}

// A text link; true when clicked.
static bool link(const char *id, ImVec2 pos, const char *label, ImU32 col) {
    ImVec2 sz = measure(F.small, label);
    ImGui::SetCursorScreenPos(pos);
    bool pressed = ImGui::InvisibleButton(id, sz);
    bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 c = hovered ? kTextHi : col;
    put(dl, F.small, pos, c, label);
    if (hovered) dl->AddLine(ImVec2(pos.x, pos.y + sz.y), ImVec2(pos.x + sz.x, pos.y + sz.y), c, 1);
    return pressed;
}

static std::string build_label(const Build &b) {
    char v[96];
    snprintf(v, sizeof v, b.version.empty() ? T->unknown_version : T->version, b.version.c_str());
    return std::string(v) + "  ·  " + short_folder(b.exe);
}

// One game's card; returns true when its Play button was pressed. k scales the card to the window.
static bool card(int g, ImVec2 pos, float k, float s, SDL_Window *window) {
    Game &gm = g_games[g];
    const Look &look = kLook[g];
    const Build *b = chosen_build(g);
    bool installed = b != nullptr, play = false;
    float w = 340 * s * k, art = w, pad = 22 * s * k, r = 18 * s * k;
    float h = art + pad + 18 * s + 34 * s + 26 * s + 22 * s + 52 * s * k + pad;
    float t = gm.hover;
    if (installed) pos.y -= 6 * s * t;
    ImVec2 a = pos, z(pos.x + w, pos.y + h);
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // the card: shadow, glow when selected, body
    for (int i = 0; i < 6; i++) {   // a soft shadow below
        float e = (2 + i * 3) * s;
        dl->AddRectFilled(ImVec2(a.x - e, a.y - e + 12 * s), ImVec2(z.x + e, z.y + e + 12 * s),
                          IM_COL32(0, 0, 0, 16 - 2 * i), r + e);
    }
    if (installed && t > 0.01f) glow(dl, a, z, r, look.accent, t, s);
    dl->AddRectFilled(a, z, kCard, r);
    dl->AddRect(a, z, installed ? with_alpha(look.accent, 0.15f + 0.6f * t) : IM_COL32(255, 255, 255, 14), r, 0,
                1.5f * s);

    // the art: the game's icon, else its colours and name
    ImVec2 art_b(a.x + w, a.y + art);
    ImU32 tint = installed ? IM_COL32_WHITE : IM_COL32(70, 74, 80, 255);
    if (gm.art.icon) {
        dl->AddImageRounded((ImTextureID)(intptr_t)gm.art.icon, a, art_b, ImVec2(0, 0), ImVec2(1, 1), tint, r,
                            ImDrawFlags_RoundCornersTop);
    } else {
        ImU32 top = installed ? look.art_top : IM_COL32(40, 44, 50, 255);
        ImU32 bottom = installed ? look.art_bottom : IM_COL32(22, 25, 30, 255);
        gradient_rect(dl, a, art_b, top, bottom, r, ImDrawFlags_RoundCornersTop);
        ImU32 tc = installed ? with_alpha(kTextHi, 0.9f) : with_alpha(kTextLo, 0.6f);
        text_centered(dl, F.small, ImVec2(a.x + w / 2, a.y + art / 2 - 28 * s), with_alpha(tc, 0.7f), "THE LEGEND OF ZELDA");
        text_centered(dl, F.title, ImVec2(a.x + w / 2, a.y + art / 2 + 6 * s), tc, kGames[g].title);
    }
    if (!installed) {   // a pill on the art says why the card is dark
        ImVec2 sz = measure(F.body, T->not_installed);
        ImVec2 c(a.x + w / 2, a.y + art / 2 + (gm.art.icon ? 0 : 56 * s));
        float pw = sz.x / 2 + 20 * s, ph = sz.y / 2 + 10 * s;
        dl->AddRectFilled(ImVec2(c.x - pw, c.y - ph), ImVec2(c.x + pw, c.y + ph), IM_COL32(10, 12, 15, 210), ph);
        text_centered(dl, F.body, c, kTextHi, T->not_installed);
    }

    // clicks on the art or the title start the game; hovering selects the card
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(g);
    if (ImGui::InvisibleButton("card", ImVec2(w, art + pad + 18 * s + 34 * s)) && installed) play = true;
    if (ImGui::IsItemHovered()) {
        if (g_mouse_travel > 6) select_card(g, "mouse");
        if (installed) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    // the text: series, title, version (a list when there are several builds)
    float x = a.x + pad, y = a.y + art + pad;
    ImU32 series = installed ? with_alpha(look.accent, 0.9f) : with_alpha(kTextLo, 0.6f);
    put(dl, F.small, ImVec2(x, y), series, "THE LEGEND OF ZELDA");
    y += 18 * s;
    put(dl, F.title, ImVec2(x, y), installed ? kTextHi : with_alpha(kTextLo, 0.8f),
                kGames[g].title);
    y += 34 * s;
    if (installed && gm.builds.size() > 1) {
        ImGui::SetCursorScreenPos(ImVec2(x - 6 * s, y - 4 * s));
        ImGui::SetNextItemWidth(w - 2 * pad + 12 * s);
        ImGui::PushFont(F.small);
        if (ImGui::BeginCombo("##build", build_label(*b).c_str(), ImGuiComboFlags_HeightLarge)) {
            for (int i = 0; i < (int)gm.builds.size(); i++)
                if (ImGui::Selectable(build_label(g_builds[gm.builds[i]]).c_str(), i == gm.chosen)) {
                    gm.chosen = i;
                    g_settings.exe[g] = g_builds[gm.builds[i]].exe;
                    save();
                }
            ImGui::EndCombo();
        }
        ImGui::PopFont();
    } else {
        std::string line = installed ? build_label(*b) : T->not_found;
        put(dl, F.small, ImVec2(x, y), kTextLo, line.c_str());
    }
    y += 26 * s;
    if (!gm.error.empty())
        put(dl, F.small, ImVec2(x, y), kError, gm.error.c_str());
    else if (link("locate", ImVec2(x, y), installed ? T->other_folder : T->locate, with_alpha(kTextLo, 0.8f)))
        locate(window, g);
    y += 22 * s;

    // the button: Play, else Install (outlined: the card stays grey until the game is there)
    ImVec2 bpos(x, y), bsize(w - 2 * pad, 52 * s * k);
    if (installed) {
        ImU32 fill = with_alpha(look.accent, 0.82f + 0.18f * t);
        if (pill("play", bpos, bsize, fill, look.accent_text, T->play)) play = true;
    } else if (g_setup) {   // one installer at a time
        bool mine = g_setup_game == g;
        pill("install", bpos, bsize, IM_COL32(40, 44, 51, 255), with_alpha(kTextLo, mine ? 0.9f : 0.45f),
             mine ? T->installing : T->install, false);
    } else if (pill("install", bpos, bsize, with_alpha(look.accent, 0.55f + 0.45f * t), kTextHi, T->install, true,
                    true)) {
        install(g);
    }
    ImGui::PopID();
    return play;
}

// The size a card takes at scale k (the same sums as card()).
static ImVec2 card_size(float k, float s) {
    float w = 340 * s * k, pad = 22 * s * k;
    return ImVec2(w, w + pad + 18 * s + 34 * s + 26 * s + 22 * s + 52 * s * k + pad);
}

// The selected game's icon, blurred, filling the window behind everything.
static void backdrop(ImDrawList *dl, ImVec2 ds) {
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ds, kBgTop, kBgTop, kBgBottom, kBgBottom);
    for (int g = 0; g < GAME_COUNT; g++) {
        Game &gm = g_games[g];
        if (gm.ambient < 0.01f) continue;
        float side = ds.x > ds.y ? ds.x : ds.y;
        ImVec2 c(ds.x / 2, ds.y / 2), half(side * 0.75f, side * 0.75f);
        if (gm.art.blur && chosen_build(g))
            dl->AddImage((ImTextureID)(intptr_t)gm.art.blur, ImVec2(c.x - half.x, c.y - half.y),
                         ImVec2(c.x + half.x, c.y + half.y), ImVec2(0, 0), ImVec2(1, 1),
                         with_alpha(IM_COL32_WHITE, 0.30f * gm.ambient));
        else {
            ImU32 col = with_alpha(chosen_build(g) ? kLook[g].art_top : IM_COL32(60, 64, 70, 255), 0.35f * gm.ambient);
            dl->AddRectFilledMultiColor(ImVec2(0, 0), ds, col, col, with_alpha(col, 0), with_alpha(col, 0));
        }
    }
    ImU32 shade = with_alpha(kBgBottom, 0.85f);   // darker towards the bottom, where the hint is
    dl->AddRectFilledMultiColor(ImVec2(0, ds.y * 0.45f), ds, with_alpha(kBgBottom, 0), with_alpha(kBgBottom, 0), shade,
                                shade);
}

// The window language, in the top right corner; the pick is saved in launcher.ini.
static void language_picker(ImVec2 ds, float margin, float s) {
    float w = 200 * s;
    ImGui::SetCursorScreenPos(ImVec2(ds.x - margin - w, margin * 0.9f));
    ImGui::SetNextItemWidth(w);
    ImGui::PushFont(F.small);
    if (ImGui::BeginCombo("##lang", kLanguages[g_lang].name)) {
        for (int i = 0; i < LANGUAGE_COUNT; i++)
            if (ImGui::Selectable(kLanguages[i].name, i == g_lang) && i != g_lang) {
                set_language(i);
                g_settings.lang = kLanguages[i].code;
                save();
            }
        ImGui::EndCombo();
    }
    ImGui::PopFont();
}

// ---- main

static bool key_pressed(std::initializer_list<ImGuiKey> keys) {
    for (ImGuiKey k : keys)
        if (ImGui::IsKeyPressed(k, true)) return true;
    return false;
}

int main(int argc, char **argv) {
    bool list = false;
    const char *lang = nullptr, *select = nullptr, *shot = nullptr;
    int win_w = 960, win_h = 760;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) list = true;
        else if (!strcmp(argv[i], "--lang") && i + 1 < argc) lang = argv[++i];
        else if (!strcmp(argv[i], "--select") && i + 1 < argc) select = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &win_w, &win_h);
        else {
            fprintf(stderr, "usage: launcher [--list] [--lang en|ru|pt-BR|zh-Hans] [--select totk|botw] [--shot FILE.png] [--size WxH]\n");
            return 2;
        }
    }

    const char *base = SDL_GetBasePath();
    fs::path dir = from_utf8(base ? base : ".");
    if (!dir.has_filename()) dir = dir.parent_path();   // SDL ends it with a separator
    g_settings_file = dir / "launcher.ini";
    g_settings = load_settings(g_settings_file);
    g_builds = find_builds(dir, g_settings);
    index_builds();
    if (list) {
        for (const Build &b : g_builds)
            printf("%s %s %s%s%s\n", kGames[b.game].key, b.version.empty() ? "?" : b.version.c_str(),
                   to_utf8(b.exe).c_str(), b.install ? " (install)" : "", b.icon.empty() ? "" : " (icon)");
        if (g_builds.empty()) printf("no builds found from %s\n", to_utf8(dir).c_str());
        return 0;
    }

    // --lang, else the one picked in the window (launcher.ini), else the system's
    int li = language_index(lang ? lang : g_settings.lang.c_str());
    set_language(li >= 0 ? li : system_language());
    g_sel = game_index(select ? select : g_settings.last);
    if (g_sel < 0) g_sel = chosen_build(GAME_BOTW) || !chosen_build(GAME_TOTK) ? GAME_BOTW : GAME_TOTK;

    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");   // launcher.rc
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "launcher: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    float display = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (display <= 0) display = 1;
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (shot ? SDL_WINDOW_HIDDEN : 0);
    SDL_Window *window = SDL_CreateWindow("TotK Recomp", (int)(win_w * display), (int)(win_h * display), flags);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        fprintf(stderr, "launcher: no window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(window, (int)(720 * display), (int)(560 * display));
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;   // the back end reads gamepads only with this
    ImGuiStyle &st = ImGui::GetStyle();
    st.FrameRounding = st.PopupRounding = 8;
    st.FramePadding = ImVec2(8, 5);
    st.Colors[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.05f);
    st.Colors[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.10f);
    st.Colors[ImGuiCol_Button] = ImVec4(1, 1, 1, 0.05f);   // the build list's arrow, as its frame
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(1, 1, 1, 0.10f);
    st.Colors[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.12f, 0.15f, 0.98f);
    st.Colors[ImGuiCol_Header] = ImVec4(1, 1, 1, 0.10f);
    st.Colors[ImGuiCol_HeaderHovered] = ImVec4(1, 1, 1, 0.16f);
    st.Colors[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(kTextHi);
    ImGuiStyle base_style = st;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    float font_scale = 0;
    bool quit = false;
    int frame = 0;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = true;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(30);
            continue;
        }
        // points to pixels: the display's scale over the window's pixel density (1 on Windows, 2 on a Retina Mac)
        float density = SDL_GetWindowPixelDensity(window), px = SDL_GetWindowDisplayScale(window);
        if (density <= 0) density = 1;
        if (px <= 0) px = 1;
        float s = px / density;
        if (px != font_scale) {
            build_fonts(px);
            io.FontGlobalScale = g_font_k = 1 / density;
            st = base_style;
            st.ScaleAllSizes(s);
            font_scale = px;
        }
        take_pick();
        poll_setup();
        update_art(renderer);

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImVec2 ds = io.DisplaySize;
        float dt = shot ? 1.f : io.DeltaTime;
        g_mouse_travel += fabsf(io.MouseDelta.x) + fabsf(io.MouseDelta.y);
        for (int g = 0; g < GAME_COUNT; g++) {   // ease towards the targets
            Game &gm = g_games[g];
            float ease = 1 - expf(-dt * 12);
            gm.hover += ((g == g_sel ? 1.f : 0.f) - gm.hover) * ease;
            gm.ambient += ((g == g_sel ? 1.f : 0.f) - gm.ambient) * (1 - expf(-dt * 5));
        }

        backdrop(ImGui::GetBackgroundDrawList(), ds);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ds);
        ImGui::Begin("launcher", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float margin = 40 * s;
        put(dl, F.small, ImVec2(margin, margin * 0.9f), with_alpha(kTextLo, 0.9f),
                    T->subheading);
        put(dl, F.heading, ImVec2(margin - 2 * s, margin * 0.9f + 20 * s), kTextHi, T->heading);
        float top = margin * 0.9f + 20 * s + 62 * s, bottom = ds.y - 56 * s;

        // the cards, scaled down when the window is small
        float gap = 44 * s;
        ImVec2 full = card_size(1, s);
        float k = fminf(1.f, fminf((ds.x - 2 * margin - gap) / (2 * full.x), (bottom - top - 20 * s) / full.y));
        ImVec2 cs = card_size(k, s);
        float x0 = floorf((ds.x - (2 * cs.x + gap)) / 2), y0 = floorf(top + (bottom - top - cs.y) / 2);
        int play = -1;
        for (int g = 0; g < GAME_COUNT; g++)
            if (card(g, ImVec2(x0 + g * (cs.x + gap), y0), k, s, window)) play = g;
        if (!chosen_build(GAME_TOTK) && !chosen_build(GAME_BOTW)) {
            ImGui::SetCursorScreenPos(ImVec2(margin, bottom - 6 * s));
            ImGui::PushTextWrapPos(ds.x - margin);
            ImGui::PushFont(F.small);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kTextLo), "%s", T->none);
            ImGui::PopFont();
            ImGui::PopTextWrapPos();
        } else {
            put(dl, F.small, ImVec2(margin, ds.y - 34 * s), with_alpha(kTextLo, 0.8f), T->hint);
        }
        language_picker(ds, margin, s);
        const char *ver = "TotK Recomp " LAUNCHER_VERSION;
        ImVec2 vs = measure(F.small, ver);
        put(dl, F.small, ImVec2(ds.x - margin - vs.x, ds.y - 34 * s), with_alpha(kTextLo, 0.5f),
                    ver);
        ImGui::End();

        // keys and gamepad (no popup open: then they belong to the build list)
        if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
            if (key_pressed({ImGuiKey_LeftArrow, ImGuiKey_A, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft}))
                select_card(0, "left");
            if (key_pressed({ImGuiKey_RightArrow, ImGuiKey_D, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight}))
                select_card(1, "right");
            if (key_pressed({ImGuiKey_Tab})) select_card((g_sel + 1) % GAME_COUNT, "tab");
            if (key_pressed({ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_GamepadFaceDown,
                             ImGuiKey_GamepadStart})) {
                if (chosen_build(g_sel)) play = g_sel;
                else if (!shot) install(g_sel);
            }
            if (key_pressed({ImGuiKey_Escape, ImGuiKey_GamepadBack})) quit = true;
        }
        if (play >= 0 && !shot && launch(play)) quit = true;

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (shot && ++frame == 3) {   // the first frames build the font texture and settle the layout
            SDL_Surface *surf = SDL_RenderReadPixels(renderer, nullptr);
            bool ok = surf && SDL_SavePNG(surf, shot);
            if (!ok) fprintf(stderr, "launcher: cannot save %s: %s\n", shot, SDL_GetError());
            SDL_DestroySurface(surf);
            quit = true;
        }
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    for (Game &gm : g_games) {
        if (gm.art.icon) SDL_DestroyTexture(gm.art.icon);
        if (gm.art.blur) SDL_DestroyTexture(gm.art.blur);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
