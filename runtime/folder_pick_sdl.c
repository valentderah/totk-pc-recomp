// The SDL front end of folder_pick.h: SDL_ShowOpenFolderDialog and SDL_ShowOpenFileDialog (the desktop portal, else
// zenity), started on the window thread, which runs SDL's event loop that delivers the portal's answer.
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "folder_pick.h"
#include "host.h"
#include "input_sdl.h"

typedef struct {
    char start[PATH_MAX];                // absolute, or empty: the dialog's own default
    FolderPickDone done;
    void *ctx;
} PickReq;

static void SDLCALL on_pick(void *userdata, const char *const *list, int filter) {
    (void)filter;
    PickReq *r = userdata;
    if (!list) rt_log("folder_pick: the folder dialog failed (%s)", SDL_GetError());
    r->done(list && list[0] ? list[0] : NULL, r->ctx);
    free(r);
}

static void show(void *arg) {
    PickReq *r = arg;
    SDL_ShowOpenFolderDialog(on_pick, r, gfx_window_sdl(), r->start[0] ? r->start : NULL, false);
}

void folder_pick(const char *start, FolderPickDone done, void *ctx) {
    PickReq *r = calloc(1, sizeof *r);
    if (!r) {
        done(NULL, ctx);
        return;
    }
    r->done = done;
    r->ctx = ctx;
    if (start && *start && !realpath(start, r->start)) r->start[0] = 0;
    gfx_window_sdl_run(show, r);
}

// ---- one file: SDL takes the patterns as one filter string, separated by ';'

typedef struct {
    char start[PATH_MAX];
    char patterns[256];
    FilePickDone done;
    void *ctx;
} FileReq;

static void SDLCALL on_file(void *userdata, const char *const *list, int filter) {
    (void)filter;
    FileReq *r = userdata;
    if (!list) rt_log("file_pick: the file dialog failed (%s)", SDL_GetError());
    r->done(list && list[0] ? list[0] : NULL, r->ctx);
    free(r);
}

static void show_file(void *arg) {
    FileReq *r = arg;
    SDL_DialogFileFilter filters[2] = {{"Supported files", r->patterns[0] ? r->patterns : "*.*"},
                                       {"All files", "*.*"}};
    SDL_ShowOpenFileDialog(on_file, r, gfx_window_sdl(), filters, r->patterns[0] ? 2 : 1,
                           r->start[0] ? r->start : NULL, false);
}

void file_pick(const char *start, const char *const *patterns, int count, FilePickDone done, void *ctx) {
    FileReq *r = calloc(1, sizeof *r);
    if (!r) {
        done(NULL, ctx);
        return;
    }
    r->done = done;
    r->ctx = ctx;
    if (start && *start && !realpath(start, r->start)) r->start[0] = 0;
    size_t n = 0;
    for (int i = 0; i < count && patterns && patterns[i]; i++) {
        int k = snprintf(r->patterns + n, sizeof r->patterns - n, "%s%s", n ? ";" : "", patterns[i]);
        if (k < 0 || (size_t)k >= sizeof r->patterns - n) break;
        n += (size_t)k;
    }
    gfx_window_sdl_run(show_file, r);
}
