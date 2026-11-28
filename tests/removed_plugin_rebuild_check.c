// Controlled pending-build states with the actual menu predicate and retry helper; no game data.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cheats.h"
#include "platform/platform.h"
#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

typedef struct { CheatInfo info; } FixtureCheat;
static FixtureCheat cheat_storage[2], *g_cheats = cheat_storage;
static int g_n, pending_patches, removed_plugins;
static char g_dir[PLAT_PATH_MAX];
int mods_exefs_pending(uint32_t *key) { *key ^= (uint32_t)pending_patches; return pending_patches; }
int ldr_plugins_gone(uint32_t *key) { *key ^= (uint32_t)removed_plugins; return removed_plugins; }
int plat_file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file);
    return 1;
}
int plat_fs_unlink(const char *path) { return remove(path) ? PLAT_FS_ERROR : PLAT_FS_OK; }

#include "rebuild_functions.c"

static int checks, failures;
#define CHECK(condition, name) do { \
    checks++; \
    if (!(condition)) { failures++; printf("FAIL removed plugin rebuild: %s\n", name); } \
} while (0)

static int write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    size_t n = strlen(text);
    int ok = fwrite(text, 1, n, file) == n;
    return fclose(file) == 0 && ok;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (snprintf(g_dir, sizeof g_dir, "%s/", argv[1]) >= (int)sizeof g_dir) return 2;
    char command[PLAT_PATH_MAX + 64], skip[PLAT_PATH_MAX + 64];
    snprintf(command, sizeof command, "%sbuild-cache" SEP "rebuild.txt", g_dir);
    snprintf(skip, sizeof skip, "%sbuild-cache" SEP "rebuild.skip", g_dir);
    if (!write_file(command, "synthetic rebuild command\n")) return 2;

    removed_plugins = 1;
    CHECK(cheats_rebuild_pending(), "removed plugin alone keeps the manual rebuild available");
    CHECK(write_file(skip, "12345678") && cheats_rebuild_pending(),
          "a previous failed or cancelled rebuild does not hide the manual retry");
    cheats_rebuild_allow();
    CHECK(!plat_file_exists(skip) && plat_file_exists(command), "manual retry removes only rebuild.skip");
    CHECK(cheats_rebuild_pending(), "pending removal remains visible after retry is allowed");

    removed_plugins = 0;
    CHECK(!cheats_rebuild_pending(), "a clean build has no rebuild button");
    pending_patches = 1;
    CHECK(cheats_rebuild_pending(), "pending ExeFS patches still expose the rebuild button");
    pending_patches = 0;
    g_n = 1;
    g_cheats[0].info.kind = CHEAT_REBUILD;
    CHECK(cheats_rebuild_pending(), "a pending code cheat still exposes the rebuild button");
    g_cheats[0].info.kind = CHEAT_DATA;
    CHECK(!cheats_rebuild_pending(), "an unrelated data cheat does not require a rebuild");
    g_n = 0;
    removed_plugins = 1;
    if (remove(command)) return 2;
    CHECK(!cheats_rebuild_pending(), "a missing rebuild command leaves the button unavailable");
    if (!write_file(command, "synthetic rebuild command\n")) return 2;
    g_dir[0] = 0;
    CHECK(!cheats_rebuild_pending(), "no initialized executable directory leaves the button unavailable");

    printf("removed plugin rebuild: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}