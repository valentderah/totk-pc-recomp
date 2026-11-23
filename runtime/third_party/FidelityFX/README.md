# AMD FidelityFX SDK 1.1.4 (subset)

From <https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4>, MIT (`LICENSE.txt`), via Savage Wind
(<https://github.com/Revivedx/SavageWindRecomp>, MIT, `third_party/FidelityFX`). `runtime/vk_fg.cpp` drives the frame
interpolation and optical flow of FSR 3 on the SDK's Vulkan backend directly, without the SDK's frame interpolation
swapchain or its ffx-api DLL layer; `runtime/vk_fsr3.cpp` drives the FSR 3.1 upscaler the same way.

What is here:

- `sdk/include/FidelityFX/host`: the SDK's public headers; `gpu`: the shader headers of frame interpolation, optical
  flow, the FSR 3 upscaler, SPD (which optical flow and the upscaler include) and FSR 1 (the upscaler's RCAS). The host
  headers of the other effects stay, unchanged, because the SDK's own headers include each other.
- `sdk/src/components/frameinterpolation`, `opticalflow`, `fsr3upscaler`: the three effects.
- `sdk/src/backends/vk/ffx_vk.cpp` and its `shaders/frameinterpolation`, `shaders/opticalflow`, `shaders/fsr3upscaler`
  (GLSL).
- `sdk/src/shared`: the SDK's assert, message and object-management helpers.
- `sdk/src/backends/shared/ffx_shader_blobs.h`: the declaration of the blob lookup, which `runtime/vk_fg.cpp` implements
  (the upscaler's blobs through `runtime/vk_fsr3.cpp` `vk_fsr3_blob`).
- `spirv/ffx_shaders_vk.h`: the shaders compiled to SPIR-V with their bindings, by `tools/ffx_shaders.py`
  (glslangValidator from MSYS2 UCRT64 or the Vulkan SDK) in place of the SDK's Windows-only FidelityFX_SC.exe. Only the
  permutations used. Committed like `runtime/*_spv.h`: AMD's shaders, no game data. Regenerate after changing the GLSL:
  `python tools/ffx_shaders.py`.
- `spirv/ffx_fsr3upscaler_vk.h`: the same for the FSR 3 upscaler (`python tools/ffx_shaders.py --effects fu`), one
  permutation per pass: HDR input (the scene's linear light before the bloom), render-size motion vectors without
  jitter, depth not inverted, the reference Lanczos and Savage Wind's Catmull-Rom history reprojection, in 32-bit and
  16-bit floats (the 16-bit ones where the SDK asks for them and the device has shaderFloat16 and shaderInt16).
- `ffx_compat.h`: force-included into the SDK's sources (`tools/build_game.py`, rule `cc_ffx`) so they build with clang.

Changes to the SDK's files (marked "Savage Wind" in them):

- `sdk/src/backends/vk/ffx_vk.cpp`: the arrays the backend lays out in its scratch memory are aligned to 64 bytes
  instead of 4, and start 64-byte aligned after its context. With 4, clang's aligned SSE stores into them (`movdqa`)
  faulted; MSVC emits unaligned ones.
- `sdk/include/FidelityFX/host/ffx_types.h`: the contexts' default size is doubled outside Windows, where `wchar_t`
  (their resources' debug names) is 4 bytes instead of 2.
- `sdk/src/backends/vk/ffx_vk.cpp`: the global descriptor pool is created with all six of the sizes it lists. It passed
  five, leaving out the storage buffers (the validation layer reports it).
- `sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_reproject.h`: the history reprojected with a Catmull-Rom
  filter from 5 bilinear samples (`FFX_FSR3UPSCALER_OPTION_REPROJECT_CATMULL_ROM_5`), a Savage Wind addition.

Changes of our own (marked "TotK Recomp"):

- `sdk/include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h`: the luma history's storage image is
  declared `rgba16f`, the format of its images (R16G16B16A16_FLOAT in `ffx_fsr3upscaler.cpp`). It was `rgba8`, which
  the Vulkan validation layer reports as a format mismatch with undefined loads and stores.
- `sdk/src/backends/vk/ffx_vk.cpp` `findMemoryTypeIndex`: memory types with `PROTECTED`, `DEVICE_COHERENT_AMD` or
  `DEVICE_UNCACHED_AMD` are skipped, since the runtime enables neither protected memory nor device-coherent memory. The
  function keeps the last matching type, and RADV lists those types after the plain device-local ones, so on a Radeon
  780M (Mesa 26.3) FSR 3.1's images were allocated in them and its whole output was noise. When no device-local type
  without `HOST_VISIBLE` exists (unified-memory GPUs: Adreno under Turnip) it takes a host-visible device-local one
  instead of failing every texture allocation.
