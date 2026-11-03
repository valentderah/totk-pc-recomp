"""tools/make_romfs_patches.py --prompts and tools/button_icons.py on a RomFS made here, with no game files: each prompt
names the label of the Xbox button that the pad layout presses, the shoulder buttons, triggers, Plus, Minus and the
d-pad are drawn as on an Xbox controller, the resource size table grows with the fonts, every other file and member
stays byte for byte, the same input gives the same output, and a RomFS the tool does not recognise fails before it
writes anything. Both game versions: 1.2.1 (Common.Product.110, 72x72 button guide images) and 1.4.3
(Common.Product.140, 108x108).

Run: .venv-py314\\Scripts\\python tests\\romfs_prompts_test.py
"""
import compression.zstd as zstd
import hashlib
import importlib.util
import io
import json
import random
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.recordingPen import RecordingPen
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / 'tools' / 'make_romfs_patches.py'
spec = importlib.util.spec_from_file_location('button_icons', ROOT / 'tools' / 'button_icons.py')
icons = importlib.util.module_from_spec(spec)
spec.loader.exec_module(icons)

# The face button each Switch button is in a pad layout (docs/controls.md, runtime/input_map.c), the label an Xbox pad
# prints on it, and the layout each overlay is made for (runtime/input_map.c input_prompts_folder).
POSITION = {'nintendo': {'A': 'east', 'B': 'south', 'X': 'north', 'Y': 'west'},
            'xbox': {'A': 'south', 'B': 'east', 'X': 'west', 'Y': 'north'}}
XBOX_LABEL = {'south': 'A', 'east': 'B', 'west': 'X', 'north': 'Y'}
STYLE_LAYOUT = {'xbox': 'nintendo', 'xbox-label': 'xbox'}
FACE = {'A': 0xE040, 'B': 0xE041, 'X': 0xE042, 'Y': 0xE043}
# The glyphs drawn as Xbox icons, with their advance: LB RB LT RT, Menu and View, the d-pad.
DRAWN = {0xE044: 820, 0xE045: 820, 0xE046: 760, 0xE047: 760, 0xE04B: 640, 0xE04C: 640, 0xE049: 640,
         **{cp: 640 for cp in range(0xE060, 0xE066)}}

# restbl: the resource size table's name after its version, in both forms the tool takes.
VERSIONS = {'v121': dict(common='110', size=72, newest=121, restbl='Nin_NX_NVN.rsizetable.zs'),
            'v143': dict(common='140', size=108, newest=143, restbl='rsizetable.zs')}
DEFAULT_FONT, REGIONAL_FONT = 'Font/Font.Nin_NX_NVN.bfarc.zs', 'Font/Font_KRko.Nin_NX_NVN.bfarc.zs'
PANES = {'T_A_00': 'A', 'T_B_00': 'B', 'T_X_00': 'X', 'T_Y_00': 'Y', 'T_Name_00': 'A'}
MESSAGES = {**PANES, 'Greeting': 'Hello'}
R8_UNORM, BC1 = 0x201, 0x1a01
IMAGE = 16384   # an R8 square up to 128 rows, two GOBs wide, block height 16: the size of the game's key images


# ---- the formats, written here from their layout so the tools' readers are checked against an independent writer
def sarc(members, align=0x80):
    names, sfnt, name_at = list(members), b'', {}
    for name in names:
        name_at[name] = len(sfnt) // 4
        sfnt += name.encode() + b'\0'
        sfnt += bytes(-len(sfnt) % 4)
    head = 0x14 + 12 + 16 * len(names) + 8 + len(sfnt)
    start = head + -head % align
    data, nodes = b'', b''
    for name in names:
        data += bytes(-len(data) % align)
        nodes += struct.pack('<4I', zlib.crc32(name.encode()), 0x01000000 | name_at[name], len(data), len(data) + len(members[name]))
        data += members[name]
    out = (b'SARC' + struct.pack('<HHIIHH', 0x14, 0xFEFF, start + len(data), start, 0x100, 0)
           + b'SFAT' + struct.pack('<HHI', 12, len(names), 0x65) + nodes + b'SFNT' + struct.pack('<HH', 8, 0) + sfnt)
    return out + bytes(start - len(out)) + data


def sarc_files(data):
    header, start = struct.unpack_from('<H', data, 4)[0], struct.unpack_from('<I', data, 12)[0]
    count, out = struct.unpack_from('<H', data, header + 6)[0], {}
    names = header + 12 + count * 16 + 8
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from('<4I', data, header + 12 + i * 16)
        at = names + (attribute & 0xffffff) * 4
        out[data[at:data.index(0, at)].decode()] = data[start + lo:start + hi]
    return out


def bflan(panes):
    """A FLAN whose pai1 section steps each pane's visibility from 0 at frame 0 to 1 at frame 1."""
    entries = [name.encode().ljust(28, b'\0') + bytes([1, 0, 0, 0]) + struct.pack('<I', 36)
               + b'FLVI' + bytes([1, 0, 0, 0]) + struct.pack('<I', 12)
               + bytes([0, 0, 1, 0]) + struct.pack('<HHI', 2, 0, 12)
               + struct.pack('<fHxx', 0.0, 0) + struct.pack('<fHxx', 1.0, 1) for name in panes]
    table, body = 20, b''
    offsets = []
    for e in entries:
        offsets.append(table + 4 * len(entries) + len(body))
        body += e
    section = struct.pack('<HBxHHI', 0, 0, 0, len(entries), table) + struct.pack(f'<{len(entries)}I', *offsets) + body
    section = b'pai1' + struct.pack('<I', 8 + len(section)) + section
    return b'FLAN' + struct.pack('<HHIIHH', 0xFEFF, 0x14, 0x08000000, 0x14 + len(section), 1, 0) + section


def visibility(data):
    """pane -> [(frame, value)] of a FLAN made by bflan()."""
    pos, out = 0x14, {}
    count, table = struct.unpack_from('<H', data, pos + 14)[0], pos + struct.unpack_from('<I', data, pos + 16)[0]
    for e in range(count):
        eo = pos + struct.unpack_from('<I', data, table + 4 * e)[0]
        keys = eo + 36 + 12 + 12
        out[data[eo:eo + 28].split(b'\0')[0].decode()] = [struct.unpack_from('<fH', data, keys + 8 * k) for k in range(2)]
    return out


def bntx(textures):
    """A BNTX of single-mip square textures: name -> (format, size, image)."""
    out = bytearray(0x30)
    out[0:4], out[0x20:0x24] = b'BNTX', b'NX  '
    table = len(out)
    out += bytes(8 * len(textures))
    struct.pack_into('<IQ', out, 0x24, len(textures), table)
    for k, (name, (fmt, size, image)) in enumerate(textures.items()):
        info = len(out)
        out += bytes(0x80)
        out[info:info + 4] = b'BRTI'
        struct.pack_into('<Q', out, table + 8 * k, info)
        struct.pack_into('<H', out, info + 0x16, 1)
        struct.pack_into('<I', out, info + 0x1c, fmt)
        struct.pack_into('<II', out, info + 0x24, size, size)
        struct.pack_into('<I', out, info + 0x34, 4)   # block height 1 << 4 GOBs
        struct.pack_into('<I', out, info + 0x50, len(image))
        struct.pack_into('<Q', out, info + 0x60, len(out))
        out += struct.pack('<H', len(name)) + name.encode() + b'\0'
        out += bytes(-len(out) % 8)
        mips = len(out)
        struct.pack_into('<Q', out, info + 0x70, mips)
        out += bytes(8)
        out += bytes(-len(out) % 0x200)
        struct.pack_into('<Q', out, mips, len(out))
        out += image
    return bytes(out)


def bntx_images(data):
    """name without the ^ suffix -> (format, size, image) of a BNTX made by bntx()."""
    count, table = struct.unpack_from('<IQ', data, 0x24)
    out = {}
    for k in range(count):
        info = struct.unpack_from('<Q', data, table + 8 * k)[0]
        at = struct.unpack_from('<Q', data, info + 0x60)[0]
        name = data[at + 2:at + 2 + struct.unpack_from('<H', data, at)[0]].decode().split('^')[0]
        start = struct.unpack_from('<Q', data, struct.unpack_from('<Q', data, info + 0x70)[0])[0]
        out[name] = (struct.unpack_from('<I', data, info + 0x1c)[0], struct.unpack_from('<I', data, info + 0x24)[0],
                     data[start:start + struct.unpack_from('<I', data, info + 0x50)[0]])
    return out


def bflyt(panes):
    """A FLYT with one txt1 section per pane (name -> text) after a pan1 section."""
    sections = b'pan1' + struct.pack('<I', 0x4c) + bytes(0x44)
    for name, text in panes.items():
        s = bytearray(0x80)
        s[0:4] = b'txt1'
        struct.pack_into('<I', s, 4, len(s))
        s[12:12 + len(name)] = name.encode()
        struct.pack_into('<H', s, 0x56, 2 * len(text) + 2)
        struct.pack_into('<I', s, 0x64, 0x70)
        s[0x70:0x70 + 2 * len(text) + 2] = text.encode('utf-16-le') + b'\0\0'
        sections += s
    return b'FLYT' + struct.pack('<HHIIHH', 0xFEFF, 0x14, 0x08000000, 0x14 + len(sections), len(panes) + 1, 0) + sections


def pane_texts(data):
    pos, out = struct.unpack_from('<H', data, 6)[0], {}
    while pos + 8 <= len(data):
        magic, size = data[pos:pos + 4], struct.unpack_from('<I', data, pos + 4)[0]
        if magic == b'txt1':
            length, at = struct.unpack_from('<H', data, pos + 0x56)[0], pos + struct.unpack_from('<I', data, pos + 0x64)[0]
            out[data[pos + 12:pos + 36].split(b'\0')[0].decode()] = data[at:at + length - 2].decode('utf-16-le')
        pos += size
    return out


def msbt(messages):
    """A UTF-16 MSBT: one LBL1 group with every label, then TXT2 (label -> text)."""
    labels = b''.join(bytes([len(name)]) + name.encode() + struct.pack('<I', i) for i, name in enumerate(messages))
    lbl1 = struct.pack('<III', 1, len(messages), 12) + labels
    texts = [text.encode('utf-16-le') + b'\0\0' for text in messages.values()]
    offsets, body = [], b''
    for text in texts:
        offsets.append(4 + 4 * len(texts) + len(body))
        body += text
    txt2 = struct.pack(f'<I{len(texts)}I', len(texts), *offsets) + body

    def section(magic, content):
        s = magic + struct.pack('<I', len(content)) + bytes(8) + content
        return s + bytes(-len(s) % 16)
    sections = section(b'LBL1', lbl1) + section(b'TXT2', txt2)
    return b'MsgStdBn' + struct.pack('<HHBBHHI', 0xFEFF, 0, 1, 3, 2, 0, 0x20 + len(sections)) + bytes(10) + sections


def message_texts(data):
    sections, pos = {}, 0x20
    for _ in range(struct.unpack_from('<H', data, 0x0e)[0]):
        size = struct.unpack_from('<I', data, pos + 4)[0]
        sections[data[pos:pos + 4]] = (pos + 16, size)
        pos = (pos + 16 + size + 15) & ~15
    (base, _), labels = sections[b'LBL1'], {}
    for g in range(struct.unpack_from('<I', data, base)[0]):
        count, at = struct.unpack_from('<II', data, base + 4 + 8 * g)
        at += base
        for _ in range(count):
            n = data[at]
            labels[struct.unpack_from('<I', data, at + 1 + n)[0]] = data[at + 1:at + 1 + n].decode()
            at += 1 + n + 4
    base, size = sections[b'TXT2']
    count = struct.unpack_from('<I', data, base)[0]
    offsets = list(struct.unpack_from(f'<{count}I', data, base + 4)) + [size]
    return {labels[i]: data[base + offsets[i]:base + offsets[i + 1]].decode('utf-16-le').rstrip('\0') for i in range(count)}


def restbl(entries):
    """A RESTBL keyed by the CRC32 of each path without .zs (path -> size)."""
    crcs = sorted((zlib.crc32(path.removesuffix('.zs').encode()), size) for path, size in entries.items())
    return b'RESTBL' + struct.pack('<IIII', 1, 160, len(crcs), 0) + b''.join(struct.pack('<II', *c) for c in crcs)


def restbl_sizes(table):
    count = struct.unpack_from('<I', table, 14)[0]
    return dict(struct.unpack_from('<II', table, 22 + 8 * i) for i in range(count))


MAGIC = 0x18029a7f   # MacNativeGames make_button_icons.py: a .bfotf's first word is its key XOR this


def bfotf(otf, key):
    """A .bfotf as MacNativeGames make_button_icons.py bfotf_encode writes it: 32-bit words XORed with the key."""
    size = len(otf)
    out = struct.pack('<II', MAGIC ^ key, struct.unpack('<I', size.to_bytes(4, 'big'))[0] ^ key)
    out += b''.join(struct.pack('<I', struct.unpack_from('<I', otf, i)[0] ^ key) for i in range(0, size - size % 4, 4))
    return out + otf[size - size % 4:]


def unbfotf(blob):
    key = struct.unpack_from('<I', blob, 0)[0] ^ MAGIC
    size = int.from_bytes(struct.pack('<I', struct.unpack_from('<I', blob, 4)[0] ^ key), 'big')
    words = b''.join(struct.pack('<I', struct.unpack_from('<I', blob, 8 + i)[0] ^ key) for i in range(0, size - size % 4, 4))
    return words + blob[8 + size - size % 4:8 + size]


def rect(pen, x0, y0, x1, y1, hole=False):
    """A rectangle, counter-clockwise (filled, the CFF way) or clockwise (a counter)."""
    points = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
    points = points[::-1] if hole else points
    pen.moveTo(points[0])
    for p in points[1:]:
        pen.lineTo(p)
    pen.closePath()


def polygon(*points):
    def draw(pen):
        pen.moveTo(points[0])
        for p in points[1:]:
            pen.lineTo(p)
        pen.closePath()
    return draw


def cff_font(name, glyphs):
    """A CFF OpenType font with a 1000-unit em: code point -> (advance, draw(pen))."""
    names = {cp: f'uni{cp:04X}' for cp in glyphs}
    fb = FontBuilder(1000, isTTF=False)
    fb.setupGlyphOrder(['.notdef'] + list(names.values()))
    fb.setupCharacterMap(names)
    charstrings, metrics = {}, {}
    for gname, (advance, draw) in [('.notdef', (500, lambda pen: rect(pen, 50, 0, 450, 700)))] + \
            [(names[cp], glyphs[cp]) for cp in glyphs]:
        pen = T2CharStringPen(advance, None)
        draw(pen)
        charstrings[gname], metrics[gname] = pen.getCharString(), (advance, 0)
    fb.setupCFF(name, {'FullName': name}, charstrings, {})
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({'familyName': name, 'styleName': 'Regular'})
    fb.setupOS2(sTypoAscender=800, usWinAscent=800, usWinDescent=200)
    fb.setupPost()
    out = io.BytesIO()
    fb.save(out)
    return out.getvalue()


# A distinct rectangle and advance for every button glyph, so a glyph that moved is found by its outline.
BUTTONS = {cp: (600 + 4 * (cp - 0xE040), lambda pen, i=cp - 0xE040: rect(pen, 20 + 5 * i, 20, 300 + 5 * i, 400 + 3 * i))
           for cp in range(0xE040, 0xE069)}
LETTERS_1 = {ord('L'): (560, polygon((60, 0), (480, 0), (480, 100), (160, 100), (160, 700), (60, 700))),
             ord('B'): (560, lambda pen: [rect(pen, 60, 0, 460, 700), rect(pen, 160, 100, 360, 300, True),
                                          rect(pen, 160, 400, 360, 600, True)]),
             ord('R'): (560, lambda pen: [rect(pen, 60, 0, 460, 700), rect(pen, 160, 400, 360, 600, True)])}
LETTERS_2 = {ord('T'): (560, polygon((230, 0), (330, 0), (330, 600), (520, 600), (520, 700), (40, 700), (40, 600), (230, 600)))}


class Romfs:
    """A RomFS (romfs.bin, index.json) holding the files make_romfs_patches.py reads, the originals kept to compare."""

    def __init__(self, folder, version, text_fonts=True, restbl_skips=(), l_format=R8_UNORM):
        v, rnd = VERSIONS[version], random.Random(version)
        words = [b'Layout', b'Pane', b'Font', b'Msg', b'Texture', b'Anim', b'Group', b'Text', b'Base', b'Key']
        samples = [b' '.join(rnd.choice(words) + str(rnd.randrange(999)).encode() for _ in range(rnd.randrange(20, 80)))
                   for _ in range(400)]
        self.dictionary = zstd.train_dict(samples, 4096)
        self.common = f'UI/LayoutArchive/Common.Product.{v["common"]}.Nin_NX_NVN.blarc.zs'
        self.mals = [f'Mals/EUen.Product.{v["newest"]}.sarc.zs', f'Mals/USen.Product.{v["newest"]}.sarc.zs']
        self.old_mals = 'Mals/USen.Product.110.sarc.zs'
        self.restbl = f'System/Resource/ResourceSizeTable.Product.{v["newest"]}.{v["restbl"]}'
        self.old_restbl = f'System/Resource/ResourceSizeTable.Product.100.{v["restbl"]}'   # an older version's: left alone
        size = v['size']
        images = {f'Nt_KeyTex{b}_00': (R8_UNORM, size, rnd.randbytes(IMAGE)) for b in 'ABXY'}
        images.update({'Nt_KeyTexL_00^z': (l_format, size, rnd.randbytes(IMAGE)),
                       'Nt_KeyTexZL_00': (R8_UNORM, size, rnd.randbytes(IMAGE)),
                       'Nt_KeyTexLSt_00': (R8_UNORM, size, rnd.randbytes(IMAGE))})
        self.text_fonts = [bfotf(cff_font('Text1', LETTERS_1), 0x01020304), bfotf(cff_font('Text2', LETTERS_2), 0x0a0b0c0d)]
        default = {'scft/External.bfotf': bfotf(cff_font('External', BUTTONS), 0x5a17c3e9), 'fnt/rest.bin': rnd.randbytes(700)}
        if text_fonts:
            default.update(zip(icons.TEXT_FONTS, self.text_fonts))
        mals = sarc({'LayoutMsg/DoCommand_00.msbt': msbt(MESSAGES), 'LayoutMsg/Other_00.msbt': msbt({'T_A_00': 'A'})})
        self.contents = {   # RomFS path -> contents before compression
            self.common: sarc({'anim/ShortCutBG_00_In.bflan': bflan(('C_CaptureBG_00', 'C_CaptureClearBG_00',
                                                                      'P_CaptureClearBG_00', 'N_Other_00')),
                               'anim/AutoBuilder_00_In.bflan': bflan(('C_CaptureBG_00',)),
                               'blyt/DoCommand_00.bflyt': bflyt(PANES),
                               'blyt/Title_00.bflyt': bflyt({'N_Title_00': 'Z'}),
                               'timg/__Combined.bntx': bntx(images)}),
            DEFAULT_FONT: sarc(default),
            # The same font under another key: drawn once, stored under each archive's own key.
            REGIONAL_FONT: sarc({'scft/External.bfotf': bfotf(cff_font('External', BUTTONS), 0x77777777),
                                 'fnt/rest.bin': rnd.randbytes(500)}),
            **{path: mals for path in self.mals + [self.old_mals]},
        }
        sizes = {path: len(data) + 256 for path, data in self.contents.items() if path not in restbl_skips}
        self.contents[self.restbl] = restbl({**sizes, **{f'Other/{i}.bin': 1000 + i for i in range(20)}})
        self.contents[self.old_restbl] = restbl({DEFAULT_FONT: 1})
        plain = (REGIONAL_FONT, self.restbl, self.old_restbl)   # stored without a dictionary
        self.files = {'Pack/ZsDic.pack.zs': zstd.compress(sarc({'zs.zsdic': self.dictionary.dict_content}))}
        self.files.update({path: zstd.compress(data, zstd_dict=None if path in plain else self.dictionary)
                           for path, data in self.contents.items()})
        self.files['Other/file.bin'] = rnd.randbytes(100)
        image, entries = bytearray(), []
        for path, data in self.files.items():
            image += bytes(-len(image) % 16)
            entries.append(dict(path=path, offset=len(image), size=len(data)))
            image += data
        folder.mkdir(parents=True)
        (folder / 'romfs.bin').write_bytes(image)
        (folder / 'index.json').write_text(json.dumps(dict(status='pass', files=entries)))
        self.folder, self.version = folder, version

    def run(self, out, *flags):
        return subprocess.run([sys.executable, '-I', str(TOOL), '--version', self.version, '--romfs', str(self.folder),
                               '--out', str(out), *flags], capture_output=True, text=True, encoding='utf-8')

    def read(self, out, path):
        """The overlay's file, decompressed."""
        raw = (out / path).read_bytes()
        return zstd.decompress(raw, zstd_dict=self.dictionary if zstd.get_frame_info(raw).dictionary_id else None)


def font(blob):
    return TTFont(io.BytesIO(unbfotf(blob)))


def glyph(f, cp):
    """(outline, advance) of the glyph of code point cp."""
    name = f.getBestCmap()[cp]
    pen = RecordingPen()
    f.getGlyphSet()[name].draw(pen)
    return pen.value, f['hmtx'][name][0]


def manifest(out):
    return {line.split('\t')[0]: line.split('\t')[1:] for line in (out / 'manifest.tsv').read_text().splitlines()
            if not line.startswith('#')}


class PromptsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='totk-prompts-')
        cls.base = Path(cls.tmp.name)
        cls.romfs = {v: Romfs(cls.base / v / 'romfs', v) for v in VERSIONS}
        cls.outs = {}
        for v, romfs in cls.romfs.items():
            for style in ('plain', 'xbox', 'xbox-label'):
                out = cls.base / v / style
                proc = romfs.run(out, *(() if style == 'plain' else ('--prompts', style)))
                assert proc.returncode == 0, proc.stderr
                cls.outs[v, style] = (out, json.loads(proc.stdout.strip().splitlines()[-1]))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def each(self, styles=('xbox', 'xbox-label')):
        """(RomFS, style, overlay folder, summary) of every run of these styles."""
        return [(self.romfs[v], style, out, summary) for (v, style), (out, summary) in self.outs.items()
                if style in styles]

    def test_plain_overlay_has_only_the_frame_rate_patch(self):
        for romfs, _, out, summary in self.each(('plain',)):
            with self.subTest(version=romfs.version, style="plain"):
                self.assertEqual(set(manifest(out)), {romfs.common})
                self.assertIsNone(summary['prompts'])
                keys = visibility(sarc_files(romfs.read(out, romfs.common))['anim/ShortCutBG_00_In.bflan'])
                self.assertEqual(keys['C_CaptureBG_00'], [(0.0, 0), (0.0, 1)])
                self.assertEqual(keys['N_Other_00'], [(0.0, 0), (1.0, 1)])

    def test_face_buttons_name_the_xbox_label_of_the_button_pressed(self):
        for romfs, style, out, _ in self.each():
            with self.subTest(version=romfs.version, style=style):
                layout = STYLE_LAYOUT[style]
                label = {b: XBOX_LABEL[POSITION[layout][b]] for b in FACE}   # Switch button -> the label of its button
                before, after = sarc_files(romfs.contents[romfs.common]), sarc_files(romfs.read(out, romfs.common))
                old, new = bntx_images(before['timg/__Combined.bntx']), bntx_images(after['timg/__Combined.bntx'])
                panes, messages = pane_texts(after['blyt/DoCommand_00.bflyt']), {}
                for path in romfs.mals if layout == 'nintendo' else []:
                    messages[path] = message_texts(sarc_files(romfs.read(out, path))['LayoutMsg/DoCommand_00.msbt'])
                original = font(sarc_files(romfs.contents[DEFAULT_FONT])['scft/External.bfotf'])
                for path in (DEFAULT_FONT, REGIONAL_FONT):
                    drawn = font(sarc_files(romfs.read(out, path))['scft/External.bfotf'])
                    for b, cp in FACE.items():
                        self.assertEqual(glyph(drawn, cp), glyph(original, FACE[label[b]]), f'{path}: glyph of {b}')
                for b in FACE:
                    self.assertEqual(new[f'Nt_KeyTex{b}_00'], old[f'Nt_KeyTex{label[b]}_00'], f'action prompt image of {b}')
                    self.assertEqual(panes[f'T_{b}_00'], label[b], f'diamond letter of {b}')
                    for path, texts in messages.items():
                        self.assertEqual(texts[f'T_{b}_00'], label[b], f'{path}: diamond message of {b}')
                # Text that only looks like a letter pane, and every other message, stays.
                self.assertEqual((panes['T_Name_00'], pane_texts(after['blyt/Title_00.bflyt'])), ('A', {'N_Title_00': 'Z'}))
                for path, texts in messages.items():
                    self.assertEqual((texts['T_Name_00'], texts['Greeting']), ('A', 'Hello'))
                    self.assertEqual(sarc_files(romfs.read(out, path))['LayoutMsg/Other_00.msbt'], msbt({'T_A_00': 'A'}))

    def test_other_buttons_are_drawn_as_xbox_icons(self):
        for romfs, style, out, _ in self.each():
            with self.subTest(version=romfs.version, style=style):
                original = font(sarc_files(romfs.contents[DEFAULT_FONT])['scft/External.bfotf'])
                default = sarc_files(romfs.read(out, DEFAULT_FONT))
                drawn = font(default['scft/External.bfotf'])
                for cp in range(0xE040, 0xE069):
                    outline, advance = glyph(drawn, cp)
                    if cp in DRAWN:
                        self.assertNotEqual(outline, glyph(original, cp)[0], f'U+{cp:04X} drawn')
                        self.assertEqual(advance, DRAWN[cp], f'U+{cp:04X} advance')
                    elif cp not in FACE.values():
                        self.assertEqual((outline, advance), glyph(original, cp), f'U+{cp:04X} kept')
                # The letters are holes in the key: LB is the key, L, and B with its two counters.
                moves = sum(op == 'moveTo' for op, _ in glyph(drawn, 0xE044)[0])
                self.assertEqual(moves, 5)
                self.assertEqual(sum(op == 'moveTo' for op, _ in glyph(drawn, 0xE047)[0]), 1 + 1 + 1 + 1)   # RT: key, R + counter, T
                self.assertEqual(sum(op == 'moveTo' for op, _ in glyph(drawn, 0xE060)[0]), 2)              # d-pad up: cross, arrow
                # The text fonts and the archives' other members stay byte for byte; both archives hold the same font.
                before = sarc_files(romfs.contents[DEFAULT_FONT])
                self.assertEqual({k: v for k, v in default.items() if k != 'scft/External.bfotf'},
                                 {k: v for k, v in before.items() if k != 'scft/External.bfotf'})
                regional = sarc_files(romfs.read(out, REGIONAL_FONT))
                self.assertEqual(regional['fnt/rest.bin'], sarc_files(romfs.contents[REGIONAL_FONT])['fnt/rest.bin'])
                self.assertEqual(unbfotf(regional['scft/External.bfotf']), unbfotf(default['scft/External.bfotf']))
                self.assertEqual(struct.unpack_from('<I', regional['scft/External.bfotf'])[0] ^ MAGIC, 0x77777777)

    def test_button_guide_images_show_the_same_icons(self):
        for romfs, style, out, _ in self.each():
            with self.subTest(version=romfs.version, style=style):
                size = VERSIONS[romfs.version]['size']
                before = sarc_files(romfs.contents[romfs.common])
                after = sarc_files(romfs.read(out, romfs.common))
                self.assertEqual(len(after['timg/__Combined.bntx']), len(before['timg/__Combined.bntx']))
                old, new = bntx_images(before['timg/__Combined.bntx']), bntx_images(after['timg/__Combined.bntx'])
                glyphs = icons.xbox_icons(icons.text_glyphs(romfs.text_fonts))
                for name, cp in icons.HUD_TEXTURES.items():
                    fmt, width, image = new[name]
                    pixels = bytes(image[icons.gob_offset(x, y, (size + 63) // 64, 16)] for y in range(size) for x in range(size))
                    self.assertEqual(pixels, icons.rasterize(glyphs[cp][1], size), name)
                    self.assertEqual({pixels[0], pixels[-1], max(pixels)}, {0, 255}, name)   # a margin around a white key
                self.assertEqual(new['Nt_KeyTexLSt_00'], old['Nt_KeyTexLSt_00'])
                for member in before:   # the 60 fps patch and the prompts touch only these
                    if member not in ('timg/__Combined.bntx', 'blyt/DoCommand_00.bflyt') and not member.startswith('anim/'):
                        self.assertEqual(after[member], before[member], member)

    def test_resource_size_table_grows_with_the_fonts(self):
        for romfs, style, out, summary in self.each():
            with self.subTest(version=romfs.version, style=style):
                table = restbl_sizes(romfs.read(out, romfs.restbl))
                original = restbl_sizes(romfs.contents[romfs.restbl])
                grown = {}
                for path in manifest(out):
                    if path != romfs.restbl:
                        grown[path] = len(romfs.read(out, path)) - len(romfs.contents[path])
                self.assertTrue(grown[DEFAULT_FONT] > 0 and grown[REGIONAL_FONT] > 0, grown)
                self.assertEqual(summary['prompts']['grown'], {p: g for p, g in grown.items() if g > 0})
                for crc, entry in original.items():
                    paths = [p for p in grown if zlib.crc32(p.removesuffix('.zs').encode()) == crc and grown[p] > 0]
                    self.assertEqual(table[crc], entry + (grown[paths[0]] if paths else 0))
                self.assertEqual(set(table), set(original))

    def test_manifest_lists_what_changed_with_both_hashes(self):
        for romfs, style, out, _ in self.each(('plain', 'xbox', 'xbox-label')):
            with self.subTest(version=romfs.version, style=style):
                expect = {romfs.common}
                if style != 'plain':
                    expect |= {DEFAULT_FONT, REGIONAL_FONT, romfs.restbl} | (set(romfs.mals) if style == 'xbox' else set())
                lines = manifest(out)
                self.assertEqual(set(lines), expect)
                for path, (size, sha, original) in lines.items():
                    data = (out / path).read_bytes()
                    self.assertEqual((int(size), sha), (len(data), hashlib.sha256(data).hexdigest()), path)
                    self.assertEqual(original, hashlib.sha256(romfs.files[path]).hexdigest(), path)

    def test_same_input_same_output_and_the_older_flag(self):
        romfs = self.romfs['v121']
        for flags in (('--prompts', 'xbox'), ('--xbox-prompts',)):
            out = self.base / 'again' / '-'.join(flags)
            proc = romfs.run(out, *flags)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual((out / 'manifest.tsv').read_bytes(), (self.outs['v121', 'xbox'][0] / 'manifest.tsv').read_bytes())

    def test_unrecognised_romfs_fails_before_writing(self):
        cases = {'no text fonts': (dict(text_fonts=False), icons.TEXT_FONTS[0]),
                 'no size entry': (dict(restbl_skips=(REGIONAL_FONT,)), 'has no entry in the resource size table'),
                 'compressed L image': (dict(l_format=BC1), 'Nt_KeyTexL_00: format')}
        for i, (case, (options, message)) in enumerate(cases.items()):
            with self.subTest(case):
                romfs = Romfs(self.base / 'bad' / str(i) / 'romfs', 'v121', **options)
                out = self.base / 'bad' / str(i) / 'out'
                proc = romfs.run(out, '--prompts', 'xbox-label')
                self.assertNotEqual(proc.returncode, 0)
                self.assertIn(message, proc.stderr)
                self.assertFalse(out.exists(), 'a failed run writes nothing')


if __name__ == '__main__':
    unittest.main()
