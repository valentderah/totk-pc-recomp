// A ".txt" blueprint file: the JSON object HyruleWorks offers as a build's export, which carries the same CAI blob a
// .cai file holds. The object has "combinedActorInfoBase64" (the 6688-byte blob, gzip-compressed and Base64-encoded),
// "combinedActorInfo" (the same content already parsed into JSON objects), "parts" and "unknownActorNames". Only the
// Base64 member is read: it is the exact bytes the game's own files hold, while the parsed members would have to be
// serialized again, and every exporter writes the Base64 member.
//
// The file is read into memory, decoded here into the 6688-byte blob, and then checked by blueprint_cai.h like a .cai
// file, so one validator decides whether anything may reach the save data.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Why a .txt file did not give a blueprint blob.
enum BtxResult {
    BTX_OK = 0,
    BTX_EMPTY,          // no input, or the output buffer is 0 bytes
    BTX_NO_JSON,        // the file is not a JSON object: it does not start with '{'
    BTX_NO_MEMBER,      // the object has no "combinedActorInfoBase64" string member
    BTX_BAD_BASE64,     // the member is not Base64, or is shorter than a gzip stream
    BTX_BAD_STREAM,     // the member decoded to something that is not a gzip stream (see blueprint_inflate.h)
    BTX_TOO_LARGE,      // the stream holds more than the blob the game stores
    BTX_WRONG_SIZE,     // the stream decoded to a byte count that is not the blob's
};

// Decodes the .txt file in data (size bytes) into blob (blob_cap bytes, so 6688 at least) and sets *blob_size on BTX_OK
// only. The blob still has to pass bpc_parse before it may be imported: this reader only unwraps the container.
enum BtxResult btx_decode(const void *data, size_t size, void *blob, size_t blob_cap, size_t *blob_size);

// A short human-readable reason for a result, for the log.
const char *btx_result_text(enum BtxResult result);

#ifdef __cplusplus
}
#endif
