// Importing a .cai or .txt blueprint into the game's own Autobuild history, so the creation appears in the native
// Autobuild page as the newest draft and builds with the native preview, rotation and Zonai costs.
//
// The history is one save flag: the BinaryArray 0xa56722b6 (AutoBuilder.Draft.Content.CombinedActorInfo), 30 items of
// {u32 size = 6688; CmbAct blob}. Which item is drawn where is decided by a second array, AutoBuilder.Draft.Content.Index
// (hash 0xd27f8651): 30 ages where age 0 is the card at the top. Both arrays are held in guest memory while the game
// runs, and an import writes both, so the save never grows and no layout changes.
//
// The write follows what the game does for a creation built in game, rather than overwriting the first card: the oldest
// free entry - the one the player would have lost anyway - receives the new blob at age 0, and every card drawn above it
// rises by one. A favourite is never that entry; it moves with the cards above it like any other. bp_rotate_ages is that
// rule. Deleting the game's cached draft pictures (<profile>/cache/<i>/icon/Draft_NNN.png) makes it draw the cards again,
// which is what makes the new creation visible on the open page instead of after a reload.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BP_SLOTS 30   // Autobuild history entries in the save data

// What bp_import_newest returned.
enum BpImport {
    BP_IMPORT_OK = 0,
    BP_IMPORT_NO_FILE,   // the file could not be read
    BP_IMPORT_BAD_FILE,  // blueprint_cai.h rejected it (the log line says why)
    BP_IMPORT_NO_SAVE,   // no loaded save data with an Autobuild draft list (still booting)
    BP_IMPORT_NO_ORDER,  // the save's Index array is not in guest memory, or is not a shape the rotation can use
    BP_IMPORT_NO_ROOM,   // the save data could not be written in guest memory (logging says why)
};

// The history rotation a new creation causes, as the game itself does it: the oldest free entry - the largest age among
// the non-favorites - is evicted and takes the new creation at age 0, and every card drawn above it rises by one. Cards
// below it keep their age. A favorite is therefore never the entry spent, but it does move when it sits above the evicted
// card: keeping its age instead would give two entries the same age as soon as a favorite is above the oldest free card,
// and the result would not be a permutation.
// ages[i] is entry i's place in the display order (0 is the card drawn first) and fav_bits' bit i marks entry i a
// favorite. Fills rotated with the new ages and returns the evicted entry index, or -1 when the input does not fit the
// rule (no free entry, or the ages are not a permutation of 0..BP_SLOTS-1) - in which case nothing must be written.
//
// Pure and side-effect free on purpose: the save-memory writes around it cannot be exercised without a running game, but
// this arithmetic can, and it is the part that decides what the player sees.
int bp_rotate_ages(const int ages[BP_SLOTS], uint32_t fav_bits, int rotated[BP_SLOTS]);

// Adds the blueprint in the file at path as the newest draft of the Autobuild history. The write happens at once, in
// guest memory; the caller must not call it while the game saves (it would be harmless but the game may overwrite it
// with the same data). The page calls it only when the player presses its import row.
int bp_import_newest(const char *path);

// A short text for a result, for the page.
const char *bp_import_text(int result);

#ifdef __cplusplus
}
#endif
