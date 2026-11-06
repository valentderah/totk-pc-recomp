"""Validate a local player-state contract and emit its build-only C configuration; no game constants live here."""
import hashlib

FIELDS = ('root_link link_data link_id unit_proc unit_id proc_state actor_components as_component '
          'player_component attention_component power_component as_slots slot_verb slot_alternate '
          'state_bytes swim_mask climb_mask attention_state power_current power_hand hand_state holding_ability').split()


def validate(config, module):
    def integer(value, low, high):
        return type(value) is int and low <= value <= high

    if not isinstance(config, dict) or not all(k in config for k in ('fields', 'global_offset', 'verb_table', 'evidence')):
        raise ValueError('Player-state contract requires fields, global_offset, verb_table and evidence')
    fields = config['fields']
    if (not isinstance(fields, dict) or set(fields) != set(FIELDS) or
            any(not integer(fields[k], 0, 65536) for k in FIELDS)):
        raise ValueError('Player-state fields must be complete bounded integer offsets/masks')
    for key in ('swim_mask', 'climb_mask'):
        if not 0 < fields[key] < 65536 or fields[key] & (fields[key] - 1):
            raise ValueError('State byte masks must select one bit in a uint16')
    if fields['holding_ability'] > 8:
        raise ValueError('Holding ability must fit the supported ability range')
    off = config['global_offset']
    if not integer(off, 0, len(module.image) - 8) or off % 8:
        raise ValueError('Player global must be an aligned module offset')
    evidence = config['evidence']
    if not isinstance(evidence, list) or not 1 <= len(evidence) <= 32:
        raise ValueError('Player-state plan needs bounded fingerprinted evidence ranges')
    for entry in evidence:
        start, size = entry['offset'], entry['size']
        if (not integer(start, 0, len(module.image)) or not integer(size, 1, 65536) or
                start + size > len(module.image) or
                hashlib.sha256(module.image[start:start + size]).hexdigest() != entry['sha256']):
            raise ValueError('Player-state evidence fingerprint does not match executable')
    table = config['verb_table']
    rows, start = table['rows'], table['offset']
    if (not isinstance(rows, list) or not 1 <= len(rows) <= 1024 or
            not integer(start, 0, len(module.image) - 8 * len(rows)) or start % 8 or
            not integer(table['invalid'], len(rows), len(rows))):
        raise ValueError('Invalid player verb table bounds')
    relatives = {o: a for o, kind, _, a in module.nso.relocs if kind == 1027}
    seen = set()
    for index, row in enumerate(rows):
        address = relatives.get(start + 8 * index)
        if address is None or not integer(address, 0, len(module.image) - 1):
            raise ValueError('Verb name must have a module-relative relocation')
        end = module.image.find(b'\0', address, min(address + 160, len(module.image)))
        name = module.image[address:end].decode('ascii') if end >= 0 else None
        if name != row['name'] or name in seen:
            raise ValueError('Verb name/order differs from the local contract')
        seen.add(name)
        if (not integer(row['body'], 0, 0xffffffff) or row['body'] & ~0xe1e or
                not integer(row['any'], 0, 0xffffffff) or row['any'] & ~0x3160 or
                not integer(row.get('body_unknown', 0), 0, 0xffffffff) or row.get('body_unknown', 0) & ~0xe1e or
                row['body'] & row.get('body_unknown', 0)):
            raise ValueError('Unsupported player classification flag')
    return config


def source(config, module, site, marker):
    """Only validated numeric literals are emitted; game-derived names stay in the ignored plan."""
    rows = config['verb_table']['rows']
    body, any_flags = [r['body'] for r in rows], [r['any'] for r in rows]
    uncertain = [r.get('body_unknown', 0) for r in rows]
    def union(values):
        result = 0
        for value in values:
            result |= value
        return result
    values = dict(main_start=module.base, main_end=module.base + len(module.image),
                  global_slot=module.base + config['global_offset'], site=site, **config['fields'],
                  invalid_verb=config['verb_table']['invalid'], verb_count=len(rows),
                  body_known=union(body), any_known=union(any_flags))
    return ('// ' + marker + '\n#include "input_player_state.h"\n' +
            'static const uint32_t body[] = {' + ','.join(map(str, body)) + '};\n' +
            'static const uint32_t upper[] = {' + ','.join(map(str, any_flags)) + '};\n' +
            'static const uint32_t uncertain[] = {' + ','.join(map(str, uncertain)) + '};\n' +
            'static const struct InputPlayerLayout layout = {\n' +
            ''.join(f'    .{key} = {value}ull,\n' for key, value in values.items()) +
            '    .body_flags = body, .any_flags = upper, .body_unknown = uncertain\n};\n' +
            'const struct InputPlayerLayout *input_player_state_config(void) { return &layout; }\n')
