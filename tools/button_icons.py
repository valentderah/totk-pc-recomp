# Adapted from MacNativeGames games/totk/scripts/make_button_icons.py @ 933b6a0 (MIT, Copyright (c) 2026
# MacNativeGames contributors).
"""Xbox button icons for the game's prompts, drawn on the player's machine for tools/make_romfs_patches.py --prompts.
The outlines are our own shapes with letters cut out of them in the game's own text fonts: nothing of the game is in
the repository.

Every prompt except the button guide in play is text in the button font, scft/External.bfotf of each
Font/Font*.Nin_NX_NVN.bfarc.zs: private-use code points from U+E040 (A, B, X, Y, then L, R, ZL, ZR, the d-pad, Plus and
Minus, the sticks). A .bfotf is an OpenType (CFF) font after an 8-byte header; from offset 4 on, every byte is XORed
with a 4-byte key, which the size in the header's second word gives away (bfotf_decode). redraw_font draws the shoulder
buttons, the triggers, Plus, Minus and the d-pad as an Xbox controller names them (LB RB, LT RT, Menu and View, a
cross), can swap the glyphs of the face buttons (A with B, X with Y), and has fontTools write the font again. The
archive then grows by a kilobyte or two, more than the game's resource size table allows it: grow_restbl raises those
entries (without it the game stops at start, MacNativeGames found).

The button guide in play (bottom right) draws L and ZL from images of the common layout archive instead
(timg/__Combined.bntx, Nt_KeyTexL_00 and Nt_KeyTexZL_00): bntx_redraw paints the same icons into them, the size
unchanged. The sticks keep the Switch icons, and so do prompts drawn as fixed images (some tutorial animations).

Needs fontTools (tools/requirements-installer.txt, tools/requirements-py314.txt).
"""
import io
import struct
import zlib

from fontTools.pens.basePen import BasePen
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.recordingPen import RecordingPen
from fontTools.pens.reverseContourPen import ReverseContourPen
from fontTools.pens.t2CharStringPen import T2CharStringPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

FONT = 'scft/External.bfotf'
# The letters on the keys come from the text fonts of the default archive; the regional archives lack them.
TEXT_ARCHIVE = 'Font/Font.Nin_NX_NVN.bfarc.zs'
TEXT_FONTS = ('scft/ninP_RodinNTLG-B_DH_003_subset.bfotf', 'scft/nintendo_NTLG-DB_DH_002.bfotf')
# The button guide's images and the glyph each one shows.
HUD_TEXTURES = {'Nt_KeyTexL_00': 0xE044, 'Nt_KeyTexZL_00': 0xE046}


def xor(data, key):
    """data with byte i XORed with key[i % 4]."""
    pad = (key * (len(data) // 4 + 1))[:len(data)]
    return (int.from_bytes(data, 'little') ^ int.from_bytes(pad, 'little')).to_bytes(len(data), 'little')


def bfotf_decode(blob):
    """(OpenType font, key) of a .bfotf (the module docstring). The font fills the rest of the file."""
    key = xor(blob[4:8], struct.pack('>I', len(blob) - 8))
    otf = xor(blob[8:], key)
    assert otf[:4] == b'OTTO', 'not a CFF OpenType font'
    return otf, key


def bfotf_encode(otf, key, head):
    """The .bfotf of otf: head (the original's first word) and the size, the rest XORed with key as bfotf_decode reads
    it. The size field holds the whole font, so its length has to be a multiple of 4 (fontTools pads its tables)."""
    assert len(otf) % 4 == 0, len(otf)
    return head[:4] + xor(struct.pack('>I', len(otf)) + otf, key)


# ---- drawing, in units of the font's 1000-unit em; the round A/B/X/Y icons fill y 20..585
Y0, Y1 = 20, 585
CY, R = (Y0 + Y1) / 2, (Y1 - Y0) / 2
K = 0.5523   # control point distance of a quarter circle drawn with one cubic curve, per unit of radius


class Glyphs:
    """Outlines of letters from the game's own text fonts, drawn as holes in a shape."""
    def __init__(self, fonts):
        self.fonts = fonts

    def outline(self, ch):
        for font in self.fonts:
            cmap = font.getBestCmap()
            if ord(ch) in cmap:
                glyphs = font.getGlyphSet(); g = glyphs[cmap[ord(ch)]]
                rec = RecordingPen(); g.draw(rec); box = BoundsPen(glyphs); g.draw(box)
                return rec, box.bounds, font['head'].unitsPerEm, g.width
        raise KeyError(f'no text font has {ch!r}')

    def hole(self, pen, text, cx, cy, cap, max_width):
        """text centred on (cx, cy), at most cap high and max_width wide, its contours reversed so they cut holes."""
        parts, x, ink = [], 0.0, [1e9, 1e9, -1e9, -1e9]
        for ch in text:
            rec, b, upm, advance = self.outline(ch); k = 1000 / upm
            parts.append((rec, k, x))
            ink = [min(ink[0], x + b[0] * k), min(ink[1], b[1] * k), max(ink[2], x + b[2] * k), max(ink[3], b[3] * k)]
            x += advance * k * 0.96
        width, height = ink[2] - ink[0], ink[3] - ink[1]
        scale = min(cap / height, max_width / width)
        ox, oy = cx - (ink[0] + width / 2) * scale, cy - (ink[1] + height / 2) * scale
        reverse = ReverseContourPen(pen)
        for rec, k, x in parts:
            rec.replay(TransformPen(reverse, (scale * k, 0, 0, scale * k, ox + x * scale, oy)))


def text_glyphs(blobs):
    """Glyphs over the text fonts in blobs (.bfotf files), the first that has a letter giving it."""
    return Glyphs([TTFont(io.BytesIO(bfotf_decode(blob)[0])) for blob in blobs])


def circle(pen, cx, cy, r):
    k = K * r
    pen.moveTo((cx + r, cy))
    pen.curveTo((cx + r, cy + k), (cx + k, cy + r), (cx, cy + r))
    pen.curveTo((cx - k, cy + r), (cx - r, cy + k), (cx - r, cy))
    pen.curveTo((cx - r, cy - k), (cx - k, cy - r), (cx, cy - r))
    pen.curveTo((cx + k, cy - r), (cx + r, cy - k), (cx + r, cy))
    pen.closePath()


def rounded(pen, x0, y0, x1, y1, bottom, top=None):
    """Counter-clockwise rounded rectangle (bottom and top corner radii)."""
    top = bottom if top is None else top
    kb, kt = K * bottom, K * top
    pen.moveTo((x0 + bottom, y0)); pen.lineTo((x1 - bottom, y0))
    pen.curveTo((x1 - bottom + kb, y0), (x1, y0 + bottom - kb), (x1, y0 + bottom)); pen.lineTo((x1, y1 - top))
    pen.curveTo((x1, y1 - top + kt), (x1 - top + kt, y1), (x1 - top, y1)); pen.lineTo((x0 + top, y1))
    pen.curveTo((x0 + top - kt, y1), (x0, y1 - top + kt), (x0, y1 - top)); pen.lineTo((x0, y0 + bottom))
    pen.curveTo((x0, y0 + bottom - kb), (x0 + bottom - kb, y0), (x0 + bottom, y0)); pen.closePath()


def polygon(pen, points):
    pen.moveTo(points[0])
    for p in points[1:]:
        pen.lineTo(p)
    pen.closePath()


def xbox_icons(g):
    """Code point -> (advance, draw(pen)) for the buttons an Xbox controller names differently from a Switch: L R = LB
    RB, ZL ZR = LT RT, Plus and Minus = Menu and View, the d-pad as a cross. g: Glyphs for the letters. The face
    buttons keep the game's own glyphs (redraw_font swaps them for the Nintendo layout)."""
    def bumper(label, advance=820):
        def draw(pen):
            rounded(pen, 40, Y0 + 70, advance - 40, Y1 - 70, 70, 150); g.hole(pen, label, advance / 2, CY, 230, advance - 200)
        return advance, draw

    def trigger(label, advance=760):
        def draw(pen):
            rounded(pen, 60, Y0, advance - 60, Y1, 60, 230); g.hole(pen, label, advance / 2, CY - 20, 230, advance - 220)
        return advance, draw

    def menu(advance=640):   # three bars, as on the Menu button
        def draw(pen):
            circle(pen, advance / 2, CY, R); reverse = ReverseContourPen(pen)
            for dy in (-115, 0, 115):
                rounded(reverse, advance / 2 - 150, CY + dy - 38, advance / 2 + 150, CY + dy + 38, 38)
        return advance, draw

    def view(advance=640):   # two overlapping windows, as on the View button
        def draw(pen):
            circle(pen, advance / 2, CY, R); reverse = ReverseContourPen(pen); s = 150
            rounded(reverse, advance / 2 - s - 15, CY + 15, advance / 2 - 15, CY + s + 15, 16)
            rounded(reverse, advance / 2 + 15, CY - s - 15, advance / 2 + s + 15, CY - 15, 16)
        return advance, draw

    def dpad(*lit, advance=640):   # a cross with an arrow cut into each direction the prompt means
        def draw(pen):
            cx, length, arm = advance / 2, R, R * 0.40
            polygon(pen, [(cx + length, CY - arm), (cx + length, CY + arm), (cx + arm, CY + arm), (cx + arm, CY + length),
                          (cx - arm, CY + length), (cx - arm, CY + arm), (cx - length, CY + arm), (cx - length, CY - arm),
                          (cx - arm, CY - arm), (cx - arm, CY - length), (cx + arm, CY - length), (cx + arm, CY - arm)])
            m, t, h = (length + arm) / 2, arm * 0.72, arm * 0.80   # arm centre, half base and height of the arrow
            heads = {'up': [(cx - t, CY + m - h / 2), (cx, CY + m + h / 2), (cx + t, CY + m - h / 2)],
                     'down': [(cx + t, CY - m + h / 2), (cx, CY - m - h / 2), (cx - t, CY - m + h / 2)],
                     'left': [(cx - m + h / 2, CY - t), (cx - m - h / 2, CY), (cx - m + h / 2, CY + t)],
                     'right': [(cx + m - h / 2, CY + t), (cx + m + h / 2, CY), (cx + m - h / 2, CY - t)]}
            for d in lit:
                polygon(pen, heads[d])   # already clockwise: a hole
        return advance, draw

    return {
        0xE044: bumper('LB'), 0xE045: bumper('RB'), 0xE046: trigger('LT'), 0xE047: trigger('RT'),
        0xE04B: menu(), 0xE04C: view(),
        0xE049: dpad('up', 'down', 'left', 'right'), 0xE060: dpad('up'), 0xE061: dpad('down'),
        0xE062: dpad('left'), 0xE063: dpad('right'), 0xE064: dpad('up', 'down'), 0xE065: dpad('left', 'right'),
    }


def redraw_font(otf, icons, swaps=()):
    """The OpenType font otf with the glyphs of each (code point, code point) pair of swaps trading places (outlines and
    horizontal metrics), then every code point of icons (code point -> (advance, draw)) drawn anew, written again by
    fontTools. The timestamps stay, so the same input gives the same bytes."""
    font = TTFont(io.BytesIO(otf), recalcTimestamp=False)
    assert font['head'].unitsPerEm == 1000, f'em of {font["head"].unitsPerEm} units, the drawings assume 1000'
    cff = font['CFF '].cff
    top = cff.topDictIndex[0]
    assert not hasattr(top, 'ROS'), 'CID-keyed CFF'   # its glyphs can use different private dictionaries
    charstrings, private, hmtx, cmap = top.CharStrings, top.Private, font['hmtx'], font.getBestCmap()
    nominal = getattr(private, 'nominalWidthX', 0)
    for a, b in swaps:
        ga, gb = cmap[a], cmap[b]
        outline_a, outline_b = charstrings[ga], charstrings[gb]
        charstrings[ga], charstrings[gb] = outline_b, outline_a
        hmtx[ga], hmtx[gb] = hmtx[gb], hmtx[ga]
    for cp, (advance, draw) in icons.items():
        pen = T2CharStringPen(advance - nominal, None); draw(pen)   # the width operand is relative to nominalWidthX
        charstrings[cmap[cp]] = pen.getCharString(private=private, globalSubrs=cff.GlobalSubrs)
    glyphs = font.getGlyphSet()
    for cp, (advance, _) in icons.items():
        box = BoundsPen(glyphs); glyphs[cmap[cp]].draw(box)
        hmtx[cmap[cp]] = (advance, int(round(box.bounds[0])))
    out = io.BytesIO(); font.save(out)
    return out.getvalue()


# ---- the button guide's images: square R8_UNORM masks (white key, black letter), one mip, block-linear, 72x72 up to
# 1.2.1 and 108x108 in 1.4.3. Each is drawn from the icon of the same Switch button, fitted into the square.
R8_UNORM, HUD_MARGIN, SUPERSAMPLE = 0x201, 4, 4


def gob_offset(x, y, width_gobs, block_height):
    """Byte offset of (x, y) in a Tegra block-linear surface of 1 byte per pixel (GOBs of 64 bytes x 8 rows)."""
    return ((y // (8 * block_height)) * 512 * block_height * width_gobs + (x // 64) * 512 * block_height
            + ((y % (8 * block_height)) // 8) * 512 + ((x % 64) // 32) * 256 + ((y % 8) // 2) * 64
            + ((x % 32) // 16) * 32 + (y % 2) * 16 + (x % 16))


class PolygonPen(BasePen):
    """Outlines flattened to closed polygons."""
    def __init__(self):
        super().__init__(None); self.polygons = []

    def _moveTo(self, pt):
        self.polygons.append([pt])

    def _lineTo(self, pt):
        self.polygons[-1].append(pt)

    def _curveToOne(self, pt1, pt2, pt3):
        p0 = self.polygons[-1][-1]
        for i in range(1, 17):
            t = i / 16; u = 1 - t
            self.polygons[-1].append(tuple(u ** 3 * p0[k] + 3 * u * u * t * pt1[k] + 3 * u * t * t * pt2[k] + t ** 3 * pt3[k]
                                           for k in (0, 1)))

    def _qCurveToOne(self, pt1, pt2):
        p0 = self.polygons[-1][-1]
        for i in range(1, 17):
            t = i / 16; u = 1 - t
            self.polygons[-1].append(tuple(u * u * p0[k] + 2 * u * t * pt1[k] + t * t * pt2[k] for k in (0, 1)))


def rasterize(draw, size):
    """A size x size coverage mask (0..255, rows top down) of draw's outline, fitted into the square, nonzero winding."""
    rec = RecordingPen(); draw(rec)
    box = BoundsPen(None); rec.replay(box); x0, y0, x1, y1 = box.bounds
    inner = size - 2 * HUD_MARGIN; scale = inner / max(x1 - x0, y1 - y0)
    ox = HUD_MARGIN + (inner - (x1 - x0) * scale) / 2 - x0 * scale
    oy = HUD_MARGIN + (inner - (y1 - y0) * scale) / 2 - y0 * scale
    flat = PolygonPen(); rec.replay(TransformPen(flat, (scale, 0, 0, -scale, ox, size - oy)))   # font y up -> rows down
    edges = [(a, b) for poly in flat.polygons for a, b in zip(poly, poly[1:] + poly[:1]) if a[1] != b[1]]
    n = size * SUPERSAMPLE; cover = [0] * (size * size)
    for row in range(n):
        y = (row + 0.5) / SUPERSAMPLE
        crossings = sorted((a[0] + (y - a[1]) * (b[0] - a[0]) / (b[1] - a[1]), 1 if b[1] > a[1] else -1)
                           for a, b in edges if min(a[1], b[1]) <= y < max(a[1], b[1]))
        winding, start = 0, None
        for x, d in crossings:
            if winding == 0:
                start = x
            winding += d
            if winding == 0:
                for col in range(max(0, round(start * SUPERSAMPLE)), min(n, round(x * SUPERSAMPLE))):
                    cover[(row // SUPERSAMPLE) * size + col // SUPERSAMPLE] += 1
    return bytes(min(255, c * 255 // (SUPERSAMPLE * SUPERSAMPLE)) for c in cover)


def bntx_redraw(data, base, textures, icons):
    """Paint the icon of each texture of textures (name without the ^ suffix -> code point; icons: code point ->
    (advance, draw)) over its image in the BNTX at data[base:] (a bytearray), in place. Returns the names painted."""
    assert data[base:base + 4] == b'BNTX' and data[base + 0x20:base + 0x24] == b'NX  '
    count, table = struct.unpack_from('<IQ', data, base + 0x24)
    done = []
    for k in range(count):
        info = base + struct.unpack_from('<Q', data, base + table + 8 * k)[0]
        assert data[info:info + 4] == b'BRTI'
        at = base + struct.unpack_from('<Q', data, info + 0x60)[0]
        name = data[at + 2:at + 2 + struct.unpack_from('<H', data, at)[0]].decode().split('^')[0]
        if name not in textures:
            continue
        mips, fmt = struct.unpack_from('<H', data, info + 0x16)[0], struct.unpack_from('<I', data, info + 0x1c)[0]
        (w, h), layout = struct.unpack_from('<II', data, info + 0x24), struct.unpack_from('<I', data, info + 0x34)[0]
        assert fmt == R8_UNORM and w == h and mips == 1, f'{name}: format {fmt:#x}, {w}x{h}, {mips} mips'
        start = base + struct.unpack_from('<Q', data, base + struct.unpack_from('<Q', data, info + 0x70)[0])[0]
        size, block = struct.unpack_from('<I', data, info + 0x50)[0], 1 << (layout & 7)
        mask = rasterize(icons[textures[name]][1], w)
        for y in range(h):
            for x in range(w):
                at = gob_offset(x, y, (w + 63) // 64, block)
                assert at < size, f'{name}: pixel ({x}, {y}) past the image'
                data[start + at] = mask[y * w + x]
        done.append(name)
    missing = sorted(set(textures) - set(done))
    assert not missing, f'not in the texture list: {", ".join(missing)}'
    return done


# ---- the resource size table: System/Resource/ResourceSizeTable.Product.<n>.Nin_NX_NVN.rsizetable.zs, a RESTBL whose
# entries are keyed by the CRC32 of the RomFS path without .zs. The game sizes the buffer it loads a file into from its
# entry, so an archive that grew needs its entry raised by as much.
def restbl_offsets(table):
    """CRC32 of a path -> offset of its size in the RESTBL table."""
    assert table[:6] == b'RESTBL', 'not a RESTBL'
    count = struct.unpack_from('<I', table, 14)[0]
    return {struct.unpack_from('<I', table, 22 + i * 8)[0]: 22 + i * 8 + 4 for i in range(count)}


def grow_restbl(table, growth):
    """table with the entry of each RomFS path of growth (path -> bytes it grew) raised by that much."""
    at, out = restbl_offsets(table), bytearray(table)
    for path, extra in growth.items():
        h = zlib.crc32(path.removesuffix('.zs').encode())
        assert h in at, f'{path} has no entry in the resource size table'
        struct.pack_into('<I', out, at[h], struct.unpack_from('<I', out, at[h])[0] + extra)
    return bytes(out)
