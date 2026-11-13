// The MeshCodec decoder's C side (model_meshcodec.h): the C++ bridge is compiled into libmeshcodec.a in build/, and
// this file only declares it so the runtime can call it. The prototype integration keeps the GPL sources out of the
// repository tree.
#include "model_meshcodec.h"

// model_meshcodec_decode is defined by the C++ bridge (build/blueprint/meshcodec_bridge.cpp, in libmeshcodec.a).
