// Player mods (mods.c): one folder per mod under mods/ next to totk.exe (TOTK_MODS_DIR), unpacked as emulators take
// them: <mod>/romfs, <mod>/exefs and <mod>/cheats, the Atmosphere layout (atmosphere/contents/<title id>/romfs,
// atmosphere/exefs_patches/<name>/*.ips) or TKMM's merged output (romfs or romfslite, exefs, cheats). ExeFS patch
// files (.ips, .pchtxt) for the game's main are patch sets the setup builds into the game (tools/exefs_patches.py),
// switched with their mod (mods_exefs_switch). The list is scanned once at start (romfs.c overlay_init);
// its order and switches live in totk.ini [mods] (TOTK_MODS_ORDER: names top first, separated by '|';
// TOTK_MODS_DISABLED: names switched off). The top mod wins a file two mods replace.
#pragma once
#include <stdint.h>

enum { MOD_ROMFS = 1, MOD_EXEFS = 2, MOD_SUBSDK = 4, MOD_CHEATS = 8 };   // ModInfo.kind bits
typedef enum {
    MOD_OK,
    MOD_EMPTY,          // no RomFS file, no ExeFS patch, no cheat file
    MOD_PLUGIN,         // has code modules (exefs/subsdk*, a replaced exefs/main): the port has no module loader, so the
                        // whole mod stays off (its RomFS files may expect that code)
    MOD_EXEFS_LATER,    // has ExeFS patches for this game's main that the game was built without: the rebuild at the
                        // next start builds them in; the whole mod stays off until then
    MOD_EXEFS_UNSUPPORTED,   // an ExeFS patch file the port cannot apply (note says why): the whole mod stays off
    MOD_EXEFS_OTHER,    // only ExeFS patches, all for another game version
    MOD_BAD_NAME,       // its folder name holds '|', the separator of the order keys
} ModProblem;

typedef struct {
    char name[256];        // folder name, UTF-8
    char romfs[2048];      // UTF-8 path of its RomFS root; "" none
    char exefs[2048];      // UTF-8 path of its ExeFS folder; "" none
    char cheats[2048];     // UTF-8 path of its cheats folder; "" none
    int kind, enabled;
    ModProblem problem;
    uint32_t replaced, added, overridden;   // romfs.c: files served (replacing / added), files a higher mod serves
    uint32_t other_version;                 // files for another game version (RomFS *.Product.<n>.*, ExeFS patches
                                            // for another build), not used
    char overridden_by[256];                // the first higher mod that took one of its files
    char note[256];                         // MOD_EXEFS_UNSUPPORTED: why
    uint32_t pending, pending_key;          // ExeFS patch files the build lacks; FNV-1a of their names and contents
    int nsets;
    uint32_t sets[16];                      // the built patch sets mod_<hash> of its ExeFS patch files
    char overrides[256];                    // the port's own patch sets its patches replace, comma separated
} ModInfo;

void mods_scan(void);              // once; later calls return at once
const char *mods_dir(void);        // the folder scanned (default: <dir of totk.exe>/mods)
int mods_count(void);
ModInfo *mods_at(int i);           // 0 = top of the order; NULL out of range
// Menu edits (present path). They change the list mods_at shows and write TOTK_MODS_ORDER / TOTK_MODS_DISABLED with
// cfg_set; romfs.c used the start state, so they apply at the next start.
void mods_set_enabled(int i, int on);
void mods_move(int i, int delta);  // -1 up, +1 down; nothing at the ends
int mods_changed(void);            // 1 when the order or a switch differs from the start
// After rt_patches_init: the patch sets of the enabled mods' ExeFS patches on, the port's own sets they replace off.
void mods_exefs_switch(void);
// The ExeFS patch files of the enabled mods that the build lacks, their FNV-1a mixed into *key (cheats.c starts the
// rebuild for them).
int mods_exefs_pending(uint32_t *key);
// The hex of the game main's build id (64 digits), "" when exefs/main cannot be read.
const char *mods_main_build_id(void);
