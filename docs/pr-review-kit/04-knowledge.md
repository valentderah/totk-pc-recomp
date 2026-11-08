# Knowledge base

Before investigating a symptom, or trusting a PR that "fixes" it, look for it here. Search by what the player sees
(black, magenta, flicker, slow motion, missing, stale, streaks) and by the object (sky, grass, shadow, fog, HUD).

## Invariants of recent changes

A PR that touches these areas must keep them.

- **Queue order.** When anything is submitted, every earlier part is already on the queue. Present, uploads, readbacks
  from guest threads and the `vkDeviceWaitIdle` sites rely on it. The recorder thread now begins, ends and submits
  the parts it records; the submitting thread still waits for that submission, so the order holds.
  `TOTK_REC_END=0` restores the old split.
- **GPU-written memory is never frozen.** Storage-buffer stores, copies and fills land in imported guest memory
  without a fault or a write-watch bit, so a region only the GPU writes looks idle. Such regions are never frozen
  read-only, and a frozen region is thawed before a GPU write. LAS Recomp never changes the protection of imported
  memory because of NVIDIA. A suspected cause of NVIDIA "write invalid" device faults (#788, #802), not proven.
- **VRAM pressure.** Under pressure, released textures whose storage the game has given to another texture go first:
  30 s after release and once no finalized slot has bound them for 600 submits. Clean mirrored entries wait 5 s.
  Both stop once use is back under the line. A PR touching texture lifetime is tested with `TOTK_VRAM_LIMIT_MIB`.
- **Texture pins are freed.** Each pin counts the recording copies naming it and is freed 1200 submits after its last
  use unless the replay holds it as a render target. Before that, pins leaked ~44 MiB a game minute (#670).

## Fixed bug classes

If a PR touches this code, confirm the fix still holds.

| Symptom | Cause | Where the fix is |
|---|---|---|
| lime silhouettes in grass; rectangles in the sky | a target outside the output map must keep its texel; zero-filling it wiped the half-res depth | `vk_pipeline.c` `target_wmask` |
| stale grass maps in the far terrain for a second after a hit | write-back of a texture the CPU had rewritten | `vk_texture.c` `write_back` |
| cut grass stays standing; fire does not spread (#132) | CPU reads of GPU-rendered textures without an invalidate | `vk_texture.c` `vk_texture_write_back_small`, `nvn_ext.c` `nvn_pool_invalidate` |
| cloud and fog volumes empty for the session | a one-shot draw skipped instead of waiting for its pipeline | `vk_exec_draw.c` `prepare_draw` |
| tile rectangles | storage buffers bound by Ryujinx first-use order, not the c0 slot | `vk_exec_draw.c` `ssbo_slot` |
| black dialog background and loading map | 3D targets rejected, the grading LUT never written | `vk_exec.c` `resolve_target` |
| streaks on cliffs | primitive restart ignored | `vk_exec_draw.c` `restart_for` |
| streaky map screen | the newest texture picked at shared storage | `vk_texture.c` `vk_texture_by_id` |
| dark 16x16 blocks on the ground, sage spirit not drawn | G-buffer feedback loop | `vk_exec_draw.c`, `TOTK_FEEDBACK` |
| magenta cave walls, brown sky | CPU-filled textures without a flush | write-watched mirrors, `vk_memory.c`, `vk_texture.c` |
| black 3D scene | deferred texture binds | `nvn_api_cmd.c` `Bind*Deferred` |
| HUD prompt boxes, black stamina wheel | `nvnTextureCompare` ignored the channel mapping | `nvn_api_obj.c` |
| crash on world load | `GetCommandMemoryUsed` did not follow the driver's sizes; the game sizes model heaps from it | `nvn_api_cmd.c` |
| 20 fps | a yield back-off starting before 2 ms | `kernel.c` SVC 0x0B: no back-off |
| weapons locked after a Fuse (#18) | timer remainder in (0, FLT_EPSILON] with the real-time step | patch under the `realtime` setting |
| magenta/blue garbage or device lost on RDNA1/2 (#5) | writes into 3D BC block views | in `dev` |
| black window after a driver update | a pipeline cache from another driver | `vk_pipeline.c` drops the cache by UUID |

## Known open risks

A PR touching these needs stronger proof.

- Buffer textures are not replayed and bind a stand-in: light and decal lists may read 0.
- New GPU-only colour images start undefined; feedback effects may show garbage.
- Clears with a partial channel or stencil mask are skipped (`W_CLEAR_MASK`, `W_STENCIL_MASK`).
- 8-bit indices are widened on the CPU at record time; a copy in the same submission gives stale indices.
- `UpdateUniformBuffer` is a copy between full barriers and ends the render pass (fps outdoors).
- Report timestamps carry host time at fence completion: the engine's GPU time is wrong.
- The game's SVC-free spin loops burn a host core (843 in `main`).
- Images aliased onto one GPU-only pool allocation do not see each other's data on AMD.

## Past PR decisions

| PR | Decision | Why |
|---|---|---|
| #690 shrine snapshots | rejected | a pass restart per copy, a `VkPipeState` copy per draw, broken `bind_sig` skip; fps loss |
| #674 | rejected | a global STXR counter on the hot path |
| #660 | rejected | the preview loses anti-aliasing |
| #713, #576 | rejected | low memory block 10 GiB for everyone; should be 6 GiB plus the extra where needed |
| #573 | rejected | dynamic resolution off by default for everyone, the 1.4.3 site never run |
| #568, #569 | rejected | fake FSR4 and frame generation |
| #487 | rejected | wrong metrics: percentiles over window means, counters over the wrong set |
| #451, #380 | rejected | not painless to take |
| #566, #594, #534 DLSS | taken with changes | #594's shared paths gated on DLSS being on; nothing changes without DLSS |
| #489 | partly taken | the runtime part checked by a frame dump; the installer's DLL download was the owner's call, its pinned hash re-checked |
| #519, #582 amiibo | rewritten by us | took the data-format fixes; left out the features and the Linux event rework |
| #483 vs #585 | #585's mapping kept | #483's stick glyph mapping was rewritten unasked while #585 already had it right |
| #528 rain | taken after a check | code review fine, merged only after a run on a rain save |
| #767 | taken with our fix | the default was skipped with `prompts = xbox|pc` when those folders did not exist |
| #625 frame queue | taken, off by default | a new frame queue, one field run with it on |
| #743 UB overlay | waiting | +25% in a Depths fight on an RTX 4080; needs review, fight runs x2 and shots on AMD |

## Our mistakes, not to repeat

- **"-11%" was an A/B artifact** of a moving scene. Whole runs off/on showed zero.
- **Micro-optimizations from a flat profile** (1-3% of samples each) gave 26.2 against 26.4 ms on the weak laptop:
  nothing. Nobody had computed a ceiling.
- **A scheduler patch merged on run statistics** added 50-75 ms hitches. Slow frames are a hard bar.
- **Ten game runs on lock theories** where one Ghidra read of the game's code narrowed it at once.
- **45 minutes of builds and benches** on an idea its own first thread census had refuted: the game pins 53 of 56
  threads itself. The cheapest refuting check goes first; when it refutes, stop and write the idea down.
- **Tests of a bug that could not appear on the hardware at hand** (NVIDIA paging on an AMD machine): 245k tokens and
  37 minutes for nothing. Ask first where the bug reproduces.
- **Turning the recorder off for a vendor** instead of finding its bug. Stopped: the feature gets fixed, not silenced.
- **Bisecting code when every build crashed the same way at once.** The cause was a bad page of a game file in the OS
  file cache. When all builds fail identically, suspect inputs and caches first.
- **A duplicate issue closed with a bare "Same as #N"** lost the logs and links. Move everything useful to the issue
  that stays open first: hardware, driver, version, when it happens, workarounds, links from comments.
