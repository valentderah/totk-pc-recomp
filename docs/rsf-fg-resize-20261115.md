# FG host resize and leases, 15 November 2026

Question: can FG follow host resize without relabeling fixed guest allocations? Baseline: TotK rsf, 57c98e9. Evidence here is source review and CPU fixtures only; no game/device/UI/GPU execution. RSF sources were inspected, never edited.

`gfx_present.c`, `present_image` records the actual crop and fitted rectangle. `vk_present_fg_output` exposes the host extent for an observed full-frame SDR mapping matching the guest composition dimensions. Startup waits for that observation. Current Present withholds the FG identity for crops or letterboxing while retaining source identity for engine events and preserving native composition. Full crop/letterbox guide remapping remains unimplemented.

`vk_dlaa.c`, `size_for` keys the cache by render, reconstruction, game composition and FG host dimensions. `fg_prepare` copies metadata and changes both record and camera output dimensions, preserving render dimensions, matrices, source and render-pixel motion units. Entry changes reset temporal history. FG-only HUD-less recognition uses the game composition size rather than the depth allocation size. The known final FXAA boundary blits the real game target into a real host-sized RGBA8_SRGB allocation, then copies encoded bytes into host-sized UNORM. Tag time checks host dimensions again. Unknown boundaries and unmatched sources remain refused.

Lifetime: `free_size` drains FG before releasing inputs, then waits for GPU completion; refusal retains allocations and allows retry rather than permanently disabling rendering. Preset cleanup and shutdown propagate refusal. `rsf_present_submitted` now accepts successful submission facts after close has blocked new recordings, allowing recorded-but-unsubmitted SDK work, including failed recordings, to retire before retrying close. No submission is fabricated. Callers must eventually submit/notify outstanding commands or retain owners and leases. Existing swapchain retirement already drains before freeing views. Producers must remain serialized through the presentation lock.

The title remains exactly `Upscaling & Framegen`. General FG admission stays closed; sessions explicitly started with nonzero FG may edit/save the restart-required selection. Other sessions see the opt-in explanation. The description states experimental operation and native fallback. No unsupported SR backend/output path was enabled. Vendor policy remains shared-runtime-owned.

| Input/path | Reviewed scope | Remaining gap |
| --- | --- | --- |
| Depth/camera | Existing scene producer; fixture checks render dimensions preserved and both output dimensions updated | Device resize, cuts/FOV and dynamic resolution |
| Motion | Existing independent object replay; render-pixel units unchanged | Existing particle/translucency/deformation audit gaps |
| HUD-less colour | Known final FXAA; actual host allocation and sRGB conversion | Moving-game brightness/HUD/fades and backend captures |
| Crop/letterbox | Actual Present rectangle prevents interpolation | Guide/camera/colour remapping still implementation work |
| Auxiliary effects | Native bloom/tonemap remain upstream | Existing shader audit rows unresolved |

Coordinator update: DLSS verbose focus suppression and actual-present count growth after activation explain earlier background real-only output. Counters do not establish image or resize acceptance.

CPU checks (all passed, no shared build artifacts):

- `C:/msys64/ucrt64/bin/clang.exe tests/rsf_present_lifetime_check.c runtime/rsf_present.c -I runtime -I D:/Coding/ReScaleFrame/runtime/orchestrator/include -I D:/Coding/ReScaleFrame/runtime/contract/include -I D:/Coding/ReScaleFrame/sdk/game/include -o build/checks/rsf-resize-worker/present.exe`; running executable returned 0. New assertion covers submission after refused close.
- `.venv/Scripts/python tests/rsf_fg_resize_check.py`: compiles actual preparation function with inert GPU calls; checks 1920x1080 guest to 2560x1440 host metadata, unchanged 1600x900 guides and caller record, failure invalidation and zero-output refusal.
- `C:/msys64/ucrt64/bin/clang.exe -fsyntax-only runtime/vk_dlaa.c runtime/gfx_present.c -I runtime -I C:/msys64/ucrt64/include/vma -I D:/Coding/ReScaleFrame/runtime/orchestrator/include -I D:/Coding/ReScaleFrame/runtime/contract/include -I D:/Coding/ReScaleFrame/sdk/game/include` returned 0. Initial attempt omitted VMA include path and failed; corrected command passes.
- `git diff --check` and `.venv/Scripts/python tools/check_content.py --staged` returned 0; worker staged nothing.

Outstanding: full cropped/letterboxed FG implementation, hardware admission and menu build validation. Coordinator GPU checks: repeated same-aspect resize, arbitrary-aspect native fallback, resize during recording, teardown retry, SR off/on, quality changes, admitted backends, encoded brightness, HUD/fades, source identity and validation cleanliness. Geometry observation comes from most recent Present; tag-time extent recheck and current Present rectangle gating prevent stale maps authorizing cropped output. No visual acceptance is claimed. Existing two RSF Anomaly device failures were neither rerun nor changed.
