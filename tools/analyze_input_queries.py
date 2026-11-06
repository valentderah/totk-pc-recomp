"""Summarize annotated input recordings without assigning unverified query IDs to actions."""
import argparse
import collections
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SWITCH_BUTTONS = ['A', 'B', 'X', 'Y', 'LCLICK', 'RCLICK', 'L', 'R', 'ZL', 'ZR', 'PLUS', 'MINUS',
                  'LEFT', 'UP', 'RIGHT', 'DOWN']


def read_jsonl(path):
    """Accept a final partial write after a crash, but reject malformed complete or intermediate records."""
    lines = path.read_text().splitlines(keepends=True)
    rows, partial = [], False
    for i, line in enumerate(lines):
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            if i != len(lines) - 1 or line.endswith('\n'):
                raise ValueError(f'{path.name}:{i + 1}: malformed recording') from None
            partial = True
    return rows, partial


def summarize(rows, markers):
    markers = sorted(markers, key=lambda m: m['ns'])
    inputs = [r for r in rows if r['type'] == 'input']
    queries = [r for r in rows if r['type'] == 'query']
    controllers = [r for r in rows if r['type'] == 'controller']
    transitions = []
    previous = 0
    for row in inputs:
        rising = row['buttons'] & ~previous
        previous = row['buttons']
        transitions.append((row['ns'], rising))
    intervals = []
    for i, marker in enumerate(markers):
        end = markers[i + 1]['ns'] if i + 1 < len(markers) else float('inf')
        start = marker['ns']
        q = [r for r in queries if start <= r['ns'] < end]
        c = [r for r in controllers if start <= r['ns'] < end]
        presses = collections.Counter()
        for t, bits in transitions:
            if start <= t < end:
                presses.update(name for bit, name in enumerate(SWITCH_BUTTONS) if bits & (1 << bit))
        intervals.append({'label': marker['label'], 'start_ns': start,
                          'duration_s': (end - start) / 1e9 if end != float('inf') else None,
                          'input_records': sum(start <= r['ns'] < end for r in inputs),
                          'press_transitions': dict(presses), 'query_records': len(q),
                          'true_query_records': sum(bool(r['result']) for r in q),
                          'controller_records': len(c), 'controller_names': sorted({r['name'] for r in c})})
    # 'calls' is cumulative per tuple. A sampled record is not one function invocation.
    counts = {}
    for row in queries:
        key = (row['site'], row['object'], row['id'], row['caller'])
        counts[key] = max(counts.get(key, 0), row['calls'])
    return {'record_types': dict(collections.Counter(r['type'] for r in rows)),
            'minimum_query_calls': sum(counts.values()), 'positive_query_records': sum(bool(r['result']) for r in queries),
            'query_sites': sorted({r['site'] for r in queries}),
            'controller_names': sorted({r['name'] for r in controllers}),
            'limits': [r['type'] for r in rows if r['type'] in ('limit', 'table_full', 'controller_table_full')],
            'intervals': intervals}


def markdown(summary):
    def safe(text):
        return str(text).replace('|', '\\|').replace('\n', ' ')

    lines = ['# Input recording analysis', '',
             'Labels describe the activity the player intended to perform; they do not independently verify on-screen behavior.',
             'Query IDs and controller masks remain unnamed until their semantics are validated.', '',
             f"- Record types: `{summary['record_types']}`.",
             f"- At least {summary['minimum_query_calls']} candidate-query calls, based on cumulative counters.",
             f"- Positive query records: {summary['positive_query_records']}.",
             f"- Named controllers observed: {', '.join(summary['controller_names']) or 'none'}.",
             f"- Output limits reached: {', '.join(summary['limits']) or 'none'}.", '',
             '| Activity | Seconds | Button press transitions | Query records | True records | Controller records |',
             '|---|---:|---|---:|---:|---:|']
    for interval in summary['intervals']:
        seconds = f"{interval['duration_s']:.1f}" if interval['duration_s'] is not None else '—'
        presses = ', '.join(f'{k}: {v}' for k, v in interval['press_transitions'].items()) or 'none'
        lines.append(f"| {safe(interval['label'])} | {seconds} | {presses} | {interval['query_records']} | "
                     f"{interval['true_query_records']} | {interval['controller_records']} |")
    lines += ['', 'Button transitions refer to the Switch state delivered by the runtime, after existing mappings. ',
              'Controller records refer to game-internal masks, whose bit layout must be established separately.',
              'Records are sampled on changes and periodic refreshes; counts of records are not counts of button presses or queries.', '']
    return '\n'.join(lines)


def main():
    from check_content import GENERATED_MARKER
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('session', type=Path)
    a = ap.parse_args()
    folder = a.session.resolve()
    if not folder.is_relative_to(ROOT / 'build'):
        ap.error('session must be under build/')
    rows, partial = read_jsonl(folder / 'queries.jsonl')
    markers, partial_marks = read_jsonl(folder / 'markers.jsonl')
    summary = summarize(rows, markers)
    summary.update(marker=GENERATED_MARKER, partial_tail=partial or partial_marks)
    (folder / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    report = markdown(summary)
    if summary['partial_tail']:
        report += '\nA final incomplete JSON line was ignored; the recording may have ended abruptly.\n'
    (folder / 'summary.md').write_text(report)
    print(report)


if __name__ == '__main__':
    main()
