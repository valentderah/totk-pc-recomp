// Finding the games' builds for the launcher (games.h).
#include "games.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

const GameInfo kGames[GAME_COUNT] = {
    {"botw", "The Legend of Zelda", "Breath of the Wild", "botw", "botw", "BotW"},
    {"totk", "The Legend of Zelda", "Tears of the Kingdom", "v", "totk", "TotK"},
};

std::string exe_name(int game) {
#ifdef _WIN32
    return std::string(kGames[game].exe) + ".exe";
#else
    return kGames[game].exe;
#endif
}

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// The key = value lines of a small text file (launcher.ini, game.ini); # starts a comment line.
static std::vector<std::pair<std::string, std::string>> read_pairs(const fs::path &file) {
    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        out.emplace_back(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
    }
    return out;
}

int game_index(const std::string &key) {
    for (int i = 0; i < GAME_COUNT; i++)
        if (key == kGames[i].key) return i;
    return -1;
}

Settings load_settings(const fs::path &file) {
    Settings s;
    for (auto &[k, v] : read_pairs(file)) {
        int g = game_index(k);
        if (k == "last") s.last = v;
        else if (k == "lang") s.lang = v;
        else if (g >= 0 && !v.empty()) s.exe[g] = from_utf8(v);
    }
    return s;
}

bool save_settings(const fs::path &file, const Settings &s) {
    std::ofstream out(file, std::ios::trunc);
    out << "# The game launcher's choices (docs/launcher.md): the last game started and each game's executable.\n";
    if (!s.last.empty()) out << "last = " << s.last << "\n";
    if (!s.lang.empty()) out << "lang = " << s.lang << "\n";
    for (int g = 0; g < GAME_COUNT; g++)
        if (!s.exe[g].empty()) out << kGames[g].key << " = " << to_utf8(s.exe[g]) << "\n";
    return bool(out);
}

// "143" -> "1.4.3", "0" -> "1.0.0" (v0 is Tears of the Kingdom 1.0.0); empty when digits is not all digits.
static std::string dotted(const std::string &digits) {
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); }))
        return {};
    if (digits == "0") return "1.0.0";
    std::string v;
    for (char c : digits) {
        if (!v.empty()) v += '.';
        v += c;
    }
    return v;
}

// The game and version of a build folder named by tools/games.py: v<digits> or botw<digits>.
static bool from_folder_name(const std::string &name, Build &out) {
    for (int g : {GAME_BOTW, GAME_TOTK}) {   // botw first: "v" is a prefix of nothing else, but keep it unambiguous
        std::string p = kGames[g].prefix;
        if (name.size() > p.size() && name.compare(0, p.size(), p) == 0) {
            std::string v = dotted(name.substr(p.size()));
            if (v.empty()) continue;
            out.game = g;
            out.version = v;
            return true;
        }
    }
    return false;
}

// A string member of totk-install.json, which installer/state.py writes with one member per line: "version": "1.4.3",
// "game": "botw". The top-level ones come after the steps', none of which has these names.
static std::string install_member(const std::string &text, const char *name) {
    std::string key = std::string("\"") + name + "\"";
    size_t at = text.find(key);
    if (at == std::string::npos) return {};
    size_t q1 = text.find('"', text.find(':', at + key.size()));
    size_t q2 = q1 == std::string::npos ? q1 : text.find('"', q1 + 1);
    return q2 == std::string::npos ? std::string() : text.substr(q1 + 1, q2 - q1 - 1);
}

bool identify(const fs::path &exe, Build &out, int fallback_game) {
    std::error_code ec;
    fs::path dir = exe.parent_path();
    if (!fs::is_regular_file(exe, ec) || !fs::is_regular_file(dir / "romfs-index.bin", ec)) return false;
    Build b;
    b.exe = exe;
    if (fs::is_regular_file(dir / "game.ini", ec)) {
        for (auto &[k, v] : read_pairs(dir / "game.ini")) {
            if (k == "game") b.game = game_index(v);
            else if (k == "version") b.version = v;
        }
    } else if (fs::is_regular_file(dir / "totk-install.json", ec)) {
        std::ifstream in(dir / "totk-install.json");
        std::stringstream ss;
        ss << in.rdbuf();
        std::string game = install_member(ss.str(), "game");
        b.game = game.empty() ? GAME_TOTK : game_index(game);   // an install from before BotW support is TotK
        b.version = install_member(ss.str(), "version");
        b.install = true;
    } else if (!from_folder_name(to_utf8(dir.filename()), b)) {
        b.game = fallback_game;
    }
    if (b.game < 0) return false;
    // The game's own executable; another one in its folder is a leftover (a BotW build's totk.exe from before the
    // rename to botw.exe), unless it is the only one there.
    if (exe.filename() != exe_name(b.game) && fs::is_regular_file(dir / exe_name(b.game), ec)) return false;
    b.install = b.install || fs::is_regular_file(dir / "totk-install.json", ec);
    if (fs::is_regular_file(dir / "icon.jpg", ec)) b.icon = dir / "icon.jpg";
    out = b;
    return true;
}

int compare_versions(const std::string &a, const std::string &b) {
    std::istringstream sa(a), sb(b);
    std::string pa, pb;
    for (;;) {
        bool ha = bool(std::getline(sa, pa, '.')), hb = bool(std::getline(sb, pb, '.'));
        if (!ha && !hb) return 0;
        long na = ha ? std::strtol(pa.c_str(), nullptr, 10) : -1, nb = hb ? std::strtol(pb.c_str(), nullptr, 10) : -1;
        if (na != nb) return na < nb ? -1 : 1;
    }
}

std::vector<Build> find_builds(const fs::path &dir, const Settings &s) {
    std::vector<fs::path> exes;
    std::error_code ec;
    auto add = [&](const fs::path &d) {
        for (int g = 0; g < GAME_COUNT; g++) exes.push_back(d / exe_name(g));
    };
    auto scan = [&](const fs::path &d) {
        add(d);
        for (auto it = fs::directory_iterator(d, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->is_directory(ec)) add(it->path());
    };
    scan(dir);
    if (dir.has_parent_path() && dir.parent_path() != dir) scan(dir.parent_path());
    size_t scanned = exes.size();
    for (const fs::path &p : s.exe) exes.push_back(p);

    std::vector<Build> out;
    std::set<fs::path> seen;
    for (size_t i = 0; i < exes.size(); i++) {
        Build b;
        if (exes[i].empty()) continue;
        fs::path key = fs::weakly_canonical(exes[i], ec);
        int fallback = i < scanned ? -1 : int(i - scanned);   // a path from the settings is the game it is saved under
        if (ec || !seen.insert(key).second || !identify(key, b, fallback)) continue;
        out.push_back(b);
    }
    std::stable_sort(out.begin(), out.end(), [](const Build &a, const Build &b) {
        if (a.game != b.game) return a.game < b.game;
        int c = compare_versions(a.version, b.version);
        if (c) return c > 0;
        return a.install && !b.install;
    });
    // A build without its own icon shows another build's of the same game (an install folder has none).
    for (Build &b : out)
        if (b.icon.empty())
            for (const Build &o : out)
                if (o.game == b.game && !o.icon.empty()) {
                    b.icon = o.icon;
                    break;
                }
    return out;
}

std::string short_folder(const fs::path &exe) {
    fs::path dir = exe.parent_path();
    fs::path up = dir.parent_path();
    if (up.has_filename()) return to_utf8((up.filename() / dir.filename()).make_preferred());
    return to_utf8(dir.make_preferred());
}

fs::path from_utf8(const std::string &s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

std::string to_utf8(const fs::path &p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}
