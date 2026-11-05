// Importing a blueprint into the game's own Autobuild favorites (the 30 "favorite" slots of the Autobuild ability),
// so the player can select it in the native Autobuild page and build it with the native preview and Zonai costs.
//
// The favorites are one BinaryArray flag in the loaded save data (progress.sav, hash 0xa56722b6, 30 items of the
// 6688-byte CAI blob). The game keeps that save data in guest memory; writing one item's blob in place is the whole
// import, and the game's own save then persists it. A replacement never changes the item count or the save layout;
// see blueprint_import.c for how the live list is found.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define BP_SLOTS 30   // Autobuild favorite slots in the save data

// What bp_import returned.
enum BpImport {
    BP_IMPORT_OK = 0,
    BP_IMPORT_NO_FILE,   // the file could not be read
    BP_IMPORT_BAD_FILE,  // blueprint_cai.h rejected it (the log line says why)
    BP_IMPORT_NO_SAVE,   // no loaded save data with an Autobuild favorites list (still booting)
    BP_IMPORT_BAD_SLOT,  // slot outside 0..BP_SLOTS-1, or beyond the list the save holds
    BP_IMPORT_NO_ROOM,   // the save data could not be reached in guest memory (logging says why)
};

// The live favorites list, for the page to draw. slots is the list length the save holds (usually 30); actors[i] is
// the actor count of slot i, 0 when that slot is empty or not read. Returns 1 on success.
typedef struct {
    int slots;
    int actors[BP_SLOTS];
} BpFavorites;

int bp_favorites(BpFavorites *out);

// Replaces slot's blueprint with the one in the file at path. The write happens at once, in guest memory; the caller
// must not call it while the game saves (it would be harmless but the game may overwrite it with the same data).
// The page calls it only when the player presses its import button.
int bp_import(const char *path, int slot);

// A short text for a result, for the page.
const char *bp_import_text(int result);

#ifdef __cplusplus
}
#endif
