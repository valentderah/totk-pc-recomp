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

## Rebased continuation: previous-block cache validity

This continuation reviewed TotK `7304e05` after rebasing onto upstream
`dev6151ac4`, with `external/ReScaleFrame` at `1753d4d`. The original pointer
fixture initially failed to compile because upstream changed `upd_apply` to
`MX_CPY`. The isolated harness now maps that documented memcpy-equivalent macro
to `memcpy`; this checks pointer/update semantics, not the SIMD copy mechanism.
The snapshot-currentness fixture passed unchanged. Both fixtures now write to
`build/checks/rsf-storage-rebase-worker`, separate from the running game build.

Two further producer/consumer defects were found in the previous-block caches.
`runtime/vk_usnap.c`, `drop` invalidates snapshot regions after inline updates,
copies and shader writes and advances `U.gen`. `mo_prev_cached` used only the
motion-list generation, address, size and bank, however, so an earlier successful
lookup could continue to return a descriptor after the producer had dropped that
snapshot within the same list. It now also checks `vk_usnap_gen`. This allows
`mo_prev_block` to refuse the invalid snapshot rather than bypassing its lookup.
Snapshot begin/end already advance the same generation.

The composite memo in `mvobj_draw_` also omitted bound sizes from its key. Two
draws can bind the same address with different uniform ranges; the previous
descriptor's range and the history copy's step both depend on that size. A new
`mo_blocks_key` includes each participating bank's address and size, plus
snapshot generation. The outer memo can therefore no longer bypass the corrected
per-block lookup on these changes. List generation remains a separate owner
check. Unavailable per-block outputs and the composite descriptor array are now
initialized, avoiding copies of indeterminate descriptors in unused bank slots.

`tests/rsf_motion_cache_check.py` compiles the actual `mo_prev_cached` and
`mo_blocks_key` functions with a synthetic previous-block producer. It checks
unchanged-state reuse, changed bone range, snapshot drop refusal, renewed
snapshot acceptance, initialized failed descriptors and a new list owner. It
does not simulate the full draw replay or establish a captured visual defect.
The two existing actual-function fixtures, this new fixture and the existing
motion-history fixture all pass. The complete changed `vk_exec_draw.c` compiles
with runtime flags and include paths pointing to the pinned external RSF tree.
The unchanged camera-motion compute shader compiles with glslangValidator and
passes spirv-val. All outputs are in the isolated directory above; no shared
build outputs, RSF files, GPU/game sessions or commits were changed.

Coverage remains the solid indexed replay described above. In particular,
`vk_mvprev_find`'s same-address case still assumes unchanged contents; an address
alone does not prove that assumption for every producer. A true previous-frame
copy for rewritten same-address blocks would need retained contents and an
explicit upload/lease, not a changed lookup label. The existing variant duplicates
uniform-dependent position arithmetic but does not provide previous storage
contents or deformation texture samples. Missing previous bone banks still
retain current inputs, and auxiliary translucency/particle/volumetric paths
still have no verified independent-motion producer. These remain limitations,
not repaired coverage claims. The next bounded game evidence should include
same-address bone writes and two draws with changing bound ranges, plus an
inline/GPU write between motion candidates, alongside the per-class moving
tests recorded earlier.

## Retained same-address previous-frame bones and placement

The preceding same-address limitation now has a concrete implementation. The
upload owner remains `vk_usnap_begin`: it copies the submission's collected
uniform ranges into its immutable arena and flushes the mapped allocation.
`vk_mvprev_draw_` records descriptors and CPU byte pointers for observed banks
by retaining that existing arena. It allocates or copies no object bytes per
draw. Repeated unchanged bindings are deduplicated by address, size and snapshot
generation; a new snapshot updates the retained record. The full arena, rather
than just the individual block, remains allocated while any history record
leases it, so this deliberately trades additional retained snapshot memory for
avoiding a separate per-block upload or copy.

`runtime/vk_mvprev.h` defines the internal retain/release/use contract without
changing the public renderer header. `vk_usnap_retain` accepts only a complete,
currently valid snapshot range. The two existing history tables now carry those
leases: frame rotation releases the older table, availability changes release
both, and `vk_usnap_shutdown` releases history before freeing retired arenas
under its existing device-idle contract. Each arena has a reference count for
its original submission completion, CPU history leases, and later consuming
submissions. `vk_usnap_use` adds a completion hold once per arena/submission
fence, before returning a previous descriptor to motion replay. Resetting CPU
history cannot recycle the arena while those GPU consumers remain pending.
No fence waits or lifecycle hooks outside the owned files were added.

`vk_mvprev_self` now resolves a same-address lookup to the retained previous
frame's allocation and bytes, not today's snapshot. It refuses missing history,
zero fence identity and any bound-size mismatch. Capture refuses ranges larger
than the snapshot's 64 KiB bound. `mo_prev_block` uses that old descriptor and
compares its bytes against the current submission's snapshot through the
existing similarity guard. Unchanged contents therefore remain unchanged;
accepted rewritten bones/placement can now produce independent motion. Existing
skip banks and similarity thresholds still apply, so this is not acceptance of
every animation or object identity. The consumer also refuses independent-motion
replay when a required c5/c6/c7 bank has no valid previous data, rather than
silently mixing current bones with an independently moving placement bank.
Native rendering and its current descriptors are unchanged; refused motion
replay leaves the existing camera-motion field in place, which remains limited
coverage and is not represented as complete deformation motion.

`tests/rsf_retained_history_check.py` combines the actual snapshot begin/end,
lookup, lease, frame-rotation and history-consumer functions with synthetic
uploads and deferred completion callbacks. It proves that the same guest bone
address overwritten between frames yields old bytes in the previous descriptor,
new bytes in the current descriptor, and distinct corresponding mapped upload
contents. It also covers unchanged data on a later frame, missing and mismatched
ranges, zero size/fence, one completion hold for repeated use, and clearing CPU
history before delayed GPU retirement. Both arenas remain referenced until the
pending callbacks run. The fixture does not call a Vulkan driver and does not
prove actual GPU execution or game quality.

That combined fixture, the snapshot-currentness fixture it builds on, the
storage-pointer fixture, the motion-cache fixture and the existing motion-history
fixture pass. Complete `vk_exec_draw.c`, `vk_mvprev.c` and `vk_usnap.c` compile
with the runtime flags and the pinned `external/ReScaleFrame` includes to
`build/checks/rsf-retained-history-worker`. No shaders changed, so this change
needs no new shader compilation. There were no GPU/game tests, shared build
writes, staging or commits.

Remaining evidence must check same-address and alternating-address animation in
a moving game, including memory cost from retaining whole snapshot arenas and
history reset while SDK work is in flight. Separate previous storage contents,
GPU-produced pointer history, texture-driven deformation, translucent geometry,
particles and volumes still need their actual producer and composition contracts
traced. Those paths were not broadened by this repair.

## Previous storage-dependent positions with immutable snapshot history

The next audit traced `MotionPrevious.Run` through resource descriptors and the
native motion replay. Merely rebinding the existing SSBO to old contents would
also change the shader's current `gl_Position`. The previous variant shared the
current storage descriptor, even when it duplicated storage-dependent arithmetic.
The repair therefore adds a distinct descriptor contract rather than replacing
the current input.

`tools/xlate/patches/0017-motion-previous-storage.patch` extends the shader
transform only. A storage load in the position dependency slice receives a
separate read-only binding. Its metadata retains the pointer offset and marks
its source constant bank as original bank + 64, within the existing signed-byte
resource field. Current loads keep their original binding; previous loads use
the new binding. Up to 16 additional bindings fit the expanded 32-entry shader
resource map and its existing 32-bit usage mask. Writable storage, unresolved
binding identities and texture-dependent position operations refuse this
variant. The translator build script now expects the additional patch, and its
identity hashing automatically separates the new translations from old caches.
The runtime also refuses legacy SSBO variants lacking the separate binding
contract, so a stale cached variant cannot silently call current storage previous.
This is shader translation work, with no ARM64 lifter changes.

`mo_storage_range` resolves the original metadata through `ssbo_bound`, including
the current submission's pointer updates/snapshot, and obtains the same capped
backing range that the normal storage descriptor would use. A user-bank pointer
is identified by its owning uniform block and metadata bank/offset, so changing
the pointee address can still find its previous content. Direct table bindings
use their storage address as identity; changing a direct binding address or
alternating the pointer-owner block without a matching identity refuses history.
This does not guess object identity from adjacent storage allocations.

`mo_storage_capture` records read-only ranges in the existing frame tables via
`vk_mvprev_storage`. Only a complete range already covered by an immutable
submission snapshot can be retained; no new pool-wide copies are taken. Ranges
absent from the snapshot, GPU-written ranges whose snapshot was dropped and
other live-only storage do not establish previous data. An unavailable later
capture invalidates that identity's earlier record rather than preserving stale
history. The arena now advertises STORAGE_BUFFER usage in addition to its
existing uniform/transfer-source usage. The previous descriptor uses the retained
arena and actual consumer fence through the same reference/retirement contract
as bones. Missing previous storage returns failure from `mo_desc`, leaving native
rendering intact and declining independent-motion replay rather than binding zero
or current storage as previous.

Storage pointer alignment is deliberately bounded: both captured and current
pointers must already satisfy the shader's storage alignment. The translator's
address compensation may still read current pointer low bits; when the bits are
zero in both frames they agree. Unaligned pointers refuse this path until their
previous address compensation has its own verified contract. Metadata that gives
only a pointer and no size requires the complete capped backing range, not a
guessed vertex subset. This can cause refusal even when some bytes happen to be
snapshotted, which is preferable to claiming incomplete ranges as valid history.

Solid depth-writing scene-camera variants with this separate storage contract
can now qualify without a c5/c6/c7 transform, since storage itself is an
independent position producer. Equal uniform banks also no longer suppress such
a motion draw. Actual descriptor admission still requires valid previous
storage. The original depth, camera, rasterization and unsupported-pass filters
remain; this adds no translucent, particle, volumetric or texture-history path.
No active TotK storage draw or visual improvement is claimed from these source
and synthetic checks.

### Lease review and memory estimate

The arena's references cover its original submission callback, each retained
history record and consuming submissions. `use_fence` avoids duplicate callback
holds for repeated use within the same submission. Returning to an earlier fence
after another fence may add a redundant hold, but cannot omit retirement;
recording/completion references prevent reuse until the final hold is released.
Snapshot drops only affect admission of new records; they never mutate already
uploaded retained bytes. Currentness is selected by `cpu_current`, including the
disabled-temporal arena-reuse case. The CPU tests cover those transitions and
reset before delayed completion; no device-level concurrency proof is claimed.

The existing guides-fixed log records one 4 MiB arena allocation before retention
was introduced. That is allocation-size evidence, not a retained-history peak.
For a 4 MiB arena with a CPU shadow, the mapped upload plus shadow is about
8 MiB. If K distinct such arenas are retained in each of two history tables,
their contents cost up to about 16K MiB, plus approximately 8 MiB per additional
distinct in-flight arena. The two 16,384-entry history tables are about 2.5 MiB
with the current 80-byte entries; optional CHECK byte tables add 4 MiB. Allocation
rounding and driver overhead are additional. The source limits records by table
capacity and complete captured range, but supplies no small global byte budget;
K and in-flight overlap still require the coordinator's runtime memory check.
Keeping an entire arena for a small block can be expensive. A compact batched
history upload or a byte budget is a future optimization, not a measured need or
a claimed cost bound for gameplay.

### Checks and next producer work

The translator was built with all patches, vendor byte-identity verification and
NativeAOT in `build/checks/rsf-storage-variant-worker/xlate`. The existing vendor
IL2075 trimming warning remains; the build succeeds. No shared translator or
SDK output was replaced. `tests/rsf_storage_variant_check.py` compiles a synthetic
IR harness into that isolated shader project and exercises the actual transform
for camera/storage and placement/storage positions. It proves the current load
retains its binding, the previous load receives a distinct bank+64 descriptor,
and writable storage refuses the variant. It then emits a synthetic GLSL
interface using those actual binding numbers; glslangValidator and spirv-val
pass. The GLSL fixture is an interface check, not a claim of game Maxwell shader
translation or GPU execution.

`tests/rsf_previous_storage_check.py` exercises actual range resolution, capture,
previous descriptor lookup, snapshot upload and lease/history functions with
synthetic bindings and deferred fences. It proves an unchanged pointer owner can
point to different storage addresses across frames while previous and current
descriptors retain different contents and uploaded bytes. Range, alignment,
write flags, missing identities and dropped-snapshot refusals pass, including a
drop after an earlier successful capture. Repeated use shares its fence hold;
reset cannot recycle either upload before completion. Existing pointer,
snapshot-currentness, retained-bones, motion-cache and motion-history fixtures
also pass. All three complete changed runtime sources compile against the pinned
external RSF headers into the same isolated check directory.

Next, capture the active storage position variants and their complete ranges to
establish how often this narrow immutable-history path admits real draws. Static
pool ownership alone is not immutability. Device-local GPU-driven storage needs
a producer-ordered copy and a resource lease at its write/read boundary; that
would require coordination with the replay/synchronization owners before adding
hooks outside this task. Unaligned pointer compensation, alternating pointer
owner identities, texture-driven deformation and changed control flow need
separate evidence. Broader independent particle/translucent/volumetric motion and
moving-game quality remain open. No game, GPU, UI or device test was run here.

## Real v143 translator census gate and coordinator rebuild recipe

The translator checks required by `CONTRIBUTING.md` were run without replacing
the shared translator or prepared game shaders. An ignored runner imports
`tools.xlate_smoke`, overrides its executable to the isolated new translator,
and runs its actual smoke test. Valid fragment translation, malformed-request
recovery, subsequent translation and Vulkan 1.3 SPIR-V validation pass.

The same runner imports `tools.shader_census` under Python 3.14, inventories the
entire verified v143 RomFS/ExeFS with `everything=True`, and calls
`write_shaders(..., everything=True)` into an isolated corpus. It finds 28,667
unique shader jobs: 12,454 startup jobs and 16,213 additional effect/layout jobs.
The inventory rejects 5,006 candidate records lacking valid control-section
magic, using the census's existing filter. Both translators receive the identical
job objects and code paths, with the normal census `spirv-val --target-env
vulkan1.3` check for every successful output.

| Translator | Identity prefix | Translated and validated | Translation failures | SPIR-V errors |
| --- | --- | --- | --- | --- |
| Shared old baseline | `e47973fdb4322c9d` | 28,667 | 0 | 0 |
| Isolated new storage variant | `c6e1f527c908b3b5` | 28,667 | 0 | 0 |

There are no new failure groups or newly failing shader keys. No expected
bind-state omission group was needed to excuse this corpus: both sets are empty.
These default-state census jobs do not themselves request motion variants or
prove runtime descriptor coverage. Timings are not a performance comparison:
the baseline took about 59 seconds and the new pass about 74 seconds while a
separate CPU motion-variant check shared the machine. Full inventory took about
73 seconds. Detailed jobs, shader outputs, runner and comparison JSON remain
under ignored `build/checks/rsf-storage-census-worker`; no game-derived inputs or
outputs were added to tracked files.

An additional comparison runs all 14,765 real v143 vertex jobs with `mvprev=true`
on both translators, retaining the same otherwise-default answers and the same
Vulkan 1.3 SPIR-V check. Both translate and validate all 14,765 jobs with zero
translation failures, validation errors, new failure groups or newly failing keys.

| Motion metadata | Old baseline | New translator |
| --- | --- | --- |
| Valid previous-position output | 14,269 | 12,994 |
| Explicitly unavailable previous-position output | 496 | 1,771 |
| Separate previous-storage descriptor and valid previous output | 0 | 140 |

There are exactly 1,275 newly refused variants and no newly admitted variants
relative to the old previous-output set; 900 of those refusals are startup
shaders. Their sources comprise 1,241 BNSH and 34 sharcb jobs. This is a material
capability reduction, not a translation error to hide in failure-group totals.
The new transform's guards refuse position operations without a supported
previous producer, rather than exposing current input as previous. The census
alone does not classify which of these shaders are active solid-scene draws or
which refusal guard each hits. Four additional metadata records contain an
allocated previous-storage descriptor but no valid previous output, and must
not be counted as admitted coverage. Exact refusal keys and sources remain in
ignored `motion-coverage.json`, with both outputs and `motion-summary.json` in
the same isolated check directory. Before rollout the coordinator must assess
the actual newly refused draw classes in moving gameplay; a passing translator
gate is not complete independent-motion coverage or visual acceptance. Motion
translation timings (about 98 seconds old and 63 seconds new) are not comparable
performance measurements, since the old pass overlapped the default census and
the algorithms can do less work when refusing unsupported positions.

The following recipe is for the coordinator's controlled preparation window;
none of its shared-output writes or device steps were executed by this worker.
Stop/wait for the coordinator-owned game and build processes before replacing
their translator or outputs, and preserve the existing recorded shader pairs.

```powershell
# Repository root, after the coordinator releases the running build/game.
.venv/Scripts/python.exe tools/build_xlate.py
.venv/Scripts/python.exe tools/xlate_smoke.py

# Force the intended executable even if an older packaged translator sits next to totk.exe.
$env:TOTK_XLATE = (Resolve-Path build/xlate/totk_xlate.exe).Path
# Verify config/environment do not force a stale TOTK_XLATE_ID or a different shader cache.
# Preserve the coordinator's existing combined-build recipe and flags.
# Skip this if its matching current runtime has already been built successfully.
& C:/msys64/ucrt64/bin/ninja.exe -f build/v143-rsf/build.ninja

# This translates seed variants and creates Vulkan stage libraries: coordinator device owner only.
.venv-py314/Scripts/python.exe tools/prepare_shaders.py build/v143-rsf --version v143

# For the coordinator's next existing bounded gameplay validation command:
$env:TOTK_TEST_PREPARE_SHADERS = '1'
# Run that approved command, then clear this test hook after its recorded-pair preparation check.
```

`prepare_shaders.py --no-run` is the CPU-only alternative for writing seed files;
it does not establish device preparation. `--warm-shaders` rebuilds driver
libraries from prepared entries and is not a substitute for translating seeds.
The recorded-pair preparation hook in `boot.c`, `prewarm_main` invokes
`gfx_shader_prepare_recorded`; preparation of seed defaults alone does not request
every motion variant. Existing recorded answers carry the `mvprev` bit. Previously
unseen motion variants are requested by `shader_for` when a qualifying draw is
encountered, so a bounded moving-scene validation must also check variant build
completion and previous-storage admission/refusal.

Keep the generated `.id` paired with its executable. `gfx_shader.c`,
`key_of_input` incorporates translator identity; `mask_answers` retains the
motion bit even when unused answers are dropped. The new identity invalidates
both ordinary and motion translation keys without deleting the old shader pack
or overwriting its identity. Do not relabel old shader outputs with the new ID.
Keep `shader-programs.bin` and `shader-answers.bin` so known real bind states can
be translated again under the new masks and identity. The coordinator should
verify the translator identity printed by the runtime before accepting the run.

The reported ten depth-only pipelines with a fragment output at location 0
cannot be attributed from warning counts alone. Patch 0017 changes vertex IR
and storage descriptors, not the motion fragment shader. The normal motion
replay in `mvobj_draw_` explicitly binds RG16F colour attachment 0, matching that
fragment output; it is not a depth-only target. A shader-library/preparation
context or native depth-only pipeline needs its shader/pipeline identity to
distinguish it, and an identical baseline device run remains required. Offline
census success does not establish absence or cause of Vulkan pipeline warnings.

## Exact triage of every newly refused variant

An isolated managed translator copy records the first refusing guard and the
position dependency slice for each of the 1,275 newly refused keys. Production
runtime, translator sources and shared translator outputs were read only during
this task. The probe preserves guard ordering and splits the storage guard's
short-circuit conditions only to name them individually. All 1,275 probe jobs
translate and validate successfully. Their complete per-key records, including
source family, exact reason, texture bindings/types/flags, storage presence,
position phi nodes and original bank masks, are in ignored
`build/checks/rsf-refusal-triage-worker/classified-keys.json`.

| Exact first new guard | Keys |
| --- | --- |
| Derived `TextureOperation`, instruction `TextureSample` in the position slice | 1,271 |
| Derived `TextureOperation`, instruction `TextureQuerySize` in the position slice | 4 |
| Dynamic, writable, unsupported-bank, missing or capacity-exhausted storage binding | 0 |
| Other derived operation types or control-flow guards | 0 |

Every refusal is texture-dependent. Twelve also have storage operations in the
position slice; none refused because of storage. Position phi nodes occur in
1,113 cases, indicating a merge in the data feeding position. There are branches
somewhere in 1,125 of the shaders; this broader program count is not proof that
every branch controls position or changes between frames. Control flow is not a
new refusing guard. Sampling types in the slices are predominantly 2D, with
array/3D/cube cases also present. These counts describe the actual transformed
IR, not an inference from texture declarations alone.

The four size-query-first keys come from
`Effect/EftLoc_MiasmaAppearance_02`, `_03` and `_11` packages (two variants of
`_02`). All four also contain integer-coordinate, explicit-LOD texture samples
in the same position slice, 19 or 20 phi nodes, and 45 or 56 program branches.
Allowing the size query alone would not provide previous position data. This
rejects the tempting but incomplete four-key query-only repair.

| Source family | Keys | Has scene camera c4 | Camera plus c5/c6/c7 | Position phi present |
| --- | --- | --- | --- | --- |
| `Shader/material.Product.140...bfsha` | 712 | 712 | 712 | 630 |
| `Effect/*.esetb.byml` | 375 | 0 | 0 | 375 |
| `Shader/terrain.Product.140...bfsha` | 70 | 70 | 0 | 43 |
| `Shader/terrain_water.Product.140...bfsha` | 45 | 45 | 21 | 45 |
| `Shader/grass.Product.140...bfsha` | 36 | 36 | 0 | 9 |
| AGL techniques/common, merge targets and application light shaders | 37 | 8 | 0 | 11 |

The 900 startup cases are the non-Effect rows. Source-family names support
material, ground, water, grass and effect classifications; the combined material
package does not identify a particular character or object. The 37 miscellaneous
cases comprise 24 `agl_technique`, four `agl_technique_proc`, three `merge_tar`,
two `agl_common`, two `agl_technique_shdw`, one `agl_technique_pfx` and one
application `AglLightShader` key. All twelve storage-plus-texture cases are in
this miscellaneous group. Specific effect package names and all key assignments
remain in the ignored per-key report rather than being presented as confirmed
player-visible particle types.

### Active evidence and prioritization

The old frame-1600 torch/cave dump was joined through actual runtime shader
inputs, not merely a matching source-package name. Exact SPIR-V bytes identify
dumped shaders in the runtime cache. The old translator identity and recorded
host-options identity reproduce their cache keys from stored `.in` input IDs;
the stored control/code then recover census keys. Loose and packed cache
entries are read without mutation. This recovers all 249 dumped shader hashes
and 115 vertex input states; 114 vertex program keys appear in the v143 census,
with one outside that inventory. None overlaps the 1,275 newly refused keys.

The 115 vertex states were also replayed through the instrumented translator
with their stored texture and vertex-input answers, constants, storage alignment
and `mvprev=true`. There were no VS images needing format reconstruction. All
115 translate and validate: 86 expose previous output, 28 hit the existing
no-uniform-bank condition and one has no spare output location. None hits a new
texture/storage guard. The input reconstruction and join tables remain ignored
in `active-jobs.json`, `active-classification.json` and
`runtime-capture-matches.json`. This is bounded offline evidence for the old
captured frame, not a fresh game acceptance run or proof of other scenes.

That frame contains 29 distinct material vertex keys across 63 recorded pipeline
entries and 141 draws. It also contains cave, torch, static-effect and miasma
families. The observed miasma package includes `_04`, not the newly refused
query-first `_02`/`_03`/`_11` variants. Material-family activity must not be
misreported as activity of the 712 different refused material keys. Terrain,
grass and water refusal families are not established active by this capture.

The evidence-based priority order is therefore:

1. **Material variants:** largest potential solid-scene risk, with all 712
   meeting the old camera/primary-bank filters, and the family observed in the
   capture. No specific refused material key is proven active. Their c11
   dependency, and often c9, also intersects the earlier deliberately skipped
   history banks; a texture-only fix cannot claim complete previous deformation.
2. **Water:** 21 of 45 meet those uniform filters, but every case has phi nodes
   and this capture proves none active. Previous samples and previous control
   selection both need evidence.
3. **Ground/grass:** useful narrow producer experiments, but these 106 cases
   lack c5/c6/c7 and were already outside ordinary uniform-only replay filters.
   Their new shader refusal count is not an equal count of newly lost active
   motion draws. There are 27 terrain and 26 grass variants with neither phi nor
   branch operations, avoiding the first previous-control-flow problem.
4. **Effect and AGL variants:** the 375 effect cases lack the scene camera and
   were already rejected by the normal scene-camera motion policy. A separate
   view/composition producer is required; do not remove that filter to recover
   an aggregate output count. The other 37 need pass-specific classification.

### Next correct repair decision and required owner hooks

The next implementable producer repair is a **versioned texture/view/sampler
lease for genuinely unchanged texture contents**, followed by duplication of a
small straight-line texture-position slice. Begin with the 27 terrain or 26
grass no-phi/no-branch candidates after an actual draw identifies their inputs.
They avoid the material c9/c11 ambiguity and water/effect control-flow problem.
Neither source filenames nor a stable VkImage handle prove immutable texels.

The narrow required contracts are:

- The texture owner supplies allocation identity, full view/mip/layer/format/
  swizzle identity, sampler state and a content generation at the native read
  boundary. That generation must conservatively change for CPU uploads, render
  or storage writes, clears/copies and overlapping aliases. Existing global
  `vk_texture_slots_gen` is a slot-resolution generation; `vk_texture_epoch` is
  a write-watch epoch. Neither alone certifies unchanged image contents.
- A texture-owner retain/release hook protects the allocation and view across
  history and consumer fences. `destroy_tex` currently marks entries gone and
  defers destruction for the submission being recorded, which does not grant
  an arbitrary previous-frame history lease. Add this with the texture owner,
  rather than holding raw image/view pointers in motion code.
- Motion history records that certificate with previous parameter snapshots.
  Reuse sampled contents only if allocation, view, sampler and content generation
  still agree at the next consumption point. Mutated or untracked resources
  refuse history. GPU-written textures need a producer-ordered prior copy if
  coverage is desired later; retaining an image alone cannot preserve texels.
- The shader owner clones the actual `TextureOperation`, preserving its sample
  opcode/type/flags/LOD and sampler contract while remapping previous coordinates
  and parameters. Separate previous descriptor provenance and runtime admission
  must be explicit. The first version refuses branch/phi-dependent slices and
  missing previous parameter banks; it must not reuse the current sample result
  merely because the texture itself was unchanged.

This requires coordination with `vk_texture.c` and its descriptor/destruction/
write paths outside the current owned files, plus the shader metadata owner if
the provenance contract needs extension. No such production hooks were edited
here. Previous GPU-storage copies, alternating object identities and independent
effect views are separate follow-ups, not substitutes for this texture producer.

A fresh opening-scene dump alone is unlikely to resolve the 1,275-key question:
the old opening capture is fully mapped and contains none. The minimal next
capture should identify one actual refused key in a field/grass/water or animated
material scene. Retain its exact VS input/bind-state record, SPIR-V/pass identity,
depth-test/write and blend policy, camera/parameter bank bindings, and only the
texture resources sampled by its position slice, including view/sampler state,
dimensions, upload/write generations and producer writes over two consecutive
frames. Existing `TOTK_DUMP`, `TOTK_DUMP_DIR`, `TOTK_DUMP_TEX` and stored shader
inputs cover much of this; `passes.txt` does not currently print all depth/blend
policy or a durable per-image content certificate. Those missing facts need
coordinator-owned bounded instrumentation rather than inference. Follow the
user's fullscreen/background0 requirement for that acceptance run. No game,
GPU, UI or device action was performed in this triage.

## Texture certificate implementation preflight: missing ordered write contract

The requested narrow texture admission has not been enabled. Auditing the actual
producer tracking found a concrete boundary that must be resolved before an
equality certificate can be trusted. An unchanged slot, image handle, upload
count and last-write fence do not establish unchanged sampled contents over the
required interval.

`vk_texture.c`, `vk_texture_storage_bound_locked` updates only `wr_fence`.
Two potential storage writes within one recording fence leave that value equal;
the function does not advance `gpu_seq` or `n_up`. `vk_texture_gpu_written`
advances `gpu_seq` only for mirrored GPU-owned entries, and updates the entry
selected for that descriptor rather than publishing a canonical interval epoch
for every physical alias. Pool-bound images therefore need an alias-aware
backing identity or must be excluded entirely. The current per-address slot
generation records resolution changes, not all content writes.

`vk_memory.c`, `vk_mem_changed` can report tracked CPU page writes and must be
queried over the whole source range, with untracked results refused. However,
`make_fresh_snap` explicitly permits CPU writes after a submission's write-watch
snapshot to wait until a later snapshot. Its cached `wgen`/`last_ch` pair is
therefore not a live no-write certificate. Direct checks also need their own
previous generation; unrelated consumers may reset the OS write-watch bits while
stamping the shared page generation table. A generation comparison must use
that table and refuse overflow/untracked/invalid ranges, not compare the last
debug result or an upload counter.

For buffer writes, `vk_exec_xfer.c`, `note_gpu_write` informs guest memory at
completion through `guest_written_cb`. It skips GPU-only addresses. Its
record-time `vk_mem_gpu_pending` helper is private shadow-sync bookkeeping and
is disabled when shadow mode is off; it is not an always-on content epoch query.
`vk_memory.c`, `gpu_writes` tracks a newest fence for freezing and is likewise
conditional on the freeze/shadow configuration. It cannot distinguish repeated
writes inside one fence or serve as a universal resource-content version.

There is also an ordering window beyond certificate comparison. Texture uploads
enter the shared upload batch. `vk_sync.c` calls `vk_upload_flush(0)` before
submitting a recorded part, so a later-recorded upload can execute before an
earlier-recorded motion sample in that part. A comparison in `mo_desc` or even
`vk_mvobj_execute` cannot alone seal an image against a later upload. An image
allocation hold prevents destruction, not this content mutation. A current
descriptor is safe to reuse as previous only if the version remains sealed
through the actual consuming GPU read, or if later writes use another image.

An ignored actual-function fixture at
`build/checks/rsf-texture-certificate-contract-worker/check.py` compiles the real
`vk_texture_storage_bound_locked` and `vk_mem_gpu_pending` functions with synthetic
state. It passes the counterexamples: two write-bind calls in fence 42 retain
the same fence and unchanged upload/GPU-sequence fields, while pending buffer
write registration vanishes with shadow mode disabled. Enabling shadow mode
records its private ranges, confirming that the omission is intentional mode
behavior, not a fixture failure. This is CPU/source evidence, not a GPU repro.

The blocking hook contract requested from the coordinator is:

1. An **always-on record-time content-write stamp** for a canonical backing
   identity and complete interval/subresource. Image target/storage/copy/clear,
   buffer fill/copy/inline update and shader SSBO writes must publish before the
   write is eligible to execute, irrespective of shadow/freeze mode. Every write
   advances a monotonic serial even inside the same fence. Unknown physical
   aliasing marks the range uncertifiable; it must not silently return clean.
   This touches `vk_memory.c` and the transfer/target recording owners outside
   this task, plus an internal header agreed with those owners; no `vk.h` edit
   is needed merely to describe that contract.
2. A **seal or ordering guarantee for the sampled image version** through its
   consuming submission. The texture owner can provide copy-on-write uploads
   while a certified image version is held, but direct target/storage writes
   and the upload-batch submission owner must honor the same guarantee. An
   alternative is a dedicated immutable image snapshot at the actual native
   read boundary, which is a different implementation scope from accepting
   current descriptors based only on equality. A late validation failure after
   motion has been recorded cannot simply mark it refused; the recorded motion
   must be rebuilt or that write must target another version.

Once these two facts exist, allocation/view/sampler identity, source-frame
history, ABA rejection, range checks and current-fence resource leases can be
implemented in the owned certificate module and tested with the requested
unchanged-pose/write/view/sampler/expiry fixtures. The first version should
exclude pool-bound aliases, mutable feedback resources, unknown write tracking
and branch/phi position slices. No synthetic model of those missing hooks will
be presented as proof that the native producers honor them. Production files,
shared translators/caches and GPU/game state remain unchanged in this preflight.

## Authorized snapshot alternative: design and unimplemented budget gate

The coordinator selected immutable GPU image snapshots at the actual native
read boundary instead of equality certificates. This changes the ordering
analysis: an upload recorded later on the CPU but submitted before a native
part executes before **both** an inline snapshot and its corresponding native
read. It need not invalidate a copy that observes exactly the contents the
native read observes. The old equality-only approach could not make that claim.
No snapshot coverage is implemented or enabled by this design note.

### Precise producer placement and command order

The producer belongs in `vk_exec_draw.c`, `op_draw_idx`, after `prepare_draw`
has resolved and bound the native shader/resources and index validation has
succeeded, immediately before the actual `vkCmdDrawIndexed`. This is earlier
than the current `mvobj_draw` collection after the draw. It must use the native
descriptor facts already resolved for that exact read, not re-resolve a slot
later and assume it still names the same image/view.

For an accepted read the same recorder-owned command buffer must contain:

1. End native rendering, preserving its attachment contents and restoring any
   needed layouts through the existing barrier machinery.
2. A source-write-to-copy-read dependency and transitions for preallocated
   private destination images; copy each required view subresource to its exact
   matching private image shape. No decoding, format relabeling or mip omission.
3. Dependencies completing the copies before the native vertex read and making
   private transfer writes visible to later previous-position samples.
4. Resume the same native attachments with LOAD, restore the exact native
   pipeline/descriptors/index/vertex/dynamic state as needed, then issue that
   native draw without an intervening upload resolution, split, producer callback
   or unrelated write to the sampled images.

`vk_rec.c` already packs `CopyImage`, `PipelineBarrier2`, `EndRendering`,
`BeginRendering` and `DrawIndexed` in recorder order. Direct Vulkan calls that
bypass this wrapper path are not acceptable. `vk_sync.c` flushes texture uploads
before submitting the part; therefore copy/read adjacency within one part is
the required unit, not CPU wall-clock adjacency or an image copied in a separate
upload command buffer. If the command buffer/part changes between the planned
copy and read, decline the snapshot rather than joining unrelated ordering.

`vk_exec_pass_begin` uses LOAD for colour/depth/stencil, but it also resolves
targets and calls contact/DLAA producer callbacks. It is not a pure resume
primitive. A minimal native-pass resume hook must either bypass those producer
callbacks using already resolved attachments or prove they are inert in this
specific path. Re-running `prepare_draw` blindly is also unsafe: a slot/view
change could make the native read use a different image than the one copied.
The existing FG end/resume fix is a pattern for recorder and cache ownership,
not permission to copy its assumptions to every geometry draw. The required
hook is an exact-native-state resume; there is no need for a new recorder packet
or a per-draw full pipeline-state copy.

### Metadata and ownership contract

The shader variant would expose a bounded list of position-sample bindings,
preserving opcode, type, flags, mip/LOD and sampler provenance. The first subset
must have no branch/phi in the position path and no image stores, feedback,
buffer textures, unsupported formats, ambiguous aliases or uncertain view
interpretation. A native read fact includes source frame/view, producer part,
image creation identity (not raw-handle identity), native descriptor/view,
format/aspect, exact mip/layer ranges, swizzles, sampler state and the stable
geometry/draw identity used to associate history. Missing previous parameter
banks or an ambiguous previous draw identity refuses the variant.

The private snapshot owns its allocation and views independently of the source
image. The native source needs only its normal current-submission lease once the
copy/read pair is recorded; no stale source-image pointer is kept as previous
history. Previous texture metadata points exclusively to a private immutable
snapshot associated with the previous pose. A sampler may be reused only while
its ownership and exact recorded state are valid; different sampler facts
cannot be silently labeled previous.

Pool slots progress through free, reserved, capture-recorded, producer-submitted,
history-owned, consumer-submitted and retired states. Publication requires the
actual producer submission result and a valid frame/view/draw association.
Failed or unsubmitted work keeps the slot unavailable for reuse. A prior
snapshot can be sampled only with the producer/consumer ordering proved on the
actual queue. Pool reuse requires release of CPU history plus completion of
the actual producer and every consuming final engine fence, including SDK split
continuations. The existing final-fence accessor and recorder submit ownership
are the relevant integration points; guessing the latest frame or using an SDK
ready signal as retirement is not sufficient. No host wait belongs in the draw.

Repeated draws get distinct captures unless a separate write seal proves reuse
safe. Merely seeing the same VkImage or sampler again is not such a seal.

### Why a useful initial subset was not implemented in this task

The 53 straight-line candidates require between one and six distinct sampled
bindings per draw: terrain has 15 one-binding and 12 two-binding variants;
grass has eight two-binding, three three-binding, three four-binding and twelve
six-binding variants. Those are shader binding counts, not runtime draw counts.
The fully mapped existing capture contains none of these exact keys, so it gives
no actual format/extent/mip shapes, repetition counts or valid draw-history
identity for a snapshot pool serving them.

A demand-created VkImage/view per encountered shape on the render thread fails
the explicit hot-path rule in `CONTRIBUTING.md` and
`docs/pr-review-kit/02-performance.md`: new GPU allocations can take 8-23 ms.
Even allocating a new VMA image from reserved memory still creates driver image
and view objects on that path. The one-per-present FG copy does not establish
the cost of restarting a solid-geometry pass up to once per candidate draw.
Reusing one capture per image would avoid work but violate the coordinator's
no-amortization condition without a proved write seal.

A viable implementation therefore first needs a fence-safe exact-shape backing
pool initialized away from draw recording, or an asynchronous allocator with
bounded requests and readiness publication. Unsupported or unready shapes must
refuse without waiting or allocation. A proposed diagnostic ceiling is 64 MiB
of actual Vulkan allocation requirements across all live/free pool slots,
4 MiB copied per source frame and eight copied resources per source frame;
these are proposed safety caps, **not measured sufficient budgets**. A draw
needing six bindings consumes six resource tokens, not one. The entire draw
must reserve its resources atomically before touching the native pass, and
budget exhaustion leaves that draw native without partial history publication.
Inflight exhaustion refuses rather than reuses a pending slot. The actual
copy, barrier and pass-restart cost still requires coordinator device timing.

No arbitrary fixed image sizes/formats were preallocated merely to show synthetic
admission, and no per-draw GPU allocation or unproven texture history was added.
The task is consequently **partial/failed as an implementation task**: concrete
architecture and source ordering evidence are delivered, but no production
snapshot module, shader admission patch, new ordering/retirement fixture or
changed-source compile is claimed. The earlier certificate counterexample
fixture remains valid evidence for rejecting equality-only admission; it is not
a test of this unimplemented GPU snapshot design.

The minimal next facts from the coordinator's outdoor capture are one exact
candidate key and native read, its one-to-six sampled view/sampler descriptions
(format, dimensions, mips/layers, swizzle), draw and part counts, and two-frame
geometry/parameter identity. Together with a device-initialization/background
pool hook and the exact-native-pass resume hook, those facts permit a useful
bounded shape subset without hot-path allocation. Fullscreen/background0 applies
to that later game acceptance. No GPU/game/UI, shared build/cache replacement,
staging or commit action occurred while preparing this design.


## Coordinator shader preparation and first combined run

The shared translator was rebuilt with patch 0017 after preserving its prior
executable/identity under ignored checks. Its new identity is `c6e1f527c908b3b5`.
Smoke tests passed. Preparing this game's shader seed and Vulkan driver cache
completed with zero translation or pipeline-build failures; existing recorded
bind-state files and the prior identity's cache entries were preserved.

The first combined fullscreen DLSS SR/FG/latency run using that identity reached
the torch/cave world, completed 1944 real presents and exited normally with zero
Vulkan validation errors, shader failures or skipped draws. The observed final
native image shows Link and Zelda walking down the cave stairs. SDK boundary
records at sources 1800 through 1802 contain a successful generated ordinal 0
and real ordinal -1 for each matching source and resource generation 1. They do
not contain generated pixel files, so they establish presentation identity and
actual generation, not generated-image quality. All captures remain ignored.

This is a first device admission for the combined path, not evidence that every
newly refused motion variant is visually harmless. Per-class previous storage,
textures, control flow, particles, translucency and volumes remain under audit;
retained arena memory peak and controlled moving before/after comparisons remain
required. The detailed refusal classifier is a separate follow-up.
