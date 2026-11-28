// The loading window's state (loadwin.h): the stages, their counted work and its rate, the log text for the Details
// view and the length of the recent starts. The window itself is loadwin_win.c; without it (other OSes) the panel never
// opens and every call returns at once.
#include "loadwin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

volatile int lw_taking_log;

// The panel has a window system on Windows only (loadwin_win.c). Elsewhere lw_start returns before it buffers or logs
// anything, so a start there is the same as without this file.
#ifdef _WIN32
#define LW_HAS_PANEL 1
#else
#define LW_HAS_PANEL 0
#endif

#define RATE_SAMPLES 48
#define LOG_TEXT_CAP (16u << 20)   // text waiting for the Details view; a trace-heavy start drops the rest

typedef struct {
    int state;                      // 0 not begun, 1 running, 2 ended
    double begun, ended;            // seconds since process start
    uint64_t total, done;           // counted work; done is the last value read
    uint64_t (*read)(void *ctx);    // NULL once the count ended
    void *ctx;
    double t[RATE_SAMPLES];         // (time, done) samples, oldest at head
    uint64_t d[RATE_SAMPLES];
    int head, n;
    double left, last;              // the smoothed time left (negative: unknown), the time it was last updated
} Stage;

static struct {
    PlatMutex lock;                 // all below
    volatile int open;              // the panel's thread runs
    int player;                     // a player's run (no --seconds): lw_fatal keeps the panel open
    int failed;
    char why[512];
    Stage st[LW_STAGES];
    char *text;                     // log text not yet taken by the panel
    size_t text_n, text_cap;
    int text_cut;
    double expect_s;                // the recent starts' length to the game window, 0 when unknown
    double was_b[LW_STAGES], was_e[LW_STAGES];   // when each stage began and ended in them, 0 0: unknown
    double view_t;                  // when lw_view last ran
    double shown_s;                 // this start's, set when the game window opened
    double overall;                 // the last share of the start shown (lw_view)
    char time_path[PLAT_PATH_MAX * 3];
} L = {.lock = PLAT_MUTEX_INIT};

static double now_s(void) { return (double)plat_process_uptime_ns() / 1e9; }

// The start's length lives in <profile>/loading-time.txt: "start_ms <n>", then "stages_ms" and each stage's begin and
// end (0 0 for a stage that did not run), each the mean of the last start's and the one kept before (lw_closed).
// Missing or unreadable: no estimate; no stages line: the length alone.
static void expect_read(void) {
    const char *prof = rt_config()->profile;
    if (!prof || snprintf(L.time_path, sizeof L.time_path, "%s/loading-time.txt", prof) >= (int)sizeof L.time_path) {
        L.time_path[0] = 0;
        return;
    }
    uint64_t size = 0;
    uint8_t *p = plat_file_read(L.time_path, &size);
    if (!p) return;
    char buf[256];
    size_t n = size < sizeof buf - 1 ? (size_t)size : sizeof buf - 1;
    memcpy(buf, p, n);
    buf[n] = 0;
    free(p);
    unsigned long long ms = 0;
    if (sscanf(buf, "start_ms %llu", &ms) != 1 || !ms || ms >= 3600000ull) return;
    L.expect_s = (double)ms / 1000.0;
    const char *at = strstr(buf, "stages_ms");
    if (!at) return;
    at += 9;
    double b[LW_STAGES], e[LW_STAGES];
    for (int i = 0; i < LW_STAGES; i++) {
        char *end;
        unsigned long long u = strtoull(at, &end, 10);
        if (end == at) return;   // cut or wrong: the length alone
        at = end;
        unsigned long long w = strtoull(at, &end, 10);
        if (end == at || w < u || w > ms) return;
        at = end;
        b[i] = (double)u / 1000.0, e[i] = (double)w / 1000.0;
    }
    memcpy(L.was_b, b, sizeof b), memcpy(L.was_e, e, sizeof e);
}

static void expect_write(double s, const double *b, const double *e) {
    if (!L.time_path[0] || s <= 0) return;
    char buf[256];
    int n = snprintf(buf, sizeof buf, "start_ms %llu\nstages_ms", (unsigned long long)(s * 1000.0));
    for (int i = 0; i < LW_STAGES; i++)
        n += snprintf(buf + n, sizeof buf - n, " %llu %llu", (unsigned long long)(b[i] * 1000.0),
                      (unsigned long long)(e[i] * 1000.0));
    n += snprintf(buf + n, sizeof buf - n, "\n");
    if (!plat_file_write_atomic(L.time_path, buf, (size_t)n)) rt_log("loadwin: cannot write %s", L.time_path);
}

static void text_add(const char *p, size_t n) {
    if (L.text_n + n > L.text_cap) {
        size_t cap = L.text_cap ? L.text_cap : 65536;
        while (cap < L.text_n + n) cap *= 2;
        if (cap > LOG_TEXT_CAP) cap = LOG_TEXT_CAP;
        char *t = L.text_n + n <= cap ? realloc(L.text, cap) : NULL;
        if (!t) {
            L.text_cut = 1;
            return;
        }
        L.text = t, L.text_cap = cap;
    }
    memcpy(L.text + L.text_n, p, n);
    L.text_n += n;
}

// A run with no loading window keeps its console, as before totk.manifest's consoleAllocationPolicy: a run that started
// without one gets one, with the lines logged so far, which went nowhere.
static void console_back(const char *early, size_t early_n) {
    if (lw_wsys_want_console() && early_n) {
        fwrite(early, 1, early_n, stderr);
        fflush(stderr);
    }
}

void lw_start(const char *early, size_t early_n) {
    const char *gpu = rt_config()->gpu;
    if (!LW_HAS_PANEL || cfg_bool("TOTK_CONSOLE") || (gpu && !strcmp(gpu, "null"))) {
        console_back(early, early_n);
        return;
    }
    L.player = rt_config()->seconds <= 0;
    expect_read();
    plat_mutex_lock(&L.lock);
    text_add(early, early_n);
    plat_mutex_unlock(&L.lock);
    lw_taking_log = 1;
    const char *bg = cfg_get("TOTK_BACKGROUND_WINDOW");
    L.st[LW_START] = (Stage){.state = 1, .begun = now_s(), .left = -1};
    if (!lw_wsys_open(bg && atoi(bg))) {
        lw_taking_log = 0;
        plat_mutex_lock(&L.lock);
        free(L.text);
        L.text = NULL, L.text_n = L.text_cap = 0;
        plat_mutex_unlock(&L.lock);
        console_back(early, early_n);
        rt_log("loadwin: the loading window did not open; the console stays");
        return;
    }
    L.open = 1;
    if (lw_wsys_detach_console())
        rt_log("loadwin: the console window is closed; the log is in the loading window's Details and in the log file "
               "(console = 1 keeps the console)");
}

void lw_begin(LwStage s) {
    if (!L.open) return;
    plat_mutex_lock(&L.lock);
    if (L.st[s].state != 1) L.st[s] = (Stage){.state = 1, .begun = now_s(), .left = -1};
    plat_mutex_unlock(&L.lock);
}

void lw_end(LwStage s) {
    if (!L.open) return;
    plat_mutex_lock(&L.lock);
    if (L.st[s].state == 1) L.st[s].ended = now_s();
    L.st[s].state = 2;
    L.st[s].read = NULL;
    plat_mutex_unlock(&L.lock);
}

void lw_count(LwStage s, uint64_t total, uint64_t (*done)(void *ctx), void *ctx) {
    if (!L.open) return;
    plat_mutex_lock(&L.lock);
    Stage *t = &L.st[s];
    t->total = total, t->done = 0, t->read = done, t->ctx = ctx;
    t->head = t->n = 0, t->left = -1, t->last = 0;
    plat_mutex_unlock(&L.lock);
}

void lw_count_end(LwStage s) {
    if (!L.open) return;
    plat_mutex_lock(&L.lock);
    if (L.st[s].read) L.st[s].done = L.st[s].read(L.st[s].ctx);
    L.st[s].read = NULL;
    plat_mutex_unlock(&L.lock);
}

void lw_log(const char *p, size_t n) {
    // A thread that crashes inside text_add logs its crash report here again: that report goes to the file only.
    static _Thread_local int inside;
    if (!lw_taking_log || inside) return;
    inside = 1;
    plat_mutex_lock(&L.lock);
    if (lw_taking_log) text_add(p, n);
    plat_mutex_unlock(&L.lock);
    inside = 0;
}

char *lw_log_take(size_t *n) {
    plat_mutex_lock(&L.lock);
    char *t = L.text;
    *n = L.text_n;
    if (L.text_cut && t) {   // the panel shows where text went missing
        static const char cut[] = "\n[loading window: log lines dropped here; logs\\totk.log has them all]\n";
        char *u = realloc(t, *n + sizeof cut);
        if (u) memcpy(u + *n, cut, sizeof cut - 1), *n += sizeof cut - 1, t = u;
    }
    L.text = NULL, L.text_n = L.text_cap = 0, L.text_cut = 0;
    plat_mutex_unlock(&L.lock);
    return t;
}

// Time left of a counted stage: the rate over the last 12 s of samples, once 2 s of them and 3% of the work are in,
// eased toward each new value over about 3 s so that it does not jump with every burst of cached items.
static void stage_sample(Stage *t, double now) {
    if (t->n && now - t->t[(t->head + t->n - 1) % RATE_SAMPLES] < 0.25) return;
    if (t->n == RATE_SAMPLES) t->head = (t->head + 1) % RATE_SAMPLES, t->n--;
    int at = (t->head + t->n) % RATE_SAMPLES;
    t->t[at] = now, t->d[at] = t->done, t->n++;
    while (t->n > 2 && now - t->t[(t->head + 1) % RATE_SAMPLES] >= 12.0) t->head = (t->head + 1) % RATE_SAMPLES, t->n--;
    double raw = -1;
    double span = now - t->t[t->head];
    uint64_t moved = t->done - t->d[t->head];
    if (t->done >= t->total) raw = 0;
    else if (span >= 2.0 && moved && (double)t->done >= 0.03 * (double)t->total)
        raw = (double)(t->total - t->done) * span / (double)moved;
    double dt = t->last > 0 ? now - t->last : 0;
    t->last = now;
    if (raw < 0) t->left = -1;
    else if (t->left < 0 || raw == 0) t->left = raw;
    else {
        t->left = t->left > dt ? t->left - dt : 0;
        t->left += (raw - t->left) * (dt < 3.0 ? dt / 3.0 : 1.0);
    }
}

// The time a running stage still needs, from its length in the recent starts (plan) and how long it has run: the
// rest of that length, until 95% of it is over. Past that nothing says when it ends (a cold file cache, a driver that
// dropped its cache), so it is taken to need 5% of that length and a quarter of the time it ran beyond the 95%: its
// end keeps moving away, but slower than the time goes, so the share keeps rising, ever more slowly, while it runs.
static double to_go(double plan, double ran) {
    double knee = 0.95 * plan;
    return ran < knee ? plan - ran : 0.05 * plan + 0.25 * (ran - knee);
}

// Seconds to the game window from the recent starts' timeline (expect_s > 0). The guest starts (LW_GAME) once every
// stage before it has ended (boot.c rt_boot joins the prewarm thread), so the window comes the kept gap and LW_GAME's
// length after the latest end of those stages: an ended stage's own end, a running one's from to_go (or its counted
// work's time left once past its length), one not begun yet its length from its kept begin or from now, whichever is
// later. Taking the latest end, not the stages' distances to the window, keeps a stage that had time to spare from
// saying the window is far: after a cold start whose RomFS took a minute, the prewarm's distance to that window was a
// minute too. The stages not begun yet and LW_GAME take this start's pace so far: the median of the ended stages' times
// against their kept lengths, between 1 and 2, so a start slow all along (a busy machine) is not taken to end at the
// usual time, while one slow stage (the RomFS on a cold file cache, pipelines compiled anew) does not slow the rest.
// Without a timeline (a loading-time.txt with the length alone) the whole start is one stage.
static double time_left(double now) {
    if (L.was_e[LW_GAME] <= 0) return to_go(L.expect_s, now);
    double ratio[LW_GAME], pace = 1;
    int n = 0;
    for (int s = 0; s < LW_GAME; s++) {
        double kept = L.was_e[s] - L.was_b[s];
        if (L.st[s].state != 2 || kept < 0.05) continue;
        double r = (L.st[s].ended - L.st[s].begun) / kept;
        int i = n++;
        for (; i > 0 && ratio[i - 1] > r; i--) ratio[i] = ratio[i - 1];
        ratio[i] = r;
    }
    if (n) pace = fmin(fmax(ratio[(n - 1) / 2], 1.0), 2.0);
    double game = (L.was_e[LW_GAME] - L.was_b[LW_GAME]) * pace;
    if (L.st[LW_GAME].state) return to_go(game, now - L.st[LW_GAME].begun);
    double ready = now, was_ready = 0;
    for (int s = 0; s < LW_GAME; s++) {
        const Stage *u = &L.st[s];
        double plan = L.was_e[s] - L.was_b[s], end;
        if (L.was_e[s] > was_ready) was_ready = L.was_e[s];
        if (u->state == 2) end = u->ended;
        else if (u->state == 1) {
            double ran = now - u->begun;
            end = now + (ran >= 0.95 * plan && u->total && u->left >= 0 ? u->left : to_go(plan, ran));
        } else if (L.was_e[s] > 0) end = fmax(L.was_b[s], now) + plan * pace;
        else continue;
        if (end > ready) ready = end;
    }
    double gap = L.was_b[LW_GAME] > was_ready ? L.was_b[LW_GAME] - was_ready : 0;
    return ready + gap + game - now;
}

void lw_view(LwView *v) {
    double now = now_s();
    *v = (LwView){.stage = -1, .task_left_s = -1, .other = -1, .game_left_s = -1, .elapsed_s = now};
    plat_mutex_lock(&L.lock);
    v->failed = L.failed;
    memcpy(v->why, L.why, sizeof v->why);
    for (int s = 0; s < LW_STAGES; s++) {
        Stage *t = &L.st[s];
        if (t->state == 1 && t->read) {
            t->done = t->read(t->ctx);
            if (t->done > t->total) t->done = t->total;
            stage_sample(t, now);
        }
    }
    // The panel names the furthest stage running; another running stage with a count goes on the line below it.
    for (int s = LW_STAGES - 1; s >= 0; s--)
        if (L.st[s].state == 1) {
            if (v->stage < 0) v->stage = s;
            else if (v->other < 0 && L.st[s].total) v->other = s;
        }
    if (v->stage < 0) v->stage = LW_GAME;   // every stage ended: the game window is about to open
    const Stage *t = &L.st[v->stage];
    if (t->total) {
        v->done = t->done, v->total = t->total, v->task_left_s = t->left;
    }
    if (v->other >= 0) v->other_done = L.st[v->other].done, v->other_total = L.st[v->other].total;
    // The share of the whole start. With the recent starts' length: the time so far against itself plus the time left
    // (time_left), so a start on their pace fills the hearts evenly, a slower one keeps filling them ever more slowly
    // while it is behind and catches up when the slow stage ends, and a faster one speeds up as its stages end early.
    // A first start: fixed shares of the stages, an ended stage its whole share, a counted one by its count, another
    // one nearing its share over a few seconds.
    double p = 0;
    if (L.expect_s > 0) {
        double left = time_left(now);
        v->game_left_s = left;
        // The guest waits for the prewarm: its own time left, when longer, is what the player waits for
        if ((v->stage == LW_SHADERS || v->stage == LW_PIPELINES) && v->task_left_s > v->game_left_s)
            v->game_left_s = v->task_left_s;
        p = now / (now + left + 0.05);
    } else {
        static const double share[LW_STAGES] = {0.05, 0.05, 0.15, 0.35, 0.25, 0.15};
        for (int s = 0; s < LW_STAGES; s++) {
            const Stage *u = &L.st[s];
            if (u->state == 2) p += share[s];
            else if (u->state == 1 && u->total) p += share[s] * (double)u->done / (double)u->total;
            else if (u->state == 1) p += share[s] * (1 - exp(-(now - u->begun) / 4.0));
        }
    }
    // Never back, and never still: when the measure is behind what was shown (a stage began later than the last time,
    // the guest's own start runs long), the share still creeps on toward 1, the distance halving in twice the time the
    // start has run so far (at least 10 s), and the panel draws in the quarter in between. Below 1 until the window.
    double dt = L.view_t > 0 ? now - L.view_t : 0;
    L.view_t = now;
    double creep = L.overall + (1 - L.overall) * (1 - exp(-dt * 0.693 / (2 * fmax(now, 5.0))));
    if (p < creep) p = creep;
    if (p > 0.995) p = 0.995;
    v->overall = L.overall = p;
    plat_mutex_unlock(&L.lock);
}

void lw_game_window(void) {
    if (!L.open) return;
    plat_mutex_lock(&L.lock);
    L.shown_s = now_s();
    for (int s = 0; s < LW_STAGES; s++) {
        if (L.st[s].state == 1) L.st[s].ended = L.shown_s;
        L.st[s].state = L.st[s].state ? 2 : 0, L.st[s].read = NULL;
    }
    double shown = L.shown_s, expect = L.expect_s;
    plat_mutex_unlock(&L.lock);
    if (expect > 0)
        rt_log("loadwin: the game window opened %.0f ms after the process start (the recent starts: %.0f ms)",
               shown * 1000, expect * 1000);
    else rt_log("loadwin: the game window opened %.0f ms after the process start", shown * 1000);
    lw_wsys_close();
}

void lw_closed(int by_player) {
    lw_taking_log = 0;
    plat_mutex_lock(&L.lock);
    L.open = 0;
    free(L.text);
    L.text = NULL, L.text_n = L.text_cap = 0;
    int failed = L.failed;
    // The timeline kept is the mean of this start's and the one read, so that one start's own quirks (the guest's start
    // alone varies by seconds between two warm starts) move the next estimate halfway only
    double shown = L.shown_s, b[LW_STAGES], e[LW_STAGES];
    if (shown > 0 && L.expect_s > 0) shown = (shown + L.expect_s) / 2;
    for (int s = 0; s < LW_STAGES; s++) {
        int ran = L.st[s].state == 2;
        b[s] = ran ? L.st[s].begun : L.was_b[s], e[s] = ran ? L.st[s].ended : L.was_e[s];
        if (ran && L.was_e[s] > 0) b[s] = (b[s] + L.was_b[s]) / 2, e[s] = (e[s] + L.was_e[s]) / 2;
        if (e[s] > shown) e[s] = shown;
        if (b[s] > e[s]) b[s] = e[s];
    }
    plat_mutex_unlock(&L.lock);
    if (failed) return;   // lw_fatal's caller ends the process
    if (by_player) {
        rt_log("loadwin: the player closed the loading window; the game exits");
        plat_exit(0);
    }
    expect_write(shown, b, e);
}

void lw_fatal(const char *why) {
    if (!L.open || !L.player || lw_wsys_on_panel_thread()) return;
    plat_mutex_lock(&L.lock);
    L.failed = 1;
    snprintf(L.why, sizeof L.why, "%s", why ? why : "");
    plat_mutex_unlock(&L.lock);
    lw_wsys_fatal();
    lw_wsys_wait_closed();
}

#ifndef _WIN32
int lw_wsys_open(int background) { (void)background; return 0; }
void lw_wsys_close(void) {}
void lw_wsys_fatal(void) {}
void lw_wsys_wait_closed(void) {}
int lw_wsys_detach_console(void) { return 0; }
int lw_wsys_want_console(void) { return 0; }
int lw_wsys_on_panel_thread(void) { return 0; }
#endif
