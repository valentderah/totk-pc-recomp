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
