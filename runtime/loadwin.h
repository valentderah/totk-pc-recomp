// The loading window (loadwin.c, loadwin_win.c): a small panel in the middle of the screen that a game run shows from
// the start until the game window opens. It names the start-up stage, shows how far its counted work has got (shaders,
// pipelines, the mods' files) with the time left, and about how long until the game starts, from the recent starts.
// Its Details button expands the log of this run (what logs\totk.log gets), live, to scroll and copy.
//
// A game run started without a console of its own (Explorer, a shortcut, a frontend) detaches from the console Windows
// made for it, so the player sees the panel only. console = 1 (TOTK_CONSOLE=1) keeps the console as before and shows
// no panel. Runs started from a terminal keep their console and show the panel too. Without a window system (Linux
// builds so far) every call does nothing.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Stages in the order a start goes through them. The main thread runs LW_START, LW_FILES and LW_GAME; the prewarm
// thread (boot.c prewarm_main) runs LW_GPU, LW_SHADERS and LW_PIPELINES meanwhile.
typedef enum {
    LW_START,       // settings, the game's code, mods
    LW_GPU,         // the Vulkan device
    LW_FILES,       // the RomFS index, its overlays and the mods' files (romfs.c init_once)
    LW_SHADERS,     // the boot prewarm's shaders (gfx_shader.c gfx_shader_prewarm)
    LW_PIPELINES,   // the boot prewarm's pipelines (vk_pipeline.c vk_pipeline_prewarm)
    LW_GAME,        // the guest's own start, up to the game window
    LW_STAGES
} LwStage;

// Nonzero while the panel takes log lines (rt_log_raw tests it before calling lw_log).
extern volatile int lw_taking_log;

// main.c, a game run (no mode), after log_open: opens the panel unless console = 1 or the run has no GPU, and then
// detaches from a console that only this process uses. early/early_n: the log lines written before (main.c's buffer).
void lw_start(const char *early, size_t early_n);
// A stage begins or ends. Any thread; a stage that never began is not shown.
void lw_begin(LwStage s);
void lw_end(LwStage s);
// The counted work of a stage that has begun: total units, and done(ctx) giving how many are done. The panel's thread
// calls done(ctx) a few times a second until lw_count_end(s) returns, so ctx stays valid until then.
void lw_count(LwStage s, uint64_t total, uint64_t (*done)(void *ctx), void *ctx);
void lw_count_end(LwStage s);
// Log text for the Details view (rt_log_raw).
void lw_log(const char *p, size_t n);
// gfx_window_win.c: the game window is on the screen. The panel closes and the start's length is kept for the next.
void lw_game_window(void);
// rt_fatal and the crash reporter, after their report: while the panel is open in a player's run, it shows that the
// game stopped and why (why: the fatal message, NULL for a crash), with the log, and the call returns when the player
// closes it. Returns at once otherwise.
void lw_fatal(const char *why);

// ---- loadwin.c to loadwin_win.c
typedef struct {
    int stage;                  // the stage the panel names, -1 before any
    int failed;                 // lw_fatal ran
    char why[512];              // its message
    double overall;             // 0..1, the share of the whole start done (the hearts), below 1 until the game
                                // window opens; it never goes back
    uint64_t done, total;       // the named stage's counted work (total 0: none)
    double task_left_s;         // its time left, negative when unknown
    int other;                  // another stage running at the same time (-1 none), with its count
    uint64_t other_done, other_total;
    double game_left_s;         // seconds until the game window, from the recent starts; negative when unknown
    double elapsed_s;
} LwView;
void lw_view(LwView *v);
// Takes the log text added since the last call (malloc'd, the caller frees; NULL when none) into *n bytes.
char *lw_log_take(size_t *n);
// The window thread ends: the panel is gone.
void lw_closed(int by_player);
// loadwin_win.c: makes the panel on its own thread; 0 when it cannot. background: TOTK_BACKGROUND_WINDOW (test runs),
// the panel opens without taking the focus.
int lw_wsys_open(int background);
void lw_wsys_close(void);         // closes the panel from another thread
void lw_wsys_fatal(void);         // shows the failure and raises the panel
void lw_wsys_wait_closed(void);   // returns when the panel's thread has ended
int lw_wsys_detach_console(void); // 1 when this process alone used its console and left it
int lw_wsys_want_console(void);   // 1 when it made a console for a run that started without one (totk.manifest)
int lw_wsys_on_panel_thread(void);
