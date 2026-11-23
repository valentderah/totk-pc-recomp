// Deflate and gzip decoding (RFC 1951 and RFC 1952), for the .txt blueprint files HyruleWorks exports: they carry the
// 6688-byte CAI blob gzip-compressed and Base64-encoded inside a JSON object. Nothing else in the port decompresses a
// file, so this is the whole of the decoder it needs.
//
// The code is a plain inflate over a caller-provided output buffer: it never allocates and never writes past the end,
// which keeps a hostile or damaged file from exhausting the host's memory. A stream that would produce more bytes than
// the buffer holds is rejected rather than truncated.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Why a stream did not decode.
enum BpiResult {
    BPI_OK = 0,
    BPI_EMPTY,          // no input, or the output buffer is 0 bytes
    BPI_NOT_DEFLATE,    // the input does not begin with a deflate or gzip stream
    BPI_DAMAGED,        // a block, a code or a stored length is inconsistent
    BPI_TRUNCATED,      // the input ended before the stream did
    BPI_TOO_LARGE,      // the stream holds more bytes than the output buffer allows
    BPI_CHECKSUM,       // the gzip trailer's CRC32 or length disagrees with the output
};

// Decodes the gzip stream in data (size bytes from the start of the file) into out (out_cap bytes). Gzip's header may
// carry a name, a comment and a header CRC; the optional fields are skipped. The trailer's CRC32 and its length are both
// checked, so a truncated download is reported instead of half-decoded.
//
// A bare deflate stream without the gzip wrapper is decoded too, with neither check, because the API that exported the
// tested files is not the only producer of these .txt files.
// *out_size is set to the number of bytes written on BPI_OK only.
enum BpiResult bpi_gunzip(const void *data, size_t size, void *out, size_t out_cap, size_t *out_size);

// A short human-readable reason for a result, for the log.
const char *bpi_result_text(enum BpiResult result);

#ifdef __cplusplus
}
#endif
