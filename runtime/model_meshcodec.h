// The C interface to the MeshCodec decoder (the GPL prototype lives in build/, linked as a static library). It decodes
// one .bfres.mc in memory to the full BFRES body plus the FMSH geometry, laid out the way bfres.h expects: the body at
// the front and the geometry pool after it.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Decodes src[0..src_size) (a .bfres.mc) into dst[0..dst_cap), the buffer the game allocates for it (the MCPK flags
// give its size). *out_size gets the allocation size. Returns 1 on success, 0 when the source is not a model container
// or the buffer is too small. dst must be at least as large as the MCPK flags declare.
int model_meshcodec_decode(const void *src, size_t src_size, void *dst, size_t dst_cap, size_t *out_size);