// Reads one blueprint file of either form into the blob the game stores (blueprint_file.h): ".cai" holds the blob
// itself, ".txt" holds the same blob gzip-compressed and Base64-encoded in a JSON object. Both end in blueprint_cai.h's
// validator, so one place decides whether a file may reach the save data.
//
// The form is decided by the content rather than the name: the first bytes of a .cai file are "CmbAct", and the first
// non-blank byte of a .txt file is '{'. A name that says neither form is refused without reading the file, which keeps a
// mis-named large file from being read at all.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "blueprint_cai.h"
#include "blueprint_file.h"
#include "blueprint_inflate.h"
#include "blueprint_txt.h"
#include "host.h"
#include "platform/platform.h"

void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// The extension of path, lower case and without the dot: "" when it has none.
static void extension_of(const char *path, char *out, size_t cap) {
    const char *dot = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') dot = NULL;   // a dot before a separator belongs to a folder, not to the name
        else if (*p == '.') dot = p;
    }
    out[0] = 0;
    if (!dot || !dot[1]) return;
    size_t n = 0;
    for (const char *p = dot + 1; *p && n + 1 < cap; p++) {
        char c = *p;
        out[n++] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    }
    out[n] = 0;
}

// The first byte of data that is not a space, a tab, a line ending or a UTF-8 byte order mark's remainder. A .txt file
// written by a Windows editor may start with a BOM, which is skipped here.
static const uint8_t *first_content(const uint8_t *data, size_t size) {
    size_t i = 0;
    if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) i = 3;
    for (; i < size; i++) {
        const uint8_t c = data[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return data + i;
    }
    return NULL;
}

enum BpcResult bp_file_read(const char *path, uint8_t *storage, BpcBlueprint *out) {
    if (!path || !*path) return BPC_TOO_SHORT;
    char ext[16];
    extension_of(path, ext, sizeof ext);
    const int cai = !strcmp(ext, "cai");
    const int txt = !strcmp(ext, "txt");
    if (!cai && !txt) return BPC_BAD_MAGIC;

    PlatFile *f = plat_file_open(path);
    if (!f) {
        rt_log("blueprint: cannot open %s", path);
        return BPC_TOO_SHORT;
    }
    // The whole file is read, up to the one size a blueprint can have: the .txt form's blob sits inside a JSON object
    // whose length the parser cannot know in advance.
    int64_t got = plat_file_pread(f, storage, (uint32_t)BPF_READ_MAX, 0);
    plat_file_close(f);
    if (got <= 0) {
        rt_log("blueprint: cannot read %s", path);
        return BPC_TOO_SHORT;
    }
    const size_t size = (size_t)got;
    const uint8_t *head = first_content(storage, size);

    enum BpcResult result;
    size_t blob_size = size;
    if (cai && size >= BPC_BLOB_SIZE && !memcmp(storage, "CmbAct", 6)) {
        result = bpc_parse(storage, size, out);
    } else if (txt && head && *head == '{') {
        // The JSON form: unwrap it into the same storage, then validate the result like a .cai file.
        uint8_t blob[BPC_BLOB_SIZE];
        size_t blob_bytes = 0;
        enum BtxResult tx = btx_decode(storage, size, blob, sizeof blob, &blob_bytes);
        if (tx != BTX_OK) {
            rt_log("blueprint: %s: %s", path, btx_result_text(tx));
            return BPC_BAD_MAGIC;
        }
        memcpy(storage, blob, blob_bytes);
        blob_size = blob_bytes;
        result = bpc_parse(storage, blob_size, out);
    } else {
        result = cai ? BPC_BAD_MAGIC : BPC_TOO_SHORT;
        rt_log("blueprint: %s: not a %s file (%llu bytes)", path, cai ? ".cai" : ".txt",
               (unsigned long long)size);
        return result;
    }
    rt_log("blueprint: %s, %llu bytes: %s", path, (unsigned long long)blob_size, bpc_result_text(result));
    return result;
}
