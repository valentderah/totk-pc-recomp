// Stubs for what runtime/cfg.c touches, so the settings table can be linked and read without a game.
// Built and run by tests/blueprint_hint_default_test.py.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }

// The environment the test sets and clears.
#define ENV_MAX 64
static char *env_key[ENV_MAX], *env_val[ENV_MAX];
static int env_n;

size_t plat_getenv(const char *name, char *out, size_t cap) {
    for (int i = 0; i < env_n; i++) {
        if (!env_key[i] || strcmp(env_key[i], name)) continue;
        const size_t n = strlen(env_val[i]);
        if (out && cap) {
            const size_t c = n < cap - 1 ? n : cap - 1;
            memcpy(out, env_val[i], c);
            out[c] = 0;
        }
        return n;
    }
    if (out && cap) out[0] = 0;
    return 0;
}

int plat_setenv(const char *name, const char *value) {
    for (int i = 0; i < env_n; i++) {
        if (!env_key[i] || strcmp(env_key[i], name)) continue;
        if (!value) {
            free(env_key[i]); free(env_val[i]);
            env_key[i] = env_val[i] = NULL;
            return 1;
        }
        free(env_val[i]);
        env_val[i] = strdup(value);
        return 1;
    }
    if (!value || env_n >= ENV_MAX) return 0;
    env_key[env_n] = strdup(name);
    env_val[env_n] = strdup(value);
    env_n++;
    return 1;
}

// No file: cfg_load leaves the table alone, which is the unset case the test is about.
uint8_t *plat_file_read(const char *path, uint64_t *size) { (void)path; if (size) *size = 0; return NULL; }
int plat_fs_kind(const char *path) { (void)path; return 0; }
uint64_t plat_now_ns(void) { return 0; }
// ---- the rest of the platform surface cfg.c references, stubbed to no-ops ----
static PlatMutex g_mu;
void plat_mutex_init(PlatMutex *m) { (void)m; }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
int plat_mutex_trylock(PlatMutex *m) { (void)m; return 1; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
void plat_mutex_lock_shared(PlatMutex *m) { (void)m; }
int plat_mutex_trylock_shared(PlatMutex *m) { (void)m; return 1; }
void plat_mutex_unlock_shared(PlatMutex *m) { (void)m; }

int plat_file_exists(const char *path) { (void)path; return 0; }
int64_t plat_file_size(const char *path) { (void)path; return -1; }
int plat_file_write_atomic(const char *path, const void *data, uint64_t n) { (void)path; (void)data; (void)n; return 0; }
int plat_exe_path(char *out, size_t cap) { if (out && cap) out[0] = 0; return 0; }
int plat_screen_size(int *w, int *h) { if (w) *w = 0; if (h) *h = 0; return 0; }

// cfg.c asks whether the install's own HUD shape is ready; the test has no install.
int romfs_shaped_prompts_ready(const char *folder, unsigned w, unsigned h) {
    (void)folder; (void)w; (void)h;
    return 0;
}
// Upstream's prompt-folder lookup: cfg.c asks which prompts folder the settings select.
int input_prompts_pick(const char *prompts, const char *pad_layout) {
    (void)prompts; (void)pad_layout;
    return 0;
}

const char *input_prompts_folder(int prompts, const char *pad_layout) {
    (void)prompts; (void)pad_layout;
    return NULL;
}
