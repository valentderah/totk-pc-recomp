// A reader for Nintendo BYML (".byml"/".bgyml") documents, the actor parameter files the game ships inside
// Pack/Actor/*.pack.zs. It is the general version of the small reader in runtime/resolution.c (functions u32_at,
// key_name, field), which reads one kind of table (the HeapDef) with one hash lookup; this module reads whole
// documents so a caller can walk the collision shapes that describe an actor's body.
//
// Layout, little endian, as TotK writes it (version 4..7):
//
//     header   16 bytes: "YB", u16 version, then u32 offsets of the hash key table, the string table and the root
//              node
//     0xC2     a string table: u8 0xC2, u24 count, count+1 u32 offsets relative to the node start (the last one ends
//              the last string), then the NUL-terminated strings in alphabetical order
//     0xC1     a hash keyed by name: u8 0xC1, u24 count, then count entries of 8 bytes {u24 key index, u8 type,
//              u32 value}; the key index is zero based into the hash key table, and the value is inline for value
//              nodes and an offset to the node for container nodes
//     0xC0     an array: u8 0xC0, u24 count, count type bytes padded to a multiple of 4, then count u32 values, each
//              inline for value nodes or an offset for container nodes
//     0x20/0x21 a hash keyed by a 32-bit hash instead of a name (0x21 entries carry a second word); the entry keys are
//              not available, so byml_entry_key returns NULL for them and byml_key never matches
//
// Value types: 0xA0 string (an index into the string table, zero based), 0xA1 binary data, 0xA2 file data, 0xD0 bool
// (the low byte), 0xD1 int32, 0xD2 float32, 0xD3 uint32, 0xD4 int64, 0xD5 uint64, 0xD6 double, 0xFF null. The 32-bit
// values are inline; the 64-bit values store an offset to their 8 bytes instead.
//
// The central unit is a node offset: the file offset of a type byte. Every accessor takes a node offset and reads
// p[node] as the type and (for value nodes) p[node+1] as the four-byte field. byml_key and byml_at return node
// offsets too: for a value entry this is the offset of the entry's type byte, for a container entry it is the offset
// of the container itself. Array elements of a value type have their type in the array's type list and their value in
// the array's value list, so they cannot be addressed by one node offset; read those with byml_array_* instead
// (byml_at returns 0 for them).
//
// Every accessor validates its offsets and returns 0, NULL or the fallback for anything malformed: the file may be
// truncated or hostile and nothing reads outside data[0..n). No allocation, no globals, no I/O; the document stays in
// the caller's buffer for as long as the returned pointers are used.

#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct BymlDoc {
    const uint8_t *p;   // the document bytes, caller-owned
    size_t n;           // its length
    uint32_t keys;      // the hash key table node offset (a 0xC2 node)
} BymlDoc;

enum {
    BYML_ARRAY = 0xC0,
    BYML_HASH = 0xC1,
    BYML_HASH_NUM = 0x20,
    BYML_HASH_NUM_VAL = 0x21,
    BYML_STRING_TABLE = 0xC2,
    BYML_STRING = 0xA0,
    BYML_BINARY = 0xA1,
    BYML_FILE = 0xA2,
    BYML_BOOL = 0xD0,
    BYML_INT = 0xD1,
    BYML_FLOAT = 0xD2,
    BYML_UINT = 0xD3,
    BYML_INT64 = 0xD4,
    BYML_UINT64 = 0xD5,
    BYML_DOUBLE = 0xD6,
    BYML_NULL = 0xFF,
};

// Fills doc from a BYML v2..v7 little-endian document and returns 1, or leaves it and returns 0 when data is not one.
int byml_open(BymlDoc *doc, const void *data, size_t n);

uint32_t byml_root(const BymlDoc *doc);                 // the root node offset, 0 when absent
uint8_t byml_type(const BymlDoc *doc, uint32_t node);   // the node's type byte, 0 out of range
size_t byml_count(const BymlDoc *doc, uint32_t node);   // array or hash entry count, 0 when not a container

// Hash nodes (BYML_HASH): byml_key finds entry `name` and returns its node offset (see the top comment), 0 when there
// is no such entry or node is not a name-keyed hash. byml_entry_key returns entry i's key name (NULL when node is not
// a name-keyed hash or i is out of range).
uint32_t byml_key(const BymlDoc *doc, uint32_t node, const char *name);
const char *byml_entry_key(const BymlDoc *doc, uint32_t node, size_t i);

// Array nodes (BYML_ARRAY). byml_at returns element i's node offset when element i is a container, 0 when it is a
// value type or i is out of range. The value-typed elements are read with the byml_array_* accessors, which take the
// element's type and value from the array's own lists.
uint32_t byml_at(const BymlDoc *doc, uint32_t node, size_t i);
uint8_t byml_array_type(const BymlDoc *doc, uint32_t node, size_t i);
uint32_t byml_array_value(const BymlDoc *doc, uint32_t node, size_t i);
double byml_array_number(const BymlDoc *doc, uint32_t node, size_t i, double fallback);
const char *byml_array_string(const BymlDoc *doc, uint32_t node, size_t i);

// Value nodes at any node offset: byml_number reads bool/int/uint/float (and the 64-bit kinds) as a double and returns
// fallback for anything else; byml_string returns a string node's bytes (and a binary/file node's data) or NULL.
double byml_number(const BymlDoc *doc, uint32_t node, double fallback);
const char *byml_string(const BymlDoc *doc, uint32_t node);
