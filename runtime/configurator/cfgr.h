// totk-config, the settings window outside the game: edits totk.ini and named settings profiles next to totk.exe
// before the game starts. It links the runtime's own settings code: cfg.c reads and writes totk.ini exactly as the game
// does (cfg_load, cfg_set, cfg_reset, cfg_save_to) and holds every key's default and accepted values (cfg_meta),
// ui_text.c gives the in-game menu's labels and descriptions in the menu language, input_map.c the binding names.
//
// Which tab, group and kind of row each setting gets comes from cr_rows[], which tools/gen_configurator.py writes at
// build time from runtime/ui/menu.cpp (docs/configurator.md, "Adding a setting"). Files:
//   cfgr_main.c    command line, the headless commands (--set, --profile, --new-profile, --list)
//   cfgr_model.c   the game folder, totk.ini, profiles, presets, starting the game
//   cfgr_window.c  the SDL3 window and the Nuklear rows
//   cfgr_nk.c      Nuklear and its SDL3 renderer (runtime/third_party/nuklear)
//   cfgr_host.c    the few runtime functions cfg.c and ui_text.c call (log, fatal error, game language)
#pragma once
#include <stddef.h>

#include "ui_text.h"

// Tabs: the in-game menu's settings tabs in its order (ui/menu.cpp kTabNames), then the window's own two.
enum { CR_TAB_SCREEN, CR_TAB_GAME, CR_TAB_SOUND, CR_TAB_CONTROLS, CR_TAB_AMIIBO, CR_TAB_MENU, CR_TAB_ADVANCED,
       CR_TAB_MODS, CR_TAB_EXPERT, CR_TAB_OTHER, CR_TAB_COUNT };
// The window's Updates page (Windows), after the tabs of settings.
enum { CR_TAB_UPDATES = CR_TAB_COUNT };
// Row kinds: what the menu row is (opt_toggle, opt_choice / opt_choice_list, opt_slider, binds_key_row), a folder, a
// binding (BIND_*), or free text.
enum { CR_TOGGLE, CR_CHOICE, CR_SLIDER, CR_TEXT, CR_KEY, CR_FOLDER, CR_BIND };
enum {
    CR_INVERTED = 1,     // the toggle shows "on" while the key is 0
    CR_DYNAMIC = 2,      // the choices are listed at run time (game languages, audio devices, GPUs)
    CR_SET_OR_NOT = 4,   // Off is the key unset, On any value (menu.cpp row_set_or_not)
};

// One choice: value NULL means "the n-th value of the key's cfg_meta list" (opt_choice, with Automatic first when the
// key has no fixed default); name a UiTextId or -1, text a fixed name or NULL (the value is shown).
typedef struct { const char *value; int name; const char *text; } CrChoice;

typedef struct {
    const char *key;         // without TOTK_
    int tab, group;          // group: a UiTextId heading, -1 none
    int label, desc;         // UiTextIds, -1 none (Expert rows show the key and doc)
    int kind, flags;
    float lo, hi, step;      // CR_SLIDER
    const char *fmt;         // CR_SLIDER: printf of the value ("%.0f %%"), NULL "%g"
    const char *fallback;    // CR_SLIDER: the key that applies while this one is unset (without TOTK_)
    int n_choices;
    const CrChoice *choices;
    const char *doc;         // English description from docs/settings.md or totk.ini.example, NULL none
    const char *doc_group;   // its heading there (Expert tab groups)
    const char *def;         // default of a key outside cfg.c metas[] (rt_knob, docs/settings.md), NULL unknown
} CrRow;

extern const CrRow cr_rows[];
extern const int cr_row_count;

// ---- cfgr_model.c
enum { CR_PATH = 1024, CR_NAME = 64, CR_MAX_PROFILES = 64 };
typedef enum { PRESET_CURRENT, PRESET_DEFAULTS, PRESET_QUALITY, PRESET_PERFORMANCE, PRESET_POTATO, PRESET_DECK,
               PRESET_COUNT } Preset;
typedef struct { const char *key, *value; } PresetChange;   // value NULL: back to the default (the line goes)

extern const char *const cr_game_exe;   // "totk.exe" on Windows, "totk" elsewhere

// The game folder: --dir, else the folder of this exe. Loads its totk.ini (missing is fine: an empty file).
void model_open(const char *dir);
const char *model_dir(void);
const char *model_ini_path(void);
int model_has_game(void);                  // the game's exe is in the folder
int model_reload(void);                    // reads totk.ini again, dropping unsaved changes; 0 when it is missing
int model_save(void);                      // writes the changed lines (cfg_save_to); 1 on success
int model_unsaved(void);                   // keys whose value differs from the file's
int model_file_changed(void);              // totk.ini's time or size changed since it was read or written

// A key's own value: what totk.ini (or an unsaved change) sets, without the environment; NULL when unset.
const char *model_own(const char *key);
int model_changed(const char *key);        // differs from what the file holds
const char *model_value(const CrRow *r);   // the value in effect: environment, own value, default ("" none)
const char *model_default(const CrRow *r); // the default shown for the key ("" none, the consumer picks)
void model_set(const char *key, const char *value);   // NULL or "" for a key with a fixed default: reset
void model_reset(const char *key);
// The keys of totk.ini the runtime does not read (Other keys); returns how many, at most max.
int model_other_keys(const char **keys, const char **values, int max);

// Profiles: profiles/<name>.ini next to totk.ini, profiles/active.txt naming the one totk.ini holds.
int model_profiles(char names[][CR_NAME], int max);
const char *model_active_profile(void);   // "" none
int model_valid_name(const char *name);   // a free, usable file name
// Saves pending changes, stores totk.ini into the active profile, copies name's file over totk.ini and reloads.
int model_switch_profile(const char *name);
int model_new_profile(const char *name, Preset from, int activate);
int model_rename_profile(const char *from, const char *to);
int model_delete_profile(const char *name);
const PresetChange *preset_changes(Preset p, int *n);   // Quality, Performance, Potato; Deck from cfg_deck_at
UiTextId preset_label(Preset p);
UiTextId preset_desc(Preset p);

int model_launch(void);   // starts the game from its folder; 1 when it started

// ---- cfgr_window.c
// The window until it closes; the exit code. shot: draw a few frames hidden, save them as a BMP there and close (tests,
// docs); tab (CR_TAB_*, -1 the first) and search preset the page.
int window_run(const char *shot, int tab, const char *search);
// --popup with --screenshot: the dialog the picture shows (1 new profile, 2 rename, 3 delete, 4 unsaved changes on close)
extern int cr_shot_popup;

// ---- cfgr_update.c: the latest release from GitHub, its download and its setup (Windows)
#include <stdint.h>
typedef struct {
    char tag[64], name[128], page[256], published[32];
    int ver[3];
    char *notes;                       // the release text (Markdown), NULL none
    char asset[160], sha256[65], url[512];
    uint64_t size;
} CrRelease;
typedef struct {
    char run[64];     // the version of the totk.exe that ran last (logs/totk.log), when it ran after the setup
    char setup[64];   // the release whose setup made the folder (totk-install.json)
    int custom;       // totk.exe is a build of its own, which an update replaces
    int have, ver[3]; // the version to compare with a release: the one that ran, else the setup's
} CrInstalled;
enum { UPD_IDLE, UPD_CHECKING, UPD_CHECKED, UPD_DOWNLOADING, UPD_VERIFYING, UPD_UNPACKING, UPD_READY, UPD_FAILED };
typedef struct {
    int phase;
    uint64_t done, total;   // UPD_DOWNLOADING
    char msg[512];          // UPD_FAILED: why
    char folder[CR_PATH];   // UPD_READY: the unpacked release
} CrUpdState;

int upd_parse_version(const char *s, int v[3]);   // "v0.5.0", "0.5.0+6151ac4f", "0.5" -> 1
int upd_version_cmp(const int a[3], const int b[3]);
int upd_version_in_log(const char *text, char *out, size_t cap);
int upd_parse_release(const char *json, const char *base, CrRelease *r, char *err, size_t en);   // base NULL: the API's
void upd_release_free(CrRelease *r);
int upd_safe_entry(const char *name);             // an archive entry that stays inside its folder
void upd_installed(const char *dir, CrInstalled *out);
int upd_game_running(const char *dir);            // the folder's game exe runs (Windows refuses to open it for writing)
int upd_check(CrRelease *out, char *err, size_t en);                             // one API call, on this thread
int upd_download(const CrRelease *r, char *folder, size_t fcap, char *err, size_t en);   // download, check, unpack
int upd_start_setup(const char *release, const char *game, char *err, size_t en);
// the window's threads: the check, then download, check and unpack of the release checked
void upd_check_async(void);
void upd_fetch_async(void);
void upd_state(CrUpdState *out);
int upd_latest(CrRelease *out);   // 1 when a check succeeded; the notes stay owned by cfgr_update.c
int upd_busy(void);
void upd_set_failed(const char *why);
// the window's own setting, apart from totk.ini: ask GitHub when the window opens
int upd_pref_check_at_start(void);
int upd_pref_set_check_at_start(int on);

// ---- text: the menu language's text of id (menu_text), English for -1
const char *cr_text(int id);
