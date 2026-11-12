# Running and observing the game as an agent

How an agent runs a build into gameplay without a person at the controls, captures what the game shows, and reads the
results without flooding its context. Setting names and file formats live in [settings.md](../settings.md) and
[controls.md](../controls.md); this page says which to use when. How to judge a performance change:
[pr-review-kit/02-performance.md](../pr-review-kit/02-performance.md).

## Before the first run

- **A build dir per change.** Build into its own folder (`tools/build_game.py --out build/<ver>-<name>`) so a
  comparison has two exes side by side. After every build check the exe's modification time: a failed link leaves the
  old `totk.exe` in place and you test old code.
- **Prepared shaders in that folder.** `tools/prepare_shaders.py build/<dir> --version <ver>` ([building.md](../building.md)
  step 6). Without it the game compiles shaders during play, and every timing and every "a draw is missing" shot of the
  first minutes says nothing about your change. Shader and pipeline caches are not copied between build folders.
- **The same `totk.ini` on both sides** of any comparison. A folder without one runs at the defaults; check the log's
  `resolution:` line of each run before comparing them.
- **Saves.** `tools/run_game.py` copies the profile `build/profile`, swaps in a save from `build/save-backup/<name>/`
  and deletes the copy afterwards, so the real profile is never written. To make a backup, copy the user folder from
  `build/profile/save/` into `build/save-backup/<name>/`. Pick a save that starts where the change acts.

## Running

```powershell
.venv\Scripts\python tools\run_game.py build\<dir> --save <name> --seconds 30 [--script <input.txt>] [--shot out.png] [--env KEY=VALUE] [--quiet] [--tag <name>]
```

- `--seconds` is game time since the world loaded (the log line `frame step: world clock starts` marks the start), at
  any frame rate. Measure and script by game time; a frame count at another fps is another scene.
- `--script` replays input ([controls.md](../controls.md), "Scripted input"). A new script starts with the presses of
  `tools/input/load_save.txt`, which load the first save from the title. `TOTK_INPUT_RECORD` turns a person's play into
  a script; ask the user to record the repro rather than guessing the button sequence.
- `--shot` saves the last frame and builds pipelines synchronously: use it for pictures, never for timing.
- `--quiet` waits until no other `totk.exe` or `ninja.exe` runs. Every timing you report comes from a `--quiet` run.
- The log goes to `<dir>\logs\<tag>.log`; the tool prints `<log>: rc=<code>`.

Run one game at a time: two instances share the GPU's memory and the CPU, and both runs' numbers are void.

Running `totk.exe` yourself: set `TOTK_ASSUME_FOCUS=1` (an unfocused window otherwise pauses the game),
`TOTK_INPUT=none` (the user's keyboard and pad stay out) and `TOTK_BACKGROUND_WINDOW=1`. Stop only the process you
started, by PID: `taskkill /IM totk.exe` also closes the user's game.

**Check that the run reached the world** before trusting it: the world clock line is in the log and the shot shows the
scene you expected. Title menus on a slow boot eat presses, and runs of one script drift apart after 10-15 game seconds
(enemy AI, physics, weather). Two runs are comparable only when shots at the same game second show the same scene.

## Observing

| You need | Use |
|---|---|
| pictures over a time span | `TOTK_SHOTS=w<lo>-<hi>:<step>` (world seconds) and `TOTK_SHOTS_DIR`; 480x270 by default |
| the frames around a hitch or a one-frame glitch | `TOTK_FLIGHT=1`: keeps the last 600 frames with timings and thumbnails, dumps on F8, on a frame over `TOTK_FLIGHT_SPIKE_MS` or at `TOTK_FLIGHT_AT=<frame>`; `python tools/flight_view.py <dump dir>` lists the worst frames and writes a contact sheet |
| every render pass of one frame | `TOTK_DUMP=<frame>` and `TOTK_DUMP_DIR`: a PNG per pass target plus `passes.txt` (draws, bound textures with swizzles, storage buffers, copies, clears) and `images.txt`; `TOTK_DUMP_BUF` adds a guest buffer |
| Vulkan misuse | `TOTK_VK_VALIDATION=1` (needs the Vulkan SDK), run into the world; compare the message kinds with a run without your change. The layer caps repeats, so counts are not totals |
| which pass the GPU was in at a device lost | the `vk: pass marks` lines; on by default on AMD, `TOTK_PASS_CRUMBS=1` elsewhere |
| a crash | the crash report at the end of the log: exception, guest and host addresses, registers |
| a run on a large card that acts like a small one | `TOTK_VRAM_LIMIT_MIB=<MiB>` |

Frame dumps: a draw's argument lines come before its pass header. Pass numbers shift between runs, so find a pass by
its draw count or its targets, not by its number. RenderDoc cannot capture this renderer ([controls.md](../controls.md),
"RenderDoc capture").

## Measuring

| What | How |
|---|---|
| fps, worst frame, render-thread waits, GPU busy time, VRAM, `bound: gpu\|cpu\|pace` | `--env TOTK_TRACE=perf,sched`, then `python tools\perf_summary.py <log> <lo> <hi>` |
| slow frames | count the `perf: slow frame` lines; each names what the render thread was blocked on (pipeline, translation, texture, fence, pacing) |
| the frame's critical path, by thread and state | `TOTK_CRIT=w<lo>-<hi>` and `TOTK_CRIT_OUT=<absolute path>` with `sched` tracing, then `python tools\crit_path.py <file>` |
| where CPU time goes | `TOTK_PROFILE=<path>` and `TOTK_PROFILE_FRAMES=w<lo>-<hi>`, then `python tools\prof_report.py <profile> <totk.map>`; `TOTK_PROFILE_RENDER=1` samples the render thread with call chains |
| a switch on a static scene | a setting set to `ab` with `TOTK_AB=<seconds>` alternates the two sides in blocks of game time; `python tools\ab_summary.py <log> --skip <seconds>`. On a moving scene compare whole runs instead |

A single run proves little: runs of one script on one build spread by a third in their 1% lows. Compare medians of at
least two whole runs per side, on a quiet machine.

## Probes

Each probe is a mode of `totk.exe`, run from the build folder; the list with what each covers is at the top of
`runtime/main.c`. `--selftest` needs no game files. `--probe-kernel`, `--probe-vkexec`, `--probe-nvn-tex` and
`--probe-shader` cover most runtime changes and run in seconds: run them before any game run.

## Reading output without filling the context

- Game logs run to megabytes. `grep` the lines you need, `tail` the end, pipe `perf_summary.py` through `grep SUMMARY`.
- Generated C runs to hundreds of thousands of lines per file. `grep -n` the function, then print its line range.
- Look at screenshots downscaled: paste several into one 1280x720 grid and read the grid once. Write down what it shows
  instead of opening it again.
- Send long builds and runs to the background with their output in a file; read the last lines when they end.
- Keep a notes file in the build folder from the first finding: what is ruled out, the leading hypothesis, the next
  step, temporary debug edits with file and line. A fresh session continues from it.
