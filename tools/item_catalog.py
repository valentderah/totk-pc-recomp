"""Build a searchable spawner catalog from the player's RomFS, never from bundled game assets.

Names and descriptions come from ActorMsg/PouchContent.msbt; actors come from Pack/Actor. An actor's presence is
not proof that it can be granted or safely spawned. Capability fields stay unverified until tested in the runtime.
"""
from __future__ import annotations

import argparse
import compression.zstd as zstd
from collections import Counter
import json
from pathlib import Path
import re
import struct
import sys


def sarc_member(data: bytes, wanted: str) -> bytes:
    if data[:4] != b'SARC':
        raise ValueError('Expected SARC archive')
    endian = '<' if data[6:8] == b'\xff\xfe' else '>'
    header, size, start = struct.unpack_from(endian + 'HxxII', data, 4)
    if size != len(data) or data[header:header + 4] != b'SFAT':
        raise ValueError('Invalid SARC archive')
    count = struct.unpack_from(endian + 'H', data, header + 6)[0]
    names = header + 12 + count * 16
    if data[names:names + 4] != b'SFNT':
        raise ValueError('Invalid SARC names')
    names += 8
    for i in range(count):
        _, attr, lo, hi = struct.unpack_from(endian + '4I', data, header + 12 + 16 * i)
        off = names + (attr & 0xffffff) * 4
        if attr >> 24 != 1 or off >= start or hi < lo or start + hi > len(data):
            raise ValueError('Invalid SARC member')
        name = data[off:data.index(0, off, start)].decode('utf-8')
        if name == wanted:
            return data[start + lo:start + hi]
    raise KeyError(wanted)


def msbt_messages(data: bytes) -> dict[str, str]:
    if data[:8] != b'MsgStdBn' or data[0xC] != 1:
        raise ValueError('Expected UTF-16 MSBT')
    endian = '<' if data[8:10] == b'\xff\xfe' else '>'
    sections = {}
    pos = 0x20
    while pos + 16 <= len(data):
        size = struct.unpack_from(endian + 'I', data, pos + 4)[0]
        sections[data[pos:pos + 4]] = data[pos + 16:pos + 16 + size]
        pos = (pos + 16 + size + 15) & ~15
    labels, text = sections[b'LBL1'], sections[b'TXT2']
    count = struct.unpack_from(endian + 'I', text)[0]
    offsets = struct.unpack_from(endian + str(count) + 'I', text, 4)

    def decode(start):
        units = []
        p = start
        while p + 2 <= len(text):
            unit = struct.unpack_from(endian + 'H', text, p)[0]
            p += 2
            if unit == 0:
                break
            if unit == 0xE:
                # Start tag: group, type, parameter byte count, then parameters.
                if p + 6 > len(text):
                    raise ValueError('Truncated MSBT control')
                size = struct.unpack_from(endian + 'H', text, p + 4)[0]
                p += 6 + size
            elif unit == 0xF:
                p += 4
            else:
                units.append(unit)
        return struct.pack(endian + str(len(units)) + 'H', *units).decode('utf-16-le' if endian == '<' else 'utf-16-be')

    result = {}
    for bucket in range(struct.unpack_from(endian + 'I', labels)[0]):
        n, p = struct.unpack_from(endian + 'II', labels, 4 + bucket * 8)
        for _ in range(n):
            length = labels[p]
            name = labels[p + 1:p + 1 + length].decode('utf-8')
            index = struct.unpack_from(endian + 'I', labels, p + 1 + length)[0]
            if index >= count:
                raise ValueError('MSBT text index outside table')
            result[name] = decode(offsets[index])
            p += length + 5
    return result


def category(actor: str) -> str:
    if actor.startswith('Obj_SubstituteCloth_'):
        return 'Paraglider fabrics'
    if actor.startswith(('GameRomHorseReins_', 'GameRomHorseSaddle_')):
        return 'Horse equipment'
    if actor.startswith(('PutRupee', 'MinusRupee_', 'Energy_Material_')):
        return 'Currency and upgrade resources'
    if actor.startswith(('NormalArrow', 'Obj_ArrowBundle_')):
        return 'Arrows'
    if actor in ('ConfusionFruit', 'ElectricalFruit', 'FireFruit', 'IceFruit', 'LightFruit', 'SmokeFruit', 'WaterFruit') or actor.startswith(('LightBall_', 'FldObj_Pinecone_')):
        return 'Materials and ingredients'
    for prefix, name in [('Weapon_Sword_', 'Weapons / One-handed'), ('Weapon_Lsword_', 'Weapons / Two-handed'),
                         ('Weapon_Spear_', 'Weapons / Spears'), ('Weapon_Bow_', 'Bows'),
                         ('Weapon_Shield_', 'Shields'), ('Armor_', 'Armor'), ('Item_Cook_', 'Meals and elixirs'),
                         ('Item_Roast_', 'Roasted and frozen food'), ('SpObj_', 'Zonai devices / World objects')]:
        if actor.startswith(prefix):
            return name
    if actor.startswith(('Item_', 'Animal_Insect_', 'Animal_Fish_')) or actor in ('BeeHome', 'Obj_FireWoodBundle', 'BombFruit'):
        return 'Materials and ingredients'
    if actor.startswith(('Obj_', 'KeySmall', 'CaveMasterMedal', 'Parasail')):
        return 'Special and key items'
    return 'Other actors'


def build(romfs: Path, out: Path, language: str = 'USen'):
    index = json.loads((romfs / 'index.json').read_text(encoding='utf-8'))
    if index.get('status') != 'pass':
        raise ValueError('RomFS extraction is not verified')
    files = {f['path']: f for f in index['files']}
    candidates = [(int(m.group(1)), path) for path in files
                  if (m := re.fullmatch(rf'Mals/{re.escape(language)}\.Product\.(\d+)\.sarc\.zs', path))]
    if not candidates:
        raise ValueError('No messages for ' + language)
    _, path = max(candidates)
    with (romfs / 'romfs.bin').open('rb') as image:
        def read(name):
            f = files[name]
            image.seek(f['offset'])
            data = image.read(f['size'])
            if len(data) != f['size']:
                raise ValueError('RomFS file is truncated: ' + name)
            return data
        dictionaries = {}
        pack = zstd.decompress(read('Pack/ZsDic.pack.zs'))
        if pack[:4] != b'SARC':
            raise ValueError('Invalid Zstandard dictionary archive')
        endian = '<' if pack[6:8] == b'\xff\xfe' else '>'
        header, _, start = struct.unpack_from(endian + 'HxxII', pack, 4)
        count = struct.unpack_from(endian + 'H', pack, header + 6)[0]
        for i in range(count):
            _, _, lo, hi = struct.unpack_from(endian + '4I', pack, header + 12 + 16 * i)
            dictionary = zstd.ZstdDict(pack[start + lo:start + hi])
            dictionaries[dictionary.dict_id] = dictionary
        raw = read(path)
    frame = zstd.get_frame_info(raw)
    archive = zstd.decompress(raw, zstd_dict=dictionaries.get(frame.dictionary_id))
    messages = msbt_messages(sarc_member(archive, 'ActorMsg/PouchContent.msbt'))
    actors = {Path(p).name.removesuffix('.pack.zs') for p in files if p.startswith('Pack/Actor/') and p.endswith('.pack.zs')}
    names = {key.removesuffix('_Name'): value for key, value in messages.items() if key.endswith('_Name')}
    rows = []
    for actor in sorted(actors | names.keys()):
        name = names.get(actor)
        rows.append({'id': actor, 'name': name or actor, 'description': messages.get(actor + '_Caption', ''),
                     'category': category(actor), 'has_actor_pack': actor in actors,
                     'has_inventory_name': bool(name), 'inventory': 'unverified', 'world_spawn': 'unverified',
                     'search': ((name or actor) + ' ' + actor).casefold()})
    out.mkdir(parents=True, exist_ok=True)
    result = {'schema': 1, 'language': language, 'message_archive': path, 'entries': rows}
    (out / 'items.json').write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding='utf-8')
    def clean(value):
        return value.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ')
    (out / 'items.tsv').write_text('\n'.join('\t'.join([row['id'], clean(row['name']), row['category'],
                                  '1' if row['has_actor_pack'] else '0', clean(row['description'])])
                                  for row in rows if row['has_inventory_name']) + '\n', encoding='utf-8')
    summary = Counter(row['category'] for row in rows if row['has_inventory_name'])
    (out / 'summary.json').write_text(json.dumps({'actor_packs': len(actors), 'named_inventory_entries': len(names),
                                                'named_categories': summary}, indent=2), encoding='utf-8')
    print(json.dumps({'actor_packs': len(actors), 'named_inventory_entries': len(names), 'named_categories': summary}))
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--romfs', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--language', default='USen')
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.out.resolve()
    if output.is_relative_to(root) and not output.is_relative_to(root / 'build'):
        ap.error('Game-derived catalog output must stay outside source files or under ignored build/.')
    build(args.romfs, args.out, args.language)


if __name__ == '__main__':
    main()
