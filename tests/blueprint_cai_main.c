// The CAI blueprint validator, tested against the real runtime/blueprint_cai.c.
//
// This parser is the gate every import passes through: a file that is not a blueprint must be refused before anything
// reaches the save data. Each rejection path and each accepted shape is built here byte by byte, so the test says what
// the format means rather than replaying a recorded file.
//
// Built and run by tests/blueprint_cai_test.py.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "blueprint_cai.h"

#define N BPC_MAX_ACTORS
#define BLOB BPC_BLOB_SIZE

// The layout blueprint_cai.c reads (see the header): a 12-byte header, then 21 actor records of 280 bytes, then the
// bond records, then the six bounding-box floats at 6144.
enum {
    OFF_MAGIC = 0,
    OFF_VERSION = 8,
    OFF_COUNT = 9,
    OFF_COUNT_MINUS_1 = 10,
    OFF_ACTOR0 = 12,
    ACTOR_BYTES = 280,
    OFF_NAME_IN_ACTOR = 152,
    OFF_BBOX = 6144,
};

static int failures, checks;

static void check(int cond, const char *what) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

// A valid blob with `actors` parts, each named "Part_i" and placed at a finite transform inside a unit box.
static void make_blob(uint8_t *b, int actors) {
    memset(b, 0, BLOB);
    memcpy(b + OFF_MAGIC, "CmbAct", 6);
    b[OFF_VERSION] = BPC_VERSION;
    b[OFF_COUNT] = (uint8_t)actors;
    b[OFF_COUNT_MINUS_1] = (uint8_t)(actors - 1);
    for (int k = 0; k < actors; k++) {
        uint8_t *a = b + OFF_ACTOR0 + k * ACTOR_BYTES;
        // The first matrix's translation: column-major elements 3, 7 and 11, i.e. bytes 12, 16 and 20.
        const float x = (float)k * 0.5f, y = 0.0f, z = (float)-k * 0.25f;
        memcpy(a + 12, &x, 4);
        memcpy(a + 16, &y, 4);
        memcpy(a + 20, &z, 4);
        // A finite rotation block, so the transform check has real numbers to accept.
        const float one = 1.0f;
        memcpy(a + 0, &one, 4);
        memcpy(a + 36, &one, 4);
        memcpy(a + 72, &one, 4);
        snprintf((char *)a + OFF_NAME_IN_ACTOR, BPC_NAME_SIZE, "Part_%d", k);
    }
    const float bbox[6] = {-1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    memcpy(b + OFF_BBOX, bbox, sizeof bbox);
}

static enum BpcResult parse(const uint8_t *b, size_t n, BpcBlueprint *out) { return bpc_parse(b, n, out); }

int main(void) {
    static uint8_t b[BLOB + 64];
    BpcBlueprint bp;

    // ---- the shapes that must be accepted
    {
        for (int actors = 1; actors <= N; actors += (actors == 1 ? N - 1 : 1)) {
            make_blob(b, actors);
            char msg[128];
            const enum BpcResult r = parse(b, BLOB, &bp);
            snprintf(msg, sizeof msg, "%d actors: accepted (result %d)", actors, r);
            check(r == BPC_OK, msg);
            snprintf(msg, sizeof msg, "%d actors: the count is reported back", actors);
            check(r == BPC_OK && bp.actors == actors, msg);
        }
    }
    // The community files are 6712 bytes: the blob followed by 24 unknown bytes.
    {
        make_blob(b, 3);
        const enum BpcResult r = parse(b, BLOB + 24, &bp);
        check(r == BPC_OK, "6712 bytes (blob plus 24 trailing bytes) is accepted");
        check(r == BPC_OK && bp.size == BLOB + 24, "the file size is reported, not the blob size");
    }
    // Positions and the bounding box come back for the picker's preview.
    {
        make_blob(b, 4);
        if (parse(b, BLOB, &bp) == BPC_OK) {
            check(bp.pos[0][0] == 0.0f && bp.pos[1][0] == 0.5f, "actor 0 and 1 keep their x translations");
            check(bp.pos[2][2] == -0.5f, "actor 2 keeps its z translation");
            check(bp.bbox_min[0] == -1.0f && bp.bbox_max[1] == 1.0f, "the bounding box is read");
            check(!strcmp(bp.names[0], "Part_0") && !strcmp(bp.names[3], "Part_3"), "actor names are read");
        } else {
            check(0, "the 4-actor blob used for the position checks parses");
        }
    }

    // ---- every rejection path
    {
        make_blob(b, 3);
        check(parse(b, BLOB - 1, &bp) == BPC_TOO_SHORT, "one byte short of a blob is refused");
        check(parse(b, 0, &bp) == BPC_TOO_SHORT, "an empty file is refused");
        check(parse(NULL, BLOB, &bp) == BPC_TOO_SHORT, "a null buffer is refused");
        check(parse(b, BLOB, NULL) == BPC_TOO_SHORT, "a null output is refused");
    }
    {
        make_blob(b, 3);
        memcpy(b + OFF_MAGIC, "XmbAct", 6);
        check(parse(b, BLOB, &bp) == BPC_BAD_MAGIC, "a wrong magic is refused");
    }
    {
        make_blob(b, 3);
        b[OFF_VERSION] = 3;
        check(parse(b, BLOB, &bp) == BPC_BAD_VERSION, "a version other than 2 is refused");
    }
    {
        make_blob(b, 3);
        b[OFF_COUNT] = 0;
        b[OFF_COUNT_MINUS_1] = 0;
        check(parse(b, BLOB, &bp) == BPC_BAD_COUNT, "zero actors is refused");
    }
    {
        make_blob(b, 3);
        b[OFF_COUNT] = N + 1;
        b[OFF_COUNT_MINUS_1] = (uint8_t)N;
        check(parse(b, BLOB, &bp) == BPC_BAD_COUNT, "more actors than the format holds is refused");
    }
    {
        make_blob(b, 3);
        b[OFF_COUNT_MINUS_1] = 1;   // the header's two copies of the count disagree
        check(parse(b, BLOB, &bp) == BPC_BAD_COUNT, "a count whose two copies disagree is refused");
    }
    {
        // An empty actor name.
        make_blob(b, 3);
        memset(b + OFF_ACTOR0 + OFF_NAME_IN_ACTOR, 0, BPC_NAME_SIZE);
        check(parse(b, BLOB, &bp) == BPC_BAD_ACTOR, "an empty actor name is refused");
    }
    {
        // A transform that is not a finite number.
        make_blob(b, 3);
        const float nan = NAN;
        memcpy(b + OFF_ACTOR0 + 12, &nan, 4);
        check(parse(b, BLOB, &bp) == BPC_BAD_ACTOR, "a NaN translation is refused");
    }
    {
        make_blob(b, 3);
        const float inf = INFINITY;
        memcpy(b + OFF_ACTOR0 + 16, &inf, 4);
        check(parse(b, BLOB, &bp) == BPC_BAD_ACTOR, "an infinite translation is refused");
    }
    {
        // The .totkab form this port knows but does not decode.
        uint8_t t[64];
        memset(t, 0, sizeof t);
        memcpy(t, "TOTKAuto", 8);
        check(parse(t, sizeof t, &bp) == BPC_UNSUPPORTED, "a TOTKAuto file is refused as unsupported");
    }

    // ---- the result texts: every code has one, and none is empty
    {
        const enum BpcResult all[] = {BPC_OK, BPC_TOO_SHORT, BPC_BAD_MAGIC, BPC_BAD_VERSION,
                                      BPC_BAD_COUNT, BPC_BAD_ACTOR, BPC_UNSUPPORTED};
        int ok = 1;
        for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
            const char *t = bpc_result_text(all[i]);
            if (!t || !*t) ok = 0;
        }
        check(ok, "every result code has a non-empty text");
        check(strcmp(bpc_result_text(BPC_OK), bpc_result_text(BPC_BAD_MAGIC)) != 0,
              "different codes have different texts");
    }

    // ---- a valid blob must not be left half-filled on failure
    {
        make_blob(b, 3);
        memcpy(b + OFF_MAGIC, "XmbAct", 6);
        BpcBlueprint dirty;
        memset(&dirty, 0xAB, sizeof dirty);
        (void)parse(b, BLOB, &dirty);
        // The parser may leave out untouched on failure; what it must never do is claim success.
        check(parse(b, BLOB, &dirty) == BPC_BAD_MAGIC, "a refused blob is never reported as accepted");
    }

    printf("blueprint cai: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
