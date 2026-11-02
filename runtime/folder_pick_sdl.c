// The SDL front end of folder_pick.h: SDL_ShowOpenFolderDialog (the desktop portal, else zenity), started on the window
// thread, which runs SDL's event loop that delivers the portal's answer.
#include <limits.h>
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
