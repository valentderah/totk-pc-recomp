// The folder a blueprint file dialog last browsed (blueprint_dir.h): one value for the process, kept in totk.ini so that
// the next run opens the dialog there too.
//
// The value is cached here rather than read from cfg on every dialog because the dialog asks on each open while the
// setting changes only when a file is chosen. The cache follows cfg_version, like the runtime's other live settings, so
// a value written by anything else (a hand-edited totk.ini reloaded by a test) is picked up as well.
#include <stdio.h>
#include <string.h>

#include "blueprint_dir.h"
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

// The totk.ini key, under [files] next to the other file settings.
#define DIR_KEY "TOTK_BLUEPRINT_DIR"

// last: the folder a dialog opens at, "" when none is remembered. seen: the cfg_version last was read at. A caller
// copies the answer before it opens the dialog, because a pick on another thread may replace it meanwhile.
static char last[PLAT_PATH_MAX];
static unsigned seen = ~0u;
static int read_done;                       // the first read has logged its one line
static PlatMutex lock = PLAT_MUTEX_INIT;    // guards the three fields above

// Reads the setting into last (lock held). A value that is no longer a directory counts as none: a folder deleted
// between two runs must not leave the dialog waiting at a path that can no longer open.
static void refresh(void) {
    const unsigned version = cfg_version();
    if (version == seen) return;
    seen = version;
    const char *set = cfg_get(DIR_KEY);
    char dir[PLAT_PATH_MAX];
    const int usable = set && *set && strlen(set) < sizeof dir && plat_dir_exists(set);
    snprintf(dir, sizeof dir, "%s", usable ? set : "");
    if (read_done && !strcmp(dir, last)) return;
    const int first = !read_done;
    read_done = 1;
    snprintf(last, sizeof last, "%s", dir);
    if (first && last[0]) rt_log("blueprint: the file dialog opens at %s (the folder browsed last)", last);
    else if (first) rt_log("blueprint: the file dialog opens at the shell's own folder, no folder is remembered");
    else if (last[0]) rt_log("blueprint: the remembered folder changed, the file dialog opens at %s", last);
    else rt_log("blueprint: the remembered folder is gone, the file dialog opens at the shell's own folder");
}

const char *bp_dir_last(void) {
    static _Thread_local char out[PLAT_PATH_MAX];   // this thread's own copy of the answer
    plat_mutex_lock(&lock);
    refresh();
    // A folder removed while the game runs is no folder to open a dialog at, so it is checked once per dialog rather
    // than once per read of the setting.
    snprintf(out, sizeof out, "%s", last[0] && plat_dir_exists(last) ? last : "");
    plat_mutex_unlock(&lock);
    return out;   // never NULL; valid for the calling thread until it calls bp_dir_last again
}

// The directory part of path into out: everything before the last '/' or '\'. A path with no separator at all, or
// one whose only separator comes first (the directory root of "/file" or "\file"), has no folder to remember: out is "".
static void dir_of(const char *path, char *out, size_t cap) {
    const char *sep = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') sep = p;
    const size_t n = sep ? (size_t)(sep - path) : 0;
    if (n == 0 || n >= cap) {
        out[0] = 0;
        return;
    }
    memcpy(out, path, n);
    out[n] = 0;
}

void bp_dir_remember(const char *path) {
    if (!path || !*path) return;
    char dir[PLAT_PATH_MAX];
    dir_of(path, dir, sizeof dir);
    // A whole file path is expected here. A path without a folder, or whose folder is already gone, says nothing about
    // where the next dialog should open, so the remembered folder stays as it was.
    if (!dir[0] || !plat_dir_exists(dir)) return;
    plat_mutex_lock(&lock);
    refresh();   // the folder before this pick is what the change is measured against
    const int changed = strcmp(dir, last) != 0;
    snprintf(last, sizeof last, "%s", dir);
    cfg_set(DIR_KEY, dir);
    cfg_save();   // to totk.ini, so the folder survives a restart
    // seen is left alone on purpose: the next read of the setting goes through cfg_get again, so an environment value
    // that overrides totk.ini keeps winning, as it does for every other setting.
    plat_mutex_unlock(&lock);
    if (changed) rt_log("blueprint: the next file dialog opens at %s", dir);
}
