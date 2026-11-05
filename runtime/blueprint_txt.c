// The ".txt" blueprint file (blueprint_txt.h): the JSON object a build site offers as a build's export, which carries
// the same 6688-byte CAI blob a .cai file holds.
//
// The object's "combinedActorInfoBase64" member is the blob, gzip-compressed and Base64-encoded. It is the member this
// reader takes, because it is exactly the bytes the game's own files hold: the object's other members ("actors", "bonds",
// the bounding box, the part list) are the same content already parsed into JSON, and writing those back would mean
// serializing them again with no guarantee of the byte layout the game expects.
//
// No general JSON parser: the files are one flat object written by one exporter, so a bounded scanner finds the member's
// name and reads the run of Base64 after it. A file that is not shaped like that is refused, not guessed at.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "blueprint_cai.h"
#include "blueprint_inflate.h"
#include "blueprint_txt.h"

// The member's name in the object. The quoted form is what the scanner looks for, so a file that mentions it outside a
// string does not match.
#define BTX_MEMBER "\"combinedActorInfoBase64\""

// The Base64 run after the member can be long only if the file is damaged: a real export's member is a few thousand
// characters, and the largest possible blob compresses to a few kilobytes. A longer run is refused before allocating.
enum { BTX_MAX_MEMBER = 1 << 20 };

// Skips a leading UTF-8 byte order mark and JSON whitespace; returns the first byte that is neither, or NULL.
static const uint8_t *first_content(const uint8_t *data, size_t size) {
    size_t i = 0;
    if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) i = 3;
    for (; i < size; i++) {
        const uint8_t c = data[i];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return data + i;
    }
    return NULL;
}

// The Base64 value of a character, or -1.
static int b64_value(uint8_t c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Decodes the Base64 run text[0..n) into out (cap bytes). *out_size gets the bytes written. 0 when the run is not Base64
// or does not fit; a run whose length is not a multiple of four is refused rather than padded, because the exporter emits
// padded Base64 and anything else is a damaged file.
static int b64_decode(const uint8_t *text, size_t n, uint8_t *out, size_t cap, size_t *out_size) {
    if (n == 0 || n % 4) return 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        const int v0 = b64_value(text[i]), v1 = b64_value(text[i + 1]);
        if (v0 < 0 || v1 < 0) return 0;
        const int pad2 = text[i + 2] == '=', pad3 = text[i + 3] == '=';
        if (pad2 && !pad3) return 0;
        const int v2 = pad2 ? 0 : b64_value(text[i + 2]);
        const int v3 = pad3 ? 0 : b64_value(text[i + 3]);
        if (v2 < 0 || v3 < 0) return 0;
        const size_t bytes = pad2 ? 1 : (pad3 ? 2 : 3);
        if (o + bytes > cap) return 0;
        out[o++] = (uint8_t)((v0 << 2) | (v1 >> 4));
        if (bytes >= 2) out[o++] = (uint8_t)((v1 << 4) | (v2 >> 2));
        if (bytes >= 3) out[o++] = (uint8_t)((v2 << 6) | v3);
        if (pad2 || pad3) {   // padding ends the value: anything after it is not Base64
            if (i + 4 != n) return 0;
        }
    }
    *out_size = o;
    return 1;
}

enum BtxResult btx_decode(const void *data, size_t size, void *blob, size_t blob_cap, size_t *blob_size) {
    if (!data || !size || !blob || !blob_cap) return BTX_EMPTY;
    const uint8_t *bytes = (const uint8_t *)data;
    const uint8_t *head = first_content(bytes, size);
    if (!head || *head != '{') return BTX_NO_JSON;

    // Find the member's name. It is written once per object, at the top level, so the first quoted occurrence is the
    // right one; a second one inside the value cannot exist, the value is Base64.
    const size_t name_len = sizeof BTX_MEMBER - 1;
    size_t at = 0;
    int found = 0;
    for (const uint8_t *p = head; (size_t)(p - bytes) + name_len <= size; p++) {
        if (!memcmp(p, BTX_MEMBER, name_len)) {
            at = (size_t)(p - bytes) + name_len;
            found = 1;
            break;
        }
    }
    if (!found) return BTX_NO_MEMBER;

    // After the name comes the colon that separates it from its value, then the opening quote of the string.
    size_t i = at;
    while (i < size && (bytes[i] == ' ' || bytes[i] == '\t' || bytes[i] == '\r' || bytes[i] == '\n')) i++;
    if (i >= size || bytes[i] != ':') return BTX_NO_MEMBER;
    i++;
    while (i < size && (bytes[i] == ' ' || bytes[i] == '\t' || bytes[i] == '\r' || bytes[i] == '\n')) i++;
    if (i >= size || bytes[i] != '"') return BTX_NO_MEMBER;
    i++;

    // The value runs to the closing quote. A backslash inside it is an escape, which a Base64 payload never contains:
    // the exporter writes the run plainly, so an escape means this is not the member it claims to be.
    const size_t start = i;
    size_t n = 0;
    while (i < size && bytes[i] != '"') {
        if (bytes[i] == '\\') return BTX_BAD_BASE64;
        i++;
        if (++n > BTX_MAX_MEMBER) return BTX_BAD_BASE64;
    }
    if (i >= size) return BTX_NO_MEMBER;   // the string is not closed

    // The run decodes to the compressed stream. Its size is bounded by the largest blob the game stores: a deflate
    // stream cannot encode 6688 bytes in fewer than a handful, and the container is not expected to be huge either.
    static uint8_t packed[BPC_BLOB_SIZE];
    size_t packed_bytes = 0;
    if (!b64_decode(bytes + start, n, packed, sizeof packed, &packed_bytes)) return BTX_BAD_BASE64;

    size_t decoded = 0;
    enum BpiResult r = bpi_gunzip(packed, packed_bytes, blob, blob_cap, &decoded);
    if (r == BPI_TOO_LARGE) return BTX_TOO_LARGE;
    if (r != BPI_OK) return BTX_BAD_STREAM;
    if (decoded != BPC_BLOB_SIZE) return BTX_WRONG_SIZE;
    if (blob_size) *blob_size = decoded;
    return BTX_OK;
}

const char *btx_result_text(enum BtxResult result) {
    switch (result) {
    case BTX_OK: return "ok";
    case BTX_EMPTY: return "no data or no room for it";
    case BTX_NO_JSON: return "not a JSON object: the file does not start with '{'";
    case BTX_NO_MEMBER: return "no combinedActorInfoBase64 string in the object";
    case BTX_BAD_BASE64: return "the combinedActorInfoBase64 member is not Base64";
    case BTX_BAD_STREAM: return "the member does not decompress to a blueprint";
    case BTX_TOO_LARGE: return "the member decompresses to more bytes than a blueprint holds";
    case BTX_WRONG_SIZE: return "the member decompresses to a byte count that is not a blueprint's";
    }
    return "unknown result";
}
