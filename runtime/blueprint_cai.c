// Reads a CAI blueprint file (blueprint_cai.h): the game's CombinedActorInfo, the data of one Autobuild creation. The
// layout is the community research (TotkMods/Research, ".imhex/cai.hexpat"); this parser trusts none of it and checks
// every field it reads, so a damaged or foreign file is rejected here instead of reaching the save data. Only the first
// 6688 bytes are the blob. A file can be longer: a distributed one adds two camera vectors (6712 bytes), and a draft the
// game saved mid-build adds a 1152-byte block (7840). Those tails are not the blueprint and are ignored here
// (blueprint_cai.h lists the three sizes).
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "blueprint_cai.h"

// The blob's parts. The actor record is two 4x4 matrices, a six-float matrix and a 128-byte name, 280 bytes in total;
// the 21 records end where the bounding box starts.
enum {
    BPC_ACTOR_OFFSET = 12,       // after the header: magic, 2 pad bytes, version, count, count - 1, pad
    BPC_ACTOR_BYTES = 280,
    BPC_ACTOR_FLOATS = 38,       // (64 + 64 + 24) / 4: the matrices, only read to reject NaN and infinity
    BPC_ACTOR_NAME_OFFSET = 152, // after the matrices
    BPC_BBOX_OFFSET = 6144,      // the six floats the game frames the draft with: min x, y, z, max x, y, z
    BPC_BBOX_FLOATS = 6,
    BPC_MAGIC_BYTES = 6,
};

// A float stored at p, read with memcpy: the blob's floats need not be aligned and the compiler may not assume they are.
static float read_float(const uint8_t *p) {
    float v;
    memcpy(&v, p, sizeof v);
    return v;
}

// 1 when every float of the actor's matrices is a number. The game multiplies the matrices, so a NaN or an infinity
// there means a damaged file, not a blueprint.
static int actor_transform_finite(const uint8_t *actor) {
    for (int i = 0; i < BPC_ACTOR_FLOATS; i++)
        if (!isfinite(read_float(actor + i * 4))) return 0;
    return 1;
}

// The actor's name: a NUL-terminated printable string inside the 128-byte field. The game looks the name up as an actor
// name, so an empty or binary field is a damaged file. Only the bytes before the NUL are looked at: the bytes after it
// may be uninitialised in the game's own files.
static int actor_name(const uint8_t *field, const char **name) {
    const uint8_t *end = memchr(field, 0, BPC_NAME_SIZE);
    if (!end || end == field) return 0;
    for (const uint8_t *p = field; p < end; p++)
        if (*p < 0x20 || *p > 0x7e) return 0;
    *name = (const char *)field;
    return 1;
}

enum BpcResult bpc_parse(const void *data, size_t size, BpcBlueprint *out) {
    if (!out) return BPC_TOO_SHORT;   // nothing to fill; the header requires out
    memset(out, 0, sizeof *out);
    if (!data) return BPC_TOO_SHORT;
    // .totkab wraps a CAI in a container of its own; this port knows the container's name but does not read it. The
    // check comes before the length one so a short wrapper is reported as unsupported rather than as a damaged blueprint.
    if (size >= 8 && !memcmp(data, "TOTKAuto", 8)) return BPC_UNSUPPORTED;
    if (size < BPC_BLOB_SIZE) return BPC_TOO_SHORT;
    const uint8_t *blob = (const uint8_t *)data;
    if (memcmp(blob, "CmbAct", BPC_MAGIC_BYTES)) return BPC_BAD_MAGIC;
    int version = blob[8];
    if (version != BPC_VERSION) return BPC_BAD_VERSION;
    int actors = blob[9];
    // The count is also stored once more, minus one; both are what the game reads, so a file whose two copies disagree
    // is not the file the game wrote.
    if (actors < 1 || actors > BPC_MAX_ACTORS || blob[10] != actors - 1) return BPC_BAD_COUNT;

    // Fill a local blueprint first: out is only written when everything passed, as the header promises. Only the actors
    // the count names are checked; the rest of the 21 records may hold anything, and the game's own files leave old data
    // there.
    BpcBlueprint bp;
    memset(&bp, 0, sizeof bp);
    for (int k = 0; k < actors; k++) {
        const uint8_t *actor = blob + BPC_ACTOR_OFFSET + (size_t)k * BPC_ACTOR_BYTES;
        if (!actor_transform_finite(actor) || !actor_name(actor + BPC_ACTOR_NAME_OFFSET, &bp.names[k]))
            return BPC_BAD_ACTOR;
        // The actor's placement is its first matrix, row-major: the upper-left 3x3 is a rotation (orthonormal and
        // determinant +1 on every community file checked, so it is a rotation and not a scale) and the position is
        // elements 3, 7 and 11. Every actor's position falls inside the bounding box, which is what confirms the
        // reading; the "column-major" alternative (elements 12, 13, 14) is always (1, 0, 0) and cannot be a position.
        // rot[0..2] are the three basis vectors, kept as the matrix's rows: a preview draws each part oriented by them.
        static const int pos_elem[3] = {3, 7, 11};
        for (int i = 0; i < 3; i++) bp.pos[k][i] = read_float(actor + (size_t)pos_elem[i] * 4);
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) bp.rot[k][r][c] = read_float(actor + (size_t)(r * 4 + c) * 4);
    }
    // The bounding box frames the draft in the game; an impossible number there is a damaged file as well. BpcResult has
    // no code of its own for it, so it is reported as bad actor data (both are "a number in the blob is not one").
    for (int i = 0; i < BPC_BBOX_FLOATS; i++) {
        float v = read_float(blob + BPC_BBOX_OFFSET + i * 4);
        if (!isfinite(v)) return BPC_BAD_ACTOR;
        if (i < 3) bp.bbox_min[i] = v;
        else bp.bbox_max[i - 3] = v;
    }
    bp.blob = blob;
    bp.size = size;   // the file's own size: 6688, or more with the tail a distributed file or a draft carries
    bp.version = version;
    bp.actors = actors;
    *out = bp;
    return BPC_OK;
}

const char *bpc_result_text(enum BpcResult result) {
    switch (result) {
    case BPC_OK: return "ok";
    case BPC_TOO_SHORT: return "shorter than a blueprint (6688 bytes)";
    case BPC_BAD_MAGIC: return "not a CAI blueprint: the file does not start with CmbAct";
    case BPC_BAD_VERSION: return "the blueprint version is not 2";
    case BPC_BAD_COUNT: return "the actor count in the header is not 1 to 21, or its two copies disagree";
    case BPC_BAD_ACTOR: return "an actor name is empty or not printable, or a transform is not a finite number";
    case BPC_UNSUPPORTED: return "TOTKAuto (.totkab) blueprints are not supported yet";
    }
    return "unknown result";
}
