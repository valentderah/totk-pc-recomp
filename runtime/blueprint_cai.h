// A CAI blueprint file (".cai"): the game's CombinedActorInfo, the data of one Autobuild creation. The picker reads a
// file into memory and this parses it before anything may be imported (ui/blueprint_picker.cpp, blueprint_import.h).
// The format is entirely from the community research (TotkMods/Research, ".imhex/cai.hexpat"); this file keeps its own
// reading of it and validates the result instead of trusting a header.
//
// A blob is 6688 bytes and holds up to 21 actors: a 12-byte header ("CmbAct", version 2, actor count), 21 actor
// records of 280 bytes (two matrices, a small matrix and a 128-byte name), 21 bond records of 12 bytes, the six
// floats of the bounding box the game frames the draft with (min x, y, z then max x, y, z), and 520 bytes this parser
// does not interpret.
//
// Three file sizes turn up, and only the first 6688 bytes are the blob this port reads and the save data stores:
//
//   6688  the blob alone, which is what the save data holds per slot.
//   6712  the blob plus two camera vectors (12 bytes each: the position the draft was framed from and the point it was
//         framed at), which is what a save editor and the community sites distribute. The game itself does not read
//         these back; the camera arrays live in the save data under their own hashes.
//   7840  a draft saved mid-build by the game, which carries a further 1152-byte block this parser does not read.
//
// Anything past 6688 is ignored, so all three parse to the same blueprint.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BPC_BLOB_SIZE 6688   // the record the game stores (the save data's BinaryArray items are this size)
#define BPC_MAX_ACTORS 21
#define BPC_NAME_SIZE 128
#define BPC_VERSION 2

// Why a file is not a blueprint this port can import.
enum BpcResult {
    BPC_OK = 0,
    BPC_TOO_SHORT,      // fewer than 6688 bytes
    BPC_BAD_MAGIC,      // does not start with "CmbAct"
    BPC_BAD_VERSION,    // version byte is not 2
    BPC_BAD_COUNT,      // actor count outside 1..21, or the count-1 byte does not match it
    BPC_BAD_ACTOR,      // an actor's name is empty, not printable, or its transform is not finite
    BPC_UNSUPPORTED,    // a magic this port knows but does not decode (.totkab's TOTKAuto)
};

// A parsed blueprint. The name pointers borrow from the parsed buffer, which must stay alive.
typedef struct {
    const uint8_t *blob;               // the first 6688 bytes of the parsed file
    size_t size;                       // bytes in the file, 6688 or more
    int version;
    int actors;                        // 1..21
    const char *names[BPC_MAX_ACTORS]; // NUL-terminated actor names inside blob
    // Each actor's placement. The first matrix is row-major with a rotation in its upper-left 3x3 (orthonormal, every
    // file checked: determinant +1, unit rows) and the actor's position at elements 3, 7 and 11. Keeping the rotation is
    // what lets a preview show which way a part faces rather than only where it sits.
    float pos[BPC_MAX_ACTORS][3];      // the actor's position, in blueprint space
    float rot[BPC_MAX_ACTORS][3][3];   // its orientation: rot[k] is three basis vectors, each (x, y, z)
    float bbox_min[3];
    float bbox_max[3];
} BpcBlueprint;

// Parses data (size bytes, from the start of the file) and fills out on BPC_OK only. Never writes outside out.
enum BpcResult bpc_parse(const void *data, size_t size, BpcBlueprint *out);

// A short human-readable reason for a result, for the log and the page.
const char *bpc_result_text(enum BpcResult result);

#ifdef __cplusplus
}
#endif
