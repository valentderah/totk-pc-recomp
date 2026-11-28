// The build's record of what a mod plugin's init writes into the game modules (patches.h RT_PLUGIN_WRITES, from the
// `expect` lines of recomp/patches.py), and the lookup plugin_init.c check_writes makes for every word an init changed
// at the start of the game.
#pragma once
#include <stdint.h>
#include <string.h>

// any: the expect line said `* *`, a word of main's .data or .bss that shapes no lifted code (tools/exefs_plugins.py
// plugin_lines). The game may hold another value there at play than in the service run (Zonai Fieldworks' Self Recall
// takes its storage off a pool size that grows with the resolution's texture heap), so it matches by place only. A
// hook branch in .text and a pointer into a plugin's .text match by place and both words.
typedef struct {
    int plugin;
    const char *module;
    uint64_t off;
    uint32_t old, now;
    int any;
} PluginWrite;

// The X of RT_PLUGIN_WRITES for a table of PluginWrite.
#define PLUGIN_WRITE(p, mod, off, old, now, any) {p, mod, off, old, now, any},

// The index of the entry of plugin that records the write of module+off from old to now, or -1.
static inline int plugin_write_find(const PluginWrite *tab, int n, int plugin, const char *module, uint64_t off,
                                    uint32_t old, uint32_t now) {
    for (int k = 0; k < n; k++)
        if (tab[k].plugin == plugin && tab[k].off == off && !strcmp(tab[k].module, module) &&
            (tab[k].any || (tab[k].old == old && tab[k].now == now)))
            return k;
    return -1;
}
