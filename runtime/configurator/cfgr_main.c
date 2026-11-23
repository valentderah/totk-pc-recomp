// totk-config: the command line. Without a command it opens the window; the commands change totk.ini without one.
//
//   totk-config [--dir <game folder>]                       the window (the folder: --dir, else this exe's folder)
//   totk-config --set fps_limit=120 --set fov=               set keys (an empty value resets one) and save
//   totk-config --profile <name>                            switch to a profile ("" for none)
//   totk-config --new-profile <name> [--from <preset>]      current, defaults, quality, performance, potato, deck
//   totk-config --list [<tab>]                              every row: tab, key, value in effect, marks
//   totk-config --check-update                              the version in the folder and the latest release
//   totk-config --download-update                           also downloads, checks and unpacks it (no setup)
//   totk-config --screenshot <file.bmp> [--tab <n>] [--search <text>] [--popup new|rename|delete|close]
//
// The window and the rows follow the menu language (TOTK_MENU_LANG, else the system's).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "cfg.h"
#include "configurator/cfgr.h"

#ifdef _WIN32
#include <windows.h>
#endif

static const char *const kTabs[CR_TAB_COUNT] = {"screen", "game", "sound", "controls", "amiibo",
                                                "menu", "advanced", "mods", "expert", "other"};
static const char *const kPresets[PRESET_COUNT] = {"current", "defaults", "quality", "performance", "potato", "deck"};

static void usage(void) {
    fputs("usage: totk-config [--dir DIR] [--set KEY=VALUE]... [--profile NAME] [--new-profile NAME [--from PRESET]]\n"
          "                   [--list [TAB]] [--screenshot FILE.bmp [--tab N] [--search TEXT]]\n"
          "                   [--check-update | --download-update]\n"
          "presets: current, defaults, quality, performance, potato, deck\n", stderr);
}

static void upper(char *s) {
    for (; *s; s++)
        if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
}

static int list(const char *tab) {
    int t = -1;
    for (int i = 0; tab && i < CR_TAB_COUNT; i++)
        if (!SDL_strcasecmp(tab, kTabs[i])) t = i;
    if (tab && t < 0) {
        fprintf(stderr, "totk-config: no tab %s\n", tab);
        return 2;
    }
    for (int i = 0; i < cr_row_count; i++) {
        const CrRow *r = &cr_rows[i];
        if (t >= 0 && r->tab != t) continue;
        char env[160];
        snprintf(env, sizeof env, "TOTK_%s", r->key);
        printf("%s\t%s\t%s\t%s%s\n", kTabs[r->tab], r->key, model_value(r), model_own(r->key) ? "set" : "",
               cfg_from_env(env) ? " env" : "");
    }
    const char *keys[256], *values[256];
    int n = model_other_keys(keys, values, 256);
    for (int i = 0; i < n && (t < 0 || t == CR_TAB_OTHER); i++) printf("other\t%s\t%s\tset\n", keys[i], values[i]);
    return 0;
}

// --check-update: the version in the game folder and the latest release; --download-update also fetches it into the
// updates folder, checks it and unpacks it, without starting its setup (cfgr_update.c).
static int update_command(int download) {
    CrInstalled in;
    upd_installed(model_dir(), &in);
    printf("installed: %s%s\n", in.run[0] ? in.run : in.setup[0] ? in.setup : "unknown", in.custom ? " (own build)" : "");
    if (in.setup[0]) printf("set up with: %s\n", in.setup);
    printf("check when the window opens: %s\n", upd_pref_check_at_start() ? "on" : "off");
    CrRelease r;
    char err[512] = "";
    if (!upd_check(&r, err, sizeof err)) {
        fprintf(stderr, "totk-config: %s\n", err);
        return 1;
    }
    int newer = !in.have || upd_version_cmp(r.ver, in.ver) > 0;
    printf("latest: %s (%s, %llu bytes, sha256 %.16s...)%s\n", r.tag, r.asset, (unsigned long long)r.size, r.sha256,
           newer ? ", newer" : ", not newer");
    if (download) {
        char folder[CR_PATH];
        if (!upd_download(&r, folder, sizeof folder, err, sizeof err)) {
            fprintf(stderr, "totk-config: %s\n", err);
            return 1;
        }
        printf("ready: %s\n", folder);
    }
    return 0;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    // a window program: the commands print into the console they were typed in, unless the output already goes to a
    // file or pipe
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    int redirected = out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN;
    if (argc > 1 && !redirected && AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
#endif
    const char *dir = NULL, *profile = NULL, *new_profile = NULL, *from = "current", *shot = NULL, *search = NULL;
    const char *sets[64];
    int nsets = 0, do_list = 0, tab = -1, check_update = 0, download_update = 0;
    const char *list_tab = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *next = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--dir") && next) dir = argv[++i];
        else if (!strcmp(a, "--set") && next && nsets < 64) sets[nsets++] = argv[++i];
        else if (!strcmp(a, "--profile") && next) profile = argv[++i];
        else if (!strcmp(a, "--new-profile") && next) new_profile = argv[++i];
        else if (!strcmp(a, "--from") && next) from = argv[++i];
        else if (!strcmp(a, "--screenshot") && next) shot = argv[++i];
        else if (!strcmp(a, "--tab") && next) tab = atoi(argv[++i]);
        else if (!strcmp(a, "--search") && next) search = argv[++i];
        else if (!strcmp(a, "--check-update")) check_update = 1;
        else if (!strcmp(a, "--download-update")) check_update = download_update = 1;
        else if (!strcmp(a, "--popup") && next) {
            static const char *const pops[] = {"new", "rename", "delete", "close"};
            const char *v = argv[++i];
            for (int k = 0; k < 4; k++)
                if (!strcmp(v, pops[k])) cr_shot_popup = k + 1;
        }
        else if (!strcmp(a, "--list")) {
            do_list = 1;
            if (next && next[0] != '-') list_tab = argv[++i];
        } else {
            usage();
            return 2;
        }
    }
    model_open(dir);
    int rc = 0;
    if (nsets) {
        for (int i = 0; i < nsets; i++) {
            char key[160];
            const char *eq = strchr(sets[i], '=');
            if (!eq || eq == sets[i] || (size_t)(eq - sets[i]) >= sizeof key) {
                fprintf(stderr, "totk-config: --set %s: expected KEY=VALUE\n", sets[i]);
                return 2;
            }
            snprintf(key, sizeof key, "%.*s", (int)(eq - sets[i]), sets[i]);
            upper(key);
            if (!strncmp(key, "TOTK_", 5)) memmove(key, key + 5, strlen(key + 5) + 1);
            if (!cfg_known(key)) fprintf(stderr, "totk-config: %s is no key this version of the game reads\n", key);
            model_set(key, eq + 1);
        }
        if (!model_save()) {
            fprintf(stderr, "totk-config: cannot write %s\n", model_ini_path());
            return 1;
        }
        printf("wrote %s\n", model_ini_path());
    }
    if (new_profile) {
        int p = -1;
        for (int i = 0; i < PRESET_COUNT; i++)
            if (!SDL_strcasecmp(from, kPresets[i])) p = i;
        if (p < 0) {
            usage();
            return 2;
        }
        if (!model_new_profile(new_profile, (Preset)p, 0)) {
            fprintf(stderr, "totk-config: cannot create profile %s (name taken or not allowed, or a write failed)\n",
                    new_profile);
            return 1;
        }
        printf("created profile %s from %s\n", new_profile, kPresets[p]);
    }
    if (profile) {
        if (!model_switch_profile(profile)) {
            fprintf(stderr, "totk-config: cannot switch to profile %s\n", profile);
            return 1;
        }
        printf("active profile: %s\n", *profile ? profile : "(none)");
    }
    if (do_list) rc = list(list_tab);
    if (check_update && !rc) rc = update_command(download_update);
    if (nsets || new_profile || profile || do_list || check_update) return rc;
    return window_run(shot, tab, search);
}
