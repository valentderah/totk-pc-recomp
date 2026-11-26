// Runs the loading window's estimate (runtime/loadwin.c lw_view, time_left) and its hearts (runtime/loadwin_win.c
// hearts_step, paint) through emulated starts with a fake clock, and checks what the player would see. Each case is two
// starts in a row: the first writes loading-time.txt (in memory), the second reads it and is the one checked. Built and
// run by tests/loadwin_pace.py; Windows only, as the panel draws with GDI.
//   loadwin_pace.exe <first> <second> [csv]
// Starts: warm (6.5 s), cold (the RomFS on a cold file cache, 70.7 s), pipes (the pipelines compiled anew, 11.1 s),
// slowgame (the guest's own start takes 19 s, 22 s), slow4 (all four times slower, 26 s); "none" as the first start
// leaves no loading-time.txt. The checks, on the second start until its game window:
//   - the share and the hearts shown never go back and never pass the end;
//   - the hearts never stand still for more than STALL_S while the start runs;
//   - the last quarter does not come before the game window;
//   - at the game window the row is at least MIN_AT_WINDOW of the way (the start was not shown as far from done),
//     unless the start is much faster than the one before: the estimate only stretches the kept timeline (loadwin.c
//     lw_view, the pace), so such a start opens its window early and the finale's flash fills the row;
//   - loading-time.txt then holds start_ms and the six stages' times.
#include "../runtime/loadwin.c"
#include "../runtime/loadwin_win.c"

static double T;   // the emulated clock, seconds since the process start
static char file_buf[512];
static size_t file_n;

uint64_t plat_process_uptime_ns(void) { return (uint64_t)(T * 1e9); }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
uint8_t *plat_file_read(const char *path, uint64_t *size) {
    (void)path;
    if (!file_n) return NULL;
    uint8_t *p = malloc(file_n);
    memcpy(p, file_buf, file_n);
    *size = file_n;
    return p;
}
int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    (void)path;
    if (n > sizeof file_buf) return 0;
    memcpy(file_buf, data, n), file_n = n;
    return 1;
}
static RtConfig cfg = {.profile = "mem"};
const RtConfig *rt_config(void) { return &cfg; }
int cfg_bool(const char *name) { (void)name; return 0; }
const char *cfg_get(const char *name) { (void)name; return NULL; }
_Noreturn void plat_exit(int code) { exit(code); }
const int rt_game_version = 143;
const char rt_port_version[] = "test";
const char *ov_language(void) { return "en"; }
unsigned cfg_version(void) { return 1; }
const char *cfg_value(const char *name) { (void)name; return NULL; }
const char *plat_ui_language(void) { return "en-US"; }
void rt_log(const char *fmt, ...) { (void)fmt; }
void plat_event_init(PlatEvent *e, int manual, int initially_set) { (void)e, (void)manual, (void)initially_set; }
void plat_event_set(PlatEvent *e) { (void)e; }
void plat_event_wait(PlatEvent *e) { (void)e; }
int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    (void)fn, (void)arg, (void)name, (void)stack;
    return 0;
}

// A start: when each stage begins and ends (seconds), the counted ones' totals; the game window opens at the end.
typedef struct { double b[LW_STAGES], e[LW_STAGES]; uint64_t total[LW_STAGES]; double window; } Run;
static const Run WARM = {.b = {0.05, 0.45, 0.45, 0.90, 1.60, 2.70}, .e = {0.45, 0.90, 1.40, 1.60, 2.50, 6.50},
                         .total = {0, 0, 0, 5812, 8009, 0}, .window = 6.50};
static const Run COLD = {.b = {0.05, 0.45, 0.45, 0.90, 1.60, 66.9}, .e = {0.45, 0.90, 66.6, 1.60, 2.50, 70.7},
                         .total = {0, 0, 0, 5812, 8009, 0}, .window = 70.7};
static const Run PIPES = {.b = {0.05, 0.45, 0.45, 0.90, 1.60, 7.30}, .e = {0.45, 0.90, 1.40, 1.60, 7.10, 11.1},
                          .total = {0, 0, 0, 5812, 8009, 0}, .window = 11.1};   // the driver dropped its cache
static const Run SLOWGAME = {.b = {0.05, 0.45, 0.45, 0.90, 1.60, 2.70}, .e = {0.45, 0.90, 1.40, 1.60, 2.50, 22.0},
                             .total = {0, 0, 0, 5812, 8009, 0}, .window = 22.0};   // the guest's own start is slow
static const Run SLOW4 = {.b = {0.10, 1.80, 1.80, 3.60, 6.40, 10.8}, .e = {1.80, 3.60, 5.60, 6.40, 10.0, 26.0},
                          .total = {0, 0, 0, 5812, 8009, 0}, .window = 26.0};   // a machine four times slower
static const Run *run;
static uint64_t count_done(void *ctx) {
    int s = (int)(intptr_t)ctx;
    double f = (T - run->b[s]) / (run->e[s] - run->b[s]);
    return (uint64_t)((f < 0 ? 0 : f > 1 ? 1 : f) * (double)run->total[s]);
}

#define STALL_S 2.0          // the longest the hearts may stand still while the start runs
// The least of the row shown when the game window opens. The pace stretches the guest's start (LW_GAME) by how much
// slower the stages before it ran; after a start whose guest was slow, a start slow everywhere but there gets more
// time for the guest than it takes (slowgame -> slow4: 28 of 48 quarters at the window).
#define MIN_AT_WINDOW 0.55
static int failed;
static double first_window;   // the first start's game window, 0 without one
#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            printf("FAIL at %.2f s: ", T); \
            printf(__VA_ARGS__); \
            printf("\n"); \
            failed = 1; \
        } \
    } while (0)

// One start, from the process start to the panel's end. check: the start is the one checked; csv gets its trace.
static void emulate(const Run *r, int check, FILE *csv) {
    run = r;
    memset(&L, 0, sizeof L), memset(&G, 0, sizeof G);
    T = 0;
    G.dpi = 96, G.finale = -1, G.swept = -1;
    L.open = 1, L.player = 1;
    snprintf(L.time_path, sizeof L.time_path, "mem");
    expect_read();
    L.st[LW_START] = (Stage){.state = 1, .begun = r->b[LW_START], .left = -1};
    fonts_make();
    int w = PANEL_W, h = PANEL_H;
    BITMAPINFO bi = {.bmiHeader = {.biSize = sizeof bi.bmiHeader, .biWidth = w, .biHeight = -h, .biPlanes = 1,
                                   .biBitCount = 32, .biCompression = BI_RGB}};
    HDC mem = CreateCompatibleDC(NULL);
    void *px = NULL;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &px, NULL, 0);
    SelectObject(mem, bmp);
    double dt = TIMER_MS / 1000.0, last_share = 0, last_shown = 0, last_pos = 0, moved_at = 0;
    int began[LW_STAGES] = {1}, ended[LW_STAGES] = {0};
    for (int i = 0; T < r->window + FIN_END; i++, T += dt) {
        for (int s = 0; s < LW_STAGES; s++) {
            if (!began[s] && T >= r->b[s]) {
                began[s] = 1, lw_begin(s);
                if (r->total[s]) lw_count(s, r->total[s], count_done, (void *)(intptr_t)s);
            }
            if (began[s] && !ended[s] && T >= r->e[s] && s != LW_GAME) {
                ended[s] = 1;
                if (r->total[s]) lw_count_end(s);
                lw_end(s);
            }
        }
        if (T >= r->window && G.finale < 0) {
            if (check && !(first_window > 0 && r->window < 0.8 * first_window))
                CHECK(G.quarters + G.part >= MIN_AT_WINDOW * N_QUARTERS, "%d quarters at the game window, want %.0f",
                      G.quarters, MIN_AT_WINDOW * N_QUARTERS);
            lw_game_window(), G.finale = 0;
        }
        G.age += dt;
        if (G.finale >= 0) G.finale += dt;
        else {
            if (i % 2 == 0) {   // the panel's timer asks for the view about every other tick
                lw_view(&G.v);
                if (csv)
                    fprintf(csv, "%.3f,%.4f,%.4f,%d,%.3f,%.2f\n", T, G.v.overall, G.shown, G.quarters, G.part,
                            G.v.game_left_s);
            }
            G.pulse += dt;
            double k = dt / 0.5 < 1 ? dt / 0.5 : 1;
            if (G.v.overall > G.shown) G.shown += (G.v.overall - G.shown) * k;
        }
        hearts_step();
        if (i % 10 == 0) {   // draws the panel now and then: the hearts' and the text's code runs as in the game
            RECT cr = {0, 0, w, h};
            paint(mem, cr, px);
            GdiFlush();
        }
        if (!check || G.finale >= 0) continue;
        double pos = G.quarters + G.part;
        CHECK(G.v.overall >= last_share - 1e-9 && G.v.overall <= 1, "share %.4f after %.4f", G.v.overall, last_share);
        CHECK(G.shown >= last_shown - 1e-9 && G.shown <= 1, "shown %.4f after %.4f", G.shown, last_shown);
        CHECK(pos >= last_pos - 1e-9, "hearts went back: %.3f after %.3f", pos, last_pos);
        CHECK(G.quarters < N_QUARTERS, "the last quarter came %.2f s before the game window", r->window - T);
        if (pos > last_pos + 1e-6) moved_at = T;
        CHECK(T - moved_at <= STALL_S, "the hearts stood still for %.2f s at %.3f quarters", T - moved_at, pos);
        if (T - moved_at > STALL_S) moved_at = T;   // one failure per stall
        last_share = G.v.overall, last_shown = G.shown, last_pos = pos;
    }
    lw_closed(0);   // writes loading-time.txt
    DeleteObject(bmp), DeleteDC(mem);
}

static const Run *pick(const char *s) {
    return !strcmp(s, "cold") ? &COLD : !strcmp(s, "warm") ? &WARM : !strcmp(s, "pipes") ? &PIPES
         : !strcmp(s, "slowgame") ? &SLOWGAME : !strcmp(s, "slow4") ? &SLOW4 : NULL;
}

int main(int argc, char **argv) {
    if (argc < 3 || !pick(argv[2]) || (strcmp(argv[1], "none") && !pick(argv[1]))) {
        fprintf(stderr, "usage: loadwin_pace <warm|cold|pipes|slowgame|slow4|none> <warm|cold|pipes|slowgame|slow4> "
                        "[csv]\n");
        return 2;
    }
    if (pick(argv[1])) emulate(pick(argv[1]), 0, NULL), first_window = pick(argv[1])->window;
    FILE *csv = argc > 3 ? fopen(argv[3], "w") : NULL;
    if (csv) fprintf(csv, "t,share,shown,quarters,part,left\n");
    emulate(pick(argv[2]), 1, csv);
    if (csv) fclose(csv);
    int start_ms = 0;
    double st[2 * LW_STAGES];
    char text[sizeof file_buf + 1];
    memcpy(text, file_buf, file_n), text[file_n] = 0;
    const char *line2 = strchr(text, '\n');
    int got = line2 ? sscanf(line2 + 1, "stages_ms %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf", &st[0], &st[1],
                             &st[2], &st[3], &st[4], &st[5], &st[6], &st[7], &st[8], &st[9], &st[10], &st[11])
                      : 0;
    CHECK(sscanf(text, "start_ms %d", &start_ms) == 1 && start_ms > 0 && got == 2 * LW_STAGES,
          "loading-time.txt: %s", text);
    printf("%s -> %s: %s\n", argv[1], argv[2], failed ? "FAIL" : "ok");
    return failed;
}
