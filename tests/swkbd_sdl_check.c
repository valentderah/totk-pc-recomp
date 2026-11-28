// Exercise the real SDL text window and event handlers without a running game.
// Link with SDL3 and compile with -I runtime -ffunction-sections -Wl,--gc-sections.
#include <assert.h>
#include <stdio.h>
#include "host.h"
#undef HOST_OVERRIDE
#define HOST_OVERRIDE(sym, fn)
#include "swkbd.c"
#include "swkbd_sdl.c"

static const char *actions;
static int failures, checks;
const char *cfg_get(const char *key) { (void)key; return NULL; }
void rt_log(const char *fmt, ...) { (void)fmt; }
const char *ui_text(UiTextId id) { (void)id; return "Text entry"; }
const char *ui_text_in(const char *lang, UiTextId id) { (void)lang; return ui_text(id); }
void plat_event_init(PlatEvent *e, int manual, int set) { e->manual = manual; e->set = set; }
void plat_event_set(PlatEvent *e) { e->set = 1; }
void plat_event_wait(PlatEvent *e) { assert(e->set); }
SDL_Window *gfx_window_sdl(void) { return (SDL_Window *)(uintptr_t)1; }

void gfx_window_sdl_run(void (*fn)(void *), void *arg) {
    fn(arg);
    assert(g_box && g_box->win && g_box->ren);
    // Each command goes through the backend's event entry point for the real window.
    for (const char *p = actions; *p && g_box; p++) {
        SDL_Event e = {0};
        switch (*p) {
        case 't':
            e.type = SDL_EVENT_TEXT_INPUT;
            e.text.windowID = g_box->id;
            e.text.text = p + 1;
            swkbd_sdl_event(&e);
            e = (SDL_Event){0};
            e.type = SDL_EVENT_KEY_DOWN;
            e.key.windowID = g_box->id;
            e.key.scancode = SDL_SCANCODE_RETURN;
            swkbd_sdl_event(&e);
            return;
        case 'v': e.type = SDL_EVENT_TEXT_INPUT; e.text.text = "Z"; break;
        case 'w': e.type = SDL_EVENT_TEXT_INPUT; e.text.text = "elda"; break;
        case 'u': e.type = SDL_EVENT_TEXT_INPUT; e.text.text = "\xf0\x9f\x90\xb4"; break;
        case 'b': e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_BACKSPACE; break;
        case 'a': e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; e.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH; break;
        case 'd': e.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN; e.gbutton.button = SDL_GAMEPAD_BUTTON_EAST; break;
        case 'o': e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_RETURN; break;
        case 'x': e.type = SDL_EVENT_KEY_DOWN; e.key.scancode = SDL_SCANCODE_ESCAPE; break;
        default: assert(0);
        }
        if (e.type == SDL_EVENT_KEY_DOWN) e.key.windowID = g_box->id;
        if (e.type == SDL_EVENT_TEXT_INPUT) e.text.windowID = g_box->id;
        swkbd_sdl_event(&e);
    }
}

static void check(const char *name, const char *initial, const char *input, const char *expected,
                  int max, int numeric, uint32_t disabled, int confirmed) {
    SwkbdRequest r = {.max_len = max, .min_len = 0, .numeric = numeric, .disabled = disabled};
    size_t k = swkbd_utf8_to_utf16(initial, r.initial, KB_LIMIT);
    r.initial[k] = 0;
    uint16_t out[KB_LIMIT + 1];
    size_t n = 0;
    actions = input;
    // Text events do not confirm themselves: the fixture closes through Enter after dispatch.
    int result = text_entry_run(&r, out, KB_LIMIT, &n);
    char got[KB_LIMIT * 3 + 1];
    swkbd_utf16_to_utf8(out, n, got, sizeof got);
    checks++;
    if (result != confirmed || strcmp(got, expected)) {
        failures++;
        printf("FAIL %s: confirmed=%d text='%s', expected confirmed=%d text='%s'\n",
               name, result, got, confirmed, expected);
    }
}

int main(void) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    check("confirm initial", "Horse", "o", "Horse", 9, 0, 0, 1);
    check("replace by typing", "Horse", "tZelda", "Zelda", 9, 0, 0, 1);
    check("replace full initial", "123456789", "tEpona", "Epona", 9, 0, 0, 1);
    check("delete selection", "Horse", "bo", "", 9, 0, 0, 1);
    check("controller replacement", "Horse", "ao", "A", 9, 0, 0, 1);
    check("controller deletion", "Horse", "dao", "A", 9, 0, 0, 1);
    check("rejected text preserves selection", "Horse", "t@", "Horse", 9, 0, 1u << 2, 1);
    check("numeric replacement", "123", "ta4", "4", 9, 1, 0, 1);
    check("Unicode replacement", "Horse", "t\xf0\x9f\x90\xb4", "\xf0\x9f\x90\xb4", 9, 0, 0, 1);
    check("later text appends", "Horse", "vwo", "Zelda", 9, 0, 0, 1);
    check("delete Unicode replacement", "Horse", "ubo", "", 9, 0, 0, 1);
    check("pair exceeding limit keeps selection", "A", "uvo", "Z", 1, 0, 0, 1);
    check("empty initial", "", "tZelda", "Zelda", 9, 0, 0, 1);
    check("rejected controller letter", "123", "ao", "123", 9, 1, 0, 1);
    check("cancel initial", "Horse", "x", "", 9, 0, 0, 0);
    SDL_Quit();
    printf("swkbd SDL: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}