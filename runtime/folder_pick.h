// The host's own file and folder dialogs, for menu pages that point at a file or a folder (the Amiibo and Blueprint
// tabs): folder_pick_win.cpp with Win32, folder_pick_sdl.c with SDL. The menu has no text entry, so a path is chosen
// here or by walking the page's list.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// done gets the chosen path (UTF-8) or NULL when the player cancelled or the host has no dialog.
typedef void (*FolderPickDone)(const char *path, void *ctx);
typedef void (*FilePickDone)(const char *path, void *ctx);

// Opens the dialog at start (may be relative or missing) and returns at once; done runs later on another thread, once.
// The caller keeps one dialog open at a time.
void folder_pick(const char *start, FolderPickDone done, void *ctx);

// The same for one file. start, when NULL or empty, means the folder the file dialog browsed last (blueprint_dir.h),
// so a player picks the next file where the previous one was; a non-empty start is used as it is. The dialog offers
// one file type, "Supported files", made of patterns ("*.txt", "*.cai"): the listed extensions are the only ones that
// can be chosen. count <= 0 shows every file instead.
void file_pick(const char *start, const char *const *patterns, int count, FilePickDone done, void *ctx);

#ifdef __cplusplus
}
#endif
