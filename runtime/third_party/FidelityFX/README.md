# AMD FidelityFX SDK 1.1.4 (subset)

From <https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4>, MIT (`LICENSE.txt`), via Savage Wind
(<https://github.com/Revivedx/SavageWindRecomp>, MIT, `third_party/FidelityFX`). `runtime/vk_fg.cpp` drives the frame
interpolation and optical flow of FSR 3 on the SDK's Vulkan backend directly, without the SDK's frame interpolation
swapchain or its ffx-api DLL layer.

What is here:

- `sdk/include/FidelityFX/host`: the SDK's public headers; `gpu`: the shader headers of frame interpolation, optical
  flow and SPD (which optical flow includes). Left out against Savage Wind's copy: the FSR 3 upscaler's and FSR 1's
  shader headers (`gpu/fsr3upscaler`, `gpu/fsr1`), its component and its GLSL. The host headers of the upscaler and
  the other effects stay, unchanged, because the SDK's own headers include each other.
- `sdk/src/components/frameinterpolation`, `opticalflow`: the two effects.
- `sdk/src/backends/vk/ffx_vk.cpp` and its `shaders/frameinterpolation`, `shaders/opticalflow` (GLSL).
- `sdk/src/shared`: the SDK's assert, message and object-management helpers.
- `sdk/src/backends/shared/ffx_shader_blobs.h`: the declaration of the blob lookup, which `runtime/vk_fg.cpp` implements.
- `spirv/ffx_shaders_vk.h`: the shaders compiled to SPIR-V with their bindings, by `tools/ffx_shaders.py`
  (glslangValidator from MSYS2 UCRT64 or the Vulkan SDK) in place of the SDK's Windows-only FidelityFX_SC.exe. Only the
  permutations used. Committed like `runtime/*_spv.h`: AMD's shaders, no game data. Regenerate after changing the GLSL:
  `python tools/ffx_shaders.py`.
- `ffx_compat.h`: force-included into the SDK's sources (`tools/build_game.py`, rule `cc_ffx`) so they build with clang.

Changes to the SDK's files (marked "Savage Wind" in them):

- `sdk/src/backends/vk/ffx_vk.cpp`: the arrays the backend lays out in its scratch memory are aligned to 64 bytes
  instead of 4, and start 64-byte aligned after its context. With 4, clang's aligned SSE stores into them (`movdqa`)
  faulted; MSVC emits unaligned ones.
- `sdk/include/FidelityFX/host/ffx_types.h`: the contexts' default size is doubled outside Windows, where `wchar_t`
  (their resources' debug names) is 4 bytes instead of 2.
- `sdk/src/backends/vk/ffx_vk.cpp`: the global descriptor pool is created with all six of the sizes it lists. It passed
  five, leaving out the storage buffers (the validation layer reports it).
