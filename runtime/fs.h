// Adapted from MacNativeGames games/totk/runtime/fs_host.h @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames
// contributors).
// The nn::fs host layer: the read-only RomFS (romfs.c) and writable save, temporary and
// cache storage under the profile directory (fs_host.c). Both register their nn::fs overrides with HOST_OVERRIDE;
// this header is what the two files and the probe share.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

// ---- nn::fs results
#define FS_R_PATH_NOT_FOUND 0x202u
#define FS_R_ALREADY_EXISTS 0x402u
#define FS_R_NOT_EMPTY 0x1002u
#define FS_R_UNSUPPORTED_OPEN 0x6402u    // RomFS: any open mode but read
#define FS_R_INVALID 0xe402u             // RomFS: bad handle, offset or argument
#define FS_R_TARGET_NOT_FOUND 0x7d402u
#define FS_R_INVALID_ARGUMENT 0x2ee202u
#define FS_R_INVALID_PATH 0x2ee602u
#define FS_R_OPEN_MODE 0x307402u
#define FS_R_APPEND_REQUIRED 0x2f5e02u

enum { FS_OPEN_READ = 1, FS_OPEN_WRITE = 2, FS_OPEN_APPEND = 4 };   // nn::fs::OpenMode
enum { FS_ENTRY_DIRECTORY = 0, FS_ENTRY_FILE = 1 };                 // nn::fs::DirectoryEntryType

// ---- romfs.c
// Handles 0x1_0000_0000 + j; host storage uses 0x2_0000_0000 + j (files) and 0x3_0000_0000 + j (directories).
#define ROMFS_HANDLE_BASE 0x100000000ull
void *romfs_load(const char *rel, size_t *n, const char *why);   // a RomFS file as the game reads it; malloc'd
void romfs_list(const char *prefix, void (*cb)(const char *rel, void *ctx), void *ctx);   // image paths under prefix
// SARC member name of archive[0..n): its start and size; 0 when it is not there.
int romfs_sarc_member(const void *archive, size_t n, const char *name, size_t *at, size_t *size);
// The dictionary member ("pack.zsdic") of Pack/ZsDic.pack.zs as served: bytes valid for the run; 0 when missing.
int romfs_zsdic(const char *member, const void **p, size_t *n);
// A zstd frame with its content size of at most max bytes, unpacked with the game's dictionary its id names (none for
// a frame without one): malloc'd, NULL when it cannot be.
void *romfs_zs_unpack(const void *in, size_t n, size_t max, size_t *out_n);
int romfs_owns(const char *path);                          // path is on the mounted RomFS ("<mount>:...")
int romfs_handle(uint64_t h);                              // h is a RomFS file handle
uint32_t romfs_mount(const char *name);                    // nn::fs::MountRom; a second name is fatal
uint32_t romfs_open(uint64_t *out, const char *path, int mode);
uint32_t romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size);
uint32_t romfs_size(int64_t *out, uint64_t h);
void romfs_close(uint64_t h);
uint32_t romfs_entry_type(int *out, const char *path);
uint64_t romfs_file_count(void);                           // files in the index (fs_init first)
const char *romfs_entry(uint64_t i, uint64_t *offset, uint64_t *size);   // path, image offset and size of index row i
const char *romfs_image_path(void);                        // the romfs.bin path fs_init opened
size_t romfs_overlay_count(void);
int romfs_patches_loaded(void);                            // a romfs-patches manifest gave at least one file
// The button prompts (input_prompts_folder): the folder loaded at start, NULL for the game's own prompts; whether a
// folder had a manifest at start; and why the folder the settings asked for at start was not loaded (ROMFS_PROMPTS_OK
// for any other folder: it was not asked for). romfs-patches-pc goes by that name also where romfs-patches-pc-<w>x<h>
// stands in for it (romfs.c overlay_init).
enum { ROMFS_PROMPTS_OK, ROMFS_PROMPTS_MISSING, ROMFS_PROMPTS_FAILED, ROMFS_PROMPTS_SHAPE, ROMFS_PROMPTS_MODS,
       ROMFS_PROMPTS_PATCHES };
const char *romfs_prompts(void);
int romfs_prompts_ready(const char *folder);
// The folders a start at game resolution w x h with prompts_folder (input_prompts_folder; NULL for the game's own
// prompts) and the selected HUD width needs and that have no manifest: ROMFS_NEED_PROMPTS, prompts_folder itself (16:9);
// at another shape (res_wants_wide_overlay) ROMFS_NEED_LAYOUT, its romfs-patches-<w>x<h>[-hud..], and
// ROMFS_NEED_SHAPED_PROMPTS, <prompts_folder>-<w>x<h>[-hud..]. A full-layout fallback does not satisfy a requested HUD
// cap. 0 with TOTK_ROMFS_PATCHES set.
enum { ROMFS_NEED_LAYOUT = 1, ROMFS_NEED_PROMPTS = 2, ROMFS_NEED_SHAPED_PROMPTS = 4 };
int romfs_folders_missing(unsigned w, unsigned h, const char *prompts_folder);
// 1 when build-cache/rebuild.txt names the setup's Python and tools/make_romfs_patches.py and both exist, with romfs/.
int romfs_folders_can_prepare(void);
// Starts the generator for each folder romfs_folders_missing names, once per folder in this process, as child
// processes the game does not wait for; the next start loads the result. 1 when every one runs, 0 when none was
// missing, the tools are not there or one could not start.
int romfs_folders_prepare(unsigned w, unsigned h, const char *prompts_folder);
int romfs_prompts_off(const char *folder);
const char *romfs_overlay(size_t k, const char **file, uint64_t *size, int *is_added);   // RomFS path of overlay k
void fs_trace(const char *what, const char *path);         // TOTK_TRACE=fs: one line per open

// ---- fs_host.c
void fs_host_set_profile(const char *dir);                 // profile directory; before the first storage mount only
                                                           // (default rt_config()->profile)
int fs_save_busy(void);                                    // a save is being written: a save file open for writing,
                                                           // or save data changed in the last 3 s and not committed
void fs_save_flush(void);                                  // wait for a save Commit still running in the background;
                                                           // every exit path calls it before the process ends
uint32_t fs_save_commits(char *slot, size_t cap);          // commits that changed save files; slot: the save folder
                                                           // (slot_00 ...) the last one wrote first
int fs_save_copy_slot(const char *slot, const char *dst);  // a folder of the newest save copied to dst (replaced)
void fs_save_read_from(const char *dir);                   // quick load: reads of save:/slot_NN/<file> open dir/<file>
                                                           // while dir is set; NULL ends it
int fs_save_read_from_hits(void);                          // opens redirected since the last fs_save_read_from(dir)
