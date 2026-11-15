// Walks a BYML document through the real runtime/byml.c for tests/byml_test.py: prints the document as text and then
// runs the accessor safety probes every caller relies on (never reads outside the buffer, never follows an offset that
// does not resolve). The script writes the documents, passes their paths as arguments and compares the dump with what
// Python built; the exit code is the number of probe failures.
//
// Built and run by tests/byml_test.py. No game files.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "byml.h"

// The walker's recursion budget: the script's expected dump repeats the cut-off, so a self-referential document still
// has a finite, predictable dump.
#define MAX_DEPTH 32
#define MAX_NODES 4096

// A double that the documents never hold: byml_number's fallback when it cannot read a value.
#define FALLBACK NAN

static int failures;
static int visited;

static int is_container(uint8_t type) {
    return type == BYML_ARRAY || type == BYML_HASH || type == BYML_HASH_NUM || type == BYML_HASH_NUM_VAL;
}

static void indent(int depth) {
    for (int i = 0; i < depth; i++) fputs("  ", stdout);
}

// The value of a value-typed array element: the element's type and value are in the array's own lists, so the walker
// reads them through the byml_array_* accessors. The 4-byte word is printed for the inline kinds so the test checks
// byml_array_value on real data too.
static void print_array_value(const BymlDoc *doc, uint32_t array, size_t i, uint8_t type) {
    switch (type) {
    case BYML_STRING: {
        const char *text = byml_array_string(doc, array, i);
        if (text) printf("0xa0 string \"%s\"\n", text);
        else puts("0xa0 string NULL");
        break;
    }
    case BYML_BINARY:
    case BYML_FILE:
        printf("0x%02x %s\n", type, byml_array_string(doc, array, i) ? "data" : "no data");
        break;
    case BYML_BOOL:
    case BYML_INT:
    case BYML_FLOAT:
    case BYML_UINT:
        printf("0x%02x number %.17g word 0x%08x\n", type, byml_array_number(doc, array, i, FALLBACK),
               byml_array_value(doc, array, i));
        break;
    case BYML_INT64:
    case BYML_UINT64:
    case BYML_DOUBLE:
        printf("0x%02x number %.17g\n", type, byml_array_number(doc, array, i, FALLBACK));
        break;
    case BYML_NULL:
        puts("0xff null");
        break;
    default:
        printf("0x%02x unknown\n", type);
        break;
    }
}

static void dump_node(const BymlDoc *doc, uint32_t node, int depth, const char *label) {
    uint8_t type;
    if (depth > MAX_DEPTH) {
        indent(depth);
        puts("...");
        return;
    }
    if (++visited > MAX_NODES) {
        indent(depth);
        puts("...");
        return;
    }
    indent(depth);
    fputs(label, stdout);
    if (!node) {            // an offset that did not resolve: absent, not a node of type 0
        puts("0x00 unknown");
        return;
    }
    type = byml_type(doc, node);
    switch (type) {
    case BYML_HASH: {
        size_t count = byml_count(doc, node);
        printf("0xc1 hash %zu\n", count);
        for (size_t i = 0; i < count; i++) {
            const char *key = byml_entry_key(doc, node, i);
            char entry[256];
            if (!key) {
                indent(depth + 1);
                printf("entry %zu key NULL\n", i);
                continue;
            }
            snprintf(entry, sizeof entry, "entry %zu key \"%s\" ", i, key);
            dump_node(doc, byml_key(doc, node, key), depth + 1, entry);
        }
        break;
    }
    case BYML_ARRAY: {
        size_t count = byml_count(doc, node);
        printf("0xc0 array %zu\n", count);
        for (size_t i = 0; i < count; i++) {
            uint8_t elem = byml_array_type(doc, node, i);
            char entry[32];
            if (!is_container(elem)) {
                indent(depth + 1);
                printf("elem %zu ", i);
                print_array_value(doc, node, i, elem);
                continue;
            }
            snprintf(entry, sizeof entry, "elem %zu ", i);
            dump_node(doc, byml_at(doc, node, i), depth + 1, entry);
        }
        break;
    }
    case BYML_HASH_NUM:
    case BYML_HASH_NUM_VAL:
        // A table keyed by 32-bit hashes: its count is readable, its entries are not addressable (they carry no type),
        // so a name can never find one and byml_at must not mistake it for an array.
        printf("0x%02x hashtable %zu\n", type, byml_count(doc, node));
        if (byml_entry_key(doc, node, 0) || byml_key(doc, node, "x") != 0 || byml_at(doc, node, 0) != 0) {
            failures++;
            fprintf(stderr, "probe failed: a hash table keyed by 32-bit hashes matched a name or an array element\n");
        }
        break;
    case BYML_STRING: {
        const char *text = byml_string(doc, node);
        if (text) printf("0xa0 string \"%s\"\n", text);
        else puts("0xa0 string NULL");
        break;
    }
    case BYML_BINARY:
    case BYML_FILE:
        printf("0x%02x %s\n", type, byml_string(doc, node) ? "data" : "no data");
        break;
    case BYML_NULL:
        puts("0xff null");
        break;
    default:
        if (type >= BYML_BOOL && type <= BYML_DOUBLE) printf("0x%02x number %.17g\n", type, byml_number(doc, node, FALLBACK));
        else printf("0x%02x unknown\n", type);
        break;
    }
}

static int same(double a, double b) { return a == b || (a != a && b != b); }

static void expect(int ok, const char *what) {
    if (!ok) {
        failures++;
        fprintf(stderr, "probe failed: %s\n", what);
    }
}

// The promises every accessor makes on bad input: an offset outside the document or 0 answers absent, never faults.
static void probes(const BymlDoc *doc) {
    uint32_t root = byml_root(doc);
    uint32_t outside = doc->n <= 0xffffffefu ? (uint32_t)doc->n + 16 : 0xffffffffu;
    expect(byml_type(doc, outside) == 0, "byml_type past the end");
    expect(byml_count(doc, outside) == 0, "byml_count past the end");
    expect(byml_at(doc, outside, 0) == 0, "byml_at past the end");
    expect(byml_array_type(doc, outside, 0) == 0, "byml_array_type past the end");
    expect(byml_array_value(doc, outside, 0) == 0, "byml_array_value past the end");
    expect(same(byml_array_number(doc, outside, 0, FALLBACK), FALLBACK), "byml_array_number past the end");
    expect(byml_array_string(doc, outside, 0) == NULL, "byml_array_string past the end");
    expect(byml_entry_key(doc, outside, 0) == NULL, "byml_entry_key past the end");
    expect(same(byml_number(doc, outside, FALLBACK), FALLBACK), "byml_number past the end");
    expect(byml_string(doc, outside) == NULL, "byml_string past the end");
    expect(byml_count(doc, 0) == 0, "byml_count(0)");
    expect(byml_at(doc, 0, 0) == 0, "byml_at(0)");
    expect(byml_array_type(doc, 0, 0) == 0, "byml_array_type(0)");
    expect(byml_array_value(doc, 0, 0) == 0, "byml_array_value(0)");
    expect(same(byml_array_number(doc, 0, 0, FALLBACK), FALLBACK), "byml_array_number(0)");
    expect(byml_array_string(doc, 0, 0) == NULL, "byml_array_string(0)");
    expect(byml_entry_key(doc, 0, 0) == NULL, "byml_entry_key(0)");
    expect(byml_key(doc, 0, "x") == 0, "byml_key(0)");
    expect(same(byml_number(doc, 0, FALLBACK), FALLBACK), "byml_number(0)");
    expect(byml_string(doc, 0) == NULL, "byml_string(0)");
    if (!root) {
        if (!failures) puts("probes ok");
        return;
    }
    expect(is_container(byml_type(doc, root)), "the root is a container");
    expect(byml_key(doc, root, NULL) == 0, "byml_key with no name");
    expect(byml_key(doc, root, "no such key in this document") == 0, "byml_key on a missing name");
    expect(byml_at(doc, root, byml_count(doc, root)) == 0, "byml_at out of range");
    expect(byml_at(doc, root, byml_count(doc, root) + 1000000) == 0, "byml_at far out of range");
    expect(byml_array_type(doc, root, byml_count(doc, root)) == 0, "byml_array_type out of range");
    expect(byml_array_value(doc, root, byml_count(doc, root)) == 0, "byml_array_value out of range");
    expect(same(byml_array_number(doc, root, byml_count(doc, root), FALLBACK), FALLBACK), "byml_array_number out of range");
    expect(byml_array_string(doc, root, byml_count(doc, root)) == NULL, "byml_array_string out of range");
    expect(byml_entry_key(doc, root, byml_count(doc, root)) == NULL, "byml_entry_key out of range");
    expect(same(byml_number(doc, root, FALLBACK), FALLBACK), "byml_number of a container");
    expect(byml_string(doc, root) == NULL, "byml_string of a container");
    if (byml_type(doc, root) == BYML_HASH) expect(byml_at(doc, root, 0) == 0, "byml_at on a hash");
    if (byml_type(doc, root) == BYML_ARRAY) {
        expect(byml_key(doc, root, "x") == 0, "byml_key on an array");
        expect(byml_entry_key(doc, root, 0) == NULL, "byml_entry_key on an array");
    }
    if (!failures) puts("probes ok");
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *cut = slash && (!backslash || slash > backslash) ? slash : backslash;
    return cut ? cut + 1 : path;
}

static uint8_t *load(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    long length;
    uint8_t *data;
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = (uint8_t *)malloc((size_t)length + 1); // +1 so an empty file still has a buffer to hand byml_open
    if (!data) {
        fclose(file);
        return NULL;
    }
    if (length && fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return data;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: byml_main <document>...\n");
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        size_t size = 0;
        uint8_t *data = load(argv[i], &size);
        BymlDoc doc;
        int opened = 0;
        memset(&doc, 0, sizeof doc);
        printf("file %s\n", base_name(argv[i]));
        if (!data) {
            failures++;
            fprintf(stderr, "cannot read %s\n", argv[i]);
        } else {
            opened = byml_open(&doc, data, size);
        }
        printf("open %d\n", opened);
        if (opened) {
            uint32_t root = byml_root(&doc);
            visited = 0;
            if (!root) puts("root none");
            else dump_node(&doc, root, 0, "");
        }
        probes(&doc);
        free(data);
    }
    return failures ? 1 : 0;
}