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
// Codes the sdk itself returns (sdk nn::fs::detail::FileAccessor::Read and Write, DirectoryAccessor::Read,
// FileSystemAccessor::OpenDirectory, nn::fs::GetFreeSpaceSize, RomFsFileSystem and RomFsFile, fsa::IFileSystem's
// DoGetFileTimeStampRaw).
#define FS_R_NOT_IMPLEMENTED 0x177202u     // GetFileTimeStampRaw on a file system without timestamps (RomFS)
#define FS_R_OUT_OF_RANGE 0x177a02u        // offset past the end; an exact ReadFile that read less
#define FS_R_INVALID_OFFSET 0x2f5a02u
#define FS_R_INVALID_SIZE 0x2f5c02u
#define FS_R_NULLPTR 0x2f5e02u
#define FS_R_INVALID_MOUNT_NAME 0x2f6202u  // GetFreeSpaceSize with a path below a mount root
#define FS_R_DIR_OPEN_MODE 0x2f7402u       // OpenDirectory without a directory or file bit, or with an unknown bit
#define FS_R_ROMFS_WRITE 0x31b802u         // any change to the RomFS: create, delete, rename
#define FS_R_ROMFS_TOTAL_SPACE 0x31bc02u   // GetTotalSpaceSize on the RomFS

enum { FS_OPEN_READ = 1, FS_OPEN_WRITE = 2, FS_OPEN_APPEND = 4 };   // nn::fs::OpenMode
enum { FS_ENTRY_DIRECTORY = 0, FS_ENTRY_FILE = 1 };                 // nn::fs::DirectoryEntryType
enum { FS_DIR_DIRECTORIES = 1, FS_DIR_FILES = 2, FS_DIR_NO_FILE_SIZE = (int)0x80000000u };   // nn::fs::OpenDirectoryMode

// ---- romfs.c
// Handles 0x1_0000_0000 + j; host storage uses 0x2_0000_0000 + j (files) and 0x3_0000_0000 + j (directories).
#define ROMFS_HANDLE_BASE 0x100000000ull
#define ROMFS_MOUNTS 8                                     // RomFS mount names at once (nn::fs::MountRom)
int romfs_owns(const char *path);                          // path is on the mounted RomFS ("<mount>:...")
int romfs_handle(uint64_t h);                              // h is a RomFS file handle
// nn::fs::MountRom: name joins the RomFS mount names (ROMFS_MOUNTS of them); the same name again returns 0.
uint32_t romfs_mount(const char *name);
int romfs_unmount(const char *name);                       // 1 when name is a RomFS mount (the first stays mounted)
uint32_t romfs_open(uint64_t *out, const char *path, int mode);
uint32_t romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size);
uint32_t romfs_size(int64_t *out, uint64_t h);
void romfs_close(uint64_t h);
uint32_t romfs_entry_type(int *out, const char *path);
// The entries right under a RomFS folder ("<mount>:/" is the root), the overlays' files with them: cb(name, PLAT_FS_DIR
// or PLAT_FS_FILE, size, ctx) for the folders first, then the files, each in name order. FS_R_PATH_NOT_FOUND for a
// missing folder or a file.
uint32_t romfs_list(const char *path, int (*cb)(const char *name, int kind, uint64_t size, void *ctx), void *ctx);
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
const char *romfs_handle_name(uint64_t h);                 // RomFS path of an open RomFS handle, NULL for any other
void romfs_crash_report(void);                             // the last RomFS opens and who served each (crash report)

// ---- fs_host.c
int fs_host_mounted(const char *name);                     // name is a save, temporary, cache or SD card mount
void fs_host_set_profile(const char *dir);                 // profile directory; before the first storage mount only
                                                           // (default rt_config()->profile)
int fs_save_busy(void);                                    // a save is being written: a save file open for writing,
                                                           // or save data changed in the last 3 s and not committed
void fs_save_flush(void);                                  // wait for a save Commit still running in the background;
                                                           // every exit path calls it before the process ends
uint32_t fs_save_commits(char *slot, size_t cap);          // commits that changed save files; slot: the save folder
                                                           // (slot_00 ...) the last one wrote first
// Quick save owns dst and the dst.new/dst.old siblings. 0 on success; recover before checking dst for quick load.
int fs_save_recover_slot(const char *dst);                 // restores the previous complete copy after interruption
int fs_save_copy_slot(const char *slot, const char *dst);  // copies and publishes a folder of the newest save
void fs_save_read_from(const char *dir);                   // quick load: reads of save:/slot_NN/<file> open dir/<file>
                                                           // while dir is set; NULL ends it
int fs_save_read_from_hits(void);                          // opens redirected since the last fs_save_read_from(dir)
// Failed nn::fs calls (fs_host.c, section "failed calls"): an override that returns a failure notes the call name, the
// path (or the handle) and the module that asked (c: the guest's registers, NULL for a host caller). open: OpenFile,
// OpenDirectory or ReadFileAtOnce, whose first distinct paths are logged.
struct Cpu;
void fs_failed(const struct Cpu *c, const char *call, const char *path, uint32_t result, int open);
void fs_failed_handle(const struct Cpu *c, const char *call, uint64_t handle, uint32_t result);
void fs_failed_summary(void);                              // one log line with this run's counts; the exit paths
void fs_crash_report(void);                                // the crash report's lines: the last failed calls, the
                                                           // last RomFS opens
