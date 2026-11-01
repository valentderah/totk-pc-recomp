# Experimental native-resolution DLAA for the v0.1.1 Windows runtime

This is a source-only research handoff for review, **not an enabled feature in the current port**. It preserves a prototype tested against commit `701bd628209956a70863d864844bc97cdaf074f1` (v0.1.1), on game update 1.2.1 and an RTX 3070. The current main branch has changed substantially, including its window-system split. The replacement translation units here must not overwrite the current runtime.

The prototype adds native-resolution NVIDIA DLAA, a camera motion field with recognized mesh motion, coordinated projection jitter, bounded input/output capture and replay diagnostics, and a restart-required K/L preset setting in F1. This is DLAA at 2560x1440, **not DLSS Super Resolution or frame generation**. Temporal quality across scenes is not qualified. It remains experimental even where the test scene looks correct.

## What is provided

- `native/`: new diagnostic and NGX modules, plus modified v0.1.1 runtime translation units, kept together to expose the exact integration seams. Existing upstream comments and attributions remain intact. `menu.cpp` and `ui_text.*` contain the preset row; `dlaa_settings.*` freeze the boot request and publish the evaluated state.
- `native/shaders/`: authored GLSL sources. `compile_motion_shaders.py` compiles them into ignored `build/` headers; no captured or compiled game shader is included.
- `tests/` and the Python analysis modules: synthetic tests of matrix/jitter conventions, temporal guards, continuity, frame validation, native state helpers, and analysis rejection paths. Capture consumers require locally produced data, never data in this repository.
- `build_runtime.ps1`: rebuilds a **copy** of the v0.1.1 packaged runtime archive in ignored `build/`. Header fingerprints reject a different packaged ABI. It does not modify the package, link a game executable, or rewrite generated game code.
- `build_ngx_bridge.ps1` and `fetch_ngx_probe.py`: separate MSVC bridge and pinned NVIDIA SDK acquisition. NVIDIA headers, libraries and DLLs are not vendored. Anyone using the SDK must review its own license. Its runtime signature is verified before use.
- `launch_dlaa_test.ps1`: opt-in launcher for an independently linked executable at `build/game/totk.exe`, using an explicitly supplied disposable `-TestProfile`. It never copies an existing save automatically. The profile is writable by the game; supply only a test copy. Logs go under ignored `build/sessions` unless `-SessionRoot` is supplied.

## Data and provenance boundary

No ROMs, keys, saves, game images, captures, extracted shader programs, generated game C, compiled objects, archives or executable game are included. Old snapshots and rejected experiments are excluded. The original workstation paths and launch shortcuts are excluded. The separate DualSense resource modification is not part of this handoff.

This work extends this repository's MIT runtime at the pinned base above. New C/C++/GLSL and analysis code in this directory is covered by the repository license. The menu retains its original LAS Recomp and SavageWindRecomp attributions. It embeds the existing Apache-2.0 Roboto resource from `runtime/ui/fonts/`; no additional font is vendored. Khronos grammar and NVIDIA SDK dependencies are fetched into ignored folders with pinned provenance, rather than copied into the public tree.

The local prototype also used an experimental GPU-time feedback adjustment to hold the tested internal 1440p resolution. **The generated-C patcher and its two context-fixture tests are deliberately excluded**, because they contained generated game-code snippets. No generated source is rewritten by the submitted build. Consequently, the original gameplay setup and its resolution conditions are not reproduced by building this archive alone. A maintainer needs a supported source-level resolution/feedback integration and a fresh resolution check before comparing gameplay. The corresponding optional native diagnostic helper is retained for inspection, with no generated call site supplied.

## Building the research components

These commands are for a Windows developer environment, from this directory. They do not install anything into a playable game. Use Python 3.12, the official **v0.1.1** Windows setup package, and an installed Visual Studio C++ toolchain. The runtime archive uses the package's Clang; the NGX bridge uses MSVC. Supply glslang and spirv-val from your own verified toolchain.

```powershell
$env:TOTK_DLAA_PACKAGE = '<unpacked official v0.1.1 Windows package>'
$env:TOTK_DLAA_PWSH = (Get-Command pwsh).Source
$env:TOTK_GLSLANG = '<path to glslang.exe>'
$env:TOTK_SPIRV_VAL = '<path to spirv-val.exe>'
python -m pip install -r requirements.txt
python fetch_test_grammar.py
python fetch_ngx_probe.py
.\build_ngx_bridge.ps1 -PackageRoot $env:TOTK_DLAA_PACKAGE
.\build_runtime.ps1 -PackageRoot $env:TOTK_DLAA_PACKAGE
python -m unittest discover -s tests -q
```

The full local suite contained 199 tests; this export has **197 tests**, excluding the two generated-context tests described above. The bridge must be built for ABI/sequence tests. Python dependencies, grammar, SDK, shaders, test DLLs and output all remain ignored.

To link the archive into a disposable game build, use the **pinned v0.1.1** `tools/build_game.py` and its `--runtime-lib <this directory>/build/libtotkrt.a` option, with your own locally generated files under ignored folders. Read that version's build documentation and help for the remaining extraction, toolchain, profile and link arguments. The current-main build has not been qualified with this archive. Do not copy it into an existing installed game.

## Integration and review points

The camera path identifies a suitable pre-HUD HDR target. Raw history is retained before the DLAA output is substituted; SDK output is a distinct image. Temporary resources and views survive until the queue fence. The live sequence retains one SDK feature; discontinuities reset history. Explicit shutdown drains the queue before releasing the SDK instead of relying on `atexit`.

The motion field is a camera reconstruction plus recognized mesh candidates, not proof of complete object motion. Unsupported geometry, effects, particles, transparencies, disocclusions and other scenes need additional investigation. The existing spatial filters are not claimed to be disabled. The private path uses `TOTK_BIND_SKIP=0` and `TOTK_PUSH_SKIP=0`; a separate descriptor-cache attempt regressed performance and is not included.

The F1 preset row uses `cfg_set`/`cfg_save`. The selected value is persisted while the SDK request is frozen before worker threads start. A subsequent selection shows the active profile and a restart note, without attempting unsafe feature recreation. An explicit launcher preset seeds the ordinary setting once; a normal launch reads the saved value. The default is K for this prototype. NGX early failures publish an unavailable state, and the regression test compiles the actual adapter with a rejecting fake bridge. The environment regression runs the launcher's actual setup/restore loops, including PowerShell's distinction between absent and empty variables.

Optional profiling includes CPU stage samples, fence-read GPU timestamps and buffered SDK reports. Timestamp markers perturb overlap; the NVIDIA interval includes barriers and output clear, not just a neural kernel. Timestamp pools are fence-owned and absent when profiling is disabled. Cache changes retain their existing identity/lifetime assumptions, which should be revisited during any mainline port.

## Validation and next steps

See [VALIDATION.md](VALIDATION.md) for the bounded local results and the checks performed on the exported source. This handoff is intended for architectural feedback and selective follow-up patches. It does not alter default runtime behavior, add an installer option, or claim production DLAA support. Before integration: port the hooks to the current renderer, replace the private feedback adjustment, review cache/lifetime and failure paths, validate other scenes, and qualify image stability and performance on more hardware.
