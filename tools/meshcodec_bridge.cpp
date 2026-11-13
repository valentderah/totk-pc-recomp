// The C door over the MeshCodec decoder that runtime/model_meshcodec.h declares.
//
// This file is this repository's own code: it contains no MeshCodec source and no Nintendo data. It is not compiled as
// part of the game build - tools/meshcodec_bridge.cpp is built by tools/build_meshcodec.py against a MeshCodec checkout,
// and the resulting object is archived together with the decoder into build/<version>/libmeshcodec.a. Only that library
// is linked into totk.exe, and only when it exists (see tools/build_game.py and THIRD_PARTY_NOTICES.md).
//
// The contract is the one symbol in runtime/model_meshcodec.h: decode one .bfres.mc, in memory, into the buffer the game
// allocates for it (MCPK's flags give the size), and report nothing when the header is not a model container.
#include "mc_MeshCodec.h"

#include <cstdlib>

extern "C" int model_meshcodec_decode(const void *src, size_t src_size, void *dst, size_t dst_cap, size_t *out_size) {
    if (!src || !dst || !out_size) return 0;
    if (src_size < 0xc) return 0;   // shorter than an MCPK header
    const auto *header = reinterpret_cast<const mc::ResMeshCodecPackageHeader *>(src);
    if (header->magic != mc::ResMeshCodecPackageHeader::cMagic) return 0;
    const size_t need = header->GetDecompressedSize();
    if (dst_cap < need) return 0;
    // The reference decoder allocates a 256 MiB work buffer. The header's own workMemSize is only known after the BFRES
    // body is unpacked, so this matches the reference's allocation rather than trying to size it first.
    void *work = malloc(0x10000000);
    if (!work) return 0;
    const bool ok = mc::DecompressMC(dst, dst_cap, src, src_size, work, 0x10000000);
    free(work);
    if (!ok) return 0;
    *out_size = need;
    return 1;
}