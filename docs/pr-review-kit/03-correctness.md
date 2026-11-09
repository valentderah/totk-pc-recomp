# Correctness: checks that keep behaviour intact

## Lifter and generated C

- Generated C is never edited by hand: change the generator in `recomp/`.
- `recomp/layout.py` alone decides module bases.
- Every change to the lifter or the CPU headers (`runtime/cpu.h`, `runtime/simd.h`) passes the differential tests
  against Unicorn, all run with `.venv`:

| Test | Checks |
|---|---|
| `tests\diff\lift.py` | instructions sampled from the modules; `--coverage` lists the words the lifter rejects |
| `tests\diff\simd.py` | the whole FP/SIMD encoding space |
| `tests\diff\atomic.py` | exclusive load/store and ARMv8.1 atomics |
| `tests\diff\func.py` | whole functions as the generator emits them: loops, jump tables, calls |
| `tests\flags_check.py` | the function-local flag helpers against the packed ones |
| `tests\excl_mt.py` | the exclusive monitor under contention and ABA, on several threads |
| `tests\jumptables_check.py` | `recomp/jumptables.py` against Unicorn: 0 disagreements on 1.2.1 and 1.4.3 |

- A `--cpu v2` build (no AVX2) passes the same tests with `--cflags=-march=x86-64-v2`. Players without AVX exist.
- A compile error in generated C on a line the generator could not have written is a bit flip on disk or in memory.
  Regenerate and compare hashes; leave `recomp/` alone.

## Runtime

- From the build dir: `totk.exe --selftest`, `--probe-kernel`, `--probe-vkexec`, `--probe-nvn-tex`, `--probe-shader`
  pass.
- Settings: `totk.exe --probe-cfg` and `python tests\settings_doc_check.py`.
- Button prompts: `.venv-py314\Scripts\python tests\romfs_prompts_test.py`. Build dirs carry only `romfs-patches`; to
  test PC or Xbox prompts in game, generate the overlay first:
  `.venv-py314\Scripts\python tools\make_romfs_patches.py --version <ver> --prompts pc --out build\<dir>\romfs-patches-pc`,
  then restart the game.
- Installer: `tests\installer_gui_test.py` (event model, form, catalogs, cancel, display scale).
- Shaders: `tools\shader_census.py --version <ver> --translate`, `tools\xlate_smoke.py`. A translator patch
  (`tools/xlate/patches/`) needs `build/xlate` rebuilt and changes shader hashes: anything keyed to a shader hash
  breaks with it.

## GPU portability

- New Vulkan code queries the feature, extension, format or limit it uses and keeps a working path or a logged
  fallback without it (provoking vertex and subgroup size pinning in `vk_pipeline.c` are examples).
- Never hardcode what one GPU reports: alignments, subgroup size (RDNA4 may run wave64, old AMD Vega is wave64 only),
  depth formats, `maxPushDescriptors`.
- One queue: every submit and present takes `vk_queue_lock`. Presenting from its own thread on a second queue gave
  device lost on GTX 10-series in another port.
- RDNA1/2 bugs do not reproduce on RDNA4 (another driver branch); NVIDIA bugs do not reproduce on AMD. A Ryzen 7000
  iGPU is RDNA2 and runs the RDNA1/2 driver branch: a cheap testbed for those reports.
- Validation: a `--validation` run in the world shows no new messages.

## Maxwell / NVN behaviour

The port must do what the Switch GPU does, not what Vulkan does by default. The full list with sources:
[docs/maxwell.md](../../docs/maxwell.md), section 7. The traps a PR breaks most easily:

- Provoking vertex: Vulkan takes the first vertex, NVN the last.
- Unwritten vertex outputs read (0, 0, 0, 1).
- A colour target outside the fragment shader's output map keeps its texel (write mask 0). AMD writes garbage there.
- Feedback loops: the G-buffer pass reads its own targets. It needs the feedback layout and a by-region barrier.
- Depth bias argument order, depth mode, depth clamp: checked against deko3d.
- Report timestamps: the engine's GPU time comes from them, and dynamic resolution and game speed follow it.

## Timers and the real-time step

- With `realtime` set, the game steps by a measured ratio, not whole 30 Hz units. A countdown remainder in
  (0, FLT_EPSILON] is never stepped again, and a consumer testing `> 0` sees it active forever. That was "weapons
  cannot be changed after a Fuse until a reload" (#18).
- Symptoms of this class: stuck until a reload or teleport, locked after an action, works sometimes.
- A PR touching the frame step, the pacer or the fps limit is checked for this class.

## Threads and scheduling

- ARMv8 `stlr` then `ldar` is ordered (RCsc); C11 release/acquire lets x86 reorder that store and load. Changes to
  the lifter's barriers or atomics pass `tests\diff\atomic.py` and `tests\excl_mt.py`.
- A scheduler change is judged from one `TOTK_TRACE=sched` run by how long a ready, better thread waits for a core it
  may use, plus the slow-frame count. Not by statistics over many runs.
- A crash that looks like a race: rule out the save first. One reported crash was the save itself; it crashed the
  same way on a real Switch.

## Files and caches

- A crash leaves zero-filled files. Caches are validated on load, not trusted because they exist.
- The pipeline cache is dropped when vendor, device or `pipelineCacheUUID` change.
- Unicode paths: W APIs with UTF-8 to UTF-16 conversion.
- A failure that happens once and does not come back: rerun before debugging it as a code bug. Broken caches and bit
  flips happen.

## Game content boundary

- The repository is public. No game files, and nothing extracted or generated outside `build/` and `generated/`.
- Keys never appear in logs, commands, files, issues or comments.
- Every file the generator emits carries the `GENERATED_MARKER` defined in `tools/check_content.py`.
- `python tools/check_content.py --staged` before every commit. CI runs the same check.
