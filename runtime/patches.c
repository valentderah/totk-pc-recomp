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
// Probe modes never call rt_patches_init: every set stays off.
#include "cfg.h"
#include "platform/platform.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cheats.h"
#include "host.h"

#if __has_include("patches.h")
#include "patches.h"
#endif
#ifndef RT_PATCH_COUNT
#define RT_PATCH_COUNT 0
#endif

uint8_t rt_patch_on[RT_PATCH_COUNT > 0 ? RT_PATCH_COUNT : 1];   // read by the generated C at every patched site

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
        const char *e = *sets[i].env ? cfg_get(sets[i].env) : NULL;   // a cheat's set has no switch of its own
        // a set with a row in the settings table takes its default from there; the generated default serves the rest
        rt_patch_on[i] = (uint8_t)(*sets[i].env && cfg_meta(sets[i].env) ? cfg_bool(sets[i].env)
                                   : e && *e ? strcmp(e, "0") != 0 : sets[i].on);
        n += (size_t)snprintf(list + n, sizeof list - n, "%s%s %s", i ? ", " : "", sets[i].name,
                              rt_patch_on[i] ? "on" : "off");
        if (n >= sizeof list) n = sizeof list - 1;
    }
    rt_log("patches: %s (page %#llx)", list, RT_PATCH_PAGE);
#endif
    rt_cheats_sync();
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

// ---- Cheats (gen.py --cheats, recomp/cheats.py): patch sets without a switch of their own. totk.ini keeps the ids of
// those that are on, "cheats = <id>, <id>"; rt_cheats_sync makes the sets follow that key whenever the settings change
// (at start, after a menu row, after the menu's Reset), so the key alone decides. A site reads its switch every time it
// runs, so a cheat takes effect at once.
#ifndef RT_CHEAT_COUNT
#define RT_CHEAT_COUNT 0
#endif
#if RT_CHEAT_COUNT
static const struct { int set; const char *id, *name; int sites; } cheats[] = {
#define C(k, i, n, s) {k, i, n, s},
    RT_CHEATS(C)
#undef C
};
#endif
static unsigned cheats_seen = ~0u;
static PlatMutex cheat_lock = PLAT_MUTEX_INIT;

// Whether the comma or space separated list names id (case-insensitive).
static int listed(const char *list, const char *id) {
    size_t n = strlen(id);
    for (const char *p = list ? list : ""; *p;) {
        p += strspn(p, ", \t");
        size_t k = strcspn(p, ", \t");
        if (k == n && !strncasecmp(p, id, n)) return 1;
        p += k;
    }
    return 0;
}

int rt_cheat_count(void) { return RT_CHEAT_COUNT; }

#if RT_CHEAT_COUNT
#define CHEAT(i, field, none) ((i) >= 0 && (i) < RT_CHEAT_COUNT ? cheats[i].field : (none))
#else
#define CHEAT(i, field, none) ((void)(i), (none))
#endif
const char *rt_cheat_id(int i) { return CHEAT(i, id, ""); }
const char *rt_cheat_name(int i) { return CHEAT(i, name, ""); }
int rt_cheat_sites(int i) { return CHEAT(i, sites, 0); }
int rt_cheat_on(int i) { return rt_patch_enabled(CHEAT(i, set, -1)); }

void rt_cheats_sync(void) {
    unsigned v = cfg_version();
    if (__atomic_load_n(&cheats_seen, __ATOMIC_RELAXED) == v) return;
    plat_mutex_lock(&cheat_lock);
    cheats_seen = v;
    const char *list = cfg_get("TOTK_CHEATS");
    for (int i = 0; i < RT_CHEAT_COUNT; i++) {
        int on = listed(list, rt_cheat_id(i));
        if (on == rt_cheat_on(i)) continue;
        rt_patch_switch(CHEAT(i, set, -1), on);
        rt_log("cheats: %s %s", rt_cheat_id(i), on ? "on" : "off");
    }
    plat_mutex_unlock(&cheat_lock);
}

void rt_cheat_toggle(int i) {
    if (i < 0 || i >= RT_CHEAT_COUNT) return;
    const char *id = rt_cheat_id(i), *old = cfg_get("TOTK_CHEATS");
    int on = !listed(old, id);
    // the new list: the old one without id (ids of cheats this build lacks stay), id appended when it goes on
    char list[1024];
    size_t n = 0;
    list[0] = 0;
    for (const char *p = old ? old : ""; *p;) {
        p += strspn(p, ", \t");
        size_t k = strcspn(p, ", \t");
        if (k && !(k == strlen(id) && !strncasecmp(p, id, k)) && n + k + 3 < sizeof list)
            n += (size_t)snprintf(list + n, sizeof list - n, "%s%.*s", n ? ", " : "", (int)k, p);
        p += k;
    }
    if (on && n + strlen(id) + 3 < sizeof list) snprintf(list + n, sizeof list - n, "%s%s", n ? ", " : "", id);
    if (list[0]) {
        cfg_set("TOTK_CHEATS", list);
    } else {
        const char *key = "CHEATS";
        cfg_reset(&key, 1);
    }
    rt_cheats_sync();
}
