// The Blueprints tab (ui/menu.cpp): choose a .cai or .txt blueprint file on this PC (folder_pick.h file_pick), check it
// with blueprint_file.h, and add it to the game's Autobuild history with blueprint_import.h. The game keeps the history
// in its save data, so the native preview, rotation and Zonai energy costs work unchanged, and the imported creation
// appears as the newest draft of the list the Autobuild page draws.
//
// The page is one row: choose a file, then import it. There is no slot to pick: a new creation in the game always takes
// the place of the oldest draft, and importing does the same, so the player never has to know which save-data slot
// holds what.
//
// The dialog runs on the folder_pick thread, so the callback only stores the path and a counter (ui/amiibo_page.cpp does
// the same); the page reads and checks the file when it draws. The file buffer stays for as long as the page shows that
// file, because BpcBlueprint's name pointers borrow from it.
//
// bp_favorites and bp_import_newest find the save data by walking the game's memory, which takes up to a second on the
// first call. The menu draws on the present path, so those calls run on a worker thread of their own (as the file dialog
// does) and the page only collects their results: the tab never blocks the game.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "ui/blueprint_page.h"
#include "ui/option_rows.h"

extern "C" {
#include "blueprint_cai.h"
#include "blueprint_dir.h"
#include "blueprint_file.h"
#include "blueprint_import.h"
#include "folder_pick.h"
#include "platform/platform.h"
#include "ui_text.h"
// host.h pulls in cpu.h, which is C only: the one function the page needs from it, as the menu declares it.
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}

// What the worker thread is doing (queued by the page, cleared when it is done).
enum { BLUEPRINT_TASK_IDLE, BLUEPRINT_TASK_LOOKUP, BLUEPRINT_TASK_IMPORT };

// Whether the worker thread ever started (the page says so when it did not).
enum { BLUEPRINT_WORKER_NONE, BLUEPRINT_WORKER_OK, BLUEPRINT_WORKER_FAILED };

// ---- the dialog's answer: written on the folder_pick thread under g_lock
static PlatMutex g_lock = PLAT_MUTEX_INIT;
static char g_path[PLAT_PATH_MAX];
static bool g_pick_open;
static unsigned g_gen;   // bumped when a file is chosen, so the page reloads it

// ---- the chosen file: read and checked on the draw thread (the menu runs on the present path)
static unsigned g_loaded_gen;
static char g_loaded_path[PLAT_PATH_MAX];
static uint8_t g_blob[BPF_READ_MAX];
static uint64_t g_file_size;   // the whole file's size, even when only the first bytes were read
static bool g_read_ok;         // the file opened and was read (it may still be too short)
static enum BpcResult g_parse;
static BpcBlueprint g_bp;

// ---- the worker: the calls that walk the game's memory, and the results waiting for the page
static PlatMutex g_worker_lock = PLAT_MUTEX_INIT;
static PlatCond g_worker_cond = PLAT_COND_INIT;
static int g_task;          // BLUEPRINT_TASK_*: the page's request; cleared by the worker when done
static bool g_task_taken;   // the worker has started g_task: a request queued now waits for it to finish
static char g_task_path[PLAT_PATH_MAX];
static bool g_lookup_done;
static bool g_lookup_found;
static BpFavorites g_lookup_fav;
static bool g_import_done;
static int g_import_result;

// ---- the page's own view (draw thread only), filled from the worker's results
static int g_worker_state = BLUEPRINT_WORKER_NONE;
static BpFavorites g_fav;
static bool g_fav_ok;
static bool g_import_pending;
static uint64_t g_last_frame = ~0ull;   // the frame the page last drew: one lookup attempt per tab entry
static bool g_result_shown;
static int g_result;

static void on_file(const char *path, void *ctx) {
    (void)ctx;
    // Remembered here rather than in the page, so the next dialog opens where this one was browsed even if the player
    // leaves the tab (or the game) before it draws again (blueprint_dir.h).
    if (path && *path) bp_dir_remember(path);
    plat_mutex_lock(&g_lock);
    if (path && *path) {
        snprintf(g_path, sizeof g_path, "%s", path);
        g_gen++;
    }
    g_pick_open = false;
    plat_mutex_unlock(&g_lock);
    rt_log("blueprint: %s", path && *path ? path : "no file chosen");
}

static const char *base_name(const char *path) {
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') name = p + 1;
    return name;
}

// Reads the file and checks it. blueprint_file.h decides the form from the content, so a .cai and a .txt file both
// arrive here as the same validated blueprint.
static void load_file(const char *path) {
    snprintf(g_loaded_path, sizeof g_loaded_path, "%s", path);
    g_file_size = 0;
    g_read_ok = false;
    g_parse = BPC_TOO_SHORT;
    memset(&g_bp, 0, sizeof g_bp);
    int64_t size = plat_file_size(path);
    if (size < 0) {
        rt_log("blueprint: cannot open %s", path);
        return;
    }
    g_file_size = (uint64_t)size;
    g_read_ok = true;
    g_parse = bp_file_read(path, g_blob, &g_bp);
    if (g_parse == BPC_OK) rt_log("blueprint: %d actors, first %s", g_bp.actors, g_bp.names[0]);
}

// Why the chosen file cannot be imported, or NULL when it can. The parser's reason goes through the text catalog, so the
// page reads it in the menu language instead of showing the log's English.
static const char *parse_message(void) {
    switch (g_parse) {
    case BPC_OK: return NULL;
    case BPC_TOO_SHORT: {
        static char text[192];
        snprintf(text, sizeof text, menu_text(UI_MENU_BLUEPRINT_ERR_TOO_SHORT), (int)g_file_size);
        return text;
    }
    case BPC_BAD_MAGIC: return menu_text(UI_MENU_BLUEPRINT_ERR_MAGIC);
    case BPC_BAD_VERSION: return menu_text(UI_MENU_BLUEPRINT_ERR_VERSION);
    case BPC_BAD_COUNT: return menu_text(UI_MENU_BLUEPRINT_ERR_COUNT);
    case BPC_BAD_ACTOR: return menu_text(UI_MENU_BLUEPRINT_ERR_ACTOR);
    case BPC_UNSUPPORTED: return menu_text(UI_MENU_BLUEPRINT_TOTKAB);
    }
    return menu_text(UI_MENU_BLUEPRINT_ERR_ACTOR);
}

// The thread that runs the slow calls: bp_favorites and bp_import_newest walk the guest memory, and the menu must not
// wait for them. It runs until the process ends, one task at a time; the page collects each result when it draws.
static void blueprint_worker(void *arg) {
    (void)arg;
    for (;;) {
        plat_mutex_lock(&g_worker_lock);
        while (g_task == BLUEPRINT_TASK_IDLE) plat_cond_wait(&g_worker_cond, &g_worker_lock);
        g_task_taken = true;
        const int task = g_task;
        char path[PLAT_PATH_MAX];
        snprintf(path, sizeof path, "%s", g_task_path);
        plat_mutex_unlock(&g_worker_lock);
        if (task == BLUEPRINT_TASK_LOOKUP) {
            BpFavorites fav;
            memset(&fav, 0, sizeof fav);
            const bool found = bp_favorites(&fav) != 0;
            plat_mutex_lock(&g_worker_lock);
            g_lookup_found = found;
            g_lookup_fav = fav;
            g_lookup_done = true;
            g_task = BLUEPRINT_TASK_IDLE;
            g_task_taken = false;
            plat_mutex_unlock(&g_worker_lock);
        } else {
            const int result = bp_import_newest(path);
            plat_mutex_lock(&g_worker_lock);
            g_import_result = result;
            g_import_done = true;
            g_task = BLUEPRINT_TASK_IDLE;
            g_task_taken = false;
            plat_mutex_unlock(&g_worker_lock);
        }
    }
}

// Starts the worker once; 1 when it runs. Draw thread only.
static bool worker_start(void) {
    if (g_worker_state != BLUEPRINT_WORKER_NONE) return g_worker_state == BLUEPRINT_WORKER_OK;
    g_worker_state = plat_thread_spawn(blueprint_worker, nullptr, "blueprint", 0) ? BLUEPRINT_WORKER_OK
                                                                                  : BLUEPRINT_WORKER_FAILED;
    if (g_worker_state == BLUEPRINT_WORKER_FAILED) rt_log("blueprint: could not start the worker thread");
    return g_worker_state == BLUEPRINT_WORKER_OK;
}

// Queues a lookup. One request waits at a time, and a new one replaces a request the worker has not started yet; a
// request the worker is running is left to finish. Draw thread only.
static void queue_lookup(void) {
    if (!worker_start()) return;
    plat_mutex_lock(&g_worker_lock);
    if (!g_lookup_done && !g_import_done && (g_task == BLUEPRINT_TASK_IDLE || !g_task_taken)) {
        g_task = BLUEPRINT_TASK_LOOKUP;
        g_import_pending = false;   // a request that has not started yet is replaced by this one
        plat_cond_signal(&g_worker_cond);
        rt_log("blueprint: looking for the Autobuild history (worker thread)");
    }
    plat_mutex_unlock(&g_worker_lock);
}

// Queues writing path in as the newest draft; the worker reads the file again and writes the game's save data in
// memory. Draw thread only. A later lookup reads the list again, so the page can say what the import changed.
static void queue_import(const char *path) {
    if (!worker_start()) return;
    plat_mutex_lock(&g_worker_lock);
    if (!g_lookup_done && !g_import_done && (g_task == BLUEPRINT_TASK_IDLE || !g_task_taken)) {
        g_task = BLUEPRINT_TASK_IMPORT;
        snprintf(g_task_path, sizeof g_task_path, "%s", path);
        g_import_pending = true;
        plat_cond_signal(&g_worker_cond);
        rt_log("blueprint: importing %s (worker thread)", path);
    }
    plat_mutex_unlock(&g_worker_lock);
}

// Takes whatever the worker finished since the last frame. Draw thread only.
static void collect_worker_results(void) {
    bool lookup_done, lookup_found, import_done;
    BpFavorites fav;
    int import_result;
    plat_mutex_lock(&g_worker_lock);
    lookup_done = g_lookup_done;
    lookup_found = g_lookup_found;
    fav = g_lookup_fav;
    import_done = g_import_done;
    import_result = g_import_result;
    g_lookup_done = false;
    g_import_done = false;
    plat_mutex_unlock(&g_worker_lock);
    if (lookup_done) {
        g_fav_ok = lookup_found;
        if (lookup_found) g_fav = fav;
    }
    if (import_done) {
        g_import_pending = false;
        g_result = import_result;
        g_result_shown = true;
        rt_log("blueprint: import: %s", bp_import_text(import_result));
        // Read the history again: bp_import_newest writes both copies of the draft, and the search address is cached by
        // now, so this is the honest way to show what the list holds after the write.
        if (import_result == BP_IMPORT_OK) queue_lookup();
    }
}

void tab_blueprint(float scale) {
    // ---- the dialog's answer, and the file it names
    plat_mutex_lock(&g_lock);
    const bool picking = g_pick_open;
    const unsigned gen = g_gen;
    char path[PLAT_PATH_MAX];
    snprintf(path, sizeof path, "%s", g_path);
    plat_mutex_unlock(&g_lock);
    if (path[0] && gen != g_loaded_gen) {
        g_loaded_gen = gen;
        load_file(path);
        g_result_shown = false;   // the last import named the file before this one
    }
    collect_worker_results();
    // One lookup attempt per tab entry: the page can say what the history holds, and a page left while the save was
    // still loading tries again when the player comes back. The worker keeps the present thread free either way.
    const uint64_t frame = ImGui::GetFrameCount();
    const bool reentered = frame > g_last_frame + 1;   // another tab was drawn last frame
    g_last_frame = frame;
    if (reentered && !g_fav_ok) queue_lookup();

    const bool have_file = path[0] != 0;
    const bool file_ok = g_read_ok && g_parse == BPC_OK;
    const bool worker_failed = g_worker_state == BLUEPRINT_WORKER_FAILED;

    opt_begin("##blueprint", scale);
    opt_text(UI_MENU_BLUEPRINT_INTRO);

    opt_group(UI_MENU_BLUEPRINT_FILE);
    static const char *const patterns[] = {"*.cai", "*.txt"};
    if (opt_button("##blueprint_pick", UI_MENU_BLUEPRINT_PICK, UI_MENU_BLUEPRINT_PICK_DESC,
                   have_file ? base_name(path) : menu_text(UI_MENU_BLUEPRINT_NONE)) &&
        !picking) {
        plat_mutex_lock(&g_lock);
        g_pick_open = true;
        plat_mutex_unlock(&g_lock);
        file_pick("", patterns, 2, on_file, nullptr);
    }
    if (have_file) {
        ImGui::TextWrapped("%s", g_loaded_path);
        const char *problem = g_read_ok ? parse_message() : menu_text(UI_MENU_BLUEPRINT_READ_FAILED);
        if (problem) {
            opt_text_str(problem, true);
        } else {
            char text[192];
            snprintf(text, sizeof text, menu_text(UI_MENU_BLUEPRINT_CAI), g_bp.actors);
            ImGui::TextWrapped("%s", text);
        }
    }

    // ---- the one action: add the chosen file to the history
    opt_group(UI_MENU_BLUEPRINT_IMPORT);
    if (worker_failed) opt_disable_next(UI_MENU_BLUEPRINT_WORKER_FAILED);
    else if (!file_ok) opt_disable_next(UI_MENU_BLUEPRINT_NEED_FILE);
    else if (g_import_pending) opt_disable_next(UI_MENU_BLUEPRINT_IMPORTING);
    const char *value = g_import_pending ? menu_text(UI_MENU_BLUEPRINT_IMPORTING) : "";
    if (opt_text_row("##blueprint_import", menu_text(UI_MENU_BLUEPRINT_IMPORT), value, nullptr,
                     menu_text(UI_MENU_BLUEPRINT_IMPORT_DESC), false) == 2 &&
        !g_import_pending && file_ok)
        queue_import(g_loaded_path);

    // What the history holds now: top is the entry the page draws first, which an import replaces.
    if (g_fav_ok && g_fav.slots > 0) {
        char text[256];
        const int top = g_fav.top >= 0 && g_fav.top < g_fav.slots ? g_fav.top : 0;
        if (g_fav.actors[top] > 0)
            snprintf(text, sizeof text, menu_text(UI_MENU_BLUEPRINT_HISTORY), g_fav.slots, g_fav.actors[top]);
        else snprintf(text, sizeof text, menu_text(UI_MENU_BLUEPRINT_HISTORY_EMPTY), g_fav.slots);
        ImGui::TextWrapped("%s", text);
    }

    if (g_result_shown) {
        const char *fmt = menu_text(UI_MENU_BLUEPRINT_IMPORT_FAILED);
        switch (g_result) {
        case BP_IMPORT_OK: fmt = menu_text(UI_MENU_BLUEPRINT_DONE); break;
        case BP_IMPORT_NO_FILE: fmt = menu_text(UI_MENU_BLUEPRINT_IMPORT_NO_FILE); break;
        case BP_IMPORT_BAD_FILE: fmt = menu_text(UI_MENU_BLUEPRINT_IMPORT_BAD_FILE); break;
        case BP_IMPORT_NO_SAVE: fmt = menu_text(UI_MENU_BLUEPRINT_NO_SAVE); break;
        case BP_IMPORT_NO_ORDER: fmt = menu_text(UI_MENU_BLUEPRINT_NO_ORDER); break;
        case BP_IMPORT_BAD_SLOT: fmt = menu_text(UI_MENU_BLUEPRINT_IMPORT_BAD_SLOT); break;
        case BP_IMPORT_NO_ROOM: fmt = menu_text(UI_MENU_BLUEPRINT_IMPORT_NO_ROOM); break;
        default: break;
        }
        const bool ok = g_result == BP_IMPORT_OK;
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ok ? ImVec4(0.55f, 0.85f, 0.45f, 1.0f) : ImVec4(0.95f, 0.66f, 0.40f, 1.0f));
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(fmt);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    opt_end();
}
