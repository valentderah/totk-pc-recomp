// Short out-of-memory spells: a failed allocation waits for memory instead of ending the game.
//
// Windows refuses commit beyond the current limit while it grows a system-managed page file, and on some drivers device
// memory counts as process commit (vk_memory.c vk_mem_host_probe), so for a moment every allocation of the process can
// fail and the same request succeeds a little later. The failure sites (memory.c commit, vk_memory.c vk_staging,
// vk_texture.c create_tex, vk_exec_draw.c transient chunks, nvn_api_cmd.c realloc_wait) ask the Vulkan back end to give
// memory back (rt_oom_request: released textures and headroom blocks, at its next submit, where its locks are free) and
// retry for up to OOM_WAIT_MS before they end the game.
//
// TOTK_TEST_OOM=<site>:<n>[,<site>:<n>...] (sites commit, staging, image, chunk) fails the first n attempts at a site, so
// the wait can be tested without running out of memory; the probes arm it with rt_oom_force.
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"

#define OOM_WAIT_MS 30000u
#define OOM_STEP_MS 10u
#define OOM_REPORTS 8
#define OOM_REQUEST_EVERY 100   // retries between two release requests: a second, since a release only frees what is old enough

volatile int32_t rt_oom_req;

static const char *const site_name[RT_OOM_SITES] = {"commit", "staging", "image", "chunk"};
static volatile int32_t force_left[RT_OOM_SITES];
static volatile int32_t forced_parsed;
static PlatOnce force_once = PLAT_ONCE_INIT;

static void force_parse(void *arg) {
    (void)arg;
    const char *s = cfg_get("TOTK_TEST_OOM");
    while (s && *s) {
        const char *colon = strchr(s, ':'), *end = strchr(s, ',');
        if (!end) end = s + strlen(s);
        if (colon && colon < end) {
            for (int i = 0; i < RT_OOM_SITES; i++)
                if ((size_t)(colon - s) == strlen(site_name[i]) && !strncmp(s, site_name[i], (size_t)(colon - s))) {
                    int n = atoi(colon + 1);
                    __atomic_store_n(&force_left[i], n > 0 ? n : 0, __ATOMIC_RELAXED);
                    rt_log("oom: TOTK_TEST_OOM fails the first %d %s attempts", n, site_name[i]);
                }
        }
        s = *end ? end + 1 : end;
    }
    __atomic_store_n(&forced_parsed, 1, __ATOMIC_RELEASE);
}

int rt_oom_forced(int site) {
    if (!__atomic_load_n(&forced_parsed, __ATOMIC_ACQUIRE)) plat_once(&force_once, force_parse, NULL);
    if (__atomic_load_n(&force_left[site], __ATOMIC_RELAXED) <= 0) return 0;
    return plat_dec32(&force_left[site]) >= 0;
}

void rt_oom_force(int site, int n) {
    if (!__atomic_load_n(&forced_parsed, __ATOMIC_ACQUIRE)) plat_once(&force_once, force_parse, NULL);
    __atomic_store_n(&force_left[site], n, __ATOMIC_RELAXED);
}

int rt_oom_forced_left(int site) {
    int n = __atomic_load_n(&force_left[site], __ATOMIC_RELAXED);
    return n > 0 ? n : 0;
}

void rt_oom_request(void) { __atomic_store_n(&rt_oom_req, 1, __ATOMIC_RELEASE); }

int rt_oom_wait(const char *what, uint64_t bytes, int (*retry)(void *arg), void *arg) {
    rt_oom_request();
    static volatile int32_t reports;
    int report = plat_inc32(&reports) <= OOM_REPORTS;
    double proc, used, limit;
    if (report) {
        plat_commit_info(&proc, &used, &limit);
        rt_log("%s of %llu bytes failed (memory commit: this process %.0f MB, the system %.0f of %.0f MB); waiting up "
               "to %u s for memory", what, (unsigned long long)bytes, proc, used, limit, OOM_WAIT_MS / 1000);
    }
    uint64_t t0 = plat_now_ms();
    for (int n = 1; plat_now_ms() - t0 < OOM_WAIT_MS; n++) {
        plat_sleep_ms(OOM_STEP_MS);
        if (retry(arg)) {
            if (report)
                rt_log("%s of %llu bytes succeeded after %llu ms", what, (unsigned long long)bytes,
                       (unsigned long long)(plat_now_ms() - t0));
            return 1;
        }
        if (n % OOM_REQUEST_EVERY == 0) rt_oom_request();
    }
    plat_commit_info(&proc, &used, &limit);
    rt_log("%s of %llu bytes still failed after %u s (memory commit: this process %.0f MB, the system %.0f of %.0f MB)",
           what, (unsigned long long)bytes, OOM_WAIT_MS / 1000, proc, used, limit);
    return 0;
}

typedef struct { void *old, *p; size_t n; } Realloc;
static int realloc_try(void *arg) {
    Realloc *r = arg;
    return (r->p = realloc(r->old, r->n)) != NULL;
}

void *rt_realloc_wait(void *old, size_t n, const char *what) {
    void *p = realloc(old, n);
    if (p || !n) return p;
    Realloc r = {old, NULL, n};
    return rt_oom_wait(what, n, realloc_try, &r) ? r.p : NULL;
}
