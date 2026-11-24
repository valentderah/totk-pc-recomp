// The software keyboard's front end for SDL windows (swkbd.h): a small text window of its own, opened on the window
// thread (gfx_window_sdl_run) and fed by that thread's events: typed text, Backspace, Enter (OK), Escape or closing the
// window (Cancel). The guest thread waits for the answer. Without a game window it takes the starting text at once.
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cfg.h"
#include "host.h"
#include "input_sdl.h"
#include "platform/platform.h"
#include "swkbd.h"
#include "ui_text.h"

typedef struct {
    const SwkbdRequest *req;
    uint16_t text[KB_LIMIT + 1];
    size_t n;
    int result;                  // 1 OK, -1 Cancel, 0 still open
    int open;
    SDL_Window *win;
    SDL_Renderer *ren;
    Uint32 id;
    PlatEvent done;
} Box;

static Box *g_box;               // one text entry at a time (swkbd.c holds the lock)

static void draw(const Box *b) {
    char prompt[0x101 * 3 + 1], text[KB_LIMIT * 3 + 2], line[KB_LIMIT * 3 + 2];
    size_t pl = 0;
    while (pl < 0x100 && b->req->prompt[pl]) pl++;
    if (pl) swkbd_utf16_to_utf8(b->req->prompt, pl, prompt, sizeof prompt);
    else snprintf(prompt, sizeof prompt, "%s", ui_text(UI_SWKBD_PROMPT));
    swkbd_utf16_to_utf8(b->text, b->n, text, sizeof text);
    snprintf(line, sizeof line, "%s_", text);
    SDL_SetRenderDrawColor(b->ren, 24, 24, 32, 255);
    SDL_RenderClear(b->ren);
    SDL_SetRenderDrawColor(b->ren, 240, 240, 240, 255);
    SDL_SetRenderScale(b->ren, 3.0f, 3.0f);
    SDL_RenderDebugText(b->ren, 4, 4, prompt);
    SDL_RenderDebugText(b->ren, 4, 28, line);
    SDL_RenderDebugText(b->ren, 4, 56, "Enter = OK   Esc = Cancel");
    SDL_RenderPresent(b->ren);
}

static void open_box(void *arg) {
    Box *b = arg;
    b->win = SDL_CreateWindow(ui_text(UI_SWKBD_TITLE), 720, 240, 0);
    if (!b->win) {
        rt_log("swkbd: cannot open the text entry window (%s)", SDL_GetError());
        return;
    }
    b->ren = SDL_CreateRenderer(b->win, NULL);
    if (!b->ren) {
        rt_log("swkbd: no renderer for the text entry (%s)", SDL_GetError());
        SDL_DestroyWindow(b->win);
        b->win = NULL;
        return;
    }
    b->id = SDL_GetWindowID(b->win);
    b->open = 1;
    g_box = b;
    SDL_StartTextInput(b->win);
    SDL_RaiseWindow(b->win);
    draw(b);
}

static void close_box(Box *b) {
    SDL_StopTextInput(b->win);
    SDL_DestroyRenderer(b->ren);
    SDL_DestroyWindow(b->win);
    b->win = NULL;
    b->ren = NULL;
    g_box = NULL;
    plat_event_set(&b->done);
}

// The window thread's events for the text entry window; returns 1 when the event was the text entry's.
int swkbd_sdl_event(const SDL_Event *e) {
    Box *b = g_box;
    if (!b) return 0;
    Uint32 id;
    switch (e->type) {
    case SDL_EVENT_TEXT_INPUT: id = e->text.windowID; break;
    case SDL_EVENT_KEY_DOWN: id = e->key.windowID; break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: id = e->window.windowID; break;
    default: return 0;
    }
    if (id != b->id) return 0;
    const SwkbdRequest *r = b->req;
    if (e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        b->result = -1;
    } else if (e->type == SDL_EVENT_TEXT_INPUT) {
        uint16_t u[KB_LIMIT + 1];
        size_t k = swkbd_utf8_to_utf16(e->text.text, u, KB_LIMIT + 1);
        for (size_t i = 0; i < k && b->n < (size_t)r->max_len && b->n < KB_LIMIT; i++) {
            int surrogate = u[i] >= 0xd800 && u[i] < 0xe000;
            if (surrogate || swkbd_allowed(u[i], r->disabled, r->numeric)) b->text[b->n++] = u[i];
        }
    } else {
        switch (e->key.scancode) {
        case SDL_SCANCODE_BACKSPACE:
            if (b->n) b->n--;
            break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
            if ((int)b->n >= r->min_len) b->result = 1;
            break;
        case SDL_SCANCODE_ESCAPE:
            b->result = -1;
            break;
        default:
            break;
        }
    }
    if (b->result) close_box(b);
    else draw(b);
    return 1;
}

int text_entry_run(const SwkbdRequest *req, uint16_t *out, size_t cap, size_t *n) {
    const char *test = cfg_get("TOTK_SWKBD_TEST");
    if (test && !strcmp(test, "cancel")) { *n = 0; return 0; }
    Box b = {.req = req};
    size_t k = 0;
    while (k < KB_LIMIT && req->initial[k]) k++;
    memcpy(b.text, req->initial, k * 2);
    b.n = k;
    if (gfx_window_sdl()) {
        plat_event_init(&b.done, 1, 0);
        gfx_window_sdl_run(open_box, &b);
    }
    if (!b.open) {
        rt_log("swkbd: no text entry window on this host: the %s name is taken", req->game_initial ? "game's" : "default");
        size_t f = b.n < cap ? b.n : cap;
        memcpy(out, b.text, f * 2);
        *n = f;
        return f > 0;
    }
    plat_event_wait(&b.done);
    if (b.result != 1) { *n = 0; return 0; }
    *n = b.n < cap ? b.n : cap;
    memcpy(out, b.text, *n * 2);
    return 1;
}
