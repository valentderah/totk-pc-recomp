# FSR 3.1 libraries for runtime/deps

`runtime/vk_fsr3.c` links three static libraries and includes one header. They are built from
[UniversalScalers-Vulkan](https://github.com/FireBurn/UniversalScalers-Vulkan) (formerly FSR-Vulkan; MIT), a Vulkan port
of the FSR 3.1.5 upscaler of AMD's FidelityFX SDK 2.3 (the files it compiles carry AMD's MIT notice; its `NOTICE.md` and
`UPSTREAM.md` list them), with the same llvm-mingw that builds `libtotkrt.a`, and go into `runtime/deps` (the `--deps`
folder of `tools/build_game.py`) like SDL3, Opus and libvpx.

**Release:** `tools/make_release.py` does all of this in `build_ffx`, from the source archive pinned in `PINS`
(`fsr-vulkan`, commit `FFX_COMMIT`, sha256 checked). It needs `cmake` on PATH or MSYS2 UCRT64's
(`pacman -S mingw-w64-ucrt-x86_64-cmake`).

**Without them** (a source build whose deps folder lacks these files) `vk_fsr3.c` compiles a stub, `build_game.py`
links nothing extra and `aa_engine` offers DLSS alone.

By hand, for a source build:

```sh
git clone https://github.com/FireBurn/UniversalScalers-Vulkan fsr-vulkan
git -C fsr-vulkan checkout c64f093404125e960813f95f1996c14780f0dd69
cmake -S fsr-vulkan -B build-ffx -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=<llvm>/bin/x86_64-w64-mingw32-clang.exe \
    -DCMAKE_CXX_COMPILER=<llvm>/bin/x86_64-w64-mingw32-clang++.exe \
    -DVulkan_INCLUDE_DIR=<deps>/include -DVulkan_LIBRARY=<deps>/lib/libvulkan-1.dll.a \
    -DFFX_VK_PORTABLE_BUILD_FSR3_3_1_5_SCAFFOLD=ON \
    -DFFX_VK_PORTABLE_BUILD_TESTS=OFF -DFFX_VK_PORTABLE_BUILD_PROBE=OFF \
    -DFFX_VK_PORTABLE_BUILD_FSR3_HOST=OFF -DFFX_VK_PORTABLE_BUILD_FSR3_VK_BACKEND=OFF \
    -DFFX_VK_PORTABLE_BUILD_FSR4_V07_VULKAN=OFF -DFFX_VK_PORTABLE_BUILD_INSTALL=OFF
cmake --build build-ffx --target ffx_vulkan_fsr3_vk_backend_3_1_5 ffx_vulkan_portable
```

`ffx_fsr3_blobs.cpp` (here) answers the SDK's two shader-blob lookups; the bridge builds its pipelines from the SPIR-V
compiled into it, by name, so the SDK's blob tables are not linked:

```sh
K=fsr-vulkan/upstream/ffx-2.3.0/Kits/FidelityFX
<llvm>/bin/x86_64-w64-mingw32-clang++.exe -O2 -std=c++17 -c \
    -include fsr-vulkan/src/ffx_1_1_4_portability.hpp \
    -I $K/api/include -I $K/api/internal -I $K/upscalers/include -I $K/upscalers/fsr3/include \
    -I $K/upscalers/fsr3/internal tools/ffx/ffx_fsr3_blobs.cpp -o ffx_fsr3_blobs.o
<llvm>/bin/llvm-ar.exe rcs libffx_vulkan_fsr3_blobs.a ffx_fsr3_blobs.o
```

Into `<deps>`: `build-ffx/libffx_vulkan_fsr3_vk_backend_3_1_5.a`, `build-ffx/libffx_vulkan_portable.a` and
`libffx_vulkan_fsr3_blobs.a` to `lib/`, `fsr-vulkan/include/ffx_vk_fsr3_3_1_5_bridge.h` to `include/`. About 0.5 MB
together, shaders included. The FSR 4 "v07" provider in the same repository (an INT8 port of unreleased AMD sources that
needs an externally supplied model) is not built.
