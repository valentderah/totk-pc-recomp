"""Compare recorded actor positions on a common world-time grid, without extrapolating through gaps."""
import argparse
import bisect
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sample(positions, times, clock, max_gap_us=150000):
    index = bisect.bisect_left(times, clock)
    if index < len(times) and times[index] == clock:
        return positions[index]['position']
    if index == 0 or index == len(times):
        return None
    lo, hi = positions[index - 1], positions[index]
    if hi['world_us'] - lo['world_us'] > max_gap_us or lo['actor'] != hi['actor']:
        return None
    amount = (clock - lo['world_us']) / (hi['world_us'] - lo['world_us'])
    return [a + (b - a) * amount for a, b in zip(lo['position'], hi['position'])]


def compare(left, right, start_us, end_us, step_us=50000):
    a = sorted((x for x in left if x['type'] == 'actor_position'), key=lambda x: x['world_us'])
    b = sorted((x for x in right if x['type'] == 'actor_position'), key=lambda x: x['world_us'])
    at, bt = [p['world_us'] for p in a], [p['world_us'] for p in b]
    rows = []
    missing = []
    for clock in range(start_us, end_us + 1, step_us):
        av, bv = sample(a, at, clock), sample(b, bt, clock)
        if av is None or bv is None:
            missing.append(clock)
            continue
        delta = [y - x for x, y in zip(av, bv)]
        rows.append(dict(world_us=clock, left=av, right=bv, delta=delta,
                         distance_m=math.dist(av, bv), horizontal_m=math.hypot(delta[0], delta[2])))
    return dict(samples=rows, missing_world_us=missing,
                max_distance_m=max((r['distance_m'] for r in rows), default=None),
                max_horizontal_m=max((r['horizontal_m'] for r in rows), default=None),
                max_abs_vertical_m=max((abs(r['delta'][1]) for r in rows), default=None))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('left', type=Path)
    ap.add_argument('right', type=Path)
    ap.add_argument('--start', type=float, required=True)
    ap.add_argument('--end', type=float, required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    if not a.out.resolve().is_relative_to(ROOT / 'build'):
        ap.error('--out must be under build/')
    if not 0 <= a.start < a.end or a.end - a.start > 60:
        ap.error('window must be ordered, nonnegative and at most 60 seconds')
    from analyze_input_queries import read_jsonl
    from check_content import GENERATED_MARKER
    lm = json.loads((a.left / 'experiment.json').read_text())
    rm = json.loads((a.right / 'experiment.json').read_text())
    for manifest in (lm, rm):
        if manifest['exit_code'] != 0 or not manifest['source_profile_unchanged']:
            ap.error('both experiments must have succeeded with unchanged source profiles')
    fields = ('executable_sha256', 'profile_source_sha256', 'fps_limit_override')
    mismatches = [field for field in fields if lm.get(field) != rm.get(field)]
    if mismatches:
        ap.error('unmatched experiment conditions: ' + ', '.join(mismatches))
    left, lp = read_jsonl(a.left / 'queries.jsonl')
    right, rp = read_jsonl(a.right / 'queries.jsonl')
    if lp or rp or any('limit' in x['type'] or 'full' in x['type'] for x in left + right):
        ap.error('comparison requires complete recordings without logging limits')
    for rows in (left, right):
        actors = {(p['site'], p['actor']) for p in rows if p['type'] == 'actor_position' and
                  a.start * 1e6 <= p['world_us'] <= a.end * 1e6}
        if len(actors) != 1:
            ap.error('window must contain one unambiguous actor/site per experiment')
    result = compare(left, right, round(a.start * 1e6), round(a.end * 1e6))
    result.update(marker=GENERATED_MARKER, left=str(a.left.resolve()), right=str(a.right.resolve()),
                  manifest_conditions=list(fields), input_scripts_identical=lm['script_sha256'] == rm['script_sha256'])
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: result[k] for k in ('max_distance_m', 'max_horizontal_m', 'max_abs_vertical_m')}))
    print(f'{len(result["samples"])} compared samples; {len(result["missing_world_us"])} missing grid points')


if __name__ == '__main__':
    main()
