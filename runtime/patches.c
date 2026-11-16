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
// Contested sites: a site that two or more ordered sets patch (the mods' ExeFS patch files, tools/exefs_patches.py)
// tests one byte, rt_patch_pick[site], instead of each set's switch. rt_patch_picks fills it from the sets that are on
// and the ranks their owner gave them (mods.c: the Mods tab's position), so the load order decides which mod's code
// runs there, as it decides RomFS files and data writes. Generic over what owns a set: a part kind that patches code
// (stage 2's plugin hooks) ranks its sets the same way.
// Probe modes never call rt_patches_init: every set stays off.
#include "cfg.h"
#include "platform/platform.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "rsf_frame.h"

#if __has_include("patches.h")
#include "patches.h"
#endif
#ifndef RT_PATCH_COUNT
#define RT_PATCH_COUNT 0
#endif
#ifndef RT_PATCH_PICK_COUNT
#define RT_PATCH_PICK_COUNT 0
#endif

#ifdef RT_PATCH_PAGE
__attribute__((constructor)) static void register_rsf_frame_hooks(void) {
    if (rt_game_version != 143) return;
    rt_register_host(RT_PATCH_PAGE + 0xf50, rsf_frame_calculation_begin);
    rt_register_host(RT_PATCH_PAGE + 0xf40, rsf_frame_calculation_end);
}
#endif

uint8_t rt_patch_on[RT_PATCH_COUNT > 0 ? RT_PATCH_COUNT : 1];   // read by the generated C at every patched site
uint8_t rt_patch_pick[RT_PATCH_PICK_COUNT > 0 ? RT_PATCH_PICK_COUNT : 1];   // read at every contested site

#if RT_PATCH_COUNT
static const struct { const char *name, *env; int on; } sets[] = {
#define X(i, n, e, d) {#n, e, d},
    RT_PATCH_SETS(X)
#undef X
};
static void *page;
#endif
#if RT_PATCH_PICK_COUNT
#define X(i, m, o, ...) static const int16_t pick_sets_##i[] = {__VA_ARGS__};
RT_PATCH_PICKS(X)
#undef X
static const RtPatchPick gen_picks[] = {
#define X(i, m, o, ...) {m, o, pick_sets_##i, (int)(sizeof pick_sets_##i / sizeof *pick_sets_##i)},
    RT_PATCH_PICKS(X)
#undef X
};
#else
static const RtPatchPick *const gen_picks = NULL;
#endif
static uint16_t gen_rank[RT_PATCH_COUNT > 0 ? RT_PATCH_COUNT : 1];   // rank + 1; 0 unranked

// The table in use: the generated one, or --probe-mods' own (rt_patch_probe_table).
static int nset = RT_PATCH_COUNT, npick = RT_PATCH_PICK_COUNT;
static uint8_t *on = rt_patch_on, *pick = rt_patch_pick;
static uint16_t *rank = gen_rank;
static const RtPatchPick *picks = gen_picks;
static const char *const *probe_names;

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
    rt_patch_picks();   // a set the environment switched on runs at its sites, unranked
#endif
}

const char *rt_patch_name(int set) {
    if (set < 0 || set >= nset) return NULL;
    if (probe_names) return probe_names[set];
#if RT_PATCH_COUNT
    return sets[set].name;
#else
    return NULL;
#endif
}

int rt_patch_set(const char *name) {
    for (int i = 0; i < nset; i++)
        if (!strcmp(rt_patch_name(i), name)) return i;
    return -1;
}

int rt_patch_enabled(int set) { return set >= 0 && set < nset && on[set]; }

// The place in p's sets of the one that runs: on, the lowest rank, a tie to the later one; -1 when none is on.
static int winner(const RtPatchPick *p) {
    int best = -1;
    unsigned best_key = 0;
    for (int j = 0; j < p->nsets; j++) {
        int s = p->sets[j];
        if (s < 0 || s >= nset || !on[s]) continue;
        unsigned key = rank[s] ? rank[s] : 0x10000u;
        if (best < 0 || key <= best_key) best = j, best_key = key;
    }
    return best;
}

// The picks of the contested sites set touches (every site for -1).
static void repick(int set) {
    for (int i = 0; i < npick; i++) {
        int has = set < 0;
        for (int j = 0; !has && j < picks[i].nsets; j++) has = picks[i].sets[j] == set;
        if (has) __atomic_store_n(&pick[i], (uint8_t)(winner(&picks[i]) + 1), __ATOMIC_RELAXED);
    }
}

void rt_patch_disable(int set) {
    if (set >= 0 && set < nset) on[set] = 0;
    repick(set);
}

void rt_patch_switch(int set, int value) {
    if (set < 0 || set >= nset) return;
    __atomic_store_n(&on[set], (uint8_t)(value != 0), __ATOMIC_RELAXED);
    repick(set);
}

void rt_patch_rank(int set, int r) {
    if (set >= 0 && set < nset) rank[set] = (uint16_t)(r < 0 ? 1 : r > 0xFFFE ? 0xFFFF : r + 1);
}

void rt_patch_picks(void) { repick(-1); }
int rt_patch_pick_count(void) { return npick; }
const RtPatchPick *rt_patch_pick_at(int i) { return i >= 0 && i < npick ? &picks[i] : NULL; }
int rt_patch_pick_winner(int i) { return i >= 0 && i < npick ? (int)__atomic_load_n(&pick[i], __ATOMIC_RELAXED) - 1 : -1; }

void rt_patch_probe_table(const char *const *names, int n, const RtPatchPick *p, int np) {
    if (probe_names) {
        free(on);
        free(pick);
        free(rank);
    }
    probe_names = NULL;
    nset = RT_PATCH_COUNT, npick = RT_PATCH_PICK_COUNT;
    on = rt_patch_on, pick = rt_patch_pick, rank = gen_rank, picks = gen_picks;
    if (!names) return;
    on = calloc((size_t)n + 1, 1), pick = calloc((size_t)np + 1, 1), rank = calloc((size_t)n + 1, sizeof *rank);
    probe_names = names, nset = n, npick = np, picks = p;
}

void *rt_patch_page(void) {
#if RT_PATCH_COUNT
    return page;
#else
    return NULL;
#endif
}
