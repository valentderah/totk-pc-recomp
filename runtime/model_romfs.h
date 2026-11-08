// The RomFS model reader (model_romfs.c): resolve an actor name to its decoded BFRES model, read straight from the
// game's own archive and decoded by the MeshCodec bridge. This is the runtime replacement for the offline mesh pack.
#pragma once

#include "bfres.h"

// The decoded model of actor, or NULL (no actor pack, no ModelInfo, no model file, or the MeshCodec bridge is missing).
// The pointer is valid until the next resolve evicts it; a 21-part blueprint never evicts anything.
const BfresModel *model_romfs_resolve(const char *actor);