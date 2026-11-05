// Importing a .cai or .txt blueprint into the game's own Autobuild history, so the creation appears in the native
// Autobuild page as the newest draft and builds with the native preview, rotation and Zonai costs.
//
// The history is one save flag: the BinaryArray 0xa56722b6 (AutoBuilder.Draft.Content.CombinedActorInfo), 30 items of
// {u32 size = 6688; CmbAct blob}. The Autobuild page draws its cards in the order the items sit in that array, so item 0
// is the card on top and item 29 the one at the bottom.
//
// An import writes item 0. The array keeps its 30 items, so the save never grows and no layout changes; the card that
// was on top slides down one position and the drafts below it keep their data. Deleting the game's cached draft pictures
// (<profile>/cache/<i>/icon/Draft_NNN.png) makes it draw the cards again, which is what makes the new creation visible
// on the open page instead of after a reload.
#pragma once

#include <stdbool.h>

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
    BP_IMPORT_NO_ORDER,  // unused: kept so existing result texts stay stable
    BP_IMPORT_BAD_SLOT,  // the array names an entry outside the list the save holds
    BP_IMPORT_NO_ROOM,   // the save data could not be written in guest memory (logging says why)
};

// The live draft list, for the page to draw. actors[i] is the actor count of entry i, 0 when empty or not read. top is
// the entry the Autobuild page draws first (the one an import replaces), which the save's Index array decides.
typedef struct {
    int slots;
    int actors[BP_SLOTS];
    int top;
} BpFavorites;

int bp_favorites(BpFavorites *out);

// Adds the blueprint in the file at path as the newest draft of the Autobuild history. The write happens at once, in
// guest memory; the caller must not call it while the game saves (it would be harmless but the game may overwrite it
// with the same data). The page calls it only when the player presses its import row.
int bp_import_newest(const char *path);

// A short text for a result, for the page.
const char *bp_import_text(int result);

#ifdef __cplusplus
}
#endif
