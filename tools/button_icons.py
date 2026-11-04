# Adapted from MacNativeGames games/totk/scripts/make_button_icons.py @ 933b6a0 (MIT, Copyright (c) 2026
# MacNativeGames contributors).
"""Xbox and keyboard-and-mouse button icons for the game's prompts, drawn on the player's machine for
tools/make_romfs_patches.py --prompts.
The outlines are our own shapes with letters cut out of them in the game's own text fonts: nothing of the game is in
the repository.

Every prompt except the button guide in play is text in the button font, scft/External.bfotf of each
Font/Font*.Nin_NX_NVN.bfarc.zs: private-use code points from U+E040 (A, B, X, Y, then L, R, ZL, ZR, the d-pad, Plus and
Minus, the sticks). A .bfotf is an OpenType (CFF) font after an 8-byte header; from offset 4 on, every byte is XORed
with a 4-byte key, which the size in the header's second word gives away (bfotf_decode). redraw_font draws the shoulder
buttons, the triggers, Plus, Minus and the d-pad as an Xbox controller names them (LB RB, LT RT, Menu and View, a
cross), or every button as the key or mouse button the keyboard presses it with (pc_icons), can swap the glyphs of
the face buttons (A with B, X with Y), and has fontTools write the font again. The archive then grows by a kilobyte or
two, more than the game's resource size table allows it: grow_restbl raises those entries (without it the game stops at
start, MacNativeGames found).

The button guide in play (bottom right) draws its buttons, L and ZL among them, from images of the common layout
archive instead (timg/__Combined.bntx, Nt_KeyTexL_00 and Nt_KeyTexZL_00): bntx_redraw paints the same icons into them,
the size unchanged. The Xbox prompts redraw L and ZL, the keyboard and mouse ones every image (PC_HUD_TEXTURES: A, B, X,
Y, L, ZL and the stick's two). Prompts drawn as fixed images (some tutorial animations) keep the Switch icons.

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


# ---- keyboard and mouse prompts (--prompts pc): every button the keyboard drives, drawn as the key or mouse button
# bound to it, in the same filled-shape style. Each glyph keeps the advance of the game's own (a wider glyph runs into
# the text of a right-aligned guide).
# The keyboard defaults of runtime/input_map.c (input_map_reset; the totk.ini bind_ lines apply on top, pc_binds). The
# right stick is the camera, which the mouse drives; SHAKE is the key that shakes it (the mouse never does).
PC_BINDS = {
    'A': 'e', 'B': 'lshift', 'X': 'space', 'Y': 'mouse_l', 'L': 'q', 'R': 'r', 'ZL': 'mouse_r', 'ZR': 'f',
    'PLUS': 'tab', 'MINUS': 'm', 'LEFT': '1', 'UP': '2', 'RIGHT': '3', 'DOWN': '4', 'LCLICK': 'lctrl', 'RCLICK': 'c',
    'LSTICK_UP': 'w', 'LSTICK_LEFT': 'a', 'LSTICK_DOWN': 's', 'LSTICK_RIGHT': 'd', 'SHAKE': 'g',
}
# What a key shows: its letter, a symbol or a short name (key names as in totk.ini; the rest show their name).
PC_LABELS = {
    'lshift': '⇧', 'rshift': '⇧', 'lctrl': 'CTRL', 'rctrl': 'CTRL', 'lalt': 'ALT', 'tab': 'TAB',
    'enter': 'ENT', 'backspace': 'BKSP', 'capslock': 'CAPS', 'insert': 'INS', 'delete': 'DEL', 'home': 'HOME',
    'end': 'END', 'pageup': 'PGUP', 'pagedown': 'PGDN', 'up': '↑', 'down': '↓', 'left': '←',
    'right': '→', 'num_add': '+', 'num_sub': '-', 'num_mul': '*', 'num_div': '/', 'num_decimal': '.',
    'minus': '-', 'equals': '=', 'lbracket': '[', 'rbracket': ']', 'semicolon': ';', 'apostrophe': "'", 'comma': ',',
    'period': '.', 'slash': '/', 'backslash': '\\', 'grave': '`', 'mouse_x1': 'M4', 'mouse_x2': 'M5',
}
# The one-letter panes around the action prompt (T_A_00 ...) are in the Normal font (Rodin B and Symbol): one character
# of it per key.
PC_LETTER_LABELS = {'lshift': '↑', 'rshift': '↑', 'space': '▬', 'mouse_l': 'L', 'mouse_r': 'R',
                    'mouse_m': 'M', 'lctrl': 'C', 'rctrl': 'C', 'tab': 'T'}
# The button guide's images in the pc style: every symbol mask of the action prompt. In the 1.4.3 Common archive
# LSt_00 shows the left stick tilted up (as U+E052) and LSt_01 the left stick (as U+E05E).
PC_HUD_TEXTURES = {'Nt_KeyTexA_00': 0xE040, 'Nt_KeyTexB_00': 0xE041, 'Nt_KeyTexX_00': 0xE042, 'Nt_KeyTexY_00': 0xE043,
                   'Nt_KeyTexL_00': 0xE044, 'Nt_KeyTexZL_00': 0xE046, 'Nt_KeyTexLSt_00': 0xE052,
                   'Nt_KeyTexLSt_01': 0xE05E}
PC_MOUSE = ('mouse_l', 'mouse_r', 'mouse_m')


def pc_binds(ini_text=None):
    """PC_BINDS with the first key of each bind_<action> line of a totk.ini text over it (a line without a key entry,
    or with key:default, keeps the default)."""
    binds = dict(PC_BINDS)
    for line in (ini_text or '').splitlines():
        line = line.split('#')[0].split(';')[0].strip()
        if not line.lower().startswith('bind_') or '=' not in line:
            continue
        action, value = (s.strip() for s in line.split('=', 1))
        keys = [e.strip()[4:].strip().lower() for e in value.split(',') if e.strip().lower().startswith('key:')]
        keys = [k for k in keys if k != 'default']
        if keys and action[5:].upper() in binds:
            binds[action[5:].upper()] = keys[0]
    return binds


def font_advances(blob):
    """Code point -> advance of every glyph of the button font (a .bfotf)."""
    font = TTFont(io.BytesIO(bfotf_decode(blob)[0]))
    return {cp: font['hmtx'][name][0] for cp, name in font.getBestCmap().items()}


def pc_letter(key):
    """The one character the panes around the action prompt show for key."""
    if key in PC_LETTER_LABELS:
        return PC_LETTER_LABELS[key]
    return PC_LABELS.get(key, key.upper())[0]


def pc_icons(g, adv, binds=PC_BINDS):
    """Code point -> (advance, draw(pen)) for every button glyph but Home and Capture, drawn as the key or mouse button
    binds puts on it. g: Glyphs for the key labels; adv: the game's advance of each glyph (font_advances), which each
    drawing keeps."""
    def mouse_body(pen, cx, y0, y1, lit=None):
        """A mouse seen from above; lit ('l', 'r', 'm' or None) its pressed button, filled, the others hollow."""
        h = y1 - y0; r = h * 0.33; t = h * 0.07; gap = h * 0.035
        x0, x1, mid = cx - r, cx + r, y0 + h * 0.52
        rounded(pen, x0, y0, x1, y1, r * 0.9, r)
        reverse = ReverseContourPen(pen)   # holes; they never overlap each other or leave the body
        ri = r - t
        if lit in ('l', 'r'):   # the seams around the pressed button
            sx0, sx1 = (x0 + t, cx - gap) if lit == 'l' else (cx + gap, x1 - t)
            polygon(reverse, [(sx0, mid - gap), (sx1, mid - gap), (sx1, mid + gap), (sx0, mid + gap)])
            polygon(reverse, [(cx - gap, mid + gap), (cx + gap, mid + gap), (cx + gap, y1 - t), (cx - gap, y1 - t)])
        if lit != 'l':   # hollow left button, its outer top corner following the body
            ix0, ix1 = x0 + t, cx - gap
            reverse.moveTo((ix1, mid + gap)); reverse.lineTo((ix1, y1 - t)); reverse.lineTo((ix0 + ri, y1 - t))
            reverse.curveTo((ix0 + ri - K * ri, y1 - t), (ix0, y1 - t - ri + K * ri), (ix0, y1 - t - ri))
            reverse.lineTo((ix0, mid + gap)); reverse.closePath()
        if lit != 'r':
            ix0, ix1 = cx + gap, x1 - t
            reverse.moveTo((ix0, mid + gap)); reverse.lineTo((ix1, mid + gap)); reverse.lineTo((ix1, y1 - t - ri))
            reverse.curveTo((ix1, y1 - t - ri + K * ri), (ix1 - ri + K * ri, y1 - t), (ix1 - ri, y1 - t))
            reverse.lineTo((ix0, y1 - t)); reverse.closePath()
        if lit == 'm':   # the wheel, filled over the gap between the hollow buttons
            rounded(pen, cx - h * 0.06, mid + h * 0.12, cx + h * 0.06, mid + h * 0.36, h * 0.06)

    def key_cap(pen, x0, y0, x1, y1, key):
        """A key: a rounded square with its label cut out; a mouse button draws the mouse."""
        if key in PC_MOUSE:
            mouse_body(pen, (x0 + x1) / 2, y0, y1, key[6:])
            return
        w, h = x1 - x0, y1 - y0
        rounded(pen, x0, y0, x1, y1, min(w, h) * 0.18)
        if key == 'space':   # an open bracket lying on its back
            t = h * 0.09
            bx0, bx1, by0, by1 = x0 + w * 0.22, x1 - w * 0.22, y0 + h * 0.36, y0 + h * 0.60
            polygon(ReverseContourPen(pen), [(bx0, by0), (bx1, by0), (bx1, by1), (bx1 - t, by1), (bx1 - t, by0 + t),
                                             (bx0 + t, by0 + t), (bx0 + t, by1), (bx0, by1)])
            return
        label = PC_LABELS.get(key, key.upper())
        cap = h * (0.52 if len(label) == 1 else 0.36 if len(label) == 2 else 0.26)
        g.hole(pen, label, (x0 + x1) / 2, (y0 + y1) / 2, cap, w * 0.74)

    def arrow(pen, cx, cy, d, s):
        """A filled arrowhead pointing d, s its half size."""
        polygon(pen, {'up': [(cx - s, cy - s * 0.6), (cx + s, cy - s * 0.6), (cx, cy + s * 0.8)],
                      'down': [(cx + s, cy + s * 0.6), (cx - s, cy + s * 0.6), (cx, cy - s * 0.8)],
                      'left': [(cx + s * 0.6, cy - s), (cx + s * 0.6, cy + s), (cx - s * 0.8, cy)],
                      'right': [(cx - s * 0.6, cy + s), (cx - s * 0.6, cy - s), (cx + s * 0.8, cy)]}[d])

    def one(key, advance):
        def draw(pen):
            side = min(advance - 80, Y1 - Y0)
            key_cap(pen, (advance - side) / 2, Y0, (advance + side) / 2, Y1, key)
        return advance, draw

    def grid(cells, advance):
        """Small keys in a grid: cells = [(key, column, row)], row 0 on top."""
        def draw(pen):
            cols, rows = max(c for _, c, _ in cells) + 1, max(r for _, _, r in cells) + 1
            gap = 24; size = min((advance - 60 - gap * (cols - 1)) / cols, (Y1 - Y0 - gap * (rows - 1)) / rows)
            left, top = (advance - cols * size - gap * (cols - 1)) / 2, CY + (rows * size + gap * (rows - 1)) / 2
            for key, c, r in cells:
                x, y = left + c * (size + gap), top - (r + 1) * size - r * gap
                key_cap(pen, x, y, x + size, y + size, key)
        return advance, draw

    def mouse_move(advance, *dirs):
        """The mouse, no button pressed, with an arrow for each direction the prompt means."""
        def draw(pen):
            mouse_body(pen, advance / 2, Y0 + 30, Y1 - 30)
            off = (Y1 - Y0 - 60) * 0.33 + 70
            spots = {'up': (advance / 2 + off, CY + 110), 'down': (advance / 2 + off, CY - 110),
                     'left': (advance / 2 - off, CY), 'right': (advance / 2 + off, CY)}
            for d in dirs:
                arrow(pen, *spots[d], d, 52)
        return advance, draw

    b = binds
    up, left, down, right = b['LSTICK_UP'], b['LSTICK_LEFT'], b['LSTICK_DOWN'], b['LSTICK_RIGHT']
    du, dl, dd, dr = b['UP'], b['LEFT'], b['DOWN'], b['RIGHT']
    return {
        **{cp: one(b[name], adv[cp]) for cp, name in ((0xE040, 'A'), (0xE041, 'B'), (0xE042, 'X'), (0xE043, 'Y'),
                                                      (0xE044, 'L'), (0xE045, 'R'), (0xE046, 'ZL'), (0xE047, 'ZR'),
                                                      (0xE04B, 'PLUS'), (0xE04C, 'MINUS'), (0xE056, 'LCLICK'),
                                                      (0xE057, 'RCLICK'))},
        # the d-pad: its four keys as a cross (up on top), or the keys of the directions the prompt means
        0xE049: grid([(du, 1, 0), (dl, 0, 1), (dd, 1, 1), (dr, 2, 1)], adv[0xE049]),
        0xE060: one(du, adv[0xE060]), 0xE061: one(dd, adv[0xE061]), 0xE062: one(dl, adv[0xE062]),
        0xE063: one(dr, adv[0xE063]), 0xE064: grid([(du, 0, 0), (dd, 0, 1)], adv[0xE064]),
        0xE065: grid([(dl, 0, 0), (dr, 1, 0)], adv[0xE065]),
        # the left stick: its keys (W A S D)
        0xE05E: grid([(up, 1, 0), (left, 0, 1), (down, 1, 1), (right, 2, 1)], adv[0xE05E]),
        0xE052: one(up, adv[0xE052]), 0xE050: one(down, adv[0xE050]), 0xE05A: one(left, adv[0xE05A]),
        0xE058: one(right, adv[0xE058]), 0xE05C: grid([(up, 0, 0), (down, 0, 1)], adv[0xE05C]),
        0xE054: grid([(left, 0, 0), (right, 1, 0)], adv[0xE054]),
        # the right stick: the mouse
        0xE05F: mouse_move(adv[0xE05F]), 0xE053: mouse_move(adv[0xE053], 'up'),
        0xE051: mouse_move(adv[0xE051], 'down'), 0xE05B: mouse_move(adv[0xE05B], 'left'),
        0xE059: mouse_move(adv[0xE059], 'right'), 0xE05D: mouse_move(adv[0xE05D], 'up', 'down'),
        0xE055: mouse_move(adv[0xE055], 'left', 'right'),
        # the right stick shaken: the shake key
        0xE068: one(b['SHAKE'], adv[0xE068]),
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
