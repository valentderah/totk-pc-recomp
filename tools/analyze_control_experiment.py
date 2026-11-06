"""Correlate delivered scripted presses with observed controller and publisher masks, without naming actions."""
import argparse
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUTTONS = ['A', 'B', 'X', 'Y', 'LCLICK', 'RCLICK', 'L', 'R', 'ZL', 'ZR', 'PLUS', 'MINUS',
           'LEFT', 'UP', 'RIGHT', 'DOWN']


def correlate(rows, window_us=350000):
    """Report changes observed after each input rising edge; correlation does not establish an action's meaning."""
    presses, previous = [], 0
    for row in rows:
        if row['type'] != 'input':
            continue
        rising = row['buttons'] & ~previous
        previous = row['buttons']
        clock = row.get('world_us', -1)
        if clock < 0 or not rising:
            continue
        controller, outputs = {}, {}
        for event in rows:
            when = event.get('world_us', -1)
            if when < clock or when >= clock + window_us:
                continue
            if event['type'] == 'controller':
                key = (event['name'], event['caller'], event['object'])
                controller.setdefault(key, set()).add((event['held'], event['pressed']))
            elif event['type'] == 'output_mask':
                key = (event['site'], event['object'], event['id'])
                outputs.setdefault(key, set()).add(event['result'])
        presses.append({'world_us': clock, 'rising_bits': rising,
                        'buttons': [n for i, n in enumerate(BUTTONS) if rising & (1 << i)],
                        'controllers': [{'name': k[0], 'caller': k[1], 'object': k[2],
                                         'held_pressed': [list(p) for p in sorted(v)]}
                                        for k, v in sorted(controller.items())],
                        'outputs': [{'site': k[0], 'object': k[1], 'field': k[2], 'values': sorted(v)}
                                    for k, v in sorted(outputs.items())]})
    return presses


def virtual_responses(rows):
    """Measure position after each virtual action press, using a recent same-site actor sample as its baseline."""
    positions = [r for r in rows if r['type'] == 'actor_position']
    result = []
    for event in rows:
        if event['type'] != 'virtual_action' or not event['result'] & 1:
            continue
        clock = event['world_us']
        record = {'world_us': clock, 'site': event['site'], 'switch_buttons': event['buttons'], 'position_response': None}
        before = [p for p in positions if p['site'] == event['site'] and clock - 100000 <= p['world_us'] <= clock]
        if before:
            base = max(before, key=lambda p: p['world_us'])
            after = [p for p in positions if p['site'] == event['site'] and p['actor'] == base['actor'] and
                     clock < p['world_us'] <= clock + 800000]
            if after:
                xyz = base['position']
                record['position_response'] = {
                    'actor': base['actor'], 'baseline': xyz, 'samples': len(after),
                    'max_y_change_m': max(p['position'][1] - xyz[1] for p in after),
                    'max_horizontal_change_m': max(math.hypot(p['position'][0] - xyz[0], p['position'][2] - xyz[2]) for p in after)}
        result.append(record)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('experiment', type=Path)
    a = ap.parse_args()
    folder = a.experiment.resolve()
    if not folder.is_relative_to(ROOT / 'build'):
        ap.error('experiment must be under build/')
    from analyze_input_queries import read_jsonl
    from check_content import GENERATED_MARKER
    rows, partial = read_jsonl(folder / 'queries.jsonl')
    presses = correlate(rows)
    virtual = virtual_responses(rows)
    report = {'marker': GENERATED_MARKER, 'window_us': 350000, 'partial_final_record': partial, 'presses': presses,
              'virtual_action_presses': virtual,
              'limits': [r for r in rows if 'limit' in r['type'] or 'full' in r['type']]}
    (folder / 'correlations.json').write_text(json.dumps(report, indent=2) + '\n')
    text = ['# Scripted control correlations', '',
            'These are observed values within 350 ms of a delivered Switch-button edge. '
            'They do not establish gameplay action names. Missing events can reflect deduplication or an inactive consumer.', '',
            '| World seconds | Switch input | Publisher field | Observed values |', '|---|---|---|---|']
    for p in presses:
        for o in p['outputs'] or [None]:
            text.append(f'| {p["world_us"] / 1e6:.3f} | {", ".join(p["buttons"])} | '
                        + (f'{o["field"]:#x} | {", ".join(hex(v) for v in o["values"])} |' if o else '— | No event |'))
    if virtual:
        text += ['', '## Virtual action responses', '',
                 'Position change is measured over the next 800 ms relative to a sample at most 100 ms before the press.', '',
                 '| World seconds | Switch buttons | Maximum Y change (m) | Maximum horizontal change (m) |', '|---|---|---|---|']
        for event in virtual:
            response = event['position_response']
            metrics = (f'{response["max_y_change_m"]:.4f} | {response["max_horizontal_change_m"]:.4f}' if response else 'No position samples | —')
            text.append(f'| {event["world_us"] / 1e6:.3f} | {event["switch_buttons"]:#x} | {metrics} |')
    (folder / 'correlations.md').write_text('\n'.join(text) + '\n')
    print(f'{len(presses)} delivered presses; {len(report["limits"])} limit notices: {folder / "correlations.md"}')


if __name__ == '__main__':
    main()
