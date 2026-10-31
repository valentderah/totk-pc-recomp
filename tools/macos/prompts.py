"""Identify prompt textures from the player's verified UI archive.

Only fingerprints and dimensions are emitted, into the ignored build folder.
The runtime draws its own device glyphs and never ships Nintendo's UI artwork.
"""
import compression.zstd as zstd
import hashlib
import json
import re
import struct
from pathlib import Path

from check_content import GENERATED_MARKER
from macos.build import write
from shader_census import checked, sarc

PROMPT_TEXTURES = {
    'Nt_KeyTexA_00^d': 0xe040, 'Nt_KeyTexB_00^d': 0xe041,
    'Nt_KeyTexX_00^d': 0xe042, 'Nt_KeyTexY_00^d': 0xe043,
    'Nt_KeyTexL_00^d': 0xe044, 'Nt_KeyTexZL_00^d': 0xe046,
    'Nt_KeyTexLSt_00^d': 0xe05e, 'Nt_KeyTexLSt_01^d': 0xe052,
    **{f'Controller_{style}{suffix}^w': 0xf000 for style in ('Console', 'Grip', 'Pro') for suffix in ('', '1')},
    **{f'ControllerSh_{style}{suffix}^s': 0xf002 for style in ('Console', 'Grip', 'Pro') for suffix in ('', '1')},
}


def texture_rows(data):
    if data[:4] != b'BNTX' or checked(data, 12, 2) != b'\xff\xfe':
        raise ValueError('unsupported prompt texture container')
    count, table = struct.unpack_from('<IQ', data, 0x24)
    checked(data, table, count * 8)
    rows = []
    found = set()
    for i in range(count):
        pos = struct.unpack_from('<Q', data, table + i * 8)[0]
        block = checked(data, pos, 0x80)
        if block[:4] != b'BRTI':
            raise ValueError('invalid prompt texture record')
        name_at = struct.unpack_from('<Q', block, 0x60)[0]
        length = struct.unpack_from('<H', checked(data, name_at, 2))[0]
        name = checked(data, name_at + 2, length).decode('utf-8')
        if name not in PROMPT_TEXTURES:
            continue
        if name in found:
            raise ValueError('duplicate prompt texture name')
        found.add(name)
        fmt, width, height = struct.unpack_from('<I4xII', block, 0x1c)
        size = struct.unpack_from('<I', block, 0x50)[0]
        levels = struct.unpack_from('<H', block, 0x16)[0]
        levels_at = struct.unpack_from('<Q', block, 0x70)[0]
        pixels_at = struct.unpack_from('<Q', checked(data, levels_at, 8))[0]
        pixels = checked(data, pixels_at, size)
        if levels != 1 or not width or not height or fmt not in (0x201, 0x1d01, 0x2d06):
            raise ValueError(f'unsupported prompt texture shape: {name}')
        rows.append({'code': PROMPT_TEXTURES[name], 'width': width, 'height': height,
                     'bytes': size, 'digest': hashlib.sha256(pixels).digest()})
    if found != PROMPT_TEXTURES.keys():
        raise ValueError('the UI archive is missing expected prompt textures')
    return rows


def write_header(rows, path):
    text = f'// {GENERATED_MARKER}\nstatic const NativePromptTextureIdentity prompt_textures[] = {{\n'
    for row in rows:
        digest = ','.join(f'0x{x:02x}' for x in row['digest'])
        text += f"    {{{row['code']}, {row['width']}, {row['height']}, {row['bytes']}, {{{digest}}}}},\n"
    if not rows:
        text += '    {0,0,0,0,{0}},\n'
    write(path, text + '};\n')


def patch_action_layout(data):
    """Keep the live action glyph and remove redundant Switch-position letters.

    These text panes use the normal text font, which cannot render controller
    private-use glyphs. Clearing their fixed-size strings preserves the font
    table, archive alignment and the game's original allocation budget.
    """
    out = bytearray(data)
    if out[:6] != b'FLYT\xff\xfe' or struct.unpack_from('<I', out, 12)[0] != len(out):
        raise ValueError('unsupported action prompt layout')
    pos = struct.unpack_from('<H', out, 6)[0]
    changed = set()
    for _ in range(struct.unpack_from('<H', out, 16)[0]):
        tag, size = struct.unpack_from('<4sI', checked(out, pos, 8))
        block = checked(out, pos, size)
        if size < 8:
            raise ValueError('empty action prompt section')
        if tag == b'txt1':
            name = checked(block, 12, 24).split(b'\0')[0].decode('ascii')
            match = re.fullmatch(r'T_([ABXY])_\d\d', name)
            if match:
                at = struct.unpack_from('<I', checked(block, 0x64, 4))[0]
                if checked(block, at, 4) != (match[1] + '\0').encode('utf-16le'):
                    raise ValueError('action prompt text no longer matches its pane')
                struct.pack_into('<H', out, pos + at, 0)
                changed.add(name)
        pos += size
    if pos != len(out) or len(changed) != 15:
        raise ValueError('unexpected action prompt layout coverage')
    return bytes(out)


def replace_member(data, name, replacement):
    """Replace one fixed-size SARC member without moving other resources."""
    members = sarc(data)
    if name not in members:
        raise ValueError('missing prompt layout member')
    if len(replacement) != len(members[name]):
        raise ValueError('prompt layout replacement must preserve its size')
    header, _, start = struct.unpack_from('<HxxII', data, 4)
    count = struct.unpack_from('<H', data, header + 6)[0]
    names = header + 12 + count * 16 + 8
    result = bytearray(data)
    for i in range(count):
        node = header + 12 + i * 16
        _, attribute, lo, hi = struct.unpack_from('<4I', data, node)
        at = names + (attribute & 0xffffff) * 4
        current = data[at:data.index(0, at, start)].decode()
        if current == name:
            result[start + lo:start + hi] = replacement
    if sarc(result) != dict(members, **{name: replacement}):
        raise ValueError('prompt archive changed an unrelated member')
    return bytes(result)


def prepare_prompts(extracted, out, version):
    folder = Path(extracted) / version / 'romfs'
    index = json.loads((folder / 'index.json').read_text())
    if index.get('status') != 'pass':
        raise ValueError('prompt preparation requires verified RomFS')
    files = {f['path']: f for f in index['files']}
    with (folder / 'romfs.bin').open('rb') as image:
        def read(name):
            item = files[name]
            image.seek(item['offset'])
            data = image.read(item['size'])
            if len(data) != item['size']:
                raise ValueError('truncated prompt archive')
            return data
        dictionaries = {}
        for value in sarc(zstd.decompress(read('Pack/ZsDic.pack.zs'))).values():
            dictionary = zstd.ZstdDict(value)
            dictionaries[dictionary.dict_id] = dictionary
        product = {'v121': '110', 'v143': '140'}[version]
        raw = read(f'UI/LayoutArchive/Common.Product.{product}.Nin_NX_NVN.blarc.zs')
    info = zstd.get_frame_info(raw)
    dictionary = dictionaries.get(info.dictionary_id)
    archive = zstd.decompress(raw, zstd_dict=dictionary)
    rows = texture_rows(sarc(archive)['timg/__Combined.bntx'])
    write_header(rows, Path(out) / 'native_prompt_textures.h')
    # Extend the existing timing overlay, preserving its other modifications.
    name = f'UI/LayoutArchive/Common.Product.{product}.Nin_NX_NVN.blarc.zs'
    patches = Path(extracted) / version / 'romfs-patches'
    target = patches / name
    overlay = zstd.decompress(target.read_bytes(), zstd_dict=dictionary)
    layout = sarc(overlay)['blyt/DoCommand_00.bflyt']
    patched = replace_member(overlay, 'blyt/DoCommand_00.bflyt', patch_action_layout(layout))
    data = zstd.compress(patched, level=19, zstd_dict=dictionary)
    if zstd.decompress(data, zstd_dict=dictionary) != patched:
        raise ValueError('prompt overlay compression did not round trip')
    target.write_bytes(data)
    manifest = patches / 'manifest.tsv'
    lines = manifest.read_text().splitlines()
    matches = 0
    for i, line in enumerate(lines):
        if line.startswith(name + '\t'):
            fields = line.split('\t')
            fields[1:3] = [str(len(data)), hashlib.sha256(data).hexdigest()]
            lines[i] = '\t'.join(fields)
            matches += 1
    if matches != 1:
        raise ValueError('missing prompt overlay manifest entry')
    manifest.write_text('\n'.join(lines) + '\n')
