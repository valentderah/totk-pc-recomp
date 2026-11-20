// Player mods (mods.c): one folder per mod under mods/ next to totk.exe (TOTK_MODS_DIR), unpacked as emulators take
// them: <mod>/romfs, <mod>/exefs and <mod>/cheats, the Atmosphere layout (atmosphere/contents/<title id>/romfs,
// atmosphere/exefs_patches/<name>/*.ips) or TKMM's merged output (romfs or romfslite, exefs, cheats). ExeFS patch
// files (.ips, .pchtxt) for the game's main are patch sets the setup builds into the game (tools/exefs_patches.py),
// switched with their mod (mods_exefs_switch), and data writes stored into main before the game runs (mods_exefs_data).
// The list is scanned once at start (romfs.c overlay_init);
// its order and switches live in totk.ini [mods] (TOTK_MODS_ORDER: names top first, separated by '|';
// TOTK_MODS_DISABLED: names switched off; TOTK_MODS_NO_CODE: names used without their code part). The top mod wins
// whatever two mods change: a file, bytes of main's data, a code site (patches.c rt_patch_picks); the conflict table
// (mods_conflict_at) records who won over whom, for the menu and the log.
#pragma once
#include <stdint.h>

enum { MOD_ROMFS = 1, MOD_EXEFS = 2, MOD_SUBSDK = 4, MOD_CHEATS = 8 };   // ModInfo.kind bits
typedef enum {
    MOD_OK,
    MOD_EMPTY,          // no RomFS file, no ExeFS patch, no cheat file
    MOD_PLUGIN,         // has code modules (exefs/subsdk*, a replaced exefs/main): the port has no module loader, so the
                        // whole mod stays off (its RomFS files may expect that code); used without its code
                        // (ModInfo.without_code) the rest of such a mod is used, without those code modules
                        // (ModInfo.code names them)
    MOD_EXEFS_LATER,    // has ExeFS patches for this game's main that the game was built without: the rebuild at the
                        // next start builds them in; the whole mod stays off until then
    MOD_EXEFS_UNSUPPORTED,   // an ExeFS patch file the port cannot apply (note says why): the whole mod stays off,
                             // unless it is used without its code (ModInfo.without_code) and has a usable rest
    MOD_EXEFS_OTHER,    // only ExeFS patches, all for another game version
    MOD_BAD_NAME,       // its folder name holds '|', the separator of the order keys
} ModProblem;

typedef struct {
    char name[256];        // folder name, UTF-8
    char romfs[2048];      // UTF-8 path of its RomFS root; "" none
    char exefs[2048];      // UTF-8 path of its ExeFS folder; "" none
    char cheats[2048];     // UTF-8 path of its cheats folder; "" none
    int kind, enabled;     // kind: MOD_SUBSDK for any code module
    int id;                // its place in the scan: names it in the conflict table, whatever the menu moves
    int no_code;           // its switch "use without its code" (TOTK_MODS_NO_CODE)
    int without_code;      // the scan's: no_code or TOTK_MODS_WITHOUT_CODE (every mod); the code parts it cannot run
                           // (code modules, unsupported ExeFS patch files) are left out and the rest is used
    char code[64];         // its code modules in exefs ("subsdk9", "main, subsdk9"): never loaded
    uint32_t unsupported;  // its ExeFS patch files the port cannot apply (the first one's reason in note)
    ModProblem problem;
    uint32_t replaced, added, overridden;   // romfs.c: files served (replacing / added), files a higher mod serves
    uint32_t other_version;                 // files named for another game version: RomFS *.Product.<n>.* the RomFS
                                            // lacks (served under their own names; another version's size table is
                                            // not) and ExeFS patches for another build (not used)
    char note[256];                         // an unsupported ExeFS patch file: which and why
    uint32_t pending, pending_key;          // ExeFS patch files the build lacks; FNV-1a of their names and contents
    int nsets;
    uint32_t sets[16];                      // the built patch sets mod_<hash> of its ExeFS patch files
    int ndata;
    uint32_t data[16];                      // the hashes of its ExeFS patch files with data writes only (no set)
    uint32_t data_bytes;                    // the bytes its usable ExeFS patch files write into main's data, as their
                                            // status lines count them ("data <bytes>")
    char overrides[256];                    // the port's own patch sets its patches replace, comma separated
    // Its exefs/subsdk* plugins (build-cache/plugins.status, tools/exefs_plugins.py): the built ones are patch sets in
    // sets[] whose init runs at start (plugin_init.c); a refused one is a code module it cannot run (code).
    int nplugins, plugin_hooks;             // built plugins and their hooks
    uint32_t plugin_keys[4], plugin_contents[4];   // their file and content hashes, for plugins.stale
    char plugin_file[64];                   // the first plugin's file name
    char plugin_note[256];                  // why the build refused a plugin ("" none)
    char plugin_off[256];                   // why its plugin stays off this run: its start differed from the build
} ModInfo;

// What two mods both change, the higher one winning: files (romfs.c), bytes of main's data and code sites
// (mods_exefs_apply). One entry per winner, loser and part, as at start.
typedef enum { MOD_PART_FILES, MOD_PART_DATA, MOD_PART_CODE, MOD_PARTS } ModPart;
typedef struct {
    int winner, loser;     // ModInfo.id
    ModPart part;
    uint32_t count;        // files, bytes, code sites
} ModConflict;
void mods_conflict(int winner, int loser, ModPart part, uint32_t count);   // adds count to the pair's entry
int mods_conflict_count(void);
const ModConflict *mods_conflict_at(int i);
void mods_conflicts_log(ModPart part);   // one line each way per entry of that part
const ModInfo *mods_by_id(int id);

void mods_scan(void);              // once; later calls return at once
// Every switched-on mod folder's RomFS root, without the rest of the scan (no ExeFS checks, no order): for mem_init,
// which runs before the paths mods_scan needs are set (resolution.c mod_tex_heap_kib). cb gets the name and root.
void mods_peek_romfs(void (*cb)(const char *name, const char *romfs, void *ctx), void *ctx);
const char *mods_dir(void);        // the folder scanned (default: <dir of totk.exe>/mods)
int mods_count(void);
ModInfo *mods_at(int i);           // 0 = top of the order; NULL out of range
// Menu edits (present path). They change the list mods_at shows and write TOTK_MODS_ORDER / TOTK_MODS_DISABLED with
// cfg_set; romfs.c used the start state, so they apply at the next start.
void mods_set_enabled(int i, int on);
void mods_set_no_code(int i, int on);   // "use without its code" (TOTK_MODS_NO_CODE)
void mods_move(int i, int delta);  // -1 up, +1 down; nothing at the ends
int mods_changed(void);            // 1 when the order or a switch differs from the start
// After ldr_load and rt_patches_init, before any game code: the patch sets of the enabled mods' ExeFS patches on and
// ranked by the order (rt_patch_rank: the top mod's code runs where several patch one site), the port's own sets they
// replace off, and their data writes stored into main (mods_exefs_data); the data and code conflicts recorded.
void mods_exefs_switch(void);
// What mods_exefs_switch does with main's image at base (size bytes, code below code_end; size 0: no data stored), for
// every enabled mod from the bottom one up, so that the top mod's data writes win where two mods write the same bytes.
// store as for mods_exefs_data.
void mods_exefs_apply(uint64_t base, uint64_t size, uint64_t code_end,
                      int (*store)(uint64_t addr, const uint8_t *bytes, int n));
// Stores the data writes of mod m's ExeFS patch files (build-cache/exefs.data) into main's image at base: size bytes,
// code below code_end. A DT_RELR word gets base added, as the dynamic linker adds it. Every line is checked before the
// first store: one that falls outside [code_end, size) or does not belong to a usable file of m keeps them all out.
// store writes n bytes at a guest address, 1 on success; NULL: into the loaded main, whose .rodata is read-only.
// Returns the bytes stored, -1 when a line kept them out or a store failed. --probe-mods passes a buffer and memcpy.
long mods_exefs_data(const ModInfo *m, uint64_t base, uint64_t size, uint64_t code_end,
                     int (*store)(uint64_t addr, const uint8_t *bytes, int n));
// --probe-mods: the texts of build-cache/exefs.status and exefs.data instead of the files (NULL: read the file), and a
// fresh scan at the next mods_scan.
void mods_probe_cache(const char *status, const char *data);
void mods_rescan(void);
// The ExeFS patch files of the enabled mods that the build lacks, their FNV-1a mixed into *key (cheats.c starts the
// rebuild for them).
int mods_exefs_pending(uint32_t *key);
// From plugin_init.c plugins_start: the plugin patch set `set` stays off this run (why), its mod's row says so,
// build-cache/plugins.stale marks its file for the rebuild at the next start, and the code conflicts are counted anew.
void mods_plugin_off(int set, const char *why);
// --probe-mods: the texts of build-cache/plugins.status and plugins.stale instead of the files; the stale lines
// mods_plugin_off adds stay in memory.
void mods_probe_plugins(const char *status, const char *stale);
// The hex of the game main's build id (64 digits), "" when exefs/main cannot be read.
const char *mods_main_build_id(void);
