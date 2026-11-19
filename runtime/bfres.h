// A reader for the BFRES model container the game ships: the models, their shapes, and each shape's vertex and index
// buffers. It reads a model that is already decoded, i.e. a buffer in which the geometry has been placed after the
// BFRES body - the layout the game allocates and fills (the MeshCodec decompressor writes exactly that). Reading the
// container is independent of how the geometry got there.
//
// Layout, from NintendoWare's BFRES as the game uses it (checked against the game's own files and against BfresLibrary's
// and bfrespy's readers, both MIT):
//
//   header      0x00 "FRES", u32 padding, u32 version, u16 byte order, u8 alignment, u8 address size, u32 name offset,
//               u16 flags, u16 block size, u32 relocation table offset, u32 file size (the BFRES body's length)
//   geometry    the geometry pool starts at body + 288, a fixed gap that also matches
//               align8(align8(body) + 0x120); every buffer offset in the body is relative to it
//   FMDL        0x08 name offset, 0x28 shape array offset (u64), 0x6A shape count (u16)
//   FSHP        0x60 bytes apart: 0x08 name offset (u64), 0x10 vertex buffer offset (u64), 0x18 mesh array (u64)
//   Mesh        0x00 submesh array, 0x08 buffer, 0x10 index buffer size, 0x20 index offset in the geometry pool (u32),
//               0x24 primitive type (3 = triangles), 0x28 index format (1 = uint16), 0x2C index count, 0x30 first vertex,
//               0x34 submesh count
//   FVTX        0x08 attributes, 0x48 vertex buffer offset in the geometry pool (u32), 0x50 vertex count (u32)
//
// A vertex's position is the first attribute: three float16, tightly packed at the start of the vertex buffer (the other
// attributes live in a second buffer), so a shape's positions are count * 3 half floats from geometry + the FVTX offset.
// Index buffers hold triangles of uint16 by default; a uint32 or non-triangle shape is refused rather than drawn wrong.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *p;      // the decoded BFRES buffer, caller-owned
    size_t n;              // its length
    uint32_t geom;         // offset of the geometry pool
    uint32_t shapes;       // offset of the FMDL shape array
    int count;             // shape count
} BfresModel;

// Reads the model container at data[0..n) and fills out. Returns 1 on a BFRES this reader understands, 0 otherwise (a
// name that is not BFRES, a version other than the game's, no geometry pool, or no model block).
int bfres_open(BfresModel *out, const void *data, size_t n);

// The name of shape index (into name, NUL-terminated) and its vertex count; 0 when the index is out of range.
int bfres_shape_name(const BfresModel *m, int index, char *name, size_t cap);
int bfres_shape_vertices(const BfresModel *m, int index);
int bfres_shape_triangles(const BfresModel *m, int index);   // the shape's index count / 3 (0 when no mesh)

// The shape's triangles: its positions and its indices, into caller buffers. Returns the vertex count on success and 0
// when the shape cannot be read (out of range, more vertices or indices than the buffers hold, uint32 indices, or a
// primitive that is not a triangle list). Indices are absolute within the shape's own vertex array.
int bfres_shape_geometry(const BfresModel *m, int index, float *verts, int max_verts, uint16_t *tris, int max_tris);

// The skeleton's bones, for hanging something on a named socket: a fused material attaches to a `Bind_*` bone, and the
// game names that bone in the material's own data. bfres_bone_find returns the bone index of `name` or -1; the index a
// vertex's blend slot maps to is a different thing (the Node_Array), so a socket lookup uses these, not the Node_Array.
int bfres_bone_find(const BfresModel *m, const char *name);
// The bone's world transform (its position, rotation and scale composed up the parent chain) as a row-major 4x4, the
// same convention as bone_world. Returns 0 when there is no skeleton or the index is out of range.
int bfres_bone_world(const BfresModel *m, int index, float out[16]);