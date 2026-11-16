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

## Crop and letterbox implementation follow-up

The earlier implementation gap above is superseded by `rsf_fg_map.c`, `rsf_fg_gpu.c` and `shaders/rsf_fg_map.comp`. This follow-up uses the pinned external/ReScaleFrame checkout (1753d4d at dispatch); it does not change RSF. The coordinator/producer worker changed the HUD-less snapshot to the actual game composition allocation size before the mapper. That avoids losing crop samples through an intermediate host resample. This worker did not edit vk_dlaa.c or vk.h.

Actual Present supplies the clamped source crop, host extent and fitted destination rectangle. The mapper retains that observation with each calculation source and tests it against the current Present. The first frame without facts, a changed crop/fit, unsupported formats/layouts or unretired commands use native output; known crop/fit maps are implemented rather than rejected. Host resize invalidates observed maps, so the first new Present establishes facts before FG can resume. Neither latest prepared source nor guessed aspect is substituted.

The compute shader uses pixel centres to map the destination rectangle into normalized game crop coordinates. Depth and motion each sample their own actual extent, including unequal guide extents. The destination is genuinely host-sized for all three resources. Bars contain far device depth (0 reversed, 1 normal), valid zero motion and encoded opaque black. Source motion in render-pixel units is multiplied by destination/cropped-motion-extent on each axis; SDK scale is then one in host render pixels. Encoded UNORM colour is copied by texel fetch without a second sRGB conversion. The upstream sRGB encode remains at the raw FXAA capture. This shader uses nearest sampling for guides/encoded colour; no claim is made that its resized colour filtering matches the native FSR1/sharpening path.

Camera matrices follow the existing `rsf_camera.c` row-vector convention, despite the abbreviated ABI description: projection is postmultiplied by the crop/fit affine transform, inverse projection is premultiplied by its inverse, and current-to-previous clip mapping is conjugated with current inverse and previous affine. Clip Y is upward while rectangle Y is downward. Jitter is scaled into host pixels and full-host vertical FOV is derived from transformed inverse-projection rays. World/view transforms and device depth convention are preserved. Changed maps and render extents request RESET; changed render extents clear previous jitter instead of interpreting it in current units.

Four host-sized backing slots retain images and views, with no steady-state image allocation. Source views are cached by actual allocation identity and fixed checked format. On slot exhaustion the integration drains RSF and waits for device completion before marking slots reusable; backing is recreated only on extent change/retirement. This is conservative reuse synchronization, not a performance claim. The mapper tracks its own recorded command buffers separately from SDK tracking, because the SDK may reject before recording anything. Both failed SDK operations and successful ones leave mapper commands pending until actual successful submission notifications arrive. Pending commands refuse recycling, engine input release and swapchain retirement. After completed drain, producer teardown clears cached source views before destroying producer images. SDK input leases, output allocations and source views remain alive through completion. Draw hooks already invalidate cached pipeline bindings after both wrapper calls.

Capability checks: push descriptors are a renderer-required enabled extension, and mapper initialization checks maxPushDescriptors >= 5. Input images must belong to this device, have nonzero extents, fixed expected formats and GENERAL or SHADER_READ_ONLY layout; sampled format support is queried. Actual producer allocations explicitly include SAMPLED usage, which the SDK image ABI cannot introspect. Output formats require storage support. Motion writes use an unformatted write-only storage image, requiring the renderer's already-required/enabled shaderStorageImageWriteWithoutFormat and the queried format-specific STORAGE_WRITE_WITHOUT_FORMAT bit. No new shaderStorageImageExtendedFormats requirement is introduced. All outputs include TRANSFER_SRC for SDK copies/readbacks and report that promise truthfully. Partial pipeline or allocation failure destroys acquired objects before retry. Missing capabilities refuse FG without changing native presentation.

Menu follow-up: low latency now offers Off, On and the existing translated On + Boost label, values 0/1/2; its text requests Boost where supported. Legacy REFLEX translation/config ownership remains with the coordinator. No dormant native instrumentation was removed, and shared runtime retains vendor decisions. General FG admission remains opt-in gated with the exact existing title.

New CPU checks, all exit 0:

- `C:/msys64/ucrt64/bin/clang.exe tests/rsf_fg_mapping_check.c runtime/rsf_fg_map.c -I runtime -I external/ReScaleFrame/sdk/game/include -o build/checks/rsf-map-worker/mapping.exe`, then the executable: full-frame resize, crop offsets, bars, fractional render coordinates over 1,080,000 host pixels, unequal motion scale, invalid rectangles, projection/inverse round trip and cross-mapping reprojection.
- `C:/msys64/ucrt64/bin/clang.exe tests/rsf_fg_gpu_check.c runtime/rsf_fg_map.c -I runtime -I C:/msys64/ucrt64/include/vma -I external/ReScaleFrame/runtime/orchestrator/include -I external/ReScaleFrame/runtime/contract/include -I external/ReScaleFrame/sdk/game/include -o build/checks/rsf-map-worker/gpu-fixture.exe`, then the executable: mocked GPU calls, partial pipeline/allocation cleanup, unequal depth/motion extents, actual output extents, independent unsubmitted command tracking, source-view/backing reuse and balanced teardown. This is not a GPU test.
- `tools.build_shaders.compile_one(Path('runtime/shaders/rsf_fg_map.comp').resolve())` invoked through Python with tools on sys.path: shader compilation and SPIR-V validation passed, 925 words. Only the new shader/header were generated.
- UCRT64 Clang `-Wall -Wextra -Werror -fsyntax-only` passes for rsf_fg_map.c and rsf_fg_gpu.c with the fixture include directories; ordinary `-fsyntax-only` passes for entire gfx_present.c and ui_text.c with those directories.
- UCRT64 Clang++ `-std=c++17 -fsyntax-only runtime/ui/menu.cpp -I runtime -I runtime/third_party/imgui` with the same VMA/RSF include directories passes for the whole menu source.
- `python tools/check_content.py --staged` and `git diff --check` pass; no staging or commit was performed.

Remaining coordinator checks are hardware acceptance: actual bars/crop/resize colour, camera/jitter/history, HUD/fades, provider input/output capture, Vulkan validation, teardown with recording in flight and performance of the conservative four-slot drain. Ordinary upstream particle/deformation/storage shader audit gaps are unchanged. No moving-game, device, UI or visual-quality acceptance was performed or inferred from these fixtures. Shared build artifacts were untouched.


## Coordinator review: recycle by completed submission

The first mapper reused its four backing slots by draining the provider and
waiting for device idle when the ring filled. That preserved ownership but
introduced a whole-device stall every four prepared sources. The normal prepare
path now obtains the exact current recording fence from `vk_sync_record_fence`
and reuses a slot only after that final engine submission has completed and
all its recorded guide/HUD-less commands have successful-submit notifications.
The pinned FG API leases input resources through actual GPU completion; each
supported provider records its input copy into these producer commands. Only
resize, source destruction and teardown retain the explicit provider/GPU drain.
If every slot is still in use, preparation refuses without a whole-device wait.

The extended mocked GPU fixture fills the ring, verifies that successful
submission without GPU completion cannot recycle it, then advances the completed
fence and reuses backing without new image/view allocation. Pending command
leases still block their slot even after a later completion value. The fixture
passes with UCRT64 Clang `-Wall -Wextra -Werror`, and the complete changed
`gfx_present.c` and `rsf_fg_gpu.c` pass syntax checks against the pinned SDK.
This removes the source-level periodic idle wait; game timing and visual
acceptance remain unrun for the combined implementation.


## Coordinator GPU recording correction and fullscreen admission

The first mapped FSR3 run exposed image-layout and compute-pipeline validation
errors, followed by an SDK access violation. The mapper had queued Vulkan
commands but called its push-descriptor function pointer directly, then entered
SDK recording before draining its own commands. It now uses `rec_push` and
finishes both guide and HUD-less mapper command streams with `rec_drain` before
the SDK records its input copies. The mocked fixture checks recorder routing
and a completed drain for every mapper dispatch. The combined build passes.

A repeat completed 1801 presents with zero validation errors, shader failures or
skipped draws, but no generated frames. That was a separate exact-identity bug,
fixed by the accepted metadata history in `rsf-fg-present-history-20261115.md`.
The following fullscreen DLSS SR/FG/latency run completed 1944 real presents
with zero errors. SDK boundary records for sources 1800-1802 contain both a
successful generated ordinal 0 and real ordinal -1 with exact source/generation
identity. The fullscreen host output was 2560x1600; scene/HUD-less source was
1920x1080. Generated pixel files were not flushed in that bounded process run,
so no interpolation-quality conclusion is drawn from its final native screenshot.
Full crop/resize/HUD and other-provider visual admission remain pending.
