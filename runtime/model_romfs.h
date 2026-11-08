// The RomFS model reader (model_romfs.c): resolve an actor name to its decoded BFRES model, read straight from the
// game's own archive and decoded by the MeshCodec bridge. This is the runtime replacement for the offline mesh pack.
#pragma once

#include "bfres.h"

// The decoded model of actor, or NULL (no actor pack, no ModelInfo, no model file, or the MeshCodec bridge is missing).
// The pointer is valid until the next resolve evicts it; a 21-part blueprint never evicts anything.
const BfresModel *model_romfs_resolve(const char *actor);

// The decoded model for a MODEL name rather than an actor name. A fused material names its model directly (its
// AttachmentParam's SubModel, e.g. Item_Enemy_151_Sub_Lsword), and those names have no Pack/Actor entry to read a
// ModelInfo from, so they take this path instead of model_romfs_resolve.
const BfresModel *model_romfs_resolve_model(const char *model_name);


// The largest side of a decoded model's geometry in metres, or 0 when it is not known.
float model_romfs_extent(const BfresModel *m);