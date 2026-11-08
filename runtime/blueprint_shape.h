// The collision shapes of a blueprint's parts, resolved from the game's own actor packs and turned into triangle meshes
// the picker's preview draws. A CAI file stores each part's name and placement and nothing else; the name is the actor
// ("AsbObj_WoodWheel_A_01"), and the actor's pack (Pack/Actor/<name>.pack.zs) carries the physics bodies the game builds
// the part from - a SARC of BYML ShapeParam files holding the primitives (cylinder, sphere, box, capsule, polytope) with
// their real centres, radii and vertex positions. Drawing those instead of one box per part is what makes the preview
// read as the build rather than as a scatter.
//
// The pack is a zstd frame using the dictionary pack.zsdic of Pack/ZsDic.pack.zs (camera_speed.c does the same dance
// for one pack); the SARC members whose names do not mark them as attack/target/chemical/sensor shapes are the bodies,
// and each holds a BYML hash whose keys name one primitive array each. Every primitive becomes a point cloud in the
// actor's own space (its OffsetRotation and OffsetTranslation applied), which runtime/hull.c turns into outward-wound
// triangles. The result is cached per part name: a blueprint repeats parts (four wheels), and the preview re-projects
// on every camera drag, so resolving a name once and reusing it is what keeps the drag smooth.
//
// All of this is read-only on the RomFS and fails soft: a part whose pack is missing or unreadable yields no mesh, and
// the caller draws the box it drew before. Nothing here allocates past a fixed cache, and nothing writes.
#pragma once

#include <stddef.h>
#include <stdint.h>

// A part's resolved body, in its own space (before the CAI's placement rotation and position are applied).
#define BP_SHAPE_MAX_VERTS 512    // per part; a polytope keeps at most 81 vertices, a sphere here has 58
#define BP_SHAPE_MAX_TRIS 1024    // per part; the game's most complex bodies stay under this

typedef struct {
    int nverts;
    float verts[BP_SHAPE_MAX_VERTS * 3];   // x y z triples
    int ntris;
    uint16_t tris[BP_SHAPE_MAX_TRIS * 3];  // three indices into verts, counter-clockwise seen from outside
} BpShapeMesh;

// The cached mesh of the actor named `name`, or NULL when it has no readable pack, no body shapes, or the cache is
// full. The pointer is valid until the next resolve evicts it; the picker uses it within one frame. Never fails the
// process: a missing pack is a box fallback, not an error.
const BpShapeMesh *bp_shape_resolve(const char *name);

// A SARC archive walker (shared with model_romfs.c): every member whose name starts with prefix and does not contain
// exclude is passed to fn as (name, data, size, ctx). Returns the members visited.
int sarc_each_matching(const uint8_t *p, size_t n, const char *prefix, const char *exclude,
                       int (*fn)(const char *name, const uint8_t *data, size_t size, void *ctx), void *ctx);

// The whole file at RomFS path rel (no mount prefix), malloc'd; NULL when it is not in the archive. The caller frees.
// Shared with model_romfs.c so it reads the model archive the same way the shape reader does.
uint8_t *bp_romfs_file(const char *rel, size_t *out_n);
