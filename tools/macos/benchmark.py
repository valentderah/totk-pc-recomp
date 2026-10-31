"""Evaluate real presentation timestamps; never count interpolation or repeated frames."""
import argparse
import csv
import json
import math
from pathlib import Path
import statistics


def evaluate(rows, target=144.0, minimum_seconds=60.0):
    if not math.isfinite(target) or target <= 0 or not math.isfinite(minimum_seconds) or minimum_seconds <= 0:
        raise ValueError('positive, finite target and duration required')
    if len(rows) < 2:
        raise ValueError('at least two presented frames are required')
    problems = []
    times = [float(row['present_seconds']) for row in rows]
    if any(not math.isfinite(t) or t <= 0 for t in times):
        raise ValueError('invalid presentation timestamp')
    # Drawable callbacks may arrive on different threads. Their timestamps,
    # rather than the order in which callbacks acquired the log lock, define
    # physical presentation order.
    rows = [row for _, row in sorted(zip(times, rows), key=lambda pair: pair[0])]
    times.sort()
    tickets = [int(row['ticket']) for row in rows]
    frames = [int(row['frame']) for row in rows]
    if any(b != a + 1 for a, b in zip(tickets, tickets[1:])):
        problems.append('missing, repeated or reordered present tickets')
    if len(set(frames)) != len(frames) or any(b <= a for a, b in zip(frames, frames[1:])):
        problems.append('repeated or reordered game frames')
    if any(int(row['strict']) != 1 for row in rows):
        problems.append('non-strict rendering mode')
    for name in ('scene', 'output', 'drawable'):
        if any((int(r[name + '_width']), int(r[name + '_height'])) != (3840, 2160) for r in rows):
            problems.append(f'{name} resolution is unchecked or differs from 3840x2160')
    intervals = [b - a for a, b in zip(times, times[1:])]
    if any(dt <= 0 for dt in intervals):
        raise ValueError('presentation timestamps must strictly increase')
    elapsed = times[-1] - times[0]
    if elapsed < minimum_seconds:
        problems.append(f'capture is shorter than {minimum_seconds:g} seconds')
    ordered = sorted(intervals)
    p99 = ordered[math.ceil(0.99 * len(ordered)) - 1]
    worst_one_percent = ordered[-max(1, math.ceil(len(ordered) * 0.01)):]
    fps = len(intervals) / elapsed
    budget = 1 / target
    # CSV timestamps have nine decimal places. Include one decimal quantum and
    # one floating point ULP; this is nanoseconds, not a frame-rate allowance.
    precision = 1e-9 + max(math.ulp(t) for t in times)
    if elapsed > len(intervals) / target + precision: problems.append('average presented FPS is below target')
    if p99 > budget + precision: problems.append('p99 frame time exceeds the frame budget')
    return dict(status='pass' if not problems else 'fail', reasons=problems, frames=len(rows),
                seconds=elapsed, average_fps=fps, p50_ms=statistics.median(intervals) * 1000,
                p99_ms=p99 * 1000, low_1_percent_fps=1 / statistics.mean(worst_one_percent),
                worst_ms=max(intervals) * 1000, over_budget=sum(dt > budget + precision for dt in intervals),
                target_fps=target, budget_ms=budget * 1000, timestamp_precision_ns=precision * 1e9)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('trace', type=Path)
    ap.add_argument('--target', type=float, default=144)
    ap.add_argument('--minimum-seconds', type=float, default=60)
    ap.add_argument('--start-ticket', type=int, help='explicit start of the measured gameplay segment')
    ap.add_argument('--end-ticket', type=int)
    ap.add_argument('--output', type=Path)
    a = ap.parse_args()
    with a.trace.open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    selected = [r for r in rows if (a.start_ticket is None or int(r['ticket']) >= a.start_ticket)
                and (a.end_ticket is None or int(r['ticket']) <= a.end_ticket)]
    result = evaluate(selected, a.target, a.minimum_seconds)
    result.update(trace=str(a.trace), start_ticket=a.start_ticket, end_ticket=a.end_ticket,
                  excluded_frames=len(rows) - len(selected))
    text = json.dumps(result, indent=2) + '\n'
    if a.output: a.output.write_text(text)
    print(text, end='')
    return 0 if result['status'] == 'pass' else 1


if __name__ == '__main__': raise SystemExit(main())
