# RSF shader-input audit

This audit is part of the integration, independent of a reported visual defect.
The entries distinguish source review, fixtures and bounded game captures. The
game extraction, native build and shader preparation have completed; those checks
alone do not establish coverage in a moving scene.

| Path | Inputs and producer | Current finding | Validation still needed |
| --- | --- | --- | --- |
| Scene colour | `vk_dlaa.c`, `rsf_scene`: armed light target, converted to RGBA16F | RSF has a sampled colour input. The copy preserves the source's existing precision; it does not restore detail lost in packed scene production. | Capture source/output, bloom, exposure and post-processing at each scale. |
| Device depth | `dlaa_mv.comp`, `main`: D32F sampled depth written to R32F | Output is produced with the camera-motion dispatch, including identity reprojection on history reset. A missing camera still prevents evaluation. | Compare raw depth, reverse/infinite conventions and foreground coverage in a capture. |
| Camera vectors | `vk_dlaa.c`, `mv_pass`: current camera snapshot and previous view-projection; RG16F output | Camera/depth production now runs for FG with reconstruction off, without injecting jitter or applying saved SR scale/LOD bias. The FG-only opening-scene check reports zero jitter and no camera vectors over 256 pixels. One check lowers whole-image warp error from zero-motion 0.0136 to 0.0074. | Broader gameplay, cuts, FOV/roll and raw convention checks remain open. |
| RSF camera constants | `rsf_camera.c`, `rsf_camera_matrices`; `vk_dlaa.c`, `mv_pass` and `rsf_scene` | Fixed zero-filled camera matrices in the RSF record. The adapter now derives view/inverse-view, projection/inverse-projection and previous-clip transforms from the recorded snapshot, normalizes OpenGL clip depth to 0..1 and transposes mathematical column transforms for RSF's row-vector contract. Invalid or singular inputs refuse evaluation. Numeric tests cover translation, rotation/FOV change, near/far mapping, reset identity and singular-history refusal. | Capture the actual camera buffer and verify jitter, orientation and reprojection in moving gameplay. Numeric fixtures do not establish the active game's camera layout. |
| Object/bone vectors | `vk_exec_draw.c`, `vk_mvobj_execute`; `vk_mvprev.c` snapshot banks | Collection now follows either SR or FG, including CPU snapshot shadows, and clears history/draw deduplication when unavailable. The fixture verifies SR-only, FG-only and disabled/re-enabled collection. One FG-only moving sample improves median object-flow error from camera-only 6.36 to 1.58 pixels; final vectors are closer for 93% of matched samples. | This is not complete material/deformation coverage. That sample still has 323 pixels with error over 1.5 pixels where camera motion fits better; trace those draws and missing variants. |
| Character history bias | `dlaa_mv.comp` R8 mask; `vk_dlaa.c`, `rsf_scene`; RSF Vulkan DLSS hint | Restored the mask dropped by migration. RSF converts its private DLSS copy to R16F because the pinned Streamline mapping omits R8_UNORM. The corrected SR run has no format/validation errors. | Moving-game visual quality and material-specific tuning remain unverified. The user's skin report belonged to Skyrim. |
| Translucency, particles, screen quads | `vk_exec_draw.c`, object-motion filtering | The object replay targets solid depth-writing geometry. It does not establish independent motion coverage for these excluded effects. | Trace their actual producers, depth and composition conventions; add reviewed producers/variants where required. |
| Storage-buffer bindings | `vk_exec_draw.c`, `ssbo_info` and motion descriptor setup | Unresolved/misaligned storage-buffer paths can bind the zero buffer and log warnings. Their presence in the source is not proof that every active scene resolves its bindings. | Capture shader metadata, slot/range mappings and actual bindings before fixing a particular producer. Zero binding is not an acceptable universal repair. |
| HUD-less FG inputs | Scene/tonemap/UI pass boundary | Source identity now reaches actual Present. The native frame-1600 dump shows final FXAA/upscale into an output-sized float target, a fade/overlay draw into that target, then a sampled copy into the sRGB output. The latter shader is a direct colour copy; hardware applies sRGB encoding. FG depth/motion preparation now records at the scene producer and receives actual submission notifications; unsubmitted recordings retain their SDK owner across retirement attempts. No HUD-less producer is connected yet. | Preserve final colour encoding when copying before UI, verify ordinary gameplay HUD, lease/match the result to its source and verify generation and resize before enabling the menu control. |
| Auxiliary effects | Bloom, exposure, DoF, reflections and volumetric consumers | Coverage remains unverified after the RSF handoff. | Inventory actual passes and inspect their depth, buffers, rectangles and history at native and reduced resolutions. |

Next evidence must identify the active shader/pass and its buffer producer rather
than hiding missing data with zero descriptors, current-frame bone substitution or
camera motion. Keep the table current as defects are fixed and game captures become
available; unresolved rows remain integration work.

The FG-only validation capture is `build/checks/rsf-fg3-guides-fixed.log`: 1,794
presents, zero validation errors, shader failures and skipped draws. Its sampled
opening camera reports normal finite depth, near 0.1, far about 1000.53, viewport
depth range 0..1 and LEQUAL. This is observed metadata for that scene, not a rule
to impose on all cameras. The motion-only diagnostic previously tried to
invalidate a nonexistent SR output allocation; it now invalidates only allocated
readbacks. The separate native frame dump emitted layout warnings from its
readback path and therefore does not establish validation acceptance.
