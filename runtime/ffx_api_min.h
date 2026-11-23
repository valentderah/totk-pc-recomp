// The part of AMD's FSR API (ffx-api) that vk_ffx.c uses: the entry points of amd_fidelityfx_loader_vk.dll, the
// Vulkan backend descriptor and the upscaler's create and dispatch descriptors. Copied from the FidelityFX SDK v2.4.0
// (GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, Kits/FidelityFX/api/include/ffx_api.h, ffx_api_types.h, vk/ffx_api_vk.h
// and Kits/FidelityFX/upscalers/include/ffx_upscale.h), trimmed to those declarations; the layouts and values are
// unchanged. The SDK's headers use a C++ attribute for deprecations, so they are not included as they are.
//
// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2026 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#include <vulkan/vulkan.h>

// ---- ffx_api.h
enum FfxApiReturnCodes {
    FFX_API_RETURN_OK = 0,
    FFX_API_RETURN_ERROR = 1,
    FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE = 2,
    FFX_API_RETURN_ERROR_RUNTIME_ERROR = 3,
    FFX_API_RETURN_NO_PROVIDER = 4,
    FFX_API_RETURN_ERROR_MEMORY = 5,
    FFX_API_RETURN_ERROR_PARAMETER = 6,
    FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE = 7,
};

typedef void *ffxContext;
typedef uint32_t ffxReturnCode_t;

#define FFX_API_EFFECT_MASK 0x00ff0000u
#define FFX_API_BACKEND_MASK 0xff000000u

typedef uint64_t ffxStructType_t;
typedef struct ffxApiHeader {
    ffxStructType_t type;
    struct ffxApiHeader *pNext;
} ffxApiHeader;

typedef ffxApiHeader ffxCreateContextDescHeader;
typedef ffxApiHeader ffxConfigureDescHeader;
typedef ffxApiHeader ffxQueryDescHeader;
typedef ffxApiHeader ffxDispatchDescHeader;

enum FfxApiMsgType { FFX_API_MESSAGE_TYPE_ERROR = 0, FFX_API_MESSAGE_TYPE_WARNING = 1 };
typedef void (*ffxApiMessage)(uint32_t type, const wchar_t *message);

#define FFX_API_QUERY_DESC_TYPE_GET_VERSIONS 4u
struct ffxQueryDescGetVersions {
    ffxQueryDescHeader header;
    uint64_t createDescType;
    void *device;   // Vulkan: a VkDeviceContext *
    uint64_t *outputCount;
    uint64_t *versionIds;
    const char **versionNames;
};

#define FFX_API_DESC_TYPE_OVERRIDE_VERSION 5u
struct ffxOverrideVersion {
    ffxApiHeader header;
    uint64_t versionId;
};

#define FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION 6u
struct ffxQueryGetProviderVersion {
    ffxQueryDescHeader header;
    uint64_t versionId;
    const char *versionName;
};

typedef struct ffxAllocationCallbacks ffxAllocationCallbacks;
typedef ffxReturnCode_t (*PfnFfxCreateContext)(ffxContext *context, ffxCreateContextDescHeader *desc,
                                               const ffxAllocationCallbacks *memCb);
typedef ffxReturnCode_t (*PfnFfxDestroyContext)(ffxContext *context, const ffxAllocationCallbacks *memCb);
typedef ffxReturnCode_t (*PfnFfxQuery)(ffxContext *context, ffxQueryDescHeader *desc);
typedef ffxReturnCode_t (*PfnFfxDispatch)(ffxContext *context, const ffxDispatchDescHeader *desc);

#define FFX_API_EFFECT_ID_UPSCALE 0x00010000u
#define FFX_API_MAKE_EFFECT_SUB_ID(effectId, subversion) ((effectId & FFX_API_EFFECT_MASK) | (subversion & ~FFX_API_EFFECT_MASK))
#define FFX_API_BACKEND_ID_VK 0x01000000u
#define FFX_API_MAKE_BACKEND_SUB_ID(backendId, subversion) ((backendId & FFX_API_BACKEND_MASK) | (subversion & ~FFX_API_BACKEND_MASK))

// ---- ffx_api_types.h
enum FfxApiSurfaceFormat {
    FFX_API_SURFACE_FORMAT_UNKNOWN,
    FFX_API_SURFACE_FORMAT_R32G32B32A32_TYPELESS,
    FFX_API_SURFACE_FORMAT_R32G32B32A32_UINT,
    FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT,
    FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT,
    FFX_API_SURFACE_FORMAT_R32G32B32_FLOAT,
    FFX_API_SURFACE_FORMAT_R32G32_FLOAT,
    FFX_API_SURFACE_FORMAT_R8_UINT,
    FFX_API_SURFACE_FORMAT_R32_UINT,
    FFX_API_SURFACE_FORMAT_R8G8B8A8_TYPELESS,
    FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM,
    FFX_API_SURFACE_FORMAT_R8G8B8A8_SNORM,
    FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB,
    FFX_API_SURFACE_FORMAT_B8G8R8A8_TYPELESS,
    FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM,
    FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB,
    FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT,
    FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM,
    FFX_API_SURFACE_FORMAT_R16G16_FLOAT,
    FFX_API_SURFACE_FORMAT_R16G16_UINT,
    FFX_API_SURFACE_FORMAT_R16G16_SINT,
    FFX_API_SURFACE_FORMAT_R16_FLOAT,
    FFX_API_SURFACE_FORMAT_R16_UINT,
    FFX_API_SURFACE_FORMAT_R16_UNORM,
    FFX_API_SURFACE_FORMAT_R16_SNORM,
    FFX_API_SURFACE_FORMAT_R8_UNORM,
    FFX_API_SURFACE_FORMAT_R8G8_UNORM,
    FFX_API_SURFACE_FORMAT_R8G8_UINT,
    FFX_API_SURFACE_FORMAT_R32_FLOAT,
};

enum FfxApiResourceUsage {
    FFX_API_RESOURCE_USAGE_READ_ONLY = 0,
    FFX_API_RESOURCE_USAGE_RENDERTARGET = (1 << 0),
    FFX_API_RESOURCE_USAGE_UAV = (1 << 1),
    FFX_API_RESOURCE_USAGE_DEPTHTARGET = (1 << 2),
};

enum FfxApiResourceState {
    FFX_API_RESOURCE_STATE_COMMON = (1 << 0),
    FFX_API_RESOURCE_STATE_UNORDERED_ACCESS = (1 << 1),
    FFX_API_RESOURCE_STATE_COMPUTE_READ = (1 << 2),
    FFX_API_RESOURCE_STATE_PIXEL_READ = (1 << 3),
    FFX_API_RESOURCE_STATE_COPY_SRC = (1 << 4),
    FFX_API_RESOURCE_STATE_GENERIC_READ = (FFX_API_RESOURCE_STATE_COPY_SRC | FFX_API_RESOURCE_STATE_COMPUTE_READ),
};

enum FfxApiResourceType { FFX_API_RESOURCE_TYPE_BUFFER, FFX_API_RESOURCE_TYPE_TEXTURE1D, FFX_API_RESOURCE_TYPE_TEXTURE2D };

struct FfxApiDimensions2D {
    uint32_t width, height;
};
struct FfxApiFloatCoords2D {
    float x, y;
};

struct FfxApiResourceDescription {
    uint32_t type;
    uint32_t format;
    uint32_t width;      // union with size (buffers)
    uint32_t height;     // union with stride
    uint32_t depth;      // union with alignment
    uint32_t mipCount;
    uint32_t flags;
    uint32_t usage;
};

struct FfxApiResource {
    void *resource;   // Vulkan: the VkImage
    struct FfxApiResourceDescription description;
    uint32_t state;
};

// ---- vk/ffx_api_vk.h
typedef struct VkDeviceContext {
    VkDevice vkDevice;
    VkPhysicalDevice vkPhysicalDevice;
    PFN_vkGetDeviceProcAddr vkDeviceProcAddr;
} VkDeviceContext;

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK FFX_API_MAKE_BACKEND_SUB_ID(FFX_API_BACKEND_ID_VK, 0x03)
struct ffxCreateBackendVKDesc {
    ffxCreateContextDescHeader header;
    VkDeviceContext deviceContext;
};

// ---- upscalers/include/ffx_upscale.h
#define FFX_UPSCALER_MAKE_VERSION(major, minor, patch) (((major) << 22) | ((minor) << 12) | (patch))
#define FFX_UPSCALER_VERSION FFX_UPSCALER_MAKE_VERSION(4, 1, 2)

enum FfxApiCreateContextUpscaleFlags {
    FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE = (1 << 0),
    FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS = (1 << 1),
    FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION = (1 << 2),
    FFX_UPSCALE_ENABLE_DEPTH_INVERTED = (1 << 3),
    FFX_UPSCALE_ENABLE_DEPTH_INFINITE = (1 << 4),
    FFX_UPSCALE_ENABLE_AUTO_EXPOSURE = (1 << 5),
    FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION = (1 << 6),
    FFX_UPSCALE_ENABLE_DEBUG_CHECKING = (1 << 7),
};

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x00)
struct ffxCreateContextDescUpscale {
    ffxCreateContextDescHeader header;
    uint32_t flags;
    struct FfxApiDimensions2D maxRenderSize;
    struct FfxApiDimensions2D maxUpscaleSize;
    ffxApiMessage fpMessage;
};

#define FFX_API_DISPATCH_DESC_TYPE_UPSCALE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x01)
struct ffxDispatchDescUpscale {
    ffxDispatchDescHeader header;
    void *commandList;   // Vulkan: the VkCommandBuffer
    struct FfxApiResource color;
    struct FfxApiResource depth;
    struct FfxApiResource motionVectors;
    struct FfxApiResource exposure;
    struct FfxApiResource reactive;
    struct FfxApiResource transparencyAndComposition;
    struct FfxApiResource output;
    struct FfxApiFloatCoords2D jitterOffset;
    struct FfxApiFloatCoords2D motionVectorScale;
    struct FfxApiDimensions2D renderSize;
    struct FfxApiDimensions2D upscaleSize;
    bool enableSharpening;
    float sharpness;
    float frameTimeDelta;   // milliseconds
    float preExposure;
    bool reset;
    float cameraNear;
    float cameraFar;
    float cameraFovAngleVertical;   // radians
    float viewSpaceToMetersFactor;
    uint32_t flags;
};

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_UPSCALE, 0x0b)
struct ffxCreateContextDescUpscaleVersion {
    ffxCreateContextDescHeader header;
    uint32_t version;   // FFX_UPSCALER_VERSION
};
