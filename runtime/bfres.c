// The BFRES reader (bfres.h): the model container, its shapes, and their geometry buffers.
#include <math.h>
#include <string.h>

#include "bfres.h"

#define BFRES_HEADER_MIN 0x20u
#define SHAPE_STRIDE 0x60u
#define GEOMETRY_GAP 288u   // the geometry pool starts this far past the BFRES body

static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)((uint32_t)p[0] | (uint32_t)p[1] << 8); }

// IEEE 754 binary16 to binary32. The game stores vertex positions as half floats; this keeps the reader free of a
// compiler builtin so it builds with any of the toolchains the port supports.
static float half_to_float(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;                       // +-0
        } else {
            int e = 0;                         // subnormal: shift the mantissa up until it has a leading one
            while (!(mant & 0x400u)) { mant <<= 1; e++; }
            mant &= 0x3FFu;
            bits = sign | ((uint32_t)(127 - 15 - e) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);   // infinity or NaN
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

// Whether off and size lie inside the buffer: a malformed or half-decoded model must not be read past its end.
static int in_bounds(const BfresModel *m, uint64_t off, uint64_t size) {
    return off <= (uint64_t)m->n && size <= (uint64_t)m->n - off;
}

// Defined later; the skeleton code below needs it before its own definition.
static const uint8_t *shape_at(const BfresModel *m, int index);

// ---- the skeleton (FSKL): each bone's world transform, accumulated down the parent chain, so a shape whose stick bone
// rotates 90 degrees has its stick upright. Only a RIGID shape (one bone per vertex) is transformed; a multi-bone skin
// is a deformation (a pile's telescoping chain, a fan's wing) whose rest pose is its extended state, so those keep their
// raw vertices.

// A 4x4 row-major transform: out = a * b.
static void bone_mul(const float a[4][4], const float b[4][4], float out[4][4]) {
    float t[4][4];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[r][k] * b[k][c];
            t[r][c] = s;
        }
    memcpy(out, t, sizeof t);
}

// A quaternion (x, y, z, w) to a 3x3 rotation (row-major), normalised first.
static void quat_to_mat(float x, float y, float z, float w, float r[3][3]) {
    float n = sqrtf(x * x + y * y + z * z + w * w);
    if (n < 1e-9f) { r[0][0] = r[1][1] = r[2][2] = 1; return; }
    x /= n; y /= n; z /= n; w /= n;
    r[0][0] = 1 - 2 * (y * y + z * z); r[0][1] = 2 * (x * y - z * w); r[0][2] = 2 * (x * z + y * w);
    r[1][0] = 2 * (x * y + z * w); r[1][1] = 1 - 2 * (x * x + z * z); r[1][2] = 2 * (y * z - x * w);
    r[2][0] = 2 * (x * z - y * w); r[2][1] = 2 * (y * z + x * w); r[2][2] = 1 - 2 * (x * x + y * y);
}

// Three Euler angles in radians (X, Y, Z applied in that order) to a 3x3 rotation, row-major.
static void euler_to_mat(float ex, float ey, float ez, float r[3][3]) {
    const float cx = cosf(ex), sx = sinf(ex);
    const float cy = cosf(ey), sy = sinf(ey);
    const float cz = cosf(ez), sz = sinf(ez);
    // R = Rx * Ry * Rz (row-major, row vectors on the left).
    r[0][0] = cy * cz;                 r[0][1] = -cy * sz;                r[0][2] = sy;
    r[1][0] = cx * sz + sx * sy * cz;  r[1][1] = cx * cz - sx * sy * sz;  r[1][2] = -sx * cy;
    r[2][0] = sx * sz - cx * sy * cz;  r[2][1] = sx * cz + cx * sy * sz;  r[2][2] = cx * cy;
}

// The rotation of one bone. The bone's flags decide how its four rotation floats are read: bit 12 set means they are
// Euler angles in radians (X, Y, Z, unused), clear means a quaternion (X, Y, Z, W). Reading an Euler bone as a
// quaternion normalises (0, 0, pi/2, 1) into a 115-degree turn instead of 90, which tilts every part on that rig by the
// same 25 degrees - exactly the "all parts lean the same way" the preview showed.
static void bone_rotation(const uint8_t *b, float r[3][3]) {
    const uint32_t flags = u32(b + 0x2C);
    float rx, ry, rz, rw;
    memcpy(&rx, b + 0x3C, 4); memcpy(&ry, b + 0x40, 4); memcpy(&rz, b + 0x44, 4); memcpy(&rw, b + 0x48, 4);
    if (flags & (1u << 12)) euler_to_mat(rx, ry, rz, r);
    else quat_to_mat(rx, ry, rz, rw, r);
}

// The world transform of one bone: its own local T*R*S, then its parent's world on the left.
// Walks the parent chain to the root, so `out` is the bone's world transform. A range check on `parent` is not enough:
// a file whose parents form a cycle (bone 0's parent is 1 and bone 1's parent is 0) would recurse forever and overflow
// the stack, so the walk is bounded by the bone count - a chain longer than that can only be a cycle.
static void bone_world_depth(const uint8_t *p, uint32_t bones, int count, int i, int budget, float out[4][4]) {
    float local[4][4] = {{0}};
    const uint8_t *b = p + bones + (uint32_t)i * 0x58u;
    float sx, sy, sz, px, py, pz;
    memcpy(&sx, b + 0x30, 4); memcpy(&sy, b + 0x34, 4); memcpy(&sz, b + 0x38, 4);
    memcpy(&px, b + 0x4C, 4); memcpy(&py, b + 0x50, 4); memcpy(&pz, b + 0x54, 4);
    float r[3][3];
    bone_rotation(b, r);
    // local = T * R * S, row-major: scale, then rotate, then translate.
    local[0][0] = r[0][0] * sx; local[0][1] = r[0][1] * sy; local[0][2] = r[0][2] * sz; local[0][3] = px;
    local[1][0] = r[1][0] * sx; local[1][1] = r[1][1] * sy; local[1][2] = r[1][2] * sz; local[1][3] = py;
    local[2][0] = r[2][0] * sx; local[2][1] = r[2][1] * sy; local[2][2] = r[2][2] * sz; local[2][3] = pz;
    local[3][3] = 1;
    // parent index at +0x22 (i16)
    int16_t parent; memcpy(&parent, b + 0x22, 2);
    if (parent < 0 || parent >= count || budget <= 0) { memcpy(out, local, sizeof local); return; }
    float pw[4][4];
    bone_world_depth(p, bones, count, parent, budget - 1, pw);
    bone_mul(pw, local, out);
}

static void bone_world(const uint8_t *p, uint32_t bones, int count, int i, float out[4][4]) {
    bone_world_depth(p, bones, count, i, count, out);
}

// Reads the FSKL skeleton's Node_Array (mtx_to_bone_list): the render order of the rigid bones. It maps a per-vertex
// blend index (a slot 0..num_rigid-1, read from the _i0 attribute) to the real bone array index. Returns the entry
// count and fills a caller buffer, or 0 when there is no skeleton or no rigid bones.
static int skeleton_node_array(const BfresModel *m, uint16_t *out, int max) {
    for (uint32_t o = 0; o + 4 <= m->n; o++) {
        if (memcmp(m->p + o, "FSKL", 4)) continue;
        if (o + 0x3E > m->n) return 0;
        const uint64_t list = u64(m->p + o + 0x18);   // mtx_to_bone_list offset
        const int num_rigid = (int)u16(m->p + o + 0x3C);
        if (!list || num_rigid <= 0 || num_rigid > max) return 0;
        if (!in_bounds(m, list, (uint64_t)num_rigid * 2)) return 0;
        for (int i = 0; i < num_rigid; i++) out[i] = u16(m->p + list + (uint64_t)i * 2);
        return num_rigid;
    }
    return 0;
}

// The bone array offset and count from the FSKL block, for bone_world. Returns 0 when there is no skeleton.
static int skeleton_bones(const BfresModel *m, uint32_t *bones, int *count) {
    for (uint32_t o = 0; o + 4 <= m->n; o++) {
        if (memcmp(m->p + o, "FSKL", 4)) continue;
        if (o + 0x3C > m->n) return 0;
        *bones = u32(m->p + o + 0x10);
        *count = (int)u16(m->p + o + 0x38);
        return *count > 0 && in_bounds(m, *bones, (uint64_t)*count * 0x58u);
    }
    return 0;
}

// Reads a BFRES string at offset `off`: a u16 length followed by that many bytes. The shape names, the FVTX attribute
// names and the ResDict keys all use this layout.
static const char *bfres_string(const BfresModel *m, uint64_t off) {
    if (!off || !in_bounds(m, off, 2)) return NULL;
    const uint16_t len = u16(m->p + off);
    if (!in_bounds(m, off + 2, (uint64_t)len + 1)) return NULL;
    return (const char *)(m->p + off + 2);
}

// Parses the FVTX attribute dict for the blend-index attribute. On success fills *buf_idx and *off so the caller can read
// one u8 blend index per vertex from the shape's vertex buffer `buf_idx` at byte `off`. Returns 1 when _i0 exists.
// Layout (bfrespy switch/model/vertex_buffer.py and models/vertex_buffer_attrib.py):
//   FVTX +0x08 values_offs (u64), +0x10 dict_offs (u64). The dict is a ResDict: u32 total_size, u32 num_nodes, then
//   num_nodes+1 nodes of {u32 ref, u16 left, u16 right, u64 key_offset} (16 bytes each, key = {u16 len, bytes}). The
//   values are num_nodes+1 VertexAttribs of {u64 name_offset, u16 format BE, u16 pad, u16 offset, u16 buffer_idx}.
static int fvtx_blend_attr(const BfresModel *m, int index, uint32_t *buf_idx, uint32_t *off) {
    const uint8_t *s = shape_at(m, index);
    if (!s) return 0;
    const uint64_t at = u64(s + 0x10);
    if (!in_bounds(m, at, 0x58)) return 0;
    const uint8_t *f = m->p + at;
    if (memcmp(f, "FVTX", 4)) return 0;
    const uint64_t values_offs = u64(f + 0x08);
    const uint64_t dict_offs = u64(f + 0x10);
    if (!dict_offs || !values_offs) return 0;
    if (!in_bounds(m, dict_offs, 8)) return 0;
    const uint32_t num_nodes = u32(m->p + dict_offs + 4);
    if (num_nodes == 0 || num_nodes > 64) return 0;
    // Walk the dict nodes' keys and the value array. The ResDict has num_nodes+1 nodes (a root first, then the real
    // entries), but the value array has only num_nodes entries, one per real node: value i belongs to node i+1.
    uint64_t np = dict_offs + 8;
    uint64_t vp = values_offs;
    for (uint32_t i = 0; i <= num_nodes; i++) {
        if (!in_bounds(m, np, 16)) return 0;
        const char *key = bfres_string(m, u64(m->p + np + 8));
        // The value entry for this node exists only for i >= 1 (the root has none).
        if (i >= 1) {
            if (!in_bounds(m, vp, 16)) return 0;
            const char *vname = bfres_string(m, u64(m->p + vp));
            // strcmp, not memcmp: bfres_string guarantees the bytes up to and including the terminating NUL and nothing
            // past it, so a three-byte compare reads past a one- or two-character name at the end of the buffer.
            if (key && vname && !strcmp(key, "_i0") && !strcmp(vname, "_i0")) {
                // VertexAttrib: +0x08 format (u16 BE), +0x0C offset (u16), +0x0E buffer_idx (u16).
                *off = u16(m->p + vp + 0x0C);
                *buf_idx = u16(m->p + vp + 0x0E);
                return 1;
            }
            vp += 16;
        }
        np += 16;
    }
    return 0;
}

// The offset of the FMDL block. The game's files keep it at a fixed 0xF0 (the header before it is a fixed size), and the
// magic is checked there first; a short scan of the metadata region is the fallback, so a model whose header grew is
// still read.
static uint32_t find_fmdl(const uint8_t *p, size_t n) {
    if (n >= 0xF4 && !memcmp(p + 0xF0, "FMDL", 4)) return 0xF0;
    const size_t limit = n < 0x2000 ? n : 0x2000;
    for (size_t i = 0; i + 4 <= limit; i++)
        if (!memcmp(p + i, "FMDL", 4)) return (uint32_t)i;
    return 0;
}

int bfres_open(BfresModel *out, const void *data, size_t n) {
    if (!out || !data) return 0;
    const uint8_t *p = data;
    memset(out, 0, sizeof *out);
    if (n < BFRES_HEADER_MIN || memcmp(p, "FRES", 4)) return 0;
    if (((u32(p + 8)) >> 16) < 10) return 0;            // the game's models are version 10 (0x000A0000)
    if (u16(p + 0x0C) != 0xFEFF) return 0;              // the byte-order field: 0xFF 0xFE means little endian
    const uint32_t body = u32(p + 0x1C);
    if (body < BFRES_HEADER_MIN || body > n) return 0;

    out->p = p;
    out->n = n;
    out->geom = body + GEOMETRY_GAP;
    // The geometry pool starts past the body and holds the FMSH-decoded vertex and index buffers. A body that was
    // unpacked but whose geometry has not been decoded yet is still a valid BFRES to open: the shape list is read, and
    // bfres_shape_geometry simply reports nothing until the pool is present. So the pool's presence is checked lazily
    // there, not here.

    const uint32_t fmdl = find_fmdl(p, n);
    if (!fmdl || !in_bounds(out, (uint64_t)fmdl + 0x6Cu, 0)) return 0;
    out->shapes = u32(p + fmdl + 0x28);
    out->count = (int)u16(p + fmdl + 0x6A);
    if (out->count <= 0 || out->shapes < fmdl) return 0;
    if (!in_bounds(out, out->shapes, (uint64_t)out->count * SHAPE_STRIDE)) return 0;
    for (int i = 0; i < out->count; i++)
        if (memcmp(p + out->shapes + (uint32_t)i * SHAPE_STRIDE, "FSHP", 4)) return 0;
    return 1;
}

static const uint8_t *shape_at(const BfresModel *m, int index) {
    if (!m || !m->p || index < 0 || index >= m->count) return NULL;
    return m->p + m->shapes + (uint32_t)index * SHAPE_STRIDE;
}

int bfres_shape_name(const BfresModel *m, int index, char *name, size_t cap) {
    const uint8_t *s = shape_at(m, index);
    if (!s || !name || cap == 0) return 0;
    const uint64_t at = u64(s + 0x08);
    if (!in_bounds(m, at, 4)) return 0;
    const uint16_t len = u16(m->p + at);
    if (!in_bounds(m, at + 2, (uint64_t)len + 1)) return 0;
    const size_t copy = len < cap - 1 ? len : cap - 1;
    memcpy(name, m->p + at + 2, copy);
    name[copy] = 0;
    return 1;
}

// The shape's vertex buffer block (FVTX): its buffer offset within the geometry pool, its vertex count, and the byte
// stride of one vertex in the position buffer (size0 / count; a wheel is 6, a cart 8). The positions are the first
// three half floats of each vertex, and the other attributes live in a second buffer.
static const uint8_t *shape_fvtx(const BfresModel *m, int index, uint32_t *buf_off, uint32_t *count, uint32_t *stride) {
    const uint8_t *s = shape_at(m, index);
    if (!s) return NULL;
    const uint64_t at = u64(s + 0x10);
    if (!in_bounds(m, at, 0x58)) return NULL;
    const uint8_t *f = m->p + at;
    if (memcmp(f, "FVTX", 4)) return NULL;
    *buf_off = u32(f + 0x48);
    *count = u32(f + 0x50);
    // The per-buffer size array's first entry is the position buffer's byte size; divided by the vertex count it is the
    // stride. The positions are three half floats at the start of each vertex, and anything after them is another attribute:
    // a stride of 7 is six bytes of position plus the one-byte `_i0` blend index, which is normal and must not be refused.
    if (*count == 0) return NULL;
    // The stride array (+0x38) declares each buffer's bytes per vertex, and it is the authority: a shape can have more than one
    // buffer, so the first buffer's byte size divided by the vertex count is not the stride - with 8 vertices and 56 bytes it
    // gives 7 while the vertices are 6 bytes wide, and reading them one byte out turns a small face into a 52 m sheet.
    uint32_t st = 0;
    const uint64_t stride_ptr = u64(f + 0x38);
    if (stride_ptr && in_bounds(m, stride_ptr, 4)) st = u32(m->p + stride_ptr);
    if (st < 6 || st > 64) {
        // A file without a usable stride array: fall back to the first buffer's size, which is right when that buffer holds
        // nothing but positions.
        const uint64_t size_ptr = u64(f + 0x30);
        if (!size_ptr || !in_bounds(m, size_ptr, 4)) return NULL;
        const uint32_t size0 = u32(m->p + size_ptr);
        if (size0 < *count * 6u) return NULL;
        st = size0 / *count;
        if (st < 6 || st > 64) return NULL;
    }
    *stride = st;
    return f;
}

// The shape's first mesh record, which carries the index buffer's place and its count.
static const uint8_t *shape_mesh(const BfresModel *m, int index, uint32_t *count) {
    const uint8_t *s = shape_at(m, index);
    if (!s) return NULL;
    const uint64_t meshes = u64(s + 0x18);
    if (!meshes || !in_bounds(m, meshes, 0x38)) return NULL;
    *count = u32(m->p + meshes + 0x2C);
    return m->p + meshes;
}

int bfres_shape_vertices(const BfresModel *m, int index) {
    uint32_t buf_off = 0, count = 0, stride = 0;
    if (!shape_fvtx(m, index, &buf_off, &count, &stride)) return 0;
    return (int)count;
}

int bfres_shape_triangles(const BfresModel *m, int index) {
    uint32_t icount = 0;
    const uint8_t *mesh = shape_mesh(m, index, &icount);
    if (!mesh || icount % 3) return 0;
    return (int)(icount / 3);
}

int bfres_shape_geometry(const BfresModel *m, int index, float *verts, int max_verts, uint16_t *tris, int max_tris) {
    uint32_t buf_off = 0, vcount = 0, icount = 0, stride = 0;
    if (!verts || !shape_fvtx(m, index, &buf_off, &vcount, &stride) || vcount == 0) return 0;
    if (vcount > (uint32_t)max_verts) return 0;

    // The positions are three half floats at the start of each vertex, one vertex per stride (a wheel is 6 bytes, a cart
    // 8). The other attributes live in a second buffer.
    const uint64_t pos_at = (uint64_t)m->geom + buf_off;
    if (!in_bounds(m, pos_at, (uint64_t)vcount * stride)) return 0;

    // Rigid skinning, per vertex. A shape with one bone per vertex (vtx_skin_count == 1) has a per-vertex blend index in
    // its _i0 attribute; that index names a slot in the skeleton's Node_Array, which maps to the real bone. Each vertex is
    // then transformed by that bone's world matrix. A multi-bone skin (vtx_skin_count >= 2) is a deformation whose rest
    // pose is its extended state, so it keeps its raw vertices. The bone matrices are computed once and reused.
    const uint8_t *shp = shape_at(m, index);
    float bones_world[64][4][4];
    uint16_t node_array[64];
    int n_rigid = 0, n_bones = 0;
    uint32_t bone_array_off = 0;
    int have_skin = 0;
    uint32_t blend_buf = 0, blend_off = 0;
    int blend_ok = 0;
    if (shp && shp[0x5A] == 1 && (n_rigid = skeleton_node_array(m, node_array, 64)) > 0 &&
        skeleton_bones(m, &bone_array_off, &n_bones) && n_bones > 0 && n_bones <= 64) {
        for (int b = 0; b < n_bones; b++)
            bone_world(m->p, bone_array_off, n_bones, b, bones_world[b]);
        have_skin = 1;
        blend_ok = fvtx_blend_attr(m, index, &blend_buf, &blend_off);
    }

    for (uint32_t v = 0; v < vcount; v++) {
        const uint8_t *b = m->p + pos_at + (uint64_t)v * stride;
        float x = half_to_float(u16(b));
        float y = half_to_float(u16(b + 2));
        float z = half_to_float(u16(b + 4));
        if (have_skin) {
            // The bone slot this vertex binds: _i0's u8 at the blend attribute's offset in the vertex buffer. For the
            // common case (_i0 in buffer 0, the position buffer) it is simply pos_at + v*stride + blend_off. When the
            // attribute lives in a second buffer its base and stride differ, but buffer 0 covers every model seen so far.
            uint32_t slot = 0;
            if (blend_ok && blend_buf == 0 && in_bounds(m, pos_at + blend_off, (uint64_t)vcount * stride))
                slot = m->p[pos_at + (uint64_t)v * stride + blend_off];
            // The slot indexes the Node_Array; map it to the real bone, then apply that bone's world matrix.
            uint32_t bone_index = 0;
            if (slot < (uint32_t)n_rigid) bone_index = node_array[slot];
            if (bone_index < (uint32_t)n_bones) {
                const float (*bm)[4] = bones_world[bone_index];
                const float tx = bm[0][0] * x + bm[0][1] * y + bm[0][2] * z + bm[0][3];
                const float ty = bm[1][0] * x + bm[1][1] * y + bm[1][2] * z + bm[1][3];
                const float tz = bm[2][0] * x + bm[2][1] * y + bm[2][2] * z + bm[2][3];
                x = tx; y = ty; z = tz;
            }
        }
        verts[3 * v] = x;
        verts[3 * v + 1] = y;
        verts[3 * v + 2] = z;
        // One wrong buffer offset can turn a tiny face into a kilometre-wide sheet across the preview. A part is metres
        // across, so a coordinate this large means the shape is being misread: refuse it rather than draw it.
        if (!(x > -1000.0f && x < 1000.0f && y > -1000.0f && y < 1000.0f && z > -1000.0f && z < 1000.0f)) {
            return 0;
        }
    }

    const uint8_t *mesh = shape_mesh(m, index, &icount);
    if (!mesh) return (int)vcount;   // a shape without a mesh still reports its vertices
    const uint32_t face_off = u32(mesh + 0x20);
    if (u32(mesh + 0x24) != 3 || u32(mesh + 0x28) != 1) return 0;   // triangle lists of uint16 only
    if (icount == 0 || icount % 3 || !tris || icount / 3 > (uint32_t)max_tris) return 0;
    const uint64_t idx_at = (uint64_t)m->geom + face_off;
    if (!in_bounds(m, idx_at, (uint64_t)icount * 2)) return 0;
    const uint32_t first = u32(mesh + 0x30);
    for (uint32_t i = 0; i < icount; i++) {
        const uint32_t index = (uint32_t)u16(m->p + idx_at + (uint64_t)i * 2) + first;
        if (index >= vcount) return 0;   // an index past the shape's vertices means the reader is misreading the shape
        tris[i] = (uint16_t)index;
    }
    return (int)vcount;
}

// A bone's name is a length-prefixed string in the model's own string pool: {u16 length, bytes}.
static void bone_name(const uint8_t *p, size_t n, uint32_t bone_off, char *out, size_t cap) {
    out[0] = 0;
    if (bone_off + 8 > n) return;
    uint64_t at;
    memcpy(&at, p + bone_off, 8);
    if (!at || at + 2 > (uint64_t)n) return;
    uint16_t len;
    memcpy(&len, p + at, 2);
    if (at + 2 + len > (uint64_t)n) return;
    const size_t take = len < cap - 1 ? len : cap - 1;
    memcpy(out, p + at + 2, take);
    out[take] = 0;
}

int bfres_bone_find(const BfresModel *m, const char *name) {
    if (!m || !name || !*name) return -1;
    uint32_t bones = 0;
    int count = 0;
    if (!skeleton_bones(m, &bones, &count)) return -1;
    for (int i = 0; i < count; i++) {
        char got[64];
        bone_name(m->p, m->n, bones + (uint32_t)i * 0x58u, got, sizeof got);
        if (!strcmp(got, name)) return i;
    }
    return -1;
}

int bfres_bone_world(const BfresModel *m, int index, float out[16]) {
    if (!m || !out) return 0;
    uint32_t bones = 0;
    int count = 0;
    if (!skeleton_bones(m, &bones, &count) || index < 0 || index >= count) return 0;
    float w[4][4];
    bone_world(m->p, bones, count, index, w);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r * 4 + c] = w[r][c];
    return 1;
}