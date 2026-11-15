# RSF shader-input audit

This audit is part of the integration, independent of a reported visual defect.
The entries below are source-review evidence, not game-capture acceptance. The
game extraction, native build and shader preparation have completed; those checks
alone do not establish coverage in a moving scene.

| Path | Inputs and producer | Current finding | Validation still needed |
| --- | --- | --- | --- |
| Scene colour | `vk_dlaa.c`, `rsf_scene`: armed light target, converted to RGBA16F | RSF has a sampled colour input. The copy preserves the source's existing precision; it does not restore detail lost in packed scene production. | Capture source/output, bloom, exposure and post-processing at each scale. |
| Device depth | `dlaa_mv.comp`, `main`: D32F sampled depth written to R32F | Output is produced with the camera-motion dispatch, including identity reprojection on history reset. A missing camera still prevents evaluation. | Compare raw depth, reverse/infinite conventions and foreground coverage in a capture. |
| Camera vectors | `vk_dlaa.c`, `mv_pass`: current camera snapshot and previous view-projection; RG16F output | The zero-motion diagnostic previously skipped depth and camera metadata production, preventing RSF evaluation. It now produces those inputs and clears only the vector image. | Raw finite vectors and static/panning/cut/reset checks. This change has compiled, not been played. |
| RSF camera constants | `rsf_camera.c`, `rsf_camera_matrices`; `vk_dlaa.c`, `mv_pass` and `rsf_scene` | Fixed zero-filled camera matrices in the RSF record. The adapter now derives view/inverse-view, projection/inverse-projection and previous-clip transforms from the recorded snapshot, normalizes OpenGL clip depth to 0..1 and transposes mathematical column transforms for RSF's row-vector contract. Invalid or singular inputs refuse evaluation. Numeric tests cover translation, rotation/FOV change, near/far mapping, reset identity and singular-history refusal. | Capture the actual camera buffer and verify jitter, orientation and reprojection in moving gameplay. Numeric fixtures do not establish the active game's camera layout. |
| Object/bone vectors | `vk_exec_draw.c`, `vk_mvobj_execute`; `vk_mvprev.c` snapshot banks | Fixed a migration defect: history collection checked legacy `dlaa`, leaving previous object/bone inputs unavailable with the new RSF menu key. Collection now follows `rsf_enabled` and clears history and draw deduplication when availability changes. A compiled fixture verifies real block collection/lookup with legacy DLAA off and no stale reuse after disable/re-enable. Missing prior data and unsupported variants still have counters; camera-only vectors remain incomplete independent-motion coverage. | Capture candidate/replayed/missing counts by material class and validate previous object/bone/deformation buffers. |
| Translucency, particles, screen quads | `vk_exec_draw.c`, object-motion filtering | The object replay targets solid depth-writing geometry. It does not establish independent motion coverage for these excluded effects. | Trace their actual producers, depth and composition conventions; add reviewed producers/variants where required. |
| Storage-buffer bindings | `vk_exec_draw.c`, `ssbo_info` and motion descriptor setup | Unresolved/misaligned storage-buffer paths can bind the zero buffer and log warnings. Their presence in the source is not proof that every active scene resolves its bindings. | Capture shader metadata, slot/range mappings and actual bindings before fixing a particular producer. Zero binding is not an acceptable universal repair. |
| HUD-less FG inputs | Scene/tonemap/UI pass boundary | The SR input is pre-tonemap. No verified tonemapped HUD-less producer or input-to-present frame identity is connected yet. | Establish actual ownership and capture before enabling FG; SR output alone is insufficient. |
| Auxiliary effects | Bloom, exposure, DoF, reflections and volumetric consumers | Coverage remains unverified after the RSF handoff. | Inventory actual passes and inspect their depth, buffers, rectangles and history at native and reduced resolutions. |

Next evidence must identify the active shader/pass and its buffer producer rather
than hiding missing data with zero descriptors, current-frame bone substitution or
camera motion. Keep the table current as defects are fixed and game captures become
available; unresolved rows remain integration work.
