// A CAI blueprint file (".cai"): the game's CombinedActorInfo, the data of one Autobuild creation. The page reads a
// file into memory and parses it here before it may be imported (ui/blueprint_page.cpp, blueprint_import.h). The
// format is entirely from the community research (TotkMods/Research, ".imhex/cai.hexpat"); this file keeps its own
// reading of it and validates the result instead of trusting a header.
//
// A blob is 6688 bytes and holds up to 21 actors: a 12-byte header ("CmbAct", version 2, actor count), 21 actor
// records of 280 bytes (two matrices, a small matrix and a 128-byte name), 21 bond records of 12 bytes, the six
// floats of the bounding box the game frames the draft with (min x, y, z then max x, y, z), and 520 bytes this parser
// does not interpret. Community downloads are 6688 bytes or 6712 bytes (the same blob followed by 24 bytes of unknown
// meaning); the saved blob in the save data is exactly 6688.
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
