// The folder a file dialog last browsed, so the next dialog opens there instead of at the shell's default. One value
// for the whole process, kept in totk.ini under [files] as BLUEPRINT_DIR, so it survives a restart and follows the
// player between the Blueprints tab and the Autobuild screen's own import entry.
//
// The value is the directory of the file that was chosen, not the file: a dialog opens a folder, and reopening it at the
// file's own folder is what makes the next pick one click away.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// The folder to open a dialog at: the saved one when it still exists, else "" (the dialog's own default). Never NULL.
const char *bp_dir_last(void);

// Remembers the folder of path (a full path to a file) for the next dialog, and writes it to totk.ini. A path with no
// separator in it, or one whose folder no longer exists, is ignored. Safe from any thread.
void bp_dir_remember(const char *path);

#ifdef __cplusplus
}
#endif
