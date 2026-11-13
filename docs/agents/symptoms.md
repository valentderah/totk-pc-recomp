# Symptoms and their usual causes

What a player reports, the causes this port and other Switch ports (nxnative, MacNativeGames, SavageWindRecomp,
LAS Recomp) found behind it, and the first check. Search by the player's words and by the object. Which of these are
already fixed here, and where: [pr-review-kit/04-knowledge.md](../pr-review-kit/04-knowledge.md), "Fixed bug classes".
GPU semantics behind the graphics rows: [maxwell.md](../maxwell.md). The method around this table:
[debugging.md](debugging.md).

## Graphics

| Symptom | Usual causes | First check |
|---|---|---|
| magenta, brown or stale textures; caves or sky wrong | the CPU filled the texture and the GPU never got the upload (no flush on the console) | frame dump: the texture's upload count and mirror state in `passes.txt` |
| effects that never happen: cut grass stays up, fire does not spread | the CPU reads a GPU-rendered texture that was never written back | which texture the game reads after its fence; write-back of that target |
| black scene, black dialog background or loading map | deferred binds read at record time; a 3D target rejected; a texture compare ignoring the channel mapping | frame dump: the first pass whose target goes black |
| lime silhouettes, rectangles in the sky | a target outside the shader's output map overwritten instead of kept | dump: the target's previous contents versus after the pass |
| dark or magenta 16x16 blocks, missing characters | a pass samples its own render targets (G-buffer feedback) | feedback-loop layout and barriers on that pass |
| tile rectangles, wrong data in tiles | storage buffers bound in the wrong slot order | `passes.txt`: storage buffers with their c0 slots |
| streaks on cliffs, characters stretched into streaks | primitive restart ignored; integer vertex inputs against a float format | validation run; the draw's topology and vertex formats in the dump |
| garbage geometry on title or load screens | 8-bit indices widened on the CPU before a copy in the same submission wrote them | whether a copy to the index buffer precedes the draw in one submission |
| black tiles, masks not cleared | clears with a partial channel or stencil mask skipped or done in full | `passes.txt` clears and their masks |
| garbage in feedback effects at first use | new GPU-only images start undefined; the console starts them at zero | clear on creation and compare |
| empty clouds or fog for the whole session | a one-shot draw skipped while its pipeline built, never drawn again | `perf: slow frame` lines naming pipeline waits; the draw's first frame |
| fog that never parts, flicker in tile rows | driver constants (c0: dispatch size, point size, base instance) not filled | the c0 contents the translator expects for that stage |
| no sun shadows, wrong lighting | unwritten vertex outputs read as zero instead of (0,0,0,1) | the shader's output map |
| one flat ambient colour | a 1-layer 2D array sampled as 2D | the view type the shader expects |
| upside-down text | lower-left window origin | origin mode in the log |
| flicker or a texture swapped with a similar one | the newest texture picked at shared storage instead of the one with the matching view | texture lookup by id in the dump |

## Timing and frame pacing

| Symptom | Usual causes | First check |
|---|---|---|
| stuck until a reload or teleport, locked after an action | a timer remainder below `FLT_EPSILON` with the real-time step ([debugging.md](debugging.md)) | `realtime = 0` in `totk.ini`; if the bug is gone, find the timer field |
| slow motion in GPU-heavy scenes, dynamic resolution never drops | the engine's GPU time comes from report timestamps; wrong timestamps make the frame look free | what the engine reads as GPU time and what the report writes |
| frame counter wanders 50-60 | two 60 Hz clocks (vsync event and present acquire) drifting apart | whether both follow one display tick |
| a 30 Hz thread runs at 27 Hz, slow cutscenes | host sleep overshoot: Windows waitable timers wake 0.3-0.5 ms late | `TOTK_TRACE=sched` sleep asked versus took |
| 20 fps after a scheduler change | a yield back-off that starts too early | yield handling against the console's |
| climbing or movement wrong only above some fps | game code that assumes a fixed step count per frame | compare at 30, 60 and the failing rate |

## Threads and crashes

| Symptom | Usual causes | First check |
|---|---|---|
| state read one step early, one frame of inconsistent data | a woken better-priority thread does not run before the waker goes on | sched trace around the handoff |
| per-core data corrupted, rare crash in a container walk | threads the console pins to one core run on any host core | the thread's core mask from its creator; a run on one core |
| rare hang or lost wake | address-arbiter signal values or wake order differ from Horizon | the arbiter path against mesosphere |
| a flag handshake loses a wake | `stlr`/`ldar` ordering lost in x86 release/acquire | the guest code's barrier pattern |
| a host core at 100% while the game waits | game spin loops without system calls | profile: which guest function spins |
| crash on world load | sizes the game reads from the driver (command memory used) differ from the console's | the sizes the game asks for against the console's |
| crash with one save only | the save itself; some crash on a real Switch | the same save on another build and, if possible, on hardware |
| random crash after minutes to hours, access violation reading `0xffffffffffffffff` or a small address such as `0x90` in a game function (Windows reports any non-canonical address as `0xffffffffffffffff`) | an invalid value (-1, `0xffff0000ffffffff`) in an optional reference the game checks only for null; in main+0x1bd0138 (Recall / Ultrahand picking a target) also a reused holder whose actor points into other memory. Sets `optref` and `ability` in `recomp/patches-v143.txt` guard both there | the crash dump's faulting host instruction, the register it loads through and the field that register came from; `ability: target dropped` lines in the log |
| crash while using Ultrahand or aiming, in the model scene's report loop (1.4.3 main+0xa22bc..0xa234c) | a report row index read from the report buffer with no bound (Weeping of the Empire #52, #73); set `report` sends a negative handle or an index from 0x8000 to the game's own reset | the faulting address against the report rows base (x12) |

## Device lost, hangs, black window

| Symptom | Usual causes | First check |
|---|---|---|
| device lost a few frames in, GPU writes to address 0 (older NVIDIA) | present from another thread or queue | every submit and present on one queue under one lock |
| device lost or garbage on one AMD generation | views the driver branch mishandles (BC block views of 3D textures) | `vk: pass marks`, validation; the formats in the pass |
| device lost on one scene, every run, one vendor | a shader or pass that driver cannot run | pass marks name the pass; skip shaders by hash to bisect |
| GPU timeout after 1-2 min, single frames over 2 s | first-time shader stalls or a huge one-shot submission on a slow GPU | prepared shaders; submission sizes at boot |
| GPU hang at boot with pipeline libraries | point lists without a point size | the shader writes point size or the device clamps it |
| black window after a driver update | caches from another driver | the log line `vk_pipeline: ... written by another device or driver`; delete the caches next to `totk.exe` |

## Memory

| Symptom | Usual causes | First check |
|---|---|---|
| host memory grows in play until a commit failure after hours | state kept per texture init or per frame and never freed | private memory over a few idle minutes |
| multi-GB peaks while travelling | upload staging grows inside one submission | staging bytes per submission |
| out of memory after minutes | dead textures re-entering tables | live images and allocator bytes over time |
| stutter as VRAM fills | new memory blocks allocated on the render thread (8-23 ms each) | profile the render thread over the hitch: `vkAllocateMemory` under the allocator's lock |
