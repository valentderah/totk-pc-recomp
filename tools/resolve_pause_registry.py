"""Match local pause-config row names to observed registry hashes and reconstruct flag transitions."""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def murmur3(data):
    """MurmurHash3 x86 32-bit, seed zero, over bytes."""
    mask, h = 0xffffffff, 0
    def rotate(x, n):
        return ((x << n) | (x >> (32 - n))) & mask
    end = len(data) // 4 * 4
    for i in range(0, end, 4):
        k = int.from_bytes(data[i:i + 4], 'little')
        k = rotate(k * 0xcc9e2d51 & mask, 15) * 0x1b873593 & mask
        h = rotate(h ^ k, 13)
        h = (h * 5 + 0xe6546b64) & mask
    if end != len(data):
        k = int.from_bytes(data[end:], 'little')
        h ^= rotate(k * 0xcc9e2d51 & mask, 15) * 0x1b873593 & mask
    h ^= len(data)
    h ^= h >> 16
    h = h * 0x85ebca6b & mask
    h ^= h >> 13
    h = h * 0xc2b2ae35 & mask
    return h ^ (h >> 16)


def resolve(config, events):
    names = sorted({cell['RowKey'] for cell in config['CellList'].values()})
    registry = {e['id']: e['result'] for e in events if e['type'] == 'context_entry'}
    result = []
    for name in names:
        key = murmur3(name.encode())
        if key not in registry:
            continue
        bit = registry[key]
        transitions = []
        previous = None
        for e in events:
            if e['type'] != 'context_word' or e['id'] != bit // 32:
                continue
            value = bool(e['result'] & (1 << (bit % 32)))
            if value != previous:
                transitions.append({'world_us': e['world_us'], 'paused': value})
                previous = value
        reasons = sorted(c['ColumnKey'] for c in config['CellList'].values()
                         if c['RowKey'] == name and c.get('IsPause', False))
        result.append({'name': name, 'hash': key, 'bit': bit, 'configured_pause_reasons': reasons,
                       'observed_transitions': transitions})
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--config', type=Path, required=True, help='local decoded pause config JSON')
    ap.add_argument('--log', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    if not a.out.resolve().is_relative_to(ROOT / 'build'):
        ap.error('--out must be under build/')
    from analyze_input_queries import read_jsonl
    from check_content import GENERATED_MARKER
    events, partial = read_jsonl(a.log)
    result = resolve(json.loads(a.config.read_text()), events)
    report = {'marker': GENERATED_MARKER, 'rows': result, 'partial_final_record': partial,
              'observed_registry_entries': len({e['id'] for e in events if e['type'] == 'context_entry'})}
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(report, indent=2) + '\n')
    print(f'{len(result)} names resolved against {report["observed_registry_entries"]} observed entries: {a.out}')


if __name__ == '__main__':
    main()
