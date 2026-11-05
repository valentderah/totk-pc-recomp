// One blueprint file, of either form this port reads, turned into the blob the game stores and the parsed view of it.
//
// ".cai" is the blob itself (blueprint_cai.h). ".txt" is the JSON export a build site offers (blueprint_txt.h): the same
// blob, gzip-compressed and Base64-encoded inside a JSON object. Both end in the same validator, so a file of either
// form has to be a real blueprint before anything may be written to the save data.
//
// The caller owns the storage: the parsed blueprint's blob pointer and its actor names point into it, so it must outlive
// every use of the result.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "blueprint_cai.h"

#ifdef __cplusplus
extern "C" {
#endif

// A file large enough to read whole. A .cai is 6688 or 6712 bytes; the largest .txt export of a 21-actor draft is under
// 60 KB. Reading past this would only tell a file that is not a blueprint from another one that is not.
#define BPF_READ_MAX 262144

// Reads the blueprint in the file at path and parses it. storage must hold BPF_READ_MAX bytes; out is filled on BPC_OK
// only. The log gets one line either way, naming the form the file turned out to be and what the parser said about it.
//
// The form is decided by the file's content, not its extension: a .txt-named file holding a bare blob is read as one,
// and the other way round. A file whose name says neither form is not read at all.
enum BpcResult bp_file_read(const char *path, uint8_t *storage, BpcBlueprint *out);

#ifdef __cplusplus
}
#endif
