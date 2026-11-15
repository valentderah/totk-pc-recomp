# Storage pointer and motion snapshot audit

Source baseline: TotK `57c98e9`, branch `rsf`, 15 November 2026. This is
source review, existing capture inspection and isolated CPU/shader verification.
No game, GPU, SDK device test or visual acceptance was performed by this audit.
The character skin report belonged to Skyrim, not TotK.

The question was whether translated shader inputs and previous deformation data
actually reach SR/FG, independently of successful vendor initialization. The RSF
game-render-analysis workflow was used to trace consumers back to their owners.
No RSF implementation or reference code was copied or changed.

## Binding findings and repairs

`runtime/vk_exec_draw.c`, `ssbo_slot` maps metadata c0 word offsets to
16-byte NVN entries: VS base 0x110, FS base 0x510, CS base 0x310. Compute
c0 0x210 through 0x300 maps uniform slots read as storage. These are not
translator resource enumeration indices. `ssbo_info` rounds the address down
by `sbuf_lead`, extends a known range by that leading offset, caps it to the
backing and device limit through `bind_range`, and records writable leases.
Unknown mappings or failed backing resolution still select the zero fallback.

The existing frame-1600 `build/checks/rsf-fg-native-dump/passes.txt` contains
VS c0 offsets 0x110/0x140/0x150, FS 0x510/0x550, and CS offsets in both
the uniform-as-storage and storage tables. Those offsets have mappings in
`ssbo_slot`. The dump prints all bound SSBO slots rather than the resolved
descriptor range; compute uniform-as-storage entries require the UBO state,
so that listing alone cannot prove their backing. It does not record a
user-bank storage pointer. The existing guides-fixed log contains no zero-buffer
warning. Neither observation establishes universal binding correctness.

The loose translated metadata in `build/v143-rsf/shader-cache` has 27 distinct
stage/bank/offset storage mappings, including 21 vertex variants referring to
c6+0. `build/xlate/xlate/Program.cs` serializes these as `cb_slot` and
`cb_offset` (words); `GfxShaderRes` exposes them as `cbuf_slot` and `handle`.
The aggregate and example metadata paths are preserved only in ignored
`build/checks/rsf-storage-worker/metadata-summary.json`. Cache presence proves
a translated path, not execution in this particular capture.

`ubo_ptr` previously resolved these pointers from recorded inline updates or
live guest memory. In contrast, `ubo_info` binds submission snapshots. A CPU
rewrite during replay could therefore make a shader's uniform data and its
resolved storage descriptor refer to different frames. `ubo_ptr` now consults
the snapshot after recorded updates and before live memory, using the current
chunk's drop position. It uses the CPU shadow through `vk_usnap_map`; it does
not read the device buffer or manufacture a pointer. Disabled/dropped snapshots
retain the existing live-memory path. This repairs a verified source contract
defect; no captured zero-buffer occurrence is attributed to it.

The `mvobj_draw_` composite memo was keyed by present frame and block addresses,
although its values include descriptors into submission snapshots. `mo_begin`
already advances `mo_gen` on fence/list changes and drops the previous list.
The memo now uses that generation, matching `mo_prev_cached`, so another
submission within one present recomputes its moving decision and descriptors.
Snapshot arenas remain leased through `vk_usnap_end`'s fence completion callback;
this change does not extend their leases or replace actual completion ordering.

## Coverage and remaining gaps

| Class | Required input and owner | Finding and remaining work |
| --- | --- | --- |
| Static solid scene | Depth and c4 camera, camera compute | Existing RG16F previous-minus-current render-pixel motion and R32F device depth. Zero motion can be valid. No coverage expansion here. |
| Rigid solid objects | c6/c7 transforms, `vk_mvprev_find`, uniform snapshot, motion variant | Existing indexed depth-tested/depth-writing replay; submission memo ownership corrected. Previous-copy selection remains heuristic. |
| Skinned solids | Previous c5 bones plus placement/camera | Existing two-copy history and similarity acceptance. Missing previous banks still use current data, which is explicitly incomplete deformation coverage. Moving animation must be checked. |
| Terrain/foliage and GPU instance data | Storage pointers/tables, deformation banks | User-pointer snapshot consistency repaired. Read-only storage descriptors are admitted by `mo_desc`, but previous storage contents are not produced. c9/c11 history remains excluded by prior failed still-camera comparisons; clock-only variants are filtered by default. |
| Translucency and particles | Independent position/deformation history and appropriate composition/depth | No producer established. `mvobj_draw_` excludes non-depth-writing draws; screen quads and non-indexed/byte-index paths do not acquire independent motion through this replay. Do not remove the filter and paint background depth with effect motion. |
| Volumetrics | Per-volume history, depth/composition and temporal convention | No previous density/position producer established. Camera reprojection is insufficient; native colour remains rendered, but temporal reconstruction of it is unverified. |
| UI and auxiliary effects | Native UI boundary; bloom/exposure/DoF/reflection inputs | No changes. Existing torch/cave generated frames establish output, not complete independent motion or reduced-scale auxiliary coverage. |

`tools/xlate/patches/0015-motion-previous-position.patch`, `MotionPrevious.Run`
duplicates the position dependency slice with uniform banks c3..c16 remapped to
18..31. Operations it cannot repeat retain their current result, including
texture operations. Previous storage/deformation textures and changed control
flow are therefore not supplied merely by binding previous uniforms. The
`tools/make_mvobj_fs.py` shader converts previous clip position to window pixels;
it cannot supply missing previous deformation. No motion variant was broadened
without evidence of those producers.

## Verification and required moving-game tests

`tests/rsf_storage_pointer_check.py` extracts the actual `ubo_ptr` implementation
and compiles a synthetic C fixture with UCRT64 Clang and warnings as errors. It
passes snapshot-versus-rewritten-live-memory, recorded-update precedence, chunk
drop-position forwarding, absent snapshot/backing and short binding checks.
The existing `tests/rsf_motion_history_check.c` was compiled and executed in the
same isolated directory and passes. The initial fixture did not exercise partial
inline pointer updates; the follow-up below adds that proof. GPU-produced pointer
contents remain a lead.

The full changed `vk_exec_draw.c` compiled with the existing runtime build flags
to `build/checks/rsf-storage-worker/vk_exec_draw.o`. The unchanged
`runtime/shaders/dlaa_mv.comp` compiled with Vulkan SDK glslangValidator and
passed spirv-val. All outputs use `build/checks/rsf-storage-worker`; shared game
builds, shader caches and SDK artifacts were only read. No full relink was run.

The coordinator still needs bounded moving-game evidence: animate Link and a
separate rigid object under a fixed camera, then pan with both moving; identify
the previous bones/placement and resolved storage descriptors at those draws.
Check torch flames, overlapping translucent surfaces, wind-driven foliage and
volumetrics separately against native source frames and camera-only motion.
Capture the c6 pointer path with its recorded updates, snapshot identity and
final descriptor offset/range, and exercise multiple submissions in one present
with rewritten blocks. Compare raw motion validity and flow, then SR/FG output,
including cuts, pause/load, backend changes and resize. The existing flow misses
must be associated with exact draw classes before proposing further variants.
These repairs do not establish complete SR/FG motion coverage.

## Follow-up: current CPU snapshots and partial inline pointer writes

Coordinator review identified two source-contract gaps in the initial patch.
The first was arena reuse after disabling SR/FG: `vk_usnap_begin` writes directly
to `Arena.map` when temporal consumers are off, but `host_copy` selected
`Arena.cpu` whenever that allocation existed from a previous use. This could
return stale pointers even though the current GPU snapshot was correct.
`Arena.cpu_current` now records which destination this use actually populated;
`host_copy`, and therefore `vk_usnap_map` and `vk_usnap_bytes`, selects those
current bytes. It preserves the CPU allocation for a later enabled frame and
does not change completion ownership or introduce a second copy when disabled.

The second was partial inline pointer writes. The producer is
`runtime/nvn_api_cmd.c`, `nvnCommandBufferUpdateUniformBuffer`: it copies payload
bytes into the command record at recording time. `runtime/vk_exec_draw.c`,
`op_update_ub` uses those immutable bytes for either a pending overlay or an
immediate scratch-to-buffer copy and calls `upd_note` on both accepted paths.
`runtime/vk_exec.c`'s scan builds the chunk update list and drops the snapshot
at the update's position; each chunk starts with `upd_n` and `drop_n` for its
own position. Chunk replay increments the prefix as it encounters each update.
The pointer consumer must use that prefix rather than future scan entries.

Previously `upd_read` returned false at a partial overlap and `ubo_ptr` then
read a base without applying that overlap. The uniform consumer's `ub_bind`
already handles merged buffers by applying updates in order. The pointer path
now follows the same byte contract: `upd_read` walks newest first and accepts
complete coverage, including split writes or a later partial overwrite of an
older whole write. Complete coverage needs no snapshot or live backing. For
incomplete coverage, `ubo_ptr` reads the current base snapshot at the chunk drop
position, or the existing live fallback if it was dropped, then `upd_apply`
overlays recorded bytes oldest first. Without a base or complete coverage it
returns unresolved rather than manufacturing the untouched pointer bytes.

`upd_apply` now accepts the chunk's scan prefix as well as the serial replay
ring, so pointer and merged uniform consumers share the ordering implementation.
Only serial replay is bounded to the last `UPD_N` ring entries; the chunk scan
is linear and retains earlier updates beyond that count. Serial entries still
require the matching submission fence. This does not reconstruct arbitrary
GPU-written pointer contents, change snapshot-drop policy, or repair historical
updates already evicted from the serial ring.

The expanded `tests/rsf_storage_pointer_check.py` compiles the actual
`upd_read`, `upd_apply` and `ubo_ptr` functions, rather than mocking update
resolution. It passes single 32-bit updates preserving snapshot bytes, split
32-bit halves without any base, overlapping writes across word boundaries,
newest partial and whole precedence, whole-update resolution without backing,
live fallback after a chunk snapshot drop, future-update exclusion by prefix,
linear scan histories longer than `UPD_N`, foreign-fence exclusion and short
binding refusal. It asserts the forwarded chunk drop positions.

`tests/rsf_snapshot_currentness_check.py` compiles the actual snapshot begin,
end, lookup and read functions with synthetic allocation/command producers.
The same arena is reused for temporal on/off/on, with three distinct byte
patterns; the disabled middle submission deliberately retains the old CPU
allocation and verifies both public CPU readers return the new mapped bytes.
Re-enabling refreshes the CPU shadow, and all three end calls reach the
synthetic completion callback. It also verifies the real lookup accepts a
snapshot before a drop and refuses it at the drop position.

Both CPU fixtures pass with UCRT64 Clang, `-Wall -Wextra -Werror`. The complete
changed `vk_exec_draw.c` and `vk_usnap.c` compile with the runtime's existing
flags to separate objects under `build/checks/rsf-storage-worker`. There were no
GPU/game tests, shared build writes or commits. The independent motion gaps and
moving-game tests in the preceding table remain open.
