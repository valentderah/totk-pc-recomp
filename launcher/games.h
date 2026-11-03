// The games the launcher offers and the builds of them it finds on disk (docs/launcher.md). Free of SDL and Dear
// ImGui, so launcher --list can print what it found.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

enum { GAME_BOTW, GAME_TOTK, GAME_COUNT };   // the cards from left to right, in the order the games came out

struct GameInfo {
    const char *key;      // tools/games.py Game.key, the game = line of game.ini, the installer's --game
    const char *series;
    const char *title;
    const char *prefix;   // build folder names: prefix + the version's digits (v143, botw190)
    const char *exe;      // the executable (tools/games.py Game.exe; .exe added on Windows)
    const char *folder;   // the install folder the launcher offers, next to itself
    bool preview;         // support still in development: the card says so
};
extern const GameInfo kGames[GAME_COUNT];

std::string exe_name(int game);   // botw.exe, totk.exe

struct Build {
    int game = -1;        // GAME_*
    std::string version;  // display version ("1.4.3"), empty when the folder does not say
    fs::path exe;         // botw.exe or totk.exe (no .exe off Windows); a Breath of the Wild build made before the
                          // rename has a totk.exe
    fs::path icon;        // icon.jpg beside it (tools/game_icon.py), empty when there is none
    bool install = false; // an install folder of the setup (totk-install.json)
};

// launcher.ini beside the launcher: "last = <game key>", "lang = <window language>" and "<game key> = <path of its
// executable>", one per line.
struct Settings {
    std::string last;
    std::string lang;   // en, ru, pt-BR, zh-Hans (the installer window's languages); empty: the system's
    fs::path exe[GAME_COUNT];
};
Settings load_settings(const fs::path &file);
bool save_settings(const fs::path &file, const Settings &s);

int game_index(const std::string &key);   // GAME_* of a key, -1 for none

// What the executable at exe is: game.ini beside it (tools/build_game.py), else a build folder named like a version
// (build/v143, build/botw190), else an install folder (totk-install.json: the setup installs Tears of the Kingdom),
// else fallback_game when that is a game. False when it has no game data beside it (romfs-index.bin) or is none of
// these.
bool identify(const fs::path &exe, Build &out, int fallback_game = -1);

// The builds in dir (the launcher's folder), its subfolders and its parent's subfolders, plus the executables the
// settings name, without duplicates; each game's newest version first, an install folder before a build folder.
std::vector<Build> find_builds(const fs::path &dir, const Settings &s);

// -1, 0 or 1 as a < b, a == b, a > b for dotted versions ("1.4.3" > "1.2.1"); an empty version is the oldest.
int compare_versions(const std::string &a, const std::string &b);

// "C:/a/b/build/v143/totk.exe" -> "build\v143" (the last two folders), for a short label.
std::string short_folder(const fs::path &exe);

// UTF-8 strings (SDL, the settings file) to paths and back, whatever the C library's narrow encoding is.
fs::path from_utf8(const std::string &s);
std::string to_utf8(const fs::path &p);
