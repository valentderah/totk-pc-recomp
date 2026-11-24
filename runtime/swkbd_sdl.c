// The software keyboard's front end for SDL windows (swkbd.h): a text window of its own, opened on the window thread
// (gfx_window_sdl_run) and fed by that thread's events. The keyboard types, Backspace deletes, Enter is OK and Escape
// cancels. The controller moves a letter grid: D-pad moves the cursor, the bottom button adds the letter, the right
// button deletes, Start is OK and the top button cancels. The guest thread waits for the answer. Without a game window
// it takes the starting text at once.
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cfg.h"
#include "host.h"
#include "input_sdl.h"
#include "platform/platform.h"
#include "swkbd.h"
#include "ui_text.h"

#define GRID_COLS 10
static const char grid_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.";
#define GRID_N ((int)(sizeof grid_chars - 1))

typedef struct {
    const SwkbdRequest *req;
    uint16_t text[KB_LIMIT + 1];
    size_t n;
    int sel;                     // the grid cell the controller's cursor is on
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
    if (b->req->password) {   // one star per character, not per UTF-16 unit
        size_t k = 0;
        for (size_t i = 0; i < b->n && k < KB_LIMIT; i++)
            if (b->text[i] < 0xdc00 || b->text[i] >= 0xe000) text[k++] = '*';
        text[k] = 0;
    } else {
        swkbd_utf16_to_utf8(b->text, b->n, text, sizeof text);
    }
    snprintf(line, sizeof line, "%s_", text);
    SDL_SetRenderDrawColor(b->ren, 24, 24, 32, 255);
    SDL_RenderClear(b->ren);
    SDL_SetRenderScale(b->ren, 4.0f, 4.0f);
    SDL_SetRenderDrawColor(b->ren, 240, 240, 240, 255);
    SDL_RenderDebugText(b->ren, 4, 4, prompt);
    SDL_RenderDebugText(b->ren, 4, 20, line);
    for (int i = 0; i < GRID_N; i++) {
        float x = 4.0f + (float)(i % GRID_COLS) * 14.0f, y = 40.0f + (float)(i / GRID_COLS) * 18.0f;
        char c[2] = {grid_chars[i], 0};
        if (i == b->sel) {
            SDL_FRect cell = {x - 2.0f, y - 2.0f, 12.0f, 12.0f};
            SDL_SetRenderDrawColor(b->ren, 200, 160, 40, 255);
            SDL_RenderFillRect(b->ren, &cell);
            SDL_SetRenderDrawColor(b->ren, 24, 24, 32, 255);
        }
        SDL_RenderDebugText(b->ren, x, y, c);
        SDL_SetRenderDrawColor(b->ren, 240, 240, 240, 255);
    }
    // the debug font draws ASCII: the hints are the catalog's English text whatever the game's language
    SDL_RenderDebugText(b->ren, 4, 116, ui_text_in("en", UI_SWKBD_SDL_KEYS_HINT));
    SDL_RenderDebugText(b->ren, 4, 130, ui_text_in("en", UI_SWKBD_SDL_PAD_HINT1));
    SDL_RenderDebugText(b->ren, 4, 144, ui_text_in("en", UI_SWKBD_SDL_PAD_HINT2));
    SDL_RenderPresent(b->ren);
}

static void close_box(Box *b);

static void open_box(void *arg) {
    Box *b = arg;
    b->win = SDL_CreateWindow(ui_text(UI_SWKBD_TITLE), 1280, 720, 0);
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
    const char *test = cfg_get("TOTK_SWKBD_TEST");
    if (test && !strcmp(test, "ok")) {   // as the Win32 window: OK pressed at once
        b->result = 1;
        close_box(b);
    }
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

// Appends typed UTF-16 within the game's limits: a surrogate pair goes in whole or not at all (as clean in swkbd.c),
// and a number pad takes digits only.
static void add_text(Box *b, const uint16_t *u, size_t k) {
    const SwkbdRequest *r = b->req;
    size_t max = (size_t)r->max_len < KB_LIMIT ? (size_t)r->max_len : KB_LIMIT;
    for (size_t i = 0; i < k; i++) {
        int pair = u[i] >= 0xd800 && u[i] < 0xdc00 && i + 1 < k && u[i + 1] >= 0xdc00 && u[i + 1] < 0xe000;
        if (pair) {
            if (!r->numeric && b->n + 2 <= max) b->text[b->n++] = u[i], b->text[b->n++] = u[i + 1];
            i++;
        } else if (b->n < max && swkbd_allowed(u[i], r->disabled, r->numeric)) {
            b->text[b->n++] = u[i];
        }
    }
}

static void delete_last(Box *b) {
    if (!b->n) return;
    int pair = b->n >= 2 && b->text[b->n - 1] >= 0xdc00 && b->text[b->n - 1] < 0xe000 && b->text[b->n - 2] >= 0xd800 &&
               b->text[b->n - 2] < 0xdc00;
    b->n -= pair ? 2 : 1;
}

static void pad_button(Box *b, Uint8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        if (b->sel % GRID_COLS) b->sel--;
        break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        if (b->sel + 1 < GRID_N && (b->sel + 1) % GRID_COLS) b->sel++;
        break;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        if (b->sel >= GRID_COLS) b->sel -= GRID_COLS;
        break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        if (b->sel + GRID_COLS < GRID_N) b->sel += GRID_COLS;
        break;
    case SDL_GAMEPAD_BUTTON_SOUTH: {
        uint16_t one = (uint16_t)grid_chars[b->sel];
        add_text(b, &one, 1);
        break;
    }
    case SDL_GAMEPAD_BUTTON_EAST:
        delete_last(b);
        break;
    case SDL_GAMEPAD_BUTTON_START:
        if ((int)b->n >= b->req->min_len) b->result = 1;
        break;
    case SDL_GAMEPAD_BUTTON_NORTH:
        b->result = -1;
        break;
    default:
        break;
    }
}

// The window thread's events for the text entry; returns 1 when the event was the text entry's.
int swkbd_sdl_event(const SDL_Event *e) {
    Box *b = g_box;
    if (!b) return 0;
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        pad_button(b, e->gbutton.button);
        if (b->result) close_box(b);
        else draw(b);
        return 1;
    }
    Uint32 id;
    switch (e->type) {
    case SDL_EVENT_TEXT_INPUT: id = e->text.windowID; break;
    case SDL_EVENT_KEY_DOWN: id = e->key.windowID; break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_WINDOW_EXPOSED: id = e->window.windowID; break;
    default: return 0;
    }
    if (id != b->id) return 0;
    const SwkbdRequest *r = b->req;
    if (e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        b->result = -1;
    } else if (e->type == SDL_EVENT_WINDOW_EXPOSED) {
        draw(b);   // the compositor dropped the contents
        return 1;
    } else if (e->type == SDL_EVENT_TEXT_INPUT) {
        uint16_t u[KB_LIMIT + 1];
        size_t k = swkbd_utf8_to_utf16(e->text.text, u, KB_LIMIT + 1);
        add_text(b, u, k);
    } else {
        switch (e->key.scancode) {
        case SDL_SCANCODE_BACKSPACE:
            delete_last(b);
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
