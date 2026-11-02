// The game's resource size table (restbl.c): System/Resource/ResourceSizeTable.Product.<n>.Nin_NX_NVN.rsizetable.zs, a
// zstd-compressed RESTBL. The game sizes the buffer it loads a file into from the file's entry, so a mod file larger
// than the original needs a larger entry, and a file a mod adds needs one of its own. romfs.c merges the tables the
// mods ship and raises the entries of the files they serve (romfs.c restbl_overlay).
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct Restbl Restbl;

// The table of a decompressed RESTBL (copied); NULL when it is not one.
Restbl *restbl_parse(const uint8_t *data, size_t n);
void restbl_free(Restbl *t);
// The table's key of a RomFS path: the path without a trailing .zs or .mc, into buf.
const char *restbl_key(const char *path, char *buf, size_t n);
// 1 with *size when key has an entry.
int restbl_get(const Restbl *t, const char *key, uint32_t *size);
// The entry of key raised to size (added when missing); 1 when the table changed.
int restbl_raise(Restbl *t, const char *key, uint32_t size);
// Every entry of shipped larger than base's (or missing from base) raised in t; the number of entries raised.
int restbl_merge(Restbl *t, const Restbl *base, const Restbl *shipped);
// Each entry of from whose key one of keys[0..n) names, raised in t; the number raised.
int restbl_take(Restbl *t, const Restbl *from, const char *const *keys, size_t n);
// The table as a decompressed RESTBL (malloc'd); NULL when out of memory.
uint8_t *restbl_write(const Restbl *t, size_t *n);

// The entry a file of the key's type and decompressed size needs when the game has none for it; 0 for a type the
// table never lists (streamed sound and video).
uint32_t restbl_estimate(const char *key, uint64_t size);
// The entry of a file the game lists with `entry` for its own copy of old bytes, now new bytes: raised by the growth
// times the type's factor, never lowered. scaled: the sizes are compressed ones (a mesh codec .mc file), whose entry
// grows in proportion.
uint32_t restbl_grow(const char *key, uint32_t entry, uint64_t old, uint64_t new_size, int scaled);
