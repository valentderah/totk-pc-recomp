// totk-config: the runtime functions cfg.c, ui_text.c and input_map.c call, for a program that is not the game. Log
// lines go to stderr when TOTK_CONFIG_LOG=1 (cfg.c names unknown keys and unparsable lines there).
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <SDL3/SDL.h>

static int log_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_CONFIG_LOG");
        on = e && *e == '1';
    }
    return on;
}

void rt_log(const char *fmt, ...) {
    if (!log_on()) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[config] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

__attribute__((noreturn)) void rt_fatal(const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "totk-config: %s\n", msg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "totk-config", msg, NULL);
    exit(70);
}

// The game's language (ov_system.c) only picks ui_text's language for the software keyboard; the window shows the
// menu language (menu_text).
const char *ov_language(void) { return "en-US"; }

// cfg.c cfg_screen_size asks whether the setup made the prompts folder of a display shape; the window shows the
// settings, not the shape the game would pick, so no folder counts as made.
int romfs_shaped_prompts_ready(const char *folder, unsigned w, unsigned h) {
    (void)folder, (void)w, (void)h;
    return 0;
}
