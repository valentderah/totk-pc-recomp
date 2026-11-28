// Synthetic fixture: production menu debounce, config persistence and normal shutdown.
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cfg.h"
#include "input_map.h"
#include "platform/platform.h"

static int failures, checks, writes, fail_write;
static void check(int ok, const char *name) {
    checks++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}

// Single-threaded adapters. cfg.c itself parses, resets, formats and reloads files.
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
uint8_t *plat_file_read(const char *path, uint64_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    uint8_t *data = malloc((size_t)n + 1);
    if (!data) { fclose(f); return NULL; }
    if (fread(data, 1, (size_t)n, f) != (size_t)n) { free(data); fclose(f); return NULL; }
    fclose(f);
    data[n] = 0;
    *size = (uint64_t)n;
    return data;
}
int plat_file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}
int64_t plat_file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int ok = !fseek(f, 0, SEEK_END);
    long n = ok ? ftell(f) : -1;
    fclose(f);
    return n;
}
int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    writes++;
    if (fail_write) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
int plat_exe_path(char *out, size_t cap) { return snprintf(out, cap, "./fixture.exe") < (int)cap; }
int plat_screen_size(int *w, int *h) { (void)w; (void)h; return 0; }
int romfs_shaped_prompts_ready(const char *folder, unsigned w, unsigned h) {
    (void)folder; (void)w; (void)h; return 0;
}
void rt_log(const char *fmt, ...) { (void)fmt; }
__attribute__((noreturn)) void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(2);
}

static uint64_t now = 1000000000ull;
uint64_t plat_now_ns(void) { return now; }
void opt_save_soon(void);
#include "settings_rows_body.h"

static volatile int32_t g_restart;
static int flushes, pipelines, playtimes, profiles, restarts, cleared_reexec;
static int expect_pending, expect_writes, expect_failure;
void fs_failed_summary(void) {}
void fs_save_flush(void) { flushes++; }
void vk_pipeline_save(void) { pipelines++; }
void playtime_save(void) { playtimes++; }
void prof_write(void) { profiles++; }
int plat_setenv(const char *name, const char *value) {
    check(!strcmp(name, "TOTK_REEXEC") && !value, "restart clears reexec setting");
    cleared_reexec++;
    return 1;
}
int plat_restart(void) {
    restarts++;
    uint64_t n;
    uint8_t *data = plat_file_read("totk.ini", &n);
    FILE *f = fopen("restart.ini", "wb");
    if (!data || !f) exit(2);
    int ok = fwrite(data, 1, (size_t)n, f) == n;
    free(data);
    if (fclose(f) || !ok) exit(2);
    f = fopen("restart.marker", "wb");
    if (!f || fclose(f)) exit(2);
    return 1;
}
_Noreturn void plat_exit(int code) {
    check(flushes == 1 && pipelines == 1 && playtimes == 1 && profiles == 1, "other shutdown saves retained");
    check(restarts == !!g_restart && cleared_reexec == !!g_restart, "restart branch retained");
    check(writes == expect_writes, "only dirty configuration writes at shutdown");
    check(cfg_last_save_ok() == !expect_failure, "configuration write status retained");
    check((g_save_at != 0) == expect_pending, "shutdown leaves UI scheduler state owned by UI");
    printf("child: %d checks, %d failures\n", checks, failures);
    fflush(stdout);
    _Exit(code || failures ? 1 : 0);
}
#include "settings_close_body.h"

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    const char *mode = argv[1];
    g_restart = atoi(argv[2]);
    if (!cfg_load("totk.ini")) return 2;
    check(cfg_int("TOTK_VOLUME") == 35, "loaded scalar before edit");
    if (!strcmp(mode, "clean")) {
        expect_pending = 0;
        expect_writes = 0;
    } else {
        if (!strcmp(mode, "binding")) {
            apply("TOTK_BIND_A", "key:g,pad:north");
            input_map_configure();
            uint8_t key[INPUT_BINDING_MAX], pad[INPUT_PAD_BINDING_MAX];
            check(input_map_keys_of(&input_map, B_A, key, INPUT_BINDING_MAX) == 1 && key[0] == KEY_G,
                  "binding edit applies keyboard before save");
            check(input_map_pads_of(&input_map, B_A, PAD_LAYOUT_NINTENDO, pad, INPUT_PAD_BINDING_MAX) == 1 &&
                  pad[0] == PAD_NORTH, "binding edit applies gamepad before save");
        } else if (!strcmp(mode, "reset")) {
            apply("TOTK_VOLUME", "");
            check(cfg_int("TOTK_VOLUME") == 100, "reset applies in memory before save");
        } else {
            apply("TOTK_VOLUME", "62");
            check(cfg_int("TOTK_VOLUME") == 62, "scalar applies in memory before save");
        }
        // A rendered frame 100 ms after the edit has not reached the 400 ms deadline.
        now += 100000000ull;
        check(opt_save_tick(0) == 1 && writes == 0, "pending debounce has not written yet");
        expect_pending = 1;
        expect_writes = 1;
        if (!strcmp(mode, "elapsed")) {
            now += 400000000ull;
            check(opt_save_tick(0) == 0 && writes == 1, "elapsed debounce already persisted edit");
            expect_pending = 0;
        } else if (!strcmp(mode, "failed")) {
            fail_write = expect_failure = 1;
        }
    }
    gfx_window_closed();
    return 2;
}
