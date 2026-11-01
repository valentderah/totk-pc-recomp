# Validation record and limits

Recorded on 31 October 2026. Hardware: RTX 3070 8 GB, i9-9900KF, 32 GB RAM, Windows; game update 1.2.1. Gameplay observations below are from the separate local **v0.1.1 prototype**, not a port to current main. No game-derived test data accompanies this document.

## Checks on this exported source

- The submitted `build_runtime.ps1` rebuilt the v0.1.1 runtime archive, including the menu and authored shaders. The packaged input archive and installed games were left untouched.
- The submitted `build_ngx_bridge.ps1` rebuilt the MSVC bridge against the pinned SDK. The NVIDIA runtime's Authenticode signature was checked.
- **197 tests passed**, including real compiled native helpers, the actual NGX adapter exercised with a synthetic rejecting bridge, and launcher environment setup/restore. Two tests containing generated-game context fixtures were excluded along with the private patcher. The grammar was fetched from the pinned Khronos commit in `fetch_test_grammar.py`.
- The exported archive was linked locally against existing ignored v0.1.1 generated objects solely to run `--selftest`: **zero failures**. The resulting executable and linker inputs remain ignored. This checks linkage and runtime selftests; it does not establish gameplay equivalence of the sanitized export.
- The only public changes are under `research/dlaa-v0.1.1/`. The current runtime, lifter, installer, CI and default settings are unchanged.

## Earlier bounded rendering check

The accepted local K profile was checked over a 32-frame native 1440p window after the CPU optimizations. It produced no Vulkan validation errors, retained one SDK feature, and shut down cleanly. Ten existing unused fragment-output warnings remained. All 31 adjacent raw-history pairs matched exactly; the checked display conversion differed by at most 0.96875 ULP. There were 32 matched projection-copy frames and nine sampled motion-field frames spanning the jitter phases and final frame.

Those checks establish the observed resource/sequence/conversion invariants in that window. They do not prove artifact-free temporal reconstruction, complete object motion, correctness in other scenes, or image quality equivalent to either native rendering or the other preset.

## Preset menu behavior

The local native UI was tested through K → L → K. Selecting L wrote the INI while showing K as active and a restart note. A normal restart selected L, and the SDK confirmed both requested and echoed preset 12. The reverse UI change persisted K; another normal restart confirmed requested and echoed preset 11, with the menu reporting K active and no pending change. Both completed sessions exited with code zero and confirmed SDK release, parameter destruction and shutdown.

The selector is restart-required. It never destroys/recreates the live feature to apply a menu edit. Before successful evaluation, it reports pending activation; bridge/entry/configuration errors report unavailable. Physical controller navigation of the new row was not separately tested; the row uses the existing option-row input path. The exported UI text is English, while the local user's validation used French labels.

## Performance observations, not a general benchmark

One earlier pair of 45-second fixed-camera runs with the same instrumented executable and native internal resolution measured 54.57 mean FPS for K and 36.56 for L. The corresponding NVIDIA command section was about 1.6 ms versus 10.9 ms. It includes barriers and output clear. Query markers affect GPU overlap, and the runs are sensitive to order, cache state, heating and scene coverage.

After the accepted CPU work, one normal K launch showed 57 FPS on screen and a 58.4 FPS average over a stable 60-second CPU-present-callback window, with no interval above 25 ms in that window. The private test setup included the GPU-time feedback adjustment described in the README; it is not part of the submitted source-level integration. These numbers must not be used as predictions for the current main branch or other scenes.

The last paired CPU experiment reduced sampled projection work from roughly 1.9 to 1.2 ms and command recording from about 10.7 to 9.9 ms. Its original 0.8 ms projection target was not reached. Long outliers occurred in an initial comparison; reverse-order and longer repeated checks showed a modest reduction, not a universal pause fix. A descriptor-cache attempt measured slower and was rejected.

The preset images are not identical. L looked more defined around some character and architecture details in the local exact-input comparison; no universal quality ranking is claimed. Motion reconstruction, disocclusion behavior, transparency/effects, performance across scenes, and other hardware all need further qualification.

## Mainline work still required

1. Agree the renderer interface and port the pre-HUD hook to the current window/presentation implementation.
2. Replace the private generated-code feedback adjustment with an acceptable supported integration, then recheck actual internal resolution.
3. Review uniform snapshot lifetimes, shader-pointer assumptions, queue locking, history resets and every failure transition.
4. Revalidate GPU fields and display/history ordering across scenes and camera/object motion, with profiling disabled for normal performance measurements.
5. Build install/runtime configuration for optional SDK loading, legal dependency acquisition and failure fallback. The current-main default path must keep working without NVIDIA components.

This is a draft research handoff for maintainer feedback, not a merge-ready production feature.
