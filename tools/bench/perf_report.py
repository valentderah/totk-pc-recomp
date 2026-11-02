#!/usr/bin/env python3
"""Summarise a TotK Recomp perf log: frame-time percentiles, the bound type, GPU time and hitching.

Run the game with TOTK_TRACE=perf (or trace = perf in totk.ini) and close the window normally: the log ends
with the "frametime: exit" line, which this script also prints. Use the same save and the same input script
(TOTK_INPUT_SCRIPT, tools/input/load_save.txt shows the format) for every run that is meant to be compared,
and keep the window in the foreground: the game runs at a few fps when another window covers it.

Only the gameplay part counts: frames after the last "frame step: world clock starts" line, which the port
writes when the first save load ends. Everything before it is boot, the title screen and the load itself.

usage:
  python tools/bench/perf_report.py logs/totk.log
  python tools/bench/perf_report.py logs/totk.log --json

No game files, keys or saves are read; the log holds only timings.
"""

import argparse
import json
import re
import sys

FRAME_RE = re.compile(r"perf: frame (\d+): interval avg ([0-9.]+) ms max ([0-9.]+) ms \(([0-9.]+) fps\)")
GPU_RE = re.compile(r"perf: frame (\d+): gpu avg ([0-9.]+) ms")
BOUND_RE = re.compile(r"bound: (cpu|gpu|pace)")
EXIT_RE = re.compile(r"frametime: exit: (\d+) frames in ([0-9.]+) s, mean ([0-9.]+) fps, 1% low ([0-9.]+) fps, "
                     r"over 25 ms (\d+), 50 ms (\d+), 100 ms (\d+)")
WORLD_RE = re.compile(r"frame step: world clock starts")


def load(path):
    with open(path, encoding="utf-8", errors="replace") as handle:
        return handle.readlines()


def gameplay_start(lines):
    """Index of the last world-clock line: the frames after it are the game, not the boot."""
    for index in range(len(lines) - 1, -1, -1):
        if WORLD_RE.search(lines[index]):
            return index
    return 0


def percentile(sorted_values, fraction):
    if not sorted_values:
        return 0.0
    position = int((len(sorted_values) - 1) * fraction)
    return sorted_values[position]


def summarise(lines):
    start = gameplay_start(lines)
    intervals, gpu, bounds, exit_line = [], [], {}, None
    for line in lines[start:]:
        match = FRAME_RE.search(line)
        if match:
            intervals.append(float(match.group(2)))
        match = GPU_RE.search(line)
        if match:
            gpu.append(float(match.group(2)))
        match = BOUND_RE.search(line)
        if match:
            bounds[match.group(1)] = bounds.get(match.group(1), 0) + 1
    for line in reversed(lines):
        match = EXIT_RE.search(line)
        if match:
            exit_line = {
                "frames": int(match.group(1)),
                "seconds": float(match.group(2)),
                "mean_fps": float(match.group(3)),
                "one_percent_low_fps": float(match.group(4)),
                "over_25_ms": int(match.group(5)),
                "over_50_ms": int(match.group(6)),
                "over_100_ms": int(match.group(7)),
            }
            break

    intervals.sort()
    report = {
        "log_lines_after_load": len(lines) - start,
        "windows": len(intervals),
        "frame_ms": {
            "p50": round(percentile(intervals, 0.50), 2),
            "p70": round(percentile(intervals, 0.70), 2),
            "p80": round(percentile(intervals, 0.80), 2),
            "p90": round(percentile(intervals, 0.90), 2),
            "p95": round(percentile(intervals, 0.95), 2),
            "p99": round(percentile(intervals, 0.99), 2),
        },
        "fps_at": {key: round(1000.0 / value, 1) for key, value in
                   {"p50": percentile(intervals, 0.50), "p90": percentile(intervals, 0.90),
                    "p99": percentile(intervals, 0.99)}.items() if value},
        "gpu_ms": {"mean": round(sum(gpu) / len(gpu), 2), "max": round(max(gpu), 2)} if gpu else None,
        "bound": bounds,
        "exit": exit_line,
        "warnings": [],
    }
    if not intervals:
        report["warnings"].append("no gameplay windows: did the save load? (look for a world-clock line)")
    if not exit_line:
        report["warnings"].append("the run did not end normally: no frametime: exit line")
    return report


def print_text(report):
    frame = report["frame_ms"]
    print(f"gameplay windows: {report['windows']}")
    if report["windows"]:
        print(f"frame time ms:    p50 {frame['p50']:6.2f}  p70 {frame['p70']:6.2f}  p80 {frame['p80']:6.2f}"
              f"  p90 {frame['p90']:6.2f}  p95 {frame['p95']:6.2f}  p99 {frame['p99']:6.2f}")
        print(f"fps at:           p50 {report['fps_at']['p50']:6.1f}  p90 {report['fps_at']['p90']:6.1f}"
              f"  p99 {report['fps_at']['p99']:6.1f}")
    if report["gpu_ms"]:
        print(f"gpu ms per frame: mean {report['gpu_ms']['mean']:.2f}  max {report['gpu_ms']['max']:.2f}")
    if report["bound"]:
        total = sum(report["bound"].values())
        parts = ", ".join(f"{name} {count * 100 // total}%" for name, count in sorted(report["bound"].items(),
                                                                                    key=lambda item: -item[1]))
        print(f"bound:            {parts}")
    exit_line = report["exit"]
    if exit_line:
        print(f"exit:             {exit_line['mean_fps']:.1f} fps mean, {exit_line['one_percent_low_fps']:.1f} fps 1% low"
              f" over {exit_line['frames']} frames in {exit_line['seconds']:.1f} s; over 25 ms {exit_line['over_25_ms']},"
              f" over 50 ms {exit_line['over_50_ms']}, over 100 ms {exit_line['over_100_ms']}")
    for warning in report["warnings"]:
        print(f"warning:          {warning}")


def selftest():
    """Parse a synthetic log with known numbers: the windows, percentiles, bound counts and exit line must match."""
    lines = [
        "[host] frame step: world clock starts at present 12, 3.00 s after the guest started (the player at 0.0 0.0 0.0)\n",
        "[host] perf: frame 60: interval avg 10.00 ms max 11.00 ms (100.0 fps); render thread blocked: pacing 90%\n",
        "[host] perf: frame 60: gpu avg 5.00 ms (120 parts timed, 0 not); vram 1 of 2 MiB; bound: cpu\n",
        "[host] perf: frame 120: interval avg 20.00 ms max 21.00 ms (50.0 fps); render thread blocked: pacing 0%\n",
        "[host] perf: frame 120: gpu avg 5.00 ms (120 parts timed, 0 not); vram 1 of 2 MiB; bound: pace\n",
        "[host] frametime: exit: 120 frames in 2.0 s, mean 60.0 fps, 1% low 50.0 fps, "
        "over 25 ms 0, 50 ms 0, 100 ms 0\n",
    ]
    report = summarise(lines)
    checks = [
        ("windows", report["windows"] == 2),
        ("percentiles", report["frame_ms"]["p50"] == 10.0 and report["frame_ms"]["p99"] == 10.0),
        ("gpu mean", report["gpu_ms"] is not None and report["gpu_ms"]["mean"] == 5.0),
        ("bound counts", report["bound"] == {"cpu": 1, "pace": 1}),
        ("exit line", report["exit"] is not None and report["exit"]["mean_fps"] == 60.0
         and report["exit"]["one_percent_low_fps"] == 50.0),
    ]
    failed = [name for name, ok in checks if not ok]
    for name, ok in checks:
        print(f"  {name}: {'ok' if ok else 'FAILED'}")
    if failed:
        print("selftest: failed (" + ", ".join(failed) + ")")
        return 1
    print("selftest: passed")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", nargs="?", help="logs/totk.log of a run started with TOTK_TRACE=perf")
    parser.add_argument("--json", action="store_true", help="print the summary as JSON")
    parser.add_argument("--selftest", action="store_true", help="parse a synthetic log and check the numbers")
    args = parser.parse_args(argv)

    if args.selftest:
        return selftest()
    if not args.log:
        parser.error("a log path is needed unless --selftest is given")

    report = summarise(load(args.log))
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print_text(report)
    return 0 if report["windows"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
