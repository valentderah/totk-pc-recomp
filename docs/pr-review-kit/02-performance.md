# Performance: no lost frames on weak hardware

## Who we tune for

- The goal is many more fps on modest hardware, and Steam Deck later (Zen 2 4c/8t APU, shared LPDDR5, the exe runs
  through Proton/Wine).
- The developer's machine (Ryzen 9 7900X + RX 9070) is fast and its numbers do not carry over. A laptop with an
  i7-9750H and a GTX 1050 Ti is a sample of a weak machine, not the target to fit constants to.
- What weak hardware measured:
  - GTX 1050 Ti (4 GiB, 214 MiB BAR, no ReBAR): the GPU read imported host pools over PCIe, 110-120 ms a frame
    against 4.3 ms on the RX 9070. The VRAM mirror brought it to 10-11 ms; that machine is now CPU-bound at 25-45 fps,
    replay 25-30 ms a frame.
  - The same write-watch scan costs about 3x the CPU time there.
  - NVIDIA and desktop AMD GPUs have no ASTC: textures decode in a compute shader. CPU decodes before that made
    0.4-0.7 s load frames on the 1050 Ti.
- Wine emulates `GetWriteWatch` with `mprotect` and a fault per first write. Anything relying on cheap OS memory
  tricks (write-watch, `VirtualProtect` freezing, fault handlers) is checked against Wine's implementation.
- The port uses about half of even a weak CPU, so the frame's critical path is the limit, not throughput. TotK keys
  per-core data by `GetCurrentProcessorNumber` without locks, so the game's threads share three virtual cores on any
  host. In a fight the main thread and five job workers keep 2.6 of the 3 busy: guest code speed sets the fps.
- Perf work builds and measures 1.4.3. A 1.2.1 build is for a change about to merge with a large gain.

## Gain ceiling first

Before writing or taking a perf change, compute its ceiling: the share of the code it removes times the share of the
frame that code holds on the critical path, on weak hardware. A ceiling of a few percent or less is not worth it.

- Use the critical path (`TOTK_CRIT` + `tools/crit_path.py`, heavy frames only), not one thread's profile. On the weak
  laptop the submit thread is 55% replay, yet the critical path of heavy frames is 67% guest code, 15%
  `nvnSyncWait`, 13% core waits and wake latency.
- Changes that can move the frame by 10% or more are usually design changes. Polishing a hot function from a flat
  profile almost always gives zero.

## A neutral safe change stays

An optimization that measures neutral on the fast machine stays on by default when:

- upstream (`refs/`, other ports) reports a gain for it, especially on weak CPUs or GPUs;
- the static read leaves no hazard, and the probe or differential test passes;
- whole heavy-scene runs off/on x2 show no fps loss and no new slow frames;
- a switch turns it off.

"No gain here" is not a reason to drop a safe change with upstream evidence. A regression or a hazard nobody ruled
out is.

## How to measure

- **By game time, never by frame counts.** Run length, analysis windows, A/B blocks and input scripts go by the world
  clock (`--seconds`, `TOTK_CRIT=w<lo>-<hi>`, `TOTK_PROFILE_FRAMES=w<lo>-<hi>`, `TOTK_SHOTS=w<lo>-<hi>:<step>`). The
  same frame count at another fps is another scene.
- **Check a scripted run reached the world.** Title menus on a slow boot eat button presses; confirm the run is in the
  world before trusting its numbers.
- **Whole runs off and on, two each at least.** An in-run A/B on a moving scene lies: the A and B blocks land on the
  same game seconds every run, so a content difference repeats. One in-run A/B showed "-11% replay"; whole runs gave
  5.84/5.73 against 5.76/5.73 ms, nothing. In-run A/B only on a static scene (standing still).
- **Spread.** One script on near-identical builds gave 1% lows from 31.5 to 50 fps. Compare medians of three runs,
  never one run against one run.
- **A quiet machine.** `tools/run_game.py --quiet` takes the machine-wide lock; no other `totk.exe`, build or test runs
  meanwhile. One game at a time.
- **A warm shader cache, prepared properly.** Shader and pipeline caches are not copied between build dirs:
  `totk.exe --prepare-shaders` or `--warm-shaders` in that dir. The AMD driver cache is keyed to the exe path and
  limited in size: preparing another build dir can evict this one's libraries (40-220 ms per new effect in play).
- **A heavy scene.** A fight with a large enemy and a crowd, fps limit at the maximum (`TOTK_FPS_LIMIT=360`). Compare
  one shot with the scene's reference before trusting numbers.
- **No base runs by habit.** Run the candidate alone and judge it against the targets: max fps, zero slow frames
  (`perf: slow frame`), no black or flat frames, no new validation errors. Glitches are found by looking at shots and
  frame dumps, not by diffing metrics. Run a base build only when a specific question needs a side-by-side.

| What | How |
|---|---|
| fps, worst frame, render-thread buckets, GPU busy, VRAM | `--env TOTK_TRACE=perf,sched`, then `python tools/perf_summary.py <log> <lo> <hi>` |
| the frame's critical path | `TOTK_CRIT=w<lo>-<hi>` + `TOTK_CRIT_OUT=<path>`, then `python tools/crit_path.py <file>` |
| sampling profiler | `TOTK_PROFILE=<path>` + `TOTK_PROFILE_FRAMES=w<lo>-<hi>`, then `tools/prof_report.py <profile> <totk.map>` |
| A/B on a static scene | `TOTK_AB=<seconds>`, then `tools/ab_summary.py --skip <seconds>` |
| VRAM pressure on a large card | `TOTK_VRAM_LIMIT_MIB=<MiB>` caps the budget the port sees |

## What the hot path must not do (Windows host)

- **Files.** The game threads (main, job workers) and the render thread never open, read, write or flush a file, and
  never wait for a thread that does. Opening a 20-byte file in a 139k-file folder: 0.1 ms idle, 11-40 ms in play while
  the translator, the pipeline cache and the save wrote to the same volume. A save's `FlushFileBuffers` and
  `MOVEFILE_WRITE_THROUGH`: 93-135 ms. Saving the pipeline cache under its lock on the present thread froze the replay
  every 20 s. File I/O goes to a background thread; results come back in memory.
- **Waits.** Short waits spin on the counter; waits over 1 ms use the high-resolution timer. A waitable timer wakes
  0.3-0.5 ms late; the SDK's 20 us sleeps took 460-515 us. A 1 ms timer thread does not help: it gave more slow frames.
- **GPU allocations.** VMA's `vkAllocateMemory` of a 256 MiB block under its lock: 8-23 ms. A new block on the render
  thread is a hitch.
- **128-bit division.** `(unsigned __int128)t * num / den` compiles to the library call `__udivti3`; counter
  conversions that way took a tenth of the game threads' time in a fight. Use the precomputed scale (`platform.h`
  `PlatScale`).
- **Per-draw copies and counters.** A pipeline-state copy per draw, a global atomic counter, a scan of every page each
  frame: on a weak CPU each is frames.
- **Memory kept per frame.** Anything allocated per texture initialization or per frame must be freed. One copy kept
  per texture init grew host memory by ~44 MiB a game minute until a commit failure after hours (#670). Check private
  memory over a few idle minutes for a PR that stores per-object state.
- **Scheduling.** Any timing or scheduling change is checked for slow frames. A preempt check at every host call made
  one bug rarer but added 50-75 ms CPU hitches and was reverted.
