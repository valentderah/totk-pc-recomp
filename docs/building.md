# Building from source

Windows 10 or later, an x86-64 CPU with SSE4.2, and a GPU with Vulkan 1.3 and `VK_EXT_external_memory_host`. The
default build targets x86-64-v3 (AVX2/FMA); a CPU without them builds with `--cpu v2` instead and runs slower (see
"Build"). Allow about 60 GB for the work folder. Extraction wants about 55 GB free while it runs and keeps about 35 GB
afterwards.

Players who only want to play can use the [installer](installer.md) instead: it builds the game on their machine
without this toolchain.

## Toolchain

Versions are the ones the build was verified with; newer patch releases are fine.

| Tool | Version | Install | Used for |
|---|---|---|---|
| MSYS2 UCRT64 | clang/lld 23.1.3 | msys2.org installer to `C:\msys64`, packages below | compiling the generated C and the runtime |
| Visual Studio Build Tools | 2026, C++ workload, Windows SDK 10.0.26100 | winget `Microsoft.VisualStudio.BuildTools` with `--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended` | the linker .NET NativeAOT calls |
| .NET SDK | 10.0 | winget `Microsoft.DotNet.SDK.10` | the shader translator (`totk_xlate.exe`) |
| Vulkan SDK | 1.4.363 | winget `KhronosGroup.VulkanSDK` | validation layers, `vulkaninfo` |
| uv | 0.13 | standalone installer from astral.sh | the two Python environments |
| hactool | 1.4.0 @ `1d64a83` | `.venv\Scripts\python tools\build_hactool.py`, see below | NCA and update extraction |

MSYS2 packages, after `pacman -Syuu` (run it twice; the first pass updates the core and exits):

```sh
pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-{clang,lld,ninja,gcc,make,pkgconf,sdl3,opus,libvpx,zstd,vulkan-devel,\
vulkan-memory-allocator,vulkan-validation-layers,shaderc,spirv-tools} base-devel git unzip
```

`ffmpeg` from MSYS2 is a GPLv3 build and is never linked; the movies go through libvpx.

PATH:

- Add `C:\Program Files (x86)\Microsoft Visual Studio\Installer` to the user PATH: NativeAOT needs `vswhere.exe`, or
  the publish fails at the link step (MSB3073).
- Another `cmake` or `ninja` earlier on PATH (a 32-bit winlibs MinGW, for example) is picked up by accident. The build
  scripts call Ninja from `C:\msys64\ucrt64\bin` themselves; from a shell, put `C:\msys64\ucrt64\bin` first.

### hactool

`tools/build_hactool.py` fetches SciresM/hactool at `1d64a83` into `toolchain\src\hactool`, applies the three patches in
`tools/hactool/` and builds a static `toolchain\hactool\hactool.exe` with MSYS2 UCRT64 gcc (a few seconds):

- `portable.patch`: the parts of MacNativeGames' `build_hactool()` that run on Windows, the `MNG_TITLE_KEYS` variable and
  the two BKTR fixes in `nca.c` for the subsection table padding and the subsection layout of newer cumulative updates;
- `unicode.patch`: a manifest with `<activeCodePage>UTF-8</activeCodePage>`, so paths under a non-ASCII user profile
  reach it intact;
- `mbedtls-asm-clobbers.patch`: the `memory` and `cc` clobbers the bundled mbedtls 2.6.1's x86-64 multiply asm lacks.
  Without them a clang build (the Linux release) fails every RSA signature check and the update merge stops.

hactool reads `-k <prod.keys>`; MSYS2 shells set `HOME`, which hides its default key location, so pass `-k` there.

## Build

From the repository root, in PowerShell. Every step that depends on the game version takes it as `<ver>`: `v143` for
1.4.3, `v121` for 1.2.1 or `v110` for 1.1.0. The tools default to `v121` when `--version` is left out. Saves load on any
of them.

1. Python environments (once). `.venv` runs the lifter and the build; `.venv-py314` runs the extraction scripts, which
   need the stdlib `compression.zstd` of Python 3.14:

   ```powershell
   uv venv --python 3.12 .venv
   uv pip install --python .venv/Scripts/python.exe -r tools/requirements.txt
   uv venv --python 3.14 .venv-py314
   uv pip install --python .venv-py314/Scripts/python.exe -r tools/requirements-py314.txt
   ```

2. Copy `config.example.toml` to `config.local.toml` and set the paths to your base game, the update and your console's
   keys file. `config.local.toml` is ignored by git. The tools read the keys file themselves.

3. Extract and verify. This writes `build\extracted\v0` (1.0.0) and `build\extracted\<ver>` for each update in
   `updates`, identified by its hash; a hash mismatch stops the run. With another `[work] dir` in `config.local.toml`,
   `extracted` goes there instead, and `build_game.py` reads the ExeFS and RomFS from there:

   ```powershell
   .venv-py314\Scripts\python tools\extract.py
   ```

4. Shader translator (no game files; again after a change to its sources, and `tools\make_release.py` stops on an older
   one). This publishes `build\xlate\totk_xlate.exe`:

   ```powershell
   .venv\Scripts\python tools\build_xlate.py
   ```

5. Lift the modules to C, write the RomFS overlay (the frame-rate fix), then compile and link
   `build\<ver>\totk.exe`:

   ```powershell
   .venv\Scripts\python recomp\gen.py --exefs build\extracted\<ver>\exefs --out generated\<ver> --patches recomp\patches-<ver>.txt
   .venv-py314\Scripts\python tools\make_romfs_patches.py --version <ver>
   .venv\Scripts\python tools\build_game.py --version <ver>
   ```

   `--cpu v2` targets x86-64-v2 (SSE4.2, no AVX2/FMA) for older CPUs; the default `v3` needs AVX2. The v2 build links
   `runtime/fma.c`, a correctly rounded software fma the FP lowering needs, and runs slower. Inline assembly,
   target attributes and intrinsics are not bound by `-march`, so a v2 build runs `tools/check_isa.py` over the runtime
   objects before it links: any AVX, BMI or other instruction above x86-64-v2 outside a function listed there as
   dispatched by a runtime CPU check stops the build.

   `gen.py` sizes its emit pool from the CPU count and the free memory; `--jobs N` sets it, and so does the environment variable `TOTK_LIFT_JOBS=N`, which the installer's lift step honours too (it has no `--jobs`). When a worker runs out of memory, the pool restarts with half the workers (`emit: out of memory with N workers, retrying with M`), down to one; only an out-of-memory error with one worker fails the step.

   Lifting takes about 15 s, a clean compile about 200 s on 24 threads; later builds recompile only what changed.
   A compiler cache in front of clang shortens it further: with `SCCACHE` set to an sccache executable, `build_game.py`
   writes it into the ninja file ahead of `cc`, and every lift rewrites the generated chunks with the same contents, so
   a second build of the same version hits all of their objects:

   ```powershell
   $env:SCCACHE = "path\to\sccache.exe"
   .venv\Scripts\python tools\build_game.py --version <ver>
   ```
   `--patches` builds in the frame-rate, resolution and other patch sets, switchable at run time. The link copies
   `SDL3.dll`, `libiconv-2.dll` and `libopus-0.dll` next to the exe.
   For Xbox button prompts, run `make_romfs_patches.py` twice more, with `--prompts xbox` and `--prompts xbox-label`:
   they write `build\<ver>\romfs-patches-xbox` and `romfs-patches-xbox-label` ([controls.md](controls.md), "Xbox
   button prompts"), which `build_game.py` copies next to `totk.exe` like `romfs-patches`. `--prompts pc` writes
   `romfs-patches-pc`, the keyboard and mouse prompts ([controls.md](controls.md), "PC button prompts").

   A build from source has no layouts for other shapes than 16:9. At an `output_res` such as 5120x1440 the game then
   draws the largest 16:9 size inside it (2560x1440) and the present adds bars, instead of a wide camera over a
   stretched HUD (`runtime/resolution.c`, `shape_fallback`). For the full shape make `romfs-patches-<w>x<h>` once per
   size: `make_romfs_patches.py --version <ver> --aspect <w>x<h> --out build\<ver>\romfs-patches-<w>x<h>` (the sizes
   the installer makes are `ASPECT_SIZES` in `installer/steps.py`); `build_game.py` copies every `romfs-patches*`
   folder next to `totk.exe`.

6. Prepare the shaders (once per build folder and translator). It needs `refs\MacNativeGames` (`tools\fetch_refs.sh`)
   for the shared shader list:

   ```powershell
   .venv-py314\Scripts\python tools\prepare_shaders.py build\<ver> --version <ver>
   ```

   Without this step the first launch compiles shaders and pipelines during play and stutters until the caches fill.
   The AMD driver keeps its pipeline cache per exe path and holds a limited amount: preparing another build folder can
   evict this one's. `totk.exe --warm-shaders` rebuilds it from the shader cache.
   When the driver's shader compiler crashes on a variant (NVIDIA 615.78.08 on an RTX 5080 does, issue #30),
   `prepare_shaders.py` runs the driver step again with `--warm-shaders`, up to three times, without the variants the
   driver was building at the crash. `shader-prepare-skip.txt` next to the exe lists them with the GPU and driver
   version; they build at their first draw instead, another driver version builds them again, and so does deleting the
   file.

7. Run. The game keeps running when its window loses the focus (`assume_focus = 1` in `totk.ini`, the default; the
   Game tab switches it). With 0 it pauses, as it does on the Switch when Home is pressed.

   ```powershell
   build\<ver>\totk.exe
   ```

Settings live in `totk.ini` next to `totk.exe` ([totk.ini.example](../totk.ini.example) lists them). Saves go to
`build\profile` for `v121` and `build\profile-<ver>` for other versions (`build_game.py --profile` changes it).

### Portable folder

`.venv\Scripts\python tools\package_build.py --version <ver>` writes `build\<ver>-portable`, which runs on another of
your machines without MSYS2, Python or .NET. `romfs.bin` stays out unless you pass `--with-romfs`; otherwise put it in
`romfs\` next to the exe or point `TOTK_ROMFS` at its folder. Run `totk.exe --prepare-shaders` once in the copied
folder. The folder holds code and data derived from your game: for your own machines only.

### Profile-guided build

`build_game.py --pgo <file>.profdata` compiles the generated code at -O2 with an execution profile, and the runtime
with it too: hot paths laid out straight, hot calls inlined, code that never ran compiled for size. On 1.4.3 the fight
bench ran 12-15% faster per frame and a fight outside the training about 6%. The installer builds with
`data/pgo-<ver>.profdata` of the release when the update has one, else at -O1.

A profile only fits the code it was trained on: functions are matched by name and control-flow hash, so a function
whose code changes loses its counts without a warning. That code is the generated C plus the runtime headers it
includes (`cpu.h`, `simd.h`, `simd_fp.h`), whose inline helpers become part of every lifted function: a change to the
floating-point helpers once left a profile with a fifth of its weight and 2% of its gain. Retrain after a lifter or
generator change or a change to those headers, from a clean `recomp/`. The instrumented build needs llvm-mingw, as
MSYS2's clang has no profile runtime:

```powershell
$llvm = "toolchain\llvm-mingw-<date>\llvm-mingw-<date>-ucrt-x86_64"
.venv\Scripts\python tools\build_game.py --version v143 --out build\v143-pgogen --toolchain $llvm --pgo gen
```

Play `build\v143-pgogen\totk.exe` through varied scenes (fights, towns, riding, building, the Depths, menus) for 20-30
minutes; it is slower than a normal build. It writes `build\v143-pgogen\pgo\*.profraw` every minute and at exit.
Scripted runs from saves (`tools/run_game.py`) add to the same file. Then:

```powershell
.venv\Scripts\python tools\pgo.py merge build\v143-pgogen --version v143
```

writes `build\pgo\pgo-v143.profdata` and its record (`.json`: the `recomp/` tree it fits and the digests of the runtime
headers the generated C included, read from the build's Ninja deps log). Merge from the checkout the instrumented build
was made in. `make_release.py` ships every profile of `build\pgo` in `data\` and warns when a record names another
`recomp/` tree than the one released or a header differs.

## Linux (in progress)

The runtime builds and runs on x86-64 Linux with a window, input and menu on SDL3 (`*_sdl.c`) in place of the Win32
files (`*_win.c`); `runtime/platform/linux.c` implements `platform.h`. Status: the selftest and the probes pass; 1.2.1
plays at 60 fps on RADV (RX 7800 XT) and NVIDIA (RTX 3060); 1.4.3 builds, probes and plays on RADV (RX 6600, and the
Vega 7 of a Ryzen 5 4600G at about 34 fps). Long play sessions and the Steam Deck itself are not verified yet.

Requirements:

- Linux 6.7 or later: the write watch uses userfaultfd write protection with `PAGEMAP_SCAN`. SteamOS 3.8 qualifies.
- A Vulkan 1.3 driver with `VK_EXT_external_memory_host` (RADV, NVIDIA).
- Packages on Arch:

  ```sh
  pacman -S --needed clang lld ninja make sdl3 opus libvpx zstd vulkan-headers vulkan-icd-loader \
      vulkan-validation-layers shaderc spirv-tools dotnet-sdk uv git
  ```

  Vulkan Memory Allocator is a single header (AUR `vulkan-memory-allocator`). Without it, `build_game.py` fetches the
  version the Windows release pins (3.4.0) into `toolchain/vma/` on first use and checks its hash.

The steps are the Windows ones with POSIX paths: `.venv/bin/python` in place of `.venv\Scripts\python`, and `totk` in
place of `totk.exe`. `build_game.py` takes clang, lld and Ninja from PATH (or the ones in `<dir>/bin` with
`--toolchain <dir>`) and links against the system libraries, with `libstdc++` static and an `$ORIGIN` rpath;
`build_hactool.py` builds `hactool` with make. `tools/prepare_shaders.py` reads the shader list from
`refs/MacNativeGames` at `origin/HEAD`, which must be the pinned commit with its history (the list format changed later).

The probes open a window, so from an SSH session they need the desktop session's display, or they stop at `gpu 0
skipped: no graphics+compute+present queue family`: on Wayland `XDG_RUNTIME_DIR=/run/user/$(id -u)
WAYLAND_DISPLAY=wayland-1 SDL_VIDEODRIVER=wayland` (the socket name is in `$XDG_RUNTIME_DIR`), on X11 `DISPLAY=:0`.

Memory: on Windows the GPU imports guest memory and `GetWriteWatch` tracks the CPU's writes to it. On Linux,
re-protecting imported memory invalidates the driver's userptr buffer on every submit (about 1.3 ms per 16 MiB on
amdgpu), and importing pins the pages, which drops their write protection. Each CPU-visible pool therefore gets a
host-visible copy (shadow storage, `runtime/vk_memory.c`): the pages the CPU wrote go in before each submit, and the GPU's
writes come back at its fence. In play that costs about 0.2 ms a frame and doubles the memory of those pools (about
1.1 GB). `TOTK_SHADOW=1` turns it on under Windows too. The Steam Deck's RADV has `VK_EXT_external_memory_host` as
well: the shadow copies are chosen for the measured cost above, not for a missing extension.

The Deck's GPU (Van Gogh) has no native ASTC, so its textures go through the compute decoder (`runtime/vk_astc.c`,
`TOTK_ASTC`) as on desktop AMD GPUs without it; frame times measured there include it.

## Checks

Before a commit:

- `python tools/check_content.py --staged`: nothing game-derived is staged. CI runs the same check;
  `git config core.hooksPath .githooks` runs it before each local commit.
- `build\<ver>\totk.exe --selftest`: layout, dispatch table and host-function registry, without game files.

A change to the button prompts (`tools/make_romfs_patches.py --prompts`, `tools/button_icons.py`) passes
`.venv-py314\Scripts\python tests\romfs_prompts_test.py`, which runs the tool on a RomFS it makes itself (no game
files); a change to the settings passes `totk.exe --probe-cfg` and `python tests\settings_doc_check.py`, which fails
when a setting is described neither in `totk.ini.example` nor in [settings.md](settings.md). A change to the mods' ExeFS patches
(`tools/exefs_patches.py`, `runtime/mods.c`, the `folded.txt` of `recomp/gen.py`) passes
`.venv\Scripts\python tests\exefs_patches_test.py`, which makes its own modules and runs `gen.py` on one with a jump
table (no game files), and `totk.exe --probe-mods`. A change to the cheat VM or the cheat opcode tables
(`runtime/cheats.c`, `tools/cheats.py`) passes `.venv\Scripts\python tests\cheats_vm_check.py`, which runs both on
cheats of its own with clang from PATH (`--cc` names another).

The lifter (`recomp/`) and the CPU headers (`runtime/cpu.h`, `runtime/simd.h`) are checked against Unicorn. Every lifter
change passes these, all run with `.venv`:

| Test | Checks | Game files |
|---|---|---|
| `tests\diff\lift.py` | instructions sampled from the extracted modules; `--coverage` lists every word the lifter rejects or traps | yes |
| `tests\diff\simd.py` | the whole FP/SIMD encoding space | no |
| `tests\diff\atomic.py` | exclusive load/store sequences and ARMv8.1 atomics | no |
| `tests\diff\func.py` | whole functions as the generator emits them, loops, jump tables and calls included (calls go to a stub that checks the registers at the call) | yes |
| `tests\flags_check.py` | the function-local flag helpers of `runtime\cpu.h` against its packed ones | no |
| `tests\excl_mt.py` | the exclusive monitor helpers under contention and ABA, on several threads | no |
| `tests\jumptables_check.py` | `recomp/jumptables.py` against a Unicorn oracle; must report 0 disagreements, on 1.4.3 too | yes |

A `--cpu v2` build passes the same differential tests with `--cflags=-march=x86-64-v2`; they compile `runtime/fma.c`
too, so they check the software fma the v2 build links rather than the C runtime's.

Runtime probes, each a mode of `totk.exe` (the list is at the top of `runtime/main.c`): `--probe-kernel`,
`--probe-vkexec`, `--probe-nvn-tex` and `--probe-shader` cover most runtime changes. `--probe-shader` and the draw
part of `--probe-vkexec` translate shaders of the census below: they look for `build/<ver>/shaders/jobs.json` of the
exe's own version first, then of the other one.

Shaders: `.venv-py314\Scripts\python tools\shader_census.py --version <ver> --translate` extracts every Maxwell shader
to `build\<ver>\shaders\` and runs it through the translator and `spirv-val`; failure groups go to `failures.txt`.
`tools\xlate_smoke.py` checks the translator alone, without game files.

## Reverse engineering (optional)

- Ghidra 12.1.2 with Adubbz's Ghidra-Switch-Loader in `toolchain\ghidra\` (JDK 21) opens the game modules (NSO).
- `tools\fetch_refs.sh` clones the upstream projects this port builds on into `refs\`, read-only.
