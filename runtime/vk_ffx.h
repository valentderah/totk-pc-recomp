// AMD's FidelityFX library (amd_fidelityfx_vk.dll of the FidelityFX SDK 1.1.4, PrebuiltSignedDLL) as vk_fsr3.c (the
// upscaler) and vk_fg.c (frame generation) use it: one file next to the executable, loaded at run time, with a C
// interface of four calls. Nothing of the SDK is compiled in; the structures come from its headers, kept unmodified
// in runtime/third_party/ffx_api (MIT). vk_fsr3.c holds what is declared here.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "vk.h"
#include "third_party/ffx_api/ffx_api.h"
#include "third_party/ffx_api/ffx_api_types.h"

// ffx-api/include/ffx_api/vk/ffx_api_vk.h of the same SDK release (MIT, Copyright (C) 2024 Advanced Micro Devices,
// Inc.), which is C++ only: the creation description of the Vulkan backend, chained behind an effect's.
#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK 0x0000003u
struct ffxCreateBackendVKDesc {
    ffxCreateContextDescHeader header;
    VkDevice vkDevice;
    VkPhysicalDevice vkPhysicalDevice;
    PFN_vkGetDeviceProcAddr vkDeviceProcAddr;
};

typedef struct {
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxConfigure configure;
    PfnFfxDispatch dispatch;
} VkFfx;

// The library's entry points, loaded at the first call (logged once); NULL when its file is not next to the executable
// or cannot be used, and always on Linux, for which AMD ships no build.
const VkFfx *vk_ffx(void);
// The backend description for this device, to chain behind an effect's creation description.
void vk_ffx_backend(struct ffxCreateBackendVKDesc *b);
// The library's messages go to the log (the first 32).
void vk_ffx_message(uint32_t type, const wchar_t *msg);
// A format as the library names it (FFX_API_SURFACE_FORMAT_UNKNOWN for one this file does not pass), and an image of
// the back end as the library takes it: in GENERAL, which is the layout its Vulkan backend maps the states COMMON and
// UNORDERED_ACCESS (written: 1) to; first level and layer. D32F goes in as R32_FLOAT with the depth usage, as the
// SDK's ffxApiGetSurfaceFormatVK and ffxApiGetImageResourceDescriptionVK describe it.
uint32_t vk_ffx_format(VkFormat f);
struct FfxApiResource vk_ffx_resource(VkImage img, VkFormat f, uint32_t w, uint32_t h, int written);
