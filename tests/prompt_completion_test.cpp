// Synthetic menu scheduler fixture; production bodies are read from the selected checkout.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>

enum { ROMFS_NEED_LAYOUT = 1, ROMFS_NEED_PROMPTS = 2, ROMFS_NEED_SHAPED_PROMPTS = 4 };
enum { ROMFS_PROMPTS_FAILED = 1, ROMFS_PROMPTS_MODS, ROMFS_PROMPTS_PATCHES };
enum {
    UI_MENU_PROMPTS_PREPARING, UI_MENU_SHAPE_PREPARE_FAILED, UI_MENU_PROMPTS_MISSING,
    UI_MENU_PROMPTS_OFF_SHAPE, UI_MENU_PROMPTS_FAILED, UI_MENU_PROMPTS_OFF_MODS,
    UI_MENU_PROMPTS_OFF_PATCHES, UI_MENU_PROMPTS_RESTART
};
static const char *notes[] = {"preparing", "start failed", "missing", "missing shape", "failed", "mods", "patches", "restart"};
static const char *menu_text(int id) { return notes[id]; }
static std::atomic<uint64_t> clock_ns{1000000000ull};
static unsigned version;
static const char *choice = "pc", *resolution = "1280x800", *hud = "full";
static const char *used;
static int off_reason, launches, prepare_ok = 1, can_prepare = 1;
static std::atomic<int> missing{7}, reads{0};
static const std::thread::id menu_thread = std::this_thread::get_id();
static std::mutex read_gate;
static std::condition_variable read_wake;
[[maybe_unused]] static bool block_read, read_entered, release_read;
static uint64_t plat_now_ns() { return clock_ns; }
static unsigned cfg_version() { return version; }
static const char *cfg_value(const char *key) {
    if (!strcmp(key, "TOTK_PAD_LAYOUT")) return "xbox";
    if (!strcmp(key, "TOTK_OUTPUT_RES")) return resolution;
    if (!strcmp(key, "TOTK_HUD_WIDTH")) return hud;
    return "";
}
static const char *cfg_get(const char *key) { return !strcmp(key, "TOTK_PROMPTS") ? choice : nullptr; }
static int cfg_screen_size(unsigned *) { return 0; }
static int input_prompts_pick(const char *value, const char *) { return !strcmp(value, "pc") ? 1 : 2; }
static const char *input_prompts_folder(int pick, const char *) {
    return pick == 1 ? "romfs-patches-pc" : "romfs-patches-playstation";
}
static int romfs_folders_missing(unsigned, unsigned, const char *) {
    reads++;
    int result = missing;
    if (std::this_thread::get_id() != menu_thread) {
        std::unique_lock<std::mutex> lock(read_gate);
        if (block_read) {
            block_read = false;
            read_entered = true;
            read_wake.notify_all();
            read_wake.wait(lock, [] { return release_read; });
        }
    }
    return result;
}
static int romfs_folders_can_prepare() { return can_prepare; }
static int romfs_folders_prepare(unsigned, unsigned, const char *) { launches++; return prepare_ok; }
static const char *romfs_prompts() { return used; }
static int romfs_prompts_off(const char *) { return off_reason; }

// Real host mutexes, condition variables and one real worker; only its clock/readiness input is synthetic.
struct PlatMutex { std::mutex *p; };
struct PlatCond { std::condition_variable *p; };
#define PLAT_MUTEX_INIT {nullptr}
#define PLAT_COND_INIT {nullptr}
static std::mutex init_lock;
static std::thread worker;
static std::atomic<bool> stop_worker{false};
static std::atomic<int> idle_waits{0};
static thread_local PlatMutex *held;
[[maybe_unused]] static int worker_starts;
static bool fail_spawn;
struct Stop {};
static std::mutex &mutex_of(PlatMutex *m) {
    std::lock_guard<std::mutex> lock(init_lock);
    if (!m->p) m->p = new std::mutex;
    return *m->p;
}
static std::condition_variable &cond_of(PlatCond *c) {
    std::lock_guard<std::mutex> lock(init_lock);
    if (!c->p) c->p = new std::condition_variable;
    return *c->p;
}
[[maybe_unused]] static void plat_mutex_lock(PlatMutex *m) { mutex_of(m).lock(); held = m; }
[[maybe_unused]] static void plat_mutex_unlock(PlatMutex *m) { held = nullptr; mutex_of(m).unlock(); }
[[maybe_unused]] static int plat_mutex_trylock(PlatMutex *m) {
    if (!mutex_of(m).try_lock()) return 0;
    held = m;
    return 1;
}
[[maybe_unused]] static void plat_cond_signal(PlatCond *c) { cond_of(c).notify_all(); }
[[maybe_unused]] static int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    std::unique_lock<std::mutex> lock(mutex_of(m), std::adopt_lock);
    auto status = cond_of(c).wait_for(lock, std::chrono::nanoseconds(ns));
    lock.release(); // the production API returns with its mutex still held
    if (stop_worker) throw Stop{};
    return status != std::cv_status::timeout;
}
[[maybe_unused]] static void plat_cond_wait(PlatCond *c, PlatMutex *m) {
    idle_waits++;
    plat_cond_wait_ns(c, m, 10000000000ull);
}
[[maybe_unused]] static int plat_thread_spawn(void (*fn)(void *), void *arg, const char *, size_t) {
    if (fail_spawn) return 0;
    worker_starts++;
    worker = std::thread([=] {
        try { fn(arg); }
        catch (Stop &) { if (held) held->p->unlock(); }
    });
    return 1;
}
[[maybe_unused]] static void rt_log(const char *, ...) {}
#include "prompt_completion_body.h"

static int checks, failures;
static void check(bool ok, const char *name) {
    checks++;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) failures++;
}
static void note_is(int id, bool expect_restart, const char *name) {
    bool restart;
    const char *note = prompts_note(&restart);
    check(note == menu_text(id) && restart == expect_restart, name);
}
static void advance(uint64_t ns) {
    clock_ns += ns;
#if HAS_FOLDER_WORKER
    // A clock jump wakes the actual worker's timed wait; spurious wakes before the deadline remain harmless.
    plat_mutex_lock(&g_folder_check.lock);
    plat_cond_signal(&g_folder_check.wake);
    plat_mutex_unlock(&g_folder_check.lock);
#endif
}
static bool await_result(int mask) {
#if HAS_FOLDER_WORKER
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        plat_mutex_lock(&g_folder_check.lock);
        bool ready = g_folder_check.result_serial == g_folder_check.serial && g_folder_check.missing == mask;
        plat_mutex_unlock(&g_folder_check.lock);
        if (ready) return true;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
#else
    (void)mask;
    return true; // unchanged scheduler has no worker; menu-state assertions below still expose the bug
#endif
}
static void fresh() {
#if HAS_FOLDER_WORKER
    int previous_idle = idle_waits;
    folders_check_cancel();
    if (worker.joinable()) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (idle_waits == previous_idle) {
            if (std::chrono::steady_clock::now() >= deadline) std::exit(2);
            std::this_thread::yield();
        }
    }
#endif
    g_folders = {};
    g_folders.seen = ~0u;
    clock_ns = 1000000000ull;
    version = 0;
    choice = "pc";
    resolution = "1280x800";
    hud = "full";
    used = nullptr;
    off_reason = 0;
    missing = 7;
    reads = launches = 0;
    prepare_ok = can_prepare = 1;
}
// A contended cache lock is retried on a later menu frame, without another filesystem poll.
static int menu_tick() {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        int waiting = folders_tick(false);
#if HAS_FOLDER_WORKER
        bool cache_due = g_folders.missing && g_folders.started > 0 &&
            !std::strcmp(g_folders.asked, g_folders.request) && clock_ns >= g_folders.check_at;
        if (cache_due && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
            continue;
        }
#else
        (void)deadline;
#endif
        return waiting;
    }
}
static void start_request() {
    check(menu_tick() == 1 && reads == 1 && launches == 0, "settle request before launch");
    clock_ns += 1500000000ull;
    check(menu_tick() == 0 && launches == 1, "launch once after settling");
    if (prepare_ok) check(await_result(missing), "worker publishes initial missing result");
}

int main() {
    fresh();
    fail_spawn = true;
    menu_tick();
    advance(1500000000ull);
    menu_tick();
#if HAS_FOLDER_WORKER
    note_is(UI_MENU_SHAPE_PREPARE_FAILED, false, "worker launch failure exposes existing retry");
#endif
    fail_spawn = false;
    folders_start();
    check(await_result(missing), "retry can start the completion worker");
    missing = 0;
    advance(1000000000ull);
    check(await_result(0), "retried worker observes readiness");
    menu_tick();
    note_is(UI_MENU_PROMPTS_RESTART, true, "worker retry enables restart");

    fresh();
    start_request();
    note_is(UI_MENU_PROMPTS_PREPARING, false, "pending generation reports preparing");
    int initial_reads = reads;
    for (int i = 0; i < 120; i++) { advance(5000000ull); menu_tick(); }
    check(reads == initial_reads && launches == 1, "120 menu frames do not poll early or duplicate launch");
    missing = ROMFS_NEED_SHAPED_PROMPTS;
    advance(400000000ull);
    check(await_result(missing), "worker publishes partial completion");
    menu_tick();
    check(reads == initial_reads + 1 && g_folders.missing == missing && launches == 1,
          "one-second check observes partial completion of missing bits");
    note_is(UI_MENU_PROMPTS_PREPARING, false, "partial completion still waits for shaped prompts");
    missing = 0;
    advance(1000000000ull);
    check(await_result(0), "worker publishes full completion");
    menu_tick();
    check(version == 0 && g_folders.missing == 0 && launches == 1, "all bits clear without configuration change");
    note_is(UI_MENU_PROMPTS_RESTART, true, "completed generation offers restart");
    int complete_reads = reads;
    advance(10000000000ull);
    menu_tick();
    check(reads == complete_reads && launches == 1, "completed request stops polling");

    fresh();
    check(menu_tick() == 1, "request waits before menu closes");
    check(folders_tick(true) == 0 && launches == 1, "closing starts waiting request without keeping pending frames");
    check(await_result(missing), "closed-menu worker starts watching");
    initial_reads = reads;
    missing = 0;
    advance(2000000000ull);
    check(await_result(0), "worker finishes while menu closed");
    initial_reads = reads;
    for (int i = 0; i < 120; i++) folders_tick(true);
    check(reads == initial_reads && launches == 1, "closed-menu frames never poll completion");
    menu_tick();
    note_is(UI_MENU_PROMPTS_RESTART, true, "reopening observes completed folders");

    fresh();
    prepare_ok = 0;
    start_request();
    note_is(UI_MENU_SHAPE_PREPARE_FAILED, false, "launch failure preserves retry state");
    initial_reads = reads;
    clock_ns += 2000000000ull;
    menu_tick();
    check(reads == initial_reads && launches == 1, "failed launch does not poll or automatically retry");
    prepare_ok = 1;
    folders_start(); // the production retry button calls this same helper
    check(launches == 2, "explicit retry launches again");
    missing = 0;
    advance(1000000000ull);
    check(await_result(0), "retry worker publishes completion");
    menu_tick();
    note_is(UI_MENU_PROMPTS_RESTART, true, "successful retry completion offers restart");

    fresh();
    start_request();
    choice = "playstation";
    version++;
    menu_tick();
    check(launches == 1 && folders_state(7) == 1, "selection change settles a separate request");
    clock_ns += 1500000000ull;
    menu_tick();
    check(launches == 2, "new selection launches once");
    check(await_result(missing), "new request has its own worker result");
    missing = 0;
    advance(1000000000ull);
    check(await_result(0), "new selection completion published");
    menu_tick();
    note_is(UI_MENU_PROMPTS_RESTART, true, "completion belongs to new selection");

    fresh();
    start_request();
    choice = "playstation";
    version++;
    menu_tick();
    choice = "pc";
    version++;
    menu_tick();
    check(launches == 1 && await_result(missing), "A to B to A resumes watcher without duplicate child");
    missing = 0;
    advance(1000000000ull);
    check(await_result(0), "resumed A watcher publishes completion");
    menu_tick();
    note_is(UI_MENU_PROMPTS_RESTART, true, "returning selection can complete");

#if HAS_FOLDER_WORKER
    fresh();
    start_request();
    {
        std::lock_guard<std::mutex> lock(read_gate);
        block_read = true;
        read_entered = release_read = false;
    }
    missing = 0;
    advance(1000000000ull);
    {
        std::unique_lock<std::mutex> lock(read_gate);
        check(read_wake.wait_for(lock, std::chrono::seconds(3), [] { return read_entered; }),
              "old request completion read is in flight");
    }
    missing = 7;
    choice = "playstation";
    version++;
    menu_tick();
    advance(1500000000ull);
    menu_tick();
    {
        std::lock_guard<std::mutex> lock(read_gate);
        release_read = true;
        read_wake.notify_all();
    }
    check(await_result(7), "new request rejects old in-flight completed result");
    menu_tick();
    note_is(UI_MENU_PROMPTS_PREPARING, false, "stale completion cannot enable restart for new selection");
#endif

    fresh();
    can_prepare = 0;
    check(menu_tick() == 0 && launches == 0, "missing tools do not queue a child");
    note_is(UI_MENU_PROMPTS_MISSING, false, "missing tools preserve missing-folder explanation");
#if HAS_FOLDER_WORKER
    check(worker_starts == 1, "requests share one persistent worker");
    stop_worker = true;
    plat_mutex_lock(&g_folder_check.lock);
    plat_cond_signal(&g_folder_check.wake);
    plat_mutex_unlock(&g_folder_check.lock);
    worker.join();
#endif
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
