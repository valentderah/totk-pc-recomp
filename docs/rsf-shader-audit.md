# RSF shader-input audit

This audit is part of the integration, independent of a reported visual defect.
The entries below are source-review evidence, not game-capture acceptance. The
game extraction is still running; the verified code modules alone do not establish
coverage in a moving scene.

| Path | Inputs and producer | Current finding | Validation still needed |
| --- | --- | --- | --- |
| Scene colour | `vk_dlaa.c`, `rsf_scene`: armed light target, converted to RGBA16F | RSF has a sampled colour input. The copy preserves the source's existing precision; it does not restore detail lost in packed scene production. | Capture source/output, bloom, exposure and post-processing at each scale. |
| Device depth | `dlaa_mv.comp`, `main`: D32F sampled depth written to R32F | Output is produced with the camera-motion dispatch, including identity reprojection on history reset. A missing camera still prevents evaluation. | Compare raw depth, reverse/infinite conventions and foreground coverage in a capture. |
| Camera vectors | `vk_dlaa.c`, `mv_pass`: current camera snapshot and previous view-projection; RG16F output | The zero-motion diagnostic previously skipped depth and camera metadata production, preventing RSF evaluation. It now produces those inputs and clears only the vector image. | Raw finite vectors and static/panning/cut/reset checks. This change has compiled, not been played. |
| Object/bone vectors | `vk_exec_draw.c`, `vk_mvobj_execute`; `vk_mvprev.c` snapshot banks | Missing prior data, unsupported variants and storage bindings have explicit counters. A camera-only vector on an independently moving object remains incomplete motion coverage. | Capture candidate/replayed/missing counts by material class and validate previous object/bone/deformation buffers. |
| Translucency, particles, screen quads | `vk_exec_draw.c`, object-motion filtering | The object replay targets solid depth-writing geometry. It does not establish independent motion coverage for these excluded effects. | Trace their actual producers, depth and composition conventions; add reviewed producers/variants where required. |
| Storage-buffer bindings | `vk_exec_draw.c`, `ssbo_info` and motion descriptor setup | Unresolved/misaligned storage-buffer paths can bind the zero buffer and log warnings. Their presence in the source is not proof that every active scene resolves its bindings. | Capture shader metadata, slot/range mappings and actual bindings before fixing a particular producer. Zero binding is not an acceptable universal repair. |
| HUD-less FG inputs | Scene/tonemap/UI pass boundary | The SR input is pre-tonemap. No verified tonemapped HUD-less producer or input-to-present frame identity is connected yet. | Establish actual ownership and capture before enabling FG; SR output alone is insufficient. |
| Auxiliary effects | Bloom, exposure, DoF, reflections and volumetric consumers | Coverage remains unverified after the RSF handoff. | Inventory actual passes and inspect their depth, buffers, rectangles and history at native and reduced resolutions. |

Next evidence must identify the active shader/pass and its buffer producer rather
than hiding missing data with zero descriptors, current-frame bone substitution or
camera motion. Keep the table current as defects are fixed and game captures become
available; unresolved rows remain integration work.
