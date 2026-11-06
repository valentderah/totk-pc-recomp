"""Decode observed player states using their build's verified local verb table, preserving unknown fields."""
import argparse
from collections import Counter
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FLAGS = {0: 'valid', 1: 'climb', 2: 'air', 3: 'glide', 4: 'swim', 5: 'bow_use', 6: 'guard_keep',
         7: 'lock_on', 8: 'ability_in_use', 9: 'horse', 10: 'vehicle', 11: 'surf', 12: 'talk',
         13: 'camera', 15: 'holding'}


def decode(row, config):
    names = config['verb_table']['rows']
    known = row['known'] if row['status'] == 2 else (row['known'] & 1)
    return dict(row, active_flags=[name for bit, name in FLAGS.items() if known & row['flags'] & (1 << bit)],
                unknown_flags=[name for bit, name in FLAGS.items() if not known & (1 << bit)],
                verb_names=[names[v]['name'] if row['status'] == 2 and row['slots_known'] & (1 << i) and
                            0 <= v < len(names) else None for i, v in enumerate(row['verbs'])],
                status_name={0: 'unknown', 1: 'absent', 2: 'ready'}.get(row['status'], 'unknown'))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('experiment', type=Path)
    a = ap.parse_args()
    folder = a.experiment.resolve()
    if not folder.is_relative_to(ROOT / 'build'):
        ap.error('recordings and derived reports must remain under build/')
    metadata = folder / 'experiment.json'
    if not metadata.exists():
        metadata = folder / 'session.json'
    manifest = json.loads(metadata.read_text())
    config = manifest['probe_sites'].get('player_state')
    if not config:
        ap.error('recording has no verified player-state contract')
    from analyze_input_queries import read_jsonl
    from check_content import GENERATED_MARKER
    rows, problems = read_jsonl(folder / 'queries.jsonl')
    if problems or any(r['type'] in ('limit', 'table_full') for r in rows):
        ap.error('recording is incomplete or has logger limits')
    states = [decode(r, config) for r in rows if r['type'] == 'player_state']
    if not states:
        ap.error('recording has no player-state observations')
    summary = dict(marker=GENERATED_MARKER, experiment=str(folder), states=states,
                   status_counts=dict(Counter(s['status_name'] for s in states)),
                   flags_seen=sorted({flag for s in states for flag in s['active_flags']}),
                   verbs_seen=sorted({v for s in states for v in s['verb_names'] if v}),
                   note='Flags describe observed state, not action availability or permission. Deduplicated records are not every sample.')
    output = folder / 'player-states.json'
    output.write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({k: summary[k] for k in ('status_counts', 'flags_seen', 'verbs_seen')}))
    print(output)


if __name__ == '__main__':
    main()
