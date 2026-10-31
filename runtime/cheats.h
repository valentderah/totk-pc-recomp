// Cheats compiled in by gen.py --cheats (recomp/cheats.py), switched while the game runs (runtime/patches.c). The
// settings key TOTK_CHEATS lists the ids that are on.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

int rt_cheat_count(void);            // cheats in this build (0 without --cheats)
const char *rt_cheat_id(int i);      // the id totk.ini lists ("inf_equipment_durability")
const char *rt_cheat_name(int i);    // the name of the cheat file, UTF-8
int rt_cheat_sites(int i);           // instructions it changes
int rt_cheat_on(int i);
void rt_cheat_toggle(int i);         // flips it and rewrites TOTK_CHEATS (the caller saves totk.ini)
void rt_cheats_sync(void);           // the sets follow TOTK_CHEATS after a settings change; cheap when nothing changed

#ifdef __cplusplus
}
#endif
