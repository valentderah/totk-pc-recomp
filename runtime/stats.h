// Statistics and achievements read from the game's save (stats.c), for the Achievements tab (ui/menu.cpp).
#pragma once

#include <stdint.h>

#define STATS_FAMILIES 18   // monster families with a defeat count of their own (stats.c kFamilies)
#define STATS_ACH_MAX 96

typedef enum {
    STATS_KILLS, STATS_KINDS, STATS_FAMILY, STATS_WEAPONS, STATS_ARMOR, STATS_SETS, STATS_ITEMS, STATS_COMPENDIUM,
    STATS_HEARTS, STATS_STAMINA
} StatsKind;

typedef struct {
    StatsKind kind;
    int family;          // STATS_FAMILY: index into StatsView.family
    uint32_t target, progress;
    uint64_t unlocked;   // Unix time of the unlock, 0 while locked
} StatsAch;

typedef struct {
    int valid;           // 1 once a save was read
    uint64_t when;       // the save's write time, Unix seconds
    uint32_t kills, kinds, kinds_max;
    struct { uint32_t count; char name[64]; } family[STATS_FAMILIES];   // name: the game's, in its language
    uint32_t weapons, weapons_max, armor, armor_max, sets, sets_max, items, items_max, compendium, compendium_max;
    uint32_t hearts;
    float stamina_wheels;
    int nach;
    StatsAch ach[STATS_ACH_MAX];
} StatsView;

void stats_init(void);              // the reading thread; game runs only (main.c)
void stats_save_changed(void);      // fs_host.c: a save commit changed files
void stats_view(StatsView *out);    // a copy of the latest statistics
