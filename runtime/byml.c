// Reads a BYML document (byml.h): the layout is documented there, and this file is the reader. It is the general form
// of the mini reader in runtime/resolution.c (its u32_at, key_name and field), which reads one HeapDef table with one
// hash lookup. Every offset is checked before it is followed, so a truncated or damaged file makes the accessors answer
// "absent" instead of taking the game down. No allocation, no globals, no I/O; the document stays in the caller's
// buffer, and the offsets and strings the accessors return borrow from it.
#include <string.h>

#include "byml.h"

enum {
    BYML_HEADER = 16,
    BYML_VERSION_MIN = 2, // resolution.c accepts 2..7 for the HeapDef table, so this reader matches it
    BYML_VERSION_MAX = 7,
};

// 1 when d[off..off+len) lies inside the document.
static int have(const BymlDoc *d, size_t off, size_t len) {
    return d && d->p && off <= d->n && len <= d->n - off;
}

static int u16_at(const BymlDoc *d, size_t off, uint16_t *v) {
    if (!have(d, off, 2)) return 0;
    memcpy(v, d->p + off, 2);
    return 1;
}

static int u32_at(const BymlDoc *d, size_t off, uint32_t *v) {
    if (!have(d, off, 4)) return 0;
    memcpy(v, d->p + off, 4);
    return 1;
}

// 1 when d is a little-endian BYML header with a version this reader knows. The accessors check it too, so a caller that
// never called byml_open cannot walk a buffer that is not a document.
static int header_ok(const BymlDoc *d) {
    uint16_t version;
    if (!d || !d->p || d->n < BYML_HEADER) return 0;
    if (d->p[0] != 'Y' || d->p[1] != 'B') return 0;
    if (!u16_at(d, 2, &version)) return 0;
    return version >= BYML_VERSION_MIN && version <= BYML_VERSION_MAX;
}

static int container(uint8_t type) {
    return type == BYML_ARRAY || type == BYML_HASH || type == BYML_HASH_NUM || type == BYML_HASH_NUM_VAL;
}

static int value_type(uint8_t type) {
    return type == BYML_STRING || type == BYML_BINARY || type == BYML_FILE || (type >= BYML_BOOL && type <= BYML_DOUBLE) ||
           type == BYML_NULL;
}

// The type, entry count and offset of the entries (node + 4) of the container at node; 0 when node is not a container
// or its entries do not all lie inside the document. The count is a u24, so a count that does not fit is how a damaged
// file usually shows itself; every container accessor starts here for that reason.
static int node_head(const BymlDoc *d, uint32_t node, uint8_t *type, uint32_t *count, size_t *body) {
    uint32_t head, at;
    size_t need;
    uint8_t kind;
    if (!node || !u32_at(d, node, &head)) return 0;
    kind = (uint8_t)(head & 0xff);
    if (!container(kind)) return 0;
    at = head >> 8;
    if (kind == BYML_ARRAY) {
        size_t types = ((size_t)at + 3) & ~(size_t)3; // the type bytes, padded to a multiple of 4
        need = 4 + types + 4 * (size_t)at;
    } else {
        need = 4 + 8 * (size_t)at; // a name-keyed hash entry is {u24 key, u8 type, u32 value}: 8 bytes
    }
    if (!have(d, node, need)) return 0;
    *type = kind;
    *count = at;
    *body = (size_t)node + 4;
    return 1;
}

// The name of key index in the document's hash key table; NULL when the table is absent or the index or its string does
// not lie inside the document. byml_open has checked that the table header and its offset array fit.
static const char *key_name(const BymlDoc *d, uint32_t index) {
    uint32_t head, at;
    size_t start;
    if (!header_ok(d) || !d->keys) return NULL;
    if (!u32_at(d, d->keys, &head) || (head & 0xff) != BYML_STRING_TABLE || index >= head >> 8) return NULL;
    if (!u32_at(d, (size_t)d->keys + 4 + 4 * (size_t)index, &at)) return NULL;
    start = (size_t)d->keys + at;
    if (!have(d, start, 1) || !memchr(d->p + start, 0, d->n - start)) return NULL;
    return (const char *)(d->p + start);
}

// The node offset a container-type entry points at: the word is an offset from the start of the file and must name a
// node of the type the entry declares, so a pointer into the middle of another node or past the file is not followed.
static uint32_t container_at(const BymlDoc *d, uint8_t type, uint32_t word) {
    if (!container(type) || !have(d, word, 4) || d->p[word] != type) return 0;
    return word;
}

// The number stored in a value of type whose 4-byte field starts at field, for the numeric types: 0 for the other types
// and for a damaged payload.
static int number_at(const BymlDoc *d, uint8_t type, size_t field, double *out) {
    uint32_t word;
    if (!u32_at(d, field, &word)) return 0;
    switch (type) {
    case BYML_BOOL:
        *out = (word & 0xffu) ? 1.0 : 0.0;
        return 1;
    case BYML_INT: {
        int32_t v;
        memcpy(&v, &word, 4);
        *out = (double)v;
        return 1;
    }
    case BYML_FLOAT: {
        float v;
        memcpy(&v, &word, 4);
        *out = (double)v;
        return 1;
    }
    case BYML_UINT:
        *out = (double)word;
        return 1;
    case BYML_INT64:
    case BYML_UINT64:
    case BYML_DOUBLE: {
        // The 64-bit kinds keep their 8 bytes at the offset the field names.
        if (word < BYML_HEADER || !have(d, word, 8)) return 0;
        if (type == BYML_INT64) {
            int64_t v;
            memcpy(&v, d->p + word, 8);
            *out = (double)v;
        } else if (type == BYML_UINT64) {
            uint64_t v;
            memcpy(&v, d->p + word, 8);
            *out = (double)v;
        } else {
            memcpy(out, d->p + word, 8);
        }
        return 1;
    }
    default:
        return 0;
    }
}

// The bytes of a value of type whose 4-byte field starts at field: a string's bytes from the string table (NUL
// terminated before the next string), or a binary/file node's data. NULL for the other types and for a damaged value.
static const char *data_at(const BymlDoc *d, uint8_t type, size_t field) {
    uint32_t word, head, count, index, start, end;
    const uint8_t *zero;
    if (type == BYML_STRING) {
        if (!u32_at(d, field, &index) || !u32_at(d, 8, &word) || !word) return NULL;
        if (!u32_at(d, word, &head) || (head & 0xff) != BYML_STRING_TABLE) return NULL;
        count = head >> 8;
        if (index >= count) return NULL;
        // The string is the bytes between its own offset and the next one, both relative to the table.
        if (!u32_at(d, (size_t)word + 4 + 4 * (size_t)index, &start)) return NULL;
        if (!u32_at(d, (size_t)word + 4 + 4 * ((size_t)index + 1), &end)) return NULL;
        if (end <= start || !have(d, (size_t)word + start, (size_t)(end - start))) return NULL;
        zero = memchr(d->p + word + start, 0, (size_t)(end - start));
        return zero ? (const char *)(d->p + word + start) : NULL;
    }
    if (type == BYML_BINARY || type == BYML_FILE) {
        uint32_t size;
        if (!u32_at(d, field, &word) || word < BYML_HEADER || !u32_at(d, word, &size)) return NULL;
        // 0xA1 is {u32 size, bytes}; 0xA2 is {u32 size, u32 pad, bytes}.
        word += type == BYML_BINARY ? 4 : 8;
        if (!have(d, word, (size_t)size)) return NULL;
        return (const char *)(d->p + word);
    }
    return NULL;
}

// ---- the API

int byml_open(BymlDoc *doc, const void *data, size_t n) {
    BymlDoc d;
    uint16_t version;
    uint32_t keys, strings, root;
    if (!doc || !data || n < BYML_HEADER) return 0;
    d.p = (const uint8_t *)data;
    d.n = n;
    d.keys = 0;
    if (d.p[0] != 'Y' || d.p[1] != 'B') return 0;
    if (!u16_at(&d, 2, &version) || version < BYML_VERSION_MIN || version > BYML_VERSION_MAX) return 0;
    if (!u32_at(&d, 4, &keys) || !u32_at(&d, 8, &strings) || !u32_at(&d, 12, &root)) return 0;
    // The two tables and the root are what the later accessors start from, so refuse the document now rather than
    // report an absent value for each one afterwards.
    if (keys) {
        uint32_t head;
        if (!u32_at(&d, keys, &head) || (head & 0xff) != BYML_STRING_TABLE ||
            !have(&d, keys, 4 + 4 * ((size_t)(head >> 8) + 1)))
            return 0;
    }
    if (strings) {
        uint32_t head;
        if (!u32_at(&d, strings, &head) || (head & 0xff) != BYML_STRING_TABLE ||
            !have(&d, strings, 4 + 4 * ((size_t)(head >> 8) + 1)))
            return 0;
    }
    if (root && (!have(&d, root, 4) || !container(d.p[root]))) return 0;
    d.keys = keys;
    *doc = d;
    return 1;
}

uint32_t byml_root(const BymlDoc *d) {
    uint32_t root;
    if (!header_ok(d) || !u32_at(d, 12, &root)) return 0;
    return root && !have(d, root, 4) ? 0 : root;
}

uint8_t byml_type(const BymlDoc *d, uint32_t node) {
    if (!header_ok(d) || !have(d, node, 1)) return 0;
    return d->p[node];
}

size_t byml_count(const BymlDoc *d, uint32_t node) {
    uint8_t type;
    uint32_t count;
    size_t body;
    if (!header_ok(d) || !node_head(d, node, &type, &count, &body)) return 0;
    return (size_t)count;
}

uint32_t byml_key(const BymlDoc *d, uint32_t node, const char *name) {
    uint8_t type;
    uint32_t count, head, word;
    size_t body;
    if (!name || !header_ok(d) || !node_head(d, node, &type, &count, &body) || type != BYML_HASH) return 0;
    for (uint32_t i = 0; i < count; i++) {
        size_t entry = body + 8 * (size_t)i;
        const char *key;
        uint8_t held;
        if (!u32_at(d, entry, &head) || !u32_at(d, entry + 4, &word)) return 0;
        key = key_name(d, head & 0xffffffu);
        if (!key || strcmp(key, name)) continue;
        held = (uint8_t)(head >> 24);
        if (container(held)) return container_at(d, held, word);
        // A value entry's node offset is the offset of its type byte, which sits right before the value word.
        return value_type(held) ? (uint32_t)(entry + 3) : 0;
    }
    return 0;
}

const char *byml_entry_key(const BymlDoc *d, uint32_t node, size_t i) {
    uint8_t type;
    uint32_t count, head;
    size_t body;
    if (!header_ok(d) || !node_head(d, node, &type, &count, &body) || type != BYML_HASH || i >= (size_t)count) return NULL;
    if (!u32_at(d, body + 8 * i, &head)) return NULL;
    return key_name(d, head & 0xffffffu);
}

uint32_t byml_at(const BymlDoc *d, uint32_t node, size_t i) {
    uint8_t type, elem;
    uint32_t count, word;
    size_t body;
    if (!header_ok(d) || !node_head(d, node, &type, &count, &body) || type != BYML_ARRAY || i >= (size_t)count) return 0;
    elem = d->p[body + i];
    // A value-typed element's type and value live in the array's own lists, not at a node offset of their own; only a
    // container element has one, and this returns that container's offset.
    if (!container(elem)) return 0;
    if (!u32_at(d, body + ((((size_t)count + 3) & ~(size_t)3)) + 4 * i, &word)) return 0;
    return container_at(d, elem, word);
}

uint8_t byml_array_type(const BymlDoc *d, uint32_t node, size_t i) {
    uint8_t type;
    uint32_t count;
    size_t body;
    if (!header_ok(d) || !node_head(d, node, &type, &count, &body) || type != BYML_ARRAY || i >= (size_t)count) return 0;
    return d->p[body + i];
}

uint32_t byml_array_value(const BymlDoc *d, uint32_t node, size_t i) {
    uint8_t type;
    uint32_t count, word;
    size_t body;
    if (!header_ok(d) || !node_head(d, node, &type, &count, &body) || type != BYML_ARRAY || i >= (size_t)count) return 0;
    if (!u32_at(d, body + ((((size_t)count + 3) & ~(size_t)3)) + 4 * i, &word)) return 0;
    return word;
}

double byml_array_number(const BymlDoc *d, uint32_t node, size_t i, double fallback) {
    uint8_t type;
    uint32_t count;
    size_t body;
    double number;
    if (!header_ok(d) || !node_head(d, node, &type, &count, &body) || type != BYML_ARRAY || i >= (size_t)count)
        return fallback;
    if (!number_at(d, d->p[body + i], body + ((((size_t)count + 3) & ~(size_t)3)) + 4 * i, &number)) return fallback;
    return number;
}

const char *byml_array_string(const BymlDoc *d, uint32_t node, size_t i) {
    uint8_t type;
    uint32_t count;
    size_t body;
    if (!header_ok(d) || !node_head(d, node, &type, &count, &body) || type != BYML_ARRAY || i >= (size_t)count) return NULL;
    return data_at(d, d->p[body + i], body + ((((size_t)count + 3) & ~(size_t)3)) + 4 * i);
}

double byml_number(const BymlDoc *d, uint32_t node, double fallback) {
    double number;
    if (!node || !header_ok(d) || node >= d->n) return fallback;
    if (!number_at(d, d->p[node], (size_t)node + 1, &number)) return fallback;
    return number;
}

const char *byml_string(const BymlDoc *d, uint32_t node) {
    if (!node || !header_ok(d) || node >= d->n) return NULL;
    return data_at(d, d->p[node], (size_t)node + 1);
}