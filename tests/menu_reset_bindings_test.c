// Synthetic fixture: production menu reset, config persistence and input tables.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cfg.h"
#include "input_map.h"
#include "platform/platform.h"

static int failures, checks, saves;
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
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
int plat_exe_path(char *out, size_t cap) { (void)out; (void)cap; return 0; }
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
static void opt_save_soon(void) { saves++; }
#include "menu_reset_body.h"

static void env(const char *key, const char *value) {
#ifdef _WIN32
    if (_putenv_s(key, value ? value : "")) exit(2);
#else
    if (value ? setenv(key, value, 1) : unsetenv(key)) exit(2);
#endif
}
static int mapping(int action, int key, int nintendo, int xbox) {
    uint8_t keys[INPUT_BINDING_MAX], pads[INPUT_PAD_BINDING_MAX];
    input_map_reset();
    input_map_configure();
    return input_map_keys_of(&input_map, action, keys, INPUT_BINDING_MAX) == 1 && keys[0] == key &&
        input_map_pads_of(&input_map, action, PAD_LAYOUT_NINTENDO, pads, INPUT_PAD_BINDING_MAX) == 1 &&
        pads[0] == nintendo &&
        input_map_pads_of(&input_map, action, PAD_LAYOUT_XBOX, pads, INPUT_PAD_BINDING_MAX) == 1 && pads[0] == xbox;
}
static void file_is(const char *path, const char *expected, const char *name) {
    uint64_t n;
    uint8_t *data = plat_file_read(path, &n);
    check(data && n == strlen(expected) && !memcmp(data, expected, (size_t)n), name);
    free(data);
}
static const char keep[] = "# keep this comment\r\n[controls]\r\n; bind_a = key:default,pad:default\r\n";
static const char unrelated[] = "custom_note = untouched\r\n";
static void seed(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) exit(2);
    fputs(keep, f);
    fputs("bind_a = key:f,pad:west\r\nbind_b = key:h,pad:north\r\nvolume = 35\r\n", f);
    fputs(unrelated, f);
    if (fclose(f)) exit(2);
    if (!cfg_load(path)) exit(2);
}

int main(void) {
    env("TOTK_DEVICE_PROFILE", "desktop");
    env("TOTK_BIND_A", NULL);
    env("TOTK_BIND_B", NULL);
    env("TOTK_VOLUME", NULL);
    const char *path = "reset.ini";
    seed(path);
    check(mapping(B_A, KEY_F, PAD_WEST, PAD_WEST), "loaded keyboard and both gamepad remaps");
    check(cfg_int("TOTK_VOLUME") == 35, "loaded scalar override");
    reset_all();
    check(saves == 1, "menu schedules persistence");
    check(!cfg_get("TOTK_BIND_A") && !cfg_get("TOTK_BIND_B"), "all binding overrides removed in memory");
    check(mapping(B_A, KEY_E, PAD_EAST, PAD_SOUTH), "A defaults restored for keyboard and both layouts");
    check(cfg_int("TOTK_VOLUME") == 100, "scalar default restored");
    check(cfg_save_to(path), "save reset settings");
    char expected[1024];
    snprintf(expected, sizeof expected, "%s%s", keep, unrelated);
    file_is(path, expected, "disk removes overrides and preserves comment and unrelated bytes");
    check(cfg_load(path), "reload saved reset");
    check(mapping(B_A, KEY_E, PAD_EAST, PAD_SOUTH) && cfg_int("TOTK_VOLUME") == 100,
          "defaults persist after reload");
    reset_all();
    check(cfg_save_to(path), "repeat reset saves successfully");
    file_is(path, expected, "repeat reset is stable on disk");

    seed(path);
    env("TOTK_BIND_B", "key:j,pad:east");
    env("TOTK_VOLUME", "80");
    check(mapping(B_B, KEY_J, PAD_EAST, PAD_EAST), "environment overrides B keyboard and gamepad");
    reset_all();
    check(!cfg_get("TOTK_BIND_A") && mapping(B_A, KEY_E, PAD_EAST, PAD_SOUTH), "unprotected A resets beside env B");
    check(mapping(B_B, KEY_J, PAD_EAST, PAD_EAST) && cfg_int("TOTK_VOLUME") == 80,
          "environment binding and scalar remain effective");
    check(cfg_save_to(path), "save with environment overrides");
    snprintf(expected, sizeof expected, "%sbind_b = key:h,pad:north\r\nvolume = 35\r\n%s", keep, unrelated);
    file_is(path, expected, "environment protected file values and unrelated bytes survive");
    check(cfg_load(path) && mapping(B_B, KEY_J, PAD_EAST, PAD_EAST), "environment still wins after reload");
    env("TOTK_BIND_B", NULL);
    env("TOTK_VOLUME", NULL);
    check(mapping(B_B, KEY_H, PAD_NORTH, PAD_NORTH) && cfg_int("TOTK_VOLUME") == 35,
          "protected file values return when environment clears");
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
