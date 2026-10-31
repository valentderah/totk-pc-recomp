// Cheats of an Atmosphere cheat file (cheats.c): cheats\<build id>.txt next to totk.exe, switched from the menu's
// Cheats tab. A cheat that writes the game's instructions is a code patch set that tools/cheats.py built into the game
// (switched with rt_patch_switch); one that writes data runs in the cheat VM every presented frame.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHEAT_HEADING,       // no opcodes: a title or a group label
    CHEAT_CODE,          // a patch set
    CHEAT_KEY,           // a patch set on while the buttons are held
    CHEAT_DATA,          // the cheat VM
    CHEAT_MASTER,        // {master}: always on
    CHEAT_RESTORE,       // writes the original instructions back: nothing to do
    CHEAT_REBUILD,       // writes instructions, but the game was built without this version of it
    CHEAT_UNSUPPORTED,   // note says why
} CheatKind;

typedef struct {
    const char *name;     // UTF-8, as in the file
    const char *note;     // why it is unsupported ("" otherwise)
    CheatKind kind;
    uint32_t buttons;     // CHEAT_KEY: HidNpadButton mask
    char key[24];         // its setting, TOTK_CHEAT_<name hash>
} CheatInfo;

void cheats_init(void);                    // after rt_patches_init: reads the file and its build status
int cheats_count(void);
const CheatInfo *cheats_info(int i);
int cheats_on(int i);                      // the player's switch (1 for a master cheat)
void cheats_set(int i, int on);            // switches at once and sets the cheat's setting (the menu saves totk.ini)
const char *cheats_file(void);             // "cheats\<build id>.txt"
// Every presented frame (the present path): the data cheats' VM and the button state of CHEAT_KEY cheats.
void cheats_frame(uint64_t frame);
// The button names of a mask ("ZL + Right stick left"), into buf.
const char *cheats_buttons(uint32_t mask, char *buf, int n);

#ifdef __cplusplus
}
#endif
