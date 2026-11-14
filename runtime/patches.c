// Code patch sets (recomp/patches.py, gen.py --patches). The generator lifts both the original and the replacement
// instructions of every patched site, and rt_patch_on[set] picks one when the site runs, so a set is switched at start
// without touching code, and also while the game runs (rt_patch_switch: the frame-limit setting of the menu). The sets,
// their environment switches and defaults, and the patch page come from the generated patches.h; without it (C
// generated without --patches) there are no sets and every site runs the original.
//
// The patch page is the first 4 KiB page after main's image, inside the module window memory.c reserved: replacement
// code reaches it with one adrp, as MacNativeGames' data page after main (games/totk/runtime/game_patches.c). Its
// layout belongs to the sets that use it (frame_rate.c: +0 the 30 Hz ratio, +4 the frame step in seconds, +8 the
// step in 60 Hz ticks).
//
// Host hooks (recomp/patches.py `hook` lines, patches.h RT_HOOKS): rt_patches_init binds each to its handler
// (plugins.c rt_hook_bind), and a hook site calls rt_hook. A hook whose handler this runtime lacks does nothing; its
// sites still run their own instructions.
// Probe modes never call rt_patches_init: every set stays off.
#include "cfg.h"
#include "platform/platform.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

#if __has_include("patches.h")
#include "patches.h"
#endif
#ifndef RT_PATCH_COUNT
#define RT_PATCH_COUNT 0
#endif
#ifndef RT_HOOK_COUNT
#define RT_HOOK_COUNT 0
#endif

uint8_t rt_patch_on[RT_PATCH_COUNT > 0 ? RT_PATCH_COUNT : 1];   // read by the generated C at every patched site

#if RT_HOOK_COUNT
static RtHook hooks[RT_HOOK_COUNT] = {
#define X(i, n, h, k, a0, a1, a2, a3) {n, h, k, {a0, a1, a2, a3}, NULL, NULL, 0},
    RT_HOOKS(X)
#undef X
};

static void hook_unbound(Cpu *c, const RtHook *h) {
    (void)c;
    (void)h;
}
#endif

void rt_hook(Cpu *c, int hook, uint64_t site) {
#if RT_HOOK_COUNT
    if (hook >= 0 && hook < RT_HOOK_COUNT && hooks[hook].fn) {
        RtHook *h = &hooks[hook];
        if (!__atomic_fetch_add(&h->calls, 1, __ATOMIC_RELAXED)) {
            char at[96];
            rt_log("hooks: %s (%s) runs for the first time, at %s", h->name, h->handler,
                   ldr_describe(site, at, sizeof at));
        }
        h->fn(c, h);
        return;
    }
#endif
    (void)c;
    rt_fatal("rt_hook: hook %d at %#llx is not bound in this build", hook, (unsigned long long)site);
}

#if RT_PATCH_COUNT
static const struct { const char *name, *env; int on; } sets[] = {
#define X(i, n, e, d) {#n, e, d},
    RT_PATCH_SETS(X)
#undef X
};
static void *page;
#endif

void rt_patches_init(void) {
#if RT_PATCH_COUNT
    if (!plat_mem_commit(RT_PATCH_PAGE, 0x1000))
        rt_fatal("patches: cannot commit the patch page at %#llx", RT_PATCH_PAGE);
    page = (void *)(uintptr_t)RT_PATCH_PAGE;
    char list[512];
    size_t n = 0;
    list[0] = 0;
    for (int i = 0; i < RT_PATCH_COUNT; i++) {
        const char *e = cfg_get(sets[i].env);
        // a set with a row in the settings table takes its default from there; the generated default serves the rest
        rt_patch_on[i] = (uint8_t)(cfg_meta(sets[i].env) ? cfg_bool(sets[i].env) : e && *e ? strcmp(e, "0") != 0 : sets[i].on);
        n += (size_t)snprintf(list + n, sizeof list - n, "%s%s %s", i ? ", " : "", sets[i].name,
                              rt_patch_on[i] ? "on" : "off");
        if (n >= sizeof list) n = sizeof list - 1;
    }
    rt_log("patches: %s (page %#llx)", list, RT_PATCH_PAGE);
#endif
#if RT_HOOK_COUNT
    int bound = 0;
    for (int i = 0; i < RT_HOOK_COUNT; i++) {
        if (rt_hook_bind(&hooks[i])) bound++;
        else hooks[i].fn = hook_unbound;
    }
    rt_log("patches: %d hooks, %d of them with a handler in this runtime", RT_HOOK_COUNT, bound);
#endif
}

int rt_patch_set(const char *name) {
#if RT_PATCH_COUNT
    for (int i = 0; i < RT_PATCH_COUNT; i++)
        if (!strcmp(sets[i].name, name)) return i;
#endif
    (void)name;
    return -1;
}

int rt_patch_enabled(int set) { return set >= 0 && set < RT_PATCH_COUNT && rt_patch_on[set]; }

void rt_patch_disable(int set) {
    if (set >= 0 && set < RT_PATCH_COUNT) rt_patch_on[set] = 0;
}

void rt_patch_switch(int set, int on) {
    if (set >= 0 && set < RT_PATCH_COUNT) __atomic_store_n(&rt_patch_on[set], (uint8_t)(on != 0), __ATOMIC_RELAXED);
}

void *rt_patch_page(void) {
#if RT_PATCH_COUNT
    return page;
#else
    return NULL;
#endif
}
