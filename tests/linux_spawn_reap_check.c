// Real short-lived generator children exercise production Linux detached spawning without game files.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
static int fail_reaper_thread;
static int fixture_pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
                                  void *(*fn)(void *), void *arg) {
    if (fail_reaper_thread) return EAGAIN;
    return pthread_create(thread, attributes, fn, arg);
}
#define pthread_create fixture_pthread_create
#include "platform/linux.c"
#undef pthread_create

static int checks, failures;
#define CHECK(condition, name) do { \
    checks++; \
    if (!(condition)) { failures++; printf("FAIL Linux generator reaping: %s\n", name); } \
} while (0)

typedef struct { pid_t pid[256]; int count, zombies; } Children;

static Children children(void) {
    Children result = {0};
    DIR *tasks = opendir("/proc/self/task");
    if (!tasks) { fprintf(stderr, "cannot inspect /proc/self/task\n"); exit(2); }
    struct dirent *task;
    while ((task = readdir(tasks))) {
        if (task->d_name[0] < '0' || task->d_name[0] > '9') continue;
        char path[512];
        snprintf(path, sizeof path, "/proc/self/task/%s/children", task->d_name);
        FILE *list = fopen(path, "r");
        if (!list) continue;   // a reaper thread can finish while the task list is being inspected
        int value;
        while (fscanf(list, "%d", &value) == 1) {
            int duplicate = 0;
            for (int i = 0; i < result.count; i++) if (result.pid[i] == value) duplicate = 1;
            if (duplicate || result.count == 256) continue;
            char stat_path[64], stat[1024];
            snprintf(stat_path, sizeof stat_path, "/proc/%d/stat", value);
            FILE *file = fopen(stat_path, "r");
            if (!file) continue;
            size_t n = fread(stat, 1, sizeof stat - 1, file);
            fclose(file);
            stat[n] = 0;
            char *end = strrchr(stat, ')');
            if (!end || end[1] != ' ' || !end[2]) continue;
            result.pid[result.count++] = (pid_t)value;
            result.zombies += end[2] == 'Z';
        }
        fclose(list);
    }
    closedir(tasks);
    return result;
}

static int file_exists_in(const char *dir, const char *name) {
    char path[2048];
    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int)sizeof path) return 0;
    return access(path, F_OK) == 0;
}

static void cleanup_observed_zombies(Children observed) {
    // Observation must happen first: waiting here earlier would hide the production resource leak.
    for (int i = 0; i < observed.count; i++) {
        int status;
        pid_t waited;
        do waited = waitpid(observed.pid[i], &status, WNOHANG);
        while (waited < 0 && errno == EINTR);
    }
}

static Children wait_until_reaped(void) {
    uint64_t end = plat_now_ms() + 500;
    Children last;
    do {
        last = children();
        if (!last.count) break;
        plat_sleep_ms(5);
    } while (plat_now_ms() < end);
    return last;
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(!children().count, "fixture starts without children");
    int launched = 0;
    for (int i = 0; i < 12; i++) {
        char script[128];
        snprintf(script, sizeof script, "printf marker > marker-%02d", i);
        const char *command[] = {"/bin/sh", "-c", script, NULL};
        launched += plat_spawn(command, argv[1]) != 0;
    }
    CHECK(launched == 12, "all twelve synthetic generator children start");
    uint64_t end = plat_now_ms() + 2000;
    int completed;
    do {
        completed = 0;
        for (int i = 0; i < 12; i++) {
            char name[32]; snprintf(name, sizeof name, "marker-%02d", i);
            completed += file_exists_in(argv[1], name);
        }
        if (completed == 12) break;
        plat_sleep_ms(5);
    } while (plat_now_ms() < end);
    CHECK(completed == 12, "all twelve generators write their completion markers");
    Children observed = wait_until_reaped();
    printf("short generator children remaining: %d, zombies: %d\n", observed.count, observed.zombies);
    CHECK(!observed.count && !observed.zombies, "completed generators leave no child process or zombie");
    cleanup_observed_zombies(observed);

    const char *missing[] = {"/definitely-missing-totk-generator", NULL};
    CHECK(!plat_spawn(missing, argv[1]), "a missing executable reports failure");
    const char *valid[] = {"/bin/sh", "-c", "exit 0", NULL};
    char bad_cwd[2048]; snprintf(bad_cwd, sizeof bad_cwd, "%s/missing-working-directory", argv[1]);
    CHECK(!plat_spawn(valid, bad_cwd), "a missing working directory reports failure");
    observed = wait_until_reaped();
    CHECK(!observed.count, "failed spawns leave no children");
    cleanup_observed_zombies(observed);

    const char *delayed[] = {"/bin/sh", "-c", "sleep 0.3; printf done > delayed-marker", NULL};
    uint64_t start = plat_now_ms();
    int ok = plat_spawn(delayed, argv[1]);
    uint64_t elapsed = plat_now_ms() - start;
    CHECK(ok && elapsed < 200 && !file_exists_in(argv[1], "delayed-marker"),
          "spawning a long generator stays asynchronous");
    end = plat_now_ms() + 2000;
    while (!file_exists_in(argv[1], "delayed-marker") && plat_now_ms() < end) plat_sleep_ms(5);
    CHECK(file_exists_in(argv[1], "delayed-marker"), "the long generator still completes normally");
    observed = wait_until_reaped();
    printf("delayed generator spawn time: %llu ms; remaining children: %d, zombies: %d\n",
           (unsigned long long)elapsed, observed.count, observed.zombies);
    CHECK(!observed.count && !observed.zombies, "the completed long generator is reaped");
    cleanup_observed_zombies(observed);
    fail_reaper_thread = 1;
    const char *unstarted[] = {"/bin/sh", "-c", "printf unexpected > thread-failure-marker", NULL};
    CHECK(!plat_spawn(unstarted, argv[1]), "reaper thread failure reports launch failure");
    fail_reaper_thread = 0;
    plat_sleep_ms(50);
    observed = children();
    CHECK(!observed.count && !file_exists_in(argv[1], "thread-failure-marker"),
          "reaper thread failure starts no child");
    cleanup_observed_zombies(observed);
    printf("Linux generator reaping: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
