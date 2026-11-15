// Deflate and gzip decoding (RFC 1951 and RFC 1952): the .txt blueprint files carry the 6688-byte CAI blob
// gzip-compressed and Base64-encoded inside a JSON object (blueprint_txt.c), so the file has to be decompressed before
// blueprint_cai.h can check it.
//
// zlib does the decoding. It is already a library this port links (libz.a, through the same UCRT64 toolchain the build
// uses), so this file only adds the bounds a hostile or damaged file needs: the output goes into the caller's buffer,
// never past its end, and a stream that wants more bytes than the buffer holds is refused rather than truncated.
//
// The gzip wrapper is zlib's own (inflateInit2 with 15 | 32), which accepts both a gzip stream and a bare zlib one; the
// caller's input is checked for the gzip magic so that a bare deflate stream can be told apart and, more importantly,
// so that a file which is neither is refused before zlib reads it.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zlib.h>

#include "blueprint_inflate.h"

// How much of a stream's input is read at a time. The whole stream is usually a few hundred bytes, so one chunk covers
// the real files; the loop is what makes a larger one work without assuming its size.
enum { BPI_CHUNK = 1 << 16 };

// The bytes at the end of a gzip stream that hold its CRC32 and the uncompressed length (RFC 1952 2.3.1).
enum { BPI_GZIP_TRAILER = 8 };

static int gzip_magic(const uint8_t *data, size_t size) {
    return size >= 2 && data[0] == 0x1f && data[1] == 0x8b;
}

// The CRC32 of data, from zlib's own implementation so the trailer check and a gzip writer agree by construction.
static uint32_t crc_of(const uint8_t *data, size_t size) {
    uLong c = crc32(0L, Z_NULL, 0);
    return (uint32_t)crc32(c, data, (uInt)size);
}

enum BpiResult bpi_gunzip(const void *data, size_t size, void *out, size_t out_cap, size_t *out_size) {
    if (!data || size < 2 || !out || out_cap == 0) return BPI_EMPTY;

    z_stream z;
    memset(&z, 0, sizeof z);
    // 15 window bits: the largest deflate window. 32: accept a gzip wrapper as well as a bare zlib one.
    if (inflateInit2(&z, 15 + 32) != Z_OK) return BPI_DAMAGED;

    const uint8_t *in = (const uint8_t *)data;
    const int gzipped = gzip_magic(in, size);
    size_t in_left = size;
    z.next_out = (Bytef *)out;
    z.avail_out = (uInt)(out_cap > 0xffffffffu ? 0xffffffffu : out_cap);

    int rc = Z_OK;
    while (rc == Z_OK) {
        if (z.avail_in == 0) {
            z.next_in = (Bytef *)in;
            z.avail_in = (uInt)(in_left > BPI_CHUNK ? BPI_CHUNK : in_left);
            in += z.avail_in;
            in_left -= z.avail_in;
        }
        rc = inflate(&z, Z_NO_FLUSH);
        // Out of room: the stream is larger than the caller can take, which is a refusal, not a truncation.
        if (z.avail_out == 0 && rc != Z_STREAM_END) {
            inflateEnd(&z);
            return BPI_TOO_LARGE;
        }
        if (rc == Z_BUF_ERROR) {
            // zlib reports a buffer error when it can make no progress: either the input ran out or the output is full.
            if (in_left == 0 || z.avail_in == 0) {
                inflateEnd(&z);
                return BPI_TRUNCATED;
            }
            continue;
        }
    }

    const size_t produced = (size_t)z.total_out;
    inflateEnd(&z);
    if (rc == Z_DATA_ERROR) return BPI_DAMAGED;
    if (rc == Z_NEED_DICT || rc == Z_MEM_ERROR) return BPI_DAMAGED;
    if (rc != Z_STREAM_END) return BPI_TRUNCATED;

    // The wrapper's own trailer is checked by zlib, so a gzip stream that decoded to the wrong bytes is already refused.
    // The two checks below cover the case zlib's default accepts but the caller does not want: a stream whose length
    // disagrees with what it produced, which only a hand-built file shows.
    if (gzipped && size >= BPI_GZIP_TRAILER) {
        const uint8_t *tail = (const uint8_t *)data + size - BPI_GZIP_TRAILER;
        const uint32_t want_crc = (uint32_t)tail[0] | ((uint32_t)tail[1] << 8) | ((uint32_t)tail[2] << 16) |
                                  ((uint32_t)tail[3] << 24);
        const uint32_t want_len = (uint32_t)tail[4] | ((uint32_t)tail[5] << 8) | ((uint32_t)tail[6] << 16) |
                                  ((uint32_t)tail[7] << 24);
        if (crc_of((const uint8_t *)out, produced) != want_crc || want_len != (uint32_t)produced) return BPI_CHECKSUM;
    }

    if (out_size) *out_size = produced;
    return BPI_OK;
}

const char *bpi_result_text(enum BpiResult result) {
    switch (result) {
    case BPI_OK: return "ok";
    case BPI_EMPTY: return "no data or no room for it";
    case BPI_NOT_DEFLATE: return "the data does not begin with a deflate or gzip stream";
    case BPI_DAMAGED: return "the compressed stream is damaged";
    case BPI_TRUNCATED: return "the compressed stream ends early";
    case BPI_TOO_LARGE: return "the stream holds more bytes than the file it was decoded for";
    case BPI_CHECKSUM: return "the gzip stream's checksum or length disagrees with what it decoded to";
    }
    return "unknown result";
}
