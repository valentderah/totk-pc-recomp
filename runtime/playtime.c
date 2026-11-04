// Play time: how long the player has spent in the game, across runs, shown on the Game tab of the settings menu
// (ui/menu.cpp tab_game). The game itself keeps no play time a player can see, and the Switch's own play log is not
// there to read, so the port counts it.
//
// One host thread (it never holds a virtual core, as discord_rpc.c's) wakes every second and adds the time since its
// last wake when the window has the focus (gfx_window_focused) and the game presented at least one frame meanwhile
// (gfx_stats). So the shader prewarm before the first frame, a window left in the background and a hung game do not
// count; loading screens and the game's own menus do, as they present frames. One wake adds at most TICK_CAP_MS, so a
// machine that slept with the game open adds two seconds, not the night.
//
// The total lives in <profile>/playtime.txt, next to account.uid and the saves, so it stays with them when the
// install is updated or moved. It is plain text, "total_seconds <n>", written atomically (plat_file_write_atomic)
// every SAVE_EVERY_MS while it grows and once more when the game exits (playtime_save from boot.c, svc.c and
// gfx_present.c, next to prof_write); a crash loses at most SAVE_EVERY_MS. A file that does not read as that is kept
// as playtime.txt.bad and the count starts again from zero.
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "platform/platform.h"

#define TICK_MS 1000
#define TICK_CAP_MS 2000
#define SAVE_EVERY_MS 30000
#define TOTAL_CAP_S (100ull * 365 * 24 * 3600)   // a hundred years: anything larger is a damaged file

static PlatMutex lock = PLAT_MUTEX_INIT;   // the totals and the file
static uint64_t total_ms, session_ms, saved_ms;
static int loaded;                         // 1 once the file was read (or found missing), -1 when there is no profile
static char path[1100];

// Holding lock.
static void load(void) {
    if (loaded) return;
    const char *dir = rt_config()->profile;
    if (!dir || !*dir || snprintf(path, sizeof path, "%s/playtime.txt", dir) >= (int)sizeof path) {
        loaded = -1;
        rt_log("playtime: no profile directory; play time is not counted");
        return;
    }
    loaded = 1;
    uint64_t n = 0;
    uint8_t *b = plat_file_read(path, &n);
    if (!b) {
        rt_log("playtime: no %s yet; counting from zero", path);
        return;
    }
    char text[64] = {0};
    memcpy(text, b, n < sizeof text - 1 ? n : sizeof text - 1);
    free(b);
    unsigned long long s = 0;
    char tail = 0;
    if (sscanf(text, "total_seconds %llu%c", &s, &tail) >= 1 && (!tail || tail == '\n' || tail == '\r') &&
        s <= TOTAL_CAP_S) {
        total_ms = saved_ms = (uint64_t)s * 1000;
        rt_log("playtime: %llu h %02llu min played before this run (%s)", s / 3600, s / 60 % 60, path);
        return;
    }
    char bad[1110];
    snprintf(bad, sizeof bad, "%s.bad", path);
    int kept = plat_file_write_atomic(bad, text, strlen(text));
    rt_log("playtime: %s does not read as \"total_seconds <n>\"; %s, counting from zero", path,
           kept ? "kept as playtime.txt.bad" : "could not keep a copy");
}

// Holding lock.
static void save_locked(void) {
    if (loaded != 1 || total_ms / 1000 == saved_ms / 1000) return;
    char text[48];
    int n = snprintf(text, sizeof text, "total_seconds %" PRIu64 "\n", total_ms / 1000);
    if (plat_file_write_atomic(path, text, (size_t)n)) saved_ms = total_ms;
    else rt_log("playtime: cannot write %s", path);
}

void playtime_save(void) {
    plat_mutex_lock(&lock);
    save_locked();
    plat_mutex_unlock(&lock);
}

int playtime_get(uint64_t *total_s, uint64_t *session_s) {
    plat_mutex_lock(&lock);
    int ok = loaded == 1;
    *total_s = total_ms / 1000;
    *session_s = session_ms / 1000;
    plat_mutex_unlock(&lock);
    return ok;
}

static void playtime_main(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    plat_mutex_lock(&lock);
    load();
    int on = loaded == 1;
    plat_mutex_unlock(&lock);
    if (!on) return;
    uint64_t last = plat_now_ms(), last_save = last, frames = gfx_stats().frames;
    for (;;) {
        plat_sleep_ms(TICK_MS);
        uint64_t now = plat_now_ms(), f = gfx_stats().frames, d = now - last;
        int playing = f != frames && gfx_window_focused();
        last = now, frames = f;
        if (d > TICK_CAP_MS) d = TICK_CAP_MS;
        plat_mutex_lock(&lock);
        if (playing) total_ms += d, session_ms += d;
        if (now - last_save >= SAVE_EVERY_MS) save_locked(), last_save = now;
        plat_mutex_unlock(&lock);
    }
}

void playtime_init(void) {
    static int32_t started;
    if (plat_xchg32(&started, 1)) return;
    if (!plat_thread_spawn(playtime_main, NULL, "playtime", 0))
        rt_log("playtime: cannot start its thread; play time is not counted");
}
