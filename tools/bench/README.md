# Benchmarking a run

`perf_report.py` turns the `TOTK_TRACE=perf` log of a run into the numbers a settings change is judged by: frame-time
percentiles, the frame rate at those percentiles, GPU time per frame, the bound type, and the `frametime: exit` summary.
It reads only the log, needs nothing but Python 3, and works on Windows and Linux.

```sh
python tools/bench/perf_report.py logs/totk.log
python tools/bench/perf_report.py logs/totk.log --json > run.json
python tools/bench/perf_report.py --selftest     # parses a synthetic log and checks the numbers
```

Output of a 0.3.1 run (2560x1440, `fps_limit = 80`, DLAA, 5070 Ti Laptop):

```
gameplay windows: 2176
frame time ms:    p50  14.81  p70  16.30  p80  17.29  p90  18.53  p95  19.43  p99  21.15
fps at:           p50   67.5  p90   54.0  p99   47.3
gpu ms per frame: mean 6.06  max 8.30
bound:            cpu 68%, pace 31%
exit:             66.3 fps mean, 32.3 fps 1% low over 3370 frames in 50.8 s; over 25 ms 134, over 50 ms 1, over 100 ms 0
```

## Making a run worth comparing

1. `trace = perf` in `totk.ini` (or `set TOTK_TRACE=perf`). Every 120 presents the log gets one line per window; the
   run ends with `frametime: exit`.
2. Use the same save and the same input script every time (`TOTK_INPUT_SCRIPT`, the format is in `docs/controls.md`,
   "Scripted input"; `tools/input/load_save.txt` loads the first save from the title screen). The world clock starts at
   the end of the first save load, and the report only counts what comes after it, so boot and loading do not skew the
   numbers.
3. **Keep the window in the foreground.** A window that another window covers, or a window on a hidden desktop, runs at
   a few fps on Windows (the compositor does not present it), which is neither the fault of the build nor a number worth
   reporting.
4. Close the window normally, not with Task Manager: the `frametime: exit` summary is written on the way out.
5. Change one setting per run. The same config drifts by several fps between runs, so compare configs interleaved
   (A B C A B C) rather than in one sequence.

## What the fields mean

- `frame time` percentiles: `p50` is the typical frame, `p99` is the worst 1 %. A limit is only met when `p99` is under
  its frame time (`fps_limit = 80` needs 12.50 ms).
- `bound`: what the frame waited on. `cpu` is the three virtual cores, `gpu` is the graphics card, `pace` is the frame
  limit. A machine that is `pace`-bound has room for more frames; one that is `cpu`-bound does not, and lowering
  `render_scale` will not help it.
- `gpu ms per frame`: if this is far below the frame time while `bound` says `cpu`, the graphics card is idle and only
  the CPU side can be tuned.
- `over 25/50/100 ms`: hitching. A handful of frames over 50 ms in a fresh area is shader translation or texture
  upload; it should not repeat when the same area is played again (the caches in the install folder are written).

## Logs in issues

`logs/totk.log` holds timings only, no keys and no game files, so it can be attached to an issue. Attach the log of the
run itself and say which settings differ from the defaults; the numbers above are what a maintainer needs to reproduce
a performance report.
