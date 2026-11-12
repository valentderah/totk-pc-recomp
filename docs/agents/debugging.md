# Debugging the port: method for agents

A game run costs minutes and a full rebuild costs several more, while reading code costs seconds. Order the work so the
cheap steps decide as much as they can before the game starts. Each step ends on its stated condition.

## Steps

1. **Evidence.** Hold one artifact that shows the symptom: the player's screenshot, log or input recording, a
   flight-recorder dump, a frame dump ([running.md](running.md), "Observing"). Done when you can point at the wrong
   pixel, line or frame.
2. **Where it reproduces.** OS, GPU vendor and generation, driver, game update, settings, mods, the save. Done when you
   know whether your hardware can show the bug at all. NVIDIA bugs do not reproduce on AMD; RDNA1/2 bugs do not
   reproduce on RDNA3/4 (another driver branch); a Ryzen 7000 iGPU is RDNA2 and runs that branch. When your hardware
   cannot show it, say so and work from the code and the reporter's logs; runs that cannot decide anything are skipped.
3. **Known already?** Search [symptoms.md](symptoms.md), [pr-review-kit/04-knowledge.md](../pr-review-kit/04-knowledge.md)
   and [maxwell.md](../maxwell.md) sections 6-7 with the words the player used (black, magenta, flicker, slow motion,
   missing, stale, streaks, stuck) and the object (sky, grass, shadow, fog, HUD, weapon). Then the upstream notes:
   `refs/nxnative/docs`, `refs/MacNativeGames/docs` and the SavageWindRecomp issue tracker (`tools/fetch_refs.sh`
   fetches `refs/`). Done when every hit is either ruled out or is your first hypothesis. A hit on a class we already
   fixed: confirm that fix still holds before deriving anything new.
4. **Read the code.** The game's function in Ghidra ([building.md](../building.md), "Reverse engineering") or in
   `generated/<ver>/`, and our runtime path in `runtime/`. Done when the code either rules the hypothesis out or names
   the exact condition a run must show. A run confirms what the code could not rule out.
5. **The cheapest falsifying check.** An existing switch (a `totk.ini` setting or a `TOTK_*` variable), a dump, a trace
   line, a probe. When it refutes the hypothesis, stop: write down what was checked and why it fell, and start again
   at step 3 with the next hypothesis. No build, no benchmark for a refuted idea.
6. **Repro red, then green.** The same repro shows the bug before the fix and its absence after. A fix without a
   reproduced failure is a guess.
7. **Fix the cause.** A crash that goes away with a feature off points at the bug in that feature; fix the feature.
   A fix that one setup needs is gated on that setup, and everyone else keeps today's behaviour.

## Traps

- **One-off failures.** A failure that happens once and not again: rerun before you debug it as a code bug. A crash or
  a forced shutdown leaves zero-filled cache files, and memory errors on the host flip bits in files and objects.
- **Every build fails the same way at once.** Suspect the inputs and caches (game files, the OS file cache, shader and
  pipeline caches), not the code. Bisecting code is pointless when the old builds fail too.
- **Compile errors in generated C** on a line the generator could not have written are a flipped bit. Regenerate and
  compare hashes; `recomp/` is fine.
- **A crash that looks like a race.** Try the save first: some saves crash the same way on a real Switch.
- **Theories about locks and threads.** Read the game's own protocol in Ghidra before running anything. Ten runs on
  lock theories once went where one read of the game's double-buffer code would have narrowed it.
- **Numbers from a fast machine.** A change neutral on a fast desktop can matter on a weak laptop or the Steam Deck,
  in either direction. Say which hardware your numbers come from.

## Bug classes of a recompiled Switch game

The port runs the game's own code on another OS and another GPU. Most bugs come from places where the host differs
from the console.

### Variable frame step and epsilon-gated timers

The console steps the game by whole 30 Hz units, so a countdown started at a whole value reaches exactly 0. With the
real-time step (`realtime = 1`, the default) the step is a measured ratio, and the last step can leave a remainder of
about 1e-8. Many of the game's updaters step a timer only while it is above `FLT_EPSILON`; a remainder in
(0, `FLT_EPSILON`] is never stepped again. A consumer that tests the same field with `> 0` then sees it active
forever, until a load reinitialises the object. Issue #18 (weapons locked after a Fuse) was this.

Symptom words: stuck until a reload, a teleport or a shrine; locked after an action; works sometimes; only above
30 fps. To check a report: find the field the stuck behaviour reads, then compare the updater's threshold with the
consumer's test. A change to the frame step, the pacer or the fps limit is checked for this class.

### Thread scheduling: Horizon is not a desktop OS

- Horizon runs a woken better-priority thread at once on its core, before the thread that woke it goes on. A host
  runs both in parallel, so state can be read one step early or freed while another thread still uses it.
- Threads created with ideal core -2 start pinned to one core on Horizon. The game keys per-core data by
  `GetCurrentProcessorNumber` without locks and expects that pinning.
- The game has hundreds of spin loops without a system call. On the console a spinning thread holds one core of three;
  on the host it burns a host core and can keep an equal-priority waiter out.
- ARMv8 `stlr` followed by `ldar` is ordered (RCsc). C11 release/acquire lets x86 reorder that store and load. A
  two-thread flag handshake that loses a wake points here; lifter changes to barriers and atomics pass
  `tests\diff\atomic.py` and `tests\excl_mt.py`.
- A scheduling change is judged by how long a ready, better thread waits for a core it may use, plus the slow-frame
  count. A check that made one bug rarer once added 50-75 ms hitches.

### Maxwell behaviour versus Vulkan defaults

The port must do what the Switch GPU does: last-vertex provoking, (0,0,0,1) for unwritten vertex outputs, colour
targets outside the shader's output map keep their texels, the G-buffer pass samples its own targets. The full list
with sources: [maxwell.md](../maxwell.md), section 7.

### Memory shared by the CPU and the GPU

- The game fills textures and buffers with the CPU and samples them on the GPU without the flush a desktop API needs.
  Missing uploads show as magenta, brown or stale textures.
- The game reads GPU-rendered textures with the CPU (grass maps, cut and burn masks). Missing write-backs show as
  effects that never happen: cut grass stays up, fire does not spread.
- Memory only the GPU writes looks idle to write-watch tracking: a store from a shader leaves no fault and no dirty bit.
- Anything kept per texture, per frame or per object without a free grows host memory until a commit failure after
  hours. Watch private memory over a few idle minutes.

### Device lost and GPU hangs

- The `vk: pass marks` lines name the pass in flight. Validation names the misuse when there is one.
- Submits and presents share one queue under one lock. A present from its own thread on a second queue gave device
  lost on NVIDIA before Turing in another port.
- Caches from another driver or GPU (pipeline caches, translated shaders) gave black windows after driver updates;
  every cache is validated on load and dropped when vendor, device or `pipelineCacheUUID` change.
- A first-time shader stall over 2 s on a slow GPU trips the Windows GPU timeout (display event 4101). Large one-shot
  submissions (a boot-time decode of every texture) do the same.
- Point lists from vertex shaders that write no point size are undefined without `VK_KHR_maintenance5`.

### Windows and Wine host costs

What the hot path must not do, with measured costs: [pr-review-kit/02-performance.md](../pr-review-kit/02-performance.md),
"What the hot path must not do". For debugging, also:

- `SuspendThread` returns at once, but `GetThreadContext` waits until the thread leaves the kernel. A sampler or a
  debugger stalls for as long as the stopped thread's file open or driver call runs.
- A thread stopped between its `sub rsp` and its stack probe has its stack pointer inside the guard page. Check
  `PAGE_GUARD` with `VirtualQuery` before reading another thread's stack.
- Paths go through the W APIs with UTF-8 to UTF-16 conversion. An A API call breaks on a path with non-Latin letters,
  such as a Windows user name in Cyrillic or Chinese.
- Wine emulates `GetWriteWatch` with `mprotect` and one fault per first write. Anything relying on cheap OS memory
  tricks (write-watch, protection changes, fault handlers) costs more under Proton.
- `win32u.dll` frames in a host stack are kernel graphics calls: memory allocation or submission.
