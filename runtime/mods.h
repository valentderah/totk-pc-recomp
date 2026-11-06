// Player mods (mods.c): one folder per mod under mods/ next to totk.exe (TOTK_MODS_DIR), unpacked as emulators take
// them: <mod>/romfs, <mod>/exefs and <mod>/cheats, the Atmosphere layout (atmosphere/contents/<title id>/romfs,
// atmosphere/exefs_patches/<name>/*.ips) or TKMM's merged output (romfs or romfslite, exefs, cheats). ExeFS patch
// files (.ips, .pchtxt) for the game's main are patch sets the setup builds into the game (tools/exefs_patches.py),
// switched with their mod (mods_exefs_switch), and data writes stored into main before the game runs (mods_exefs_data).
// A code module (exefs/subsdk*) of a plugin build the port knows runs as the port's own code: the setup builds its
// hooks in as a patch set (tools/plugins.py), plugins.c has the handlers.
// The list is scanned once at start (romfs.c overlay_init);
// its order and switches live in totk.ini [mods] (TOTK_MODS_ORDER: names top first, separated by '|';
// TOTK_MODS_DISABLED: names switched off). The top mod wins a file two mods replace.
#pragma once
#include <stdint.h>

enum { MOD_ROMFS = 1, MOD_EXEFS = 2, MOD_SUBSDK = 4, MOD_CHEATS = 8 };   // ModInfo.kind bits
typedef enum {
    MOD_OK,
    MOD_EMPTY,          // no RomFS file, no ExeFS patch, no cheat file
    MOD_PLUGIN,         // has code modules the port cannot run (exefs/subsdk* of a build it does not know, a replaced
                        // exefs/main): it has no module loader, so the whole mod stays off (its RomFS files may expect
                        // that code); with TOTK_MODS_WITHOUT_CODE=1 the rest of such a mod is used, without those code
                        // modules (ModInfo.code names them)
    MOD_EXEFS_LATER,    // has ExeFS patches for this game's main, or a plugin build the port runs as its own code,
                        // that the game was built without: the rebuild at the next start builds them in; the whole mod
                        // stays off until then
    MOD_EXEFS_UNSUPPORTED,   // an ExeFS patch file the port cannot apply (note says why): the whole mod stays off
    MOD_EXEFS_OTHER,    // only ExeFS patches, all for another game version
    MOD_BAD_NAME,       // its folder name holds '|', the separator of the order keys
} ModProblem;

typedef struct {
    char name[256];        // folder name, UTF-8
    char romfs[2048];      // UTF-8 path of its RomFS root; "" none
    char exefs[2048];      // UTF-8 path of its ExeFS folder; "" none
    char cheats[2048];     // UTF-8 path of its cheats folder; "" none
    int kind, enabled;     // kind: MOD_SUBSDK for any code module, run or not
    char code[64];         // its code modules in exefs the port cannot run ("subsdk9", "main, subsdk9"): never loaded
    int nplugins;
    struct {
        char module[16];   // its code modules the port runs as its own code: the file name in exefs ("subsdk9"),
        char name[96];     // the build's name (tools/plugins.py), the patch set of its hooks (none with 0 hooks: the
        uint32_t set;      // build installs none on this game version, as on the Switch)
        int hooks;
        char shadowed[64]; // mods_exefs_switch: the mod higher in the list whose code module of that name the Switch
                           // would load instead ("" when this one runs)
    } plugin[4];
    ModProblem problem;
    uint32_t replaced, added, overridden;   // romfs.c: files served (replacing / added), files a higher mod serves
    uint32_t other_version;                 // files for another game version (RomFS *.Product.<n>.*, ExeFS patches
                                            // for another build), not used
    char overridden_by[256];                // the first higher mod that took one of its files
    char note[256];                         // MOD_EXEFS_UNSUPPORTED: why
    uint32_t pending, pending_key;          // ExeFS patch files the build lacks; FNV-1a of their names and contents
    int nsets;
    uint32_t sets[16];                      // the built patch sets mod_<hash> of its ExeFS patch files
    int ndata;
    uint32_t data[16];                      // the hashes of its ExeFS patch files with data writes only (no set)
    uint32_t data_bytes;                    // the bytes its usable ExeFS patch files write into main's data, as their
                                            // status lines count them ("data <bytes>")
    char overrides[256];                    // the port's own patch sets its patches replace, comma separated
} ModInfo;

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
void mods_move(int i, int delta);  // -1 up, +1 down; nothing at the ends
int mods_changed(void);            // 1 when the order or a switch differs from the start
// After ldr_load and rt_patches_init, before any game code: the patch sets of the enabled mods' ExeFS patches on, the
// port's own sets they replace off, and their data writes stored into main (mods_exefs_data).
void mods_exefs_switch(void);
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
// The ExeFS files of the enabled mods that the build lacks (patch files, code modules of known plugin builds), their
// FNV-1a mixed into *key (cheats.c starts the rebuild for them).
int mods_exefs_pending(uint32_t *key);
// The hex of the game main's build id (64 digits), "" when exefs/main cannot be read.
const char *mods_main_build_id(void);
