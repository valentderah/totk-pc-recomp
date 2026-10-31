// The two shader-blob lookups FidelityFX's FSR 3.1.5 host code links against. FSR-Vulkan's bridge builds every
// pipeline from the SPIR-V modules compiled into it, by name, so the SDK's own blob tables are not linked: these
// answer "no blob" and "wave32", as the stubs of FSR-Vulkan's tests/fsr3_3_1_5_bridge_test.cpp do (MIT, Copyright (c)
// 2026 Q2RTX FSR Vulkan contributors). Built into libffx_vulkan_fsr3_blobs.a next to the two FSR-Vulkan libraries
// (tools/ffx/README.md).
#include <cstdint>
#include <cstring>

#include "ffx_fsr3upscaler.h"
#include "ffx_interface.h"

extern "C" FfxErrorCode fsr3UpscalerGetPermutationBlobByIndex(FfxFsr3UpscalerPass, uint32_t, FfxShaderBlob *outBlob) {
    if (!outBlob) return static_cast<FfxErrorCode>(FFX_ERROR_INVALID_POINTER);
    std::memset(outBlob, 0, sizeof(*outBlob));
    return FFX_OK;
}

extern "C" FfxErrorCode fsr3UpscalerIsWave64(uint32_t, bool &isWave64) {
    isWave64 = false;
    return FFX_OK;
}
