// The host's own folder dialog, for menu pages that point at a folder (the Amiibo tab): folder_pick_win.cpp with Win32,
// folder_pick_sdl.c with SDL. The menu has no text entry, so a folder is chosen here or by walking the page's list.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// done gets the chosen folder (UTF-8) or NULL when the player cancelled or the host has no dialog.
typedef void (*FolderPickDone)(const char *path, void *ctx);

// Opens the dialog at start (may be relative or missing) and returns at once; done runs later on another thread, once.
// The caller keeps one dialog open at a time.
void folder_pick(const char *start, FolderPickDone done, void *ctx);

#ifdef __cplusplus
}
#endif
