# Adapted from MacNativeGames games/totk/scripts/make_button_icons.py @ 933b6a0 (MIT, Copyright (c) 2026
# MacNativeGames contributors).
"""Xbox and keyboard button icons for the game's prompts, drawn on the player's machine for
tools/make_romfs_patches.py --prompts. The outlines are our own shapes with letters cut out of them in the game's own
text fonts: nothing of the game is in the repository.

Every prompt except the button guide in play is text in the button font, scft/External.bfotf of each
Font/Font*.Nin_NX_NVN.bfarc.zs: private-use code points from U+E040 (A, B, X, Y, then L, R, ZL, ZR, the d-pad, Plus and
Minus, the sticks). A .bfotf is an OpenType (CFF) font after an 8-byte header; from offset 4 on, every byte is XORed
with a 4-byte key, which the size in the header's second word gives away (bfotf_decode). redraw_font draws the shoulder
buttons, the triggers, Plus, Minus and the d-pad as an Xbox controller names them (LB RB, LT RT, Menu and View, a
cross), can swap the glyphs of the face buttons (A with B, X with Y), or draws keycaps and mouse icons for keyboard
and mouse prompts, and has fontTools write the font again. The archive then grows by a kilobyte or two, more than the
game's resource size table allows it: grow_restbl raises those entries (without it the game stops at start,
MacNativeGames found).

The button guide in play (bottom right) draws L and ZL from images of the common layout archive instead
(timg/__Combined.bntx, Nt_KeyTexL_00 and Nt_KeyTexZL_00; keyboard prompts also redraw Nt_KeyTexA/B/X/Y_00 and
Nt_KeyTexLSt_00/01): bntx_redraw paints the same icons into them, the size unchanged. The sticks keep the Switch
icons, and so do prompts drawn as fixed images (some tutorial animations).

Needs fontTools (tools/requirements-installer.txt, tools/requirements-py314.txt).
"""
import io
from pathlib import Path
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
HUD_KEYBOARD_TEXTURES = {'Nt_KeyTexA_00': 0xE040, 'Nt_KeyTexB_00': 0xE041, 'Nt_KeyTexX_00': 0xE042, 'Nt_KeyTexY_00': 0xE043,
                        'Nt_KeyTexL_00': 0xE044, 'Nt_KeyTexZL_00': 0xE046, 'Nt_KeyTexLSt_00': 0xE05E, 'Nt_KeyTexLSt_01': 0xE05E}
HUD_KEYBOARD_OPTIONAL = frozenset({'Nt_KeyTexLSt_01'})

DEFAULT_KEYS = {
    'a': 'e', 'b': 'lshift', 'x': 'space', 'y': 'mouse_l', 'l': 'q', 'r': 'r', 'zl': 'mouse_r', 'zr': 'f',
    'plus': 'tab', 'minus': 'm', 'lclick': 'lctrl', 'rclick': 'c', 'left': '1', 'up': '2', 'right': '3', 'down': '4',
    'lstick_up': 'w', 'lstick_left': 'a', 'lstick_down': 's', 'lstick_right': 'd'
}

KNOWN_KEYS = frozenset({
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
    'space', 'tab', 'lshift', 'lctrl', 'up', 'down', 'left', 'right',
    'mouse_l', 'mouse_r', 'mouse_m', 'mouse_x1', 'mouse_x2',
    'enter', 'backspace', 'rshift', 'rctrl', 'lalt', 'capslock', 'insert', 'delete', 'home', 'end',
    'pageup', 'pagedown',
    'f2', 'f3', 'f4', 'f5', 'f6', 'f7', 'f9', 'f12',
    'num0', 'num1', 'num2', 'num3', 'num4', 'num5', 'num6', 'num7', 'num8', 'num9',
    'num_add', 'num_sub', 'num_mul', 'num_div', 'num_decimal',
    'minus', 'equals', 'lbracket', 'rbracket', 'semicolon', 'apostrophe', 'comma', 'period', 'slash',
    'backslash', 'grave',
})

KNOWN_PADS = frozenset({
    'south', 'east', 'west', 'north', 'lstick', 'rstick', 'l', 'r', 'start', 'back',
    'dpad_left', 'dpad_up', 'dpad_right', 'dpad_down', 'zl', 'zr',
    'lpaddle1', 'rpaddle1', 'lpaddle2', 'rpaddle2',
})

KEY_LABELS = {
    **{c: c.upper() for c in 'abcdefghijklmnopqrstuvwxyz'},
    **{str(i): str(i) for i in range(10)},
    'space': 'Space', 'tab': 'Tab', 'lshift': 'Shift', 'rshift': 'Shift',
    'lctrl': 'Ctrl', 'rctrl': 'Ctrl', 'lalt': 'Alt', 'capslock': 'Caps',
    'enter': 'Enter', 'backspace': 'Bksp', 'insert': 'Ins', 'delete': 'Del',
    'home': 'Home', 'end': 'End', 'pageup': 'PgUp', 'pagedown': 'PgDn',
    **{f'f{i}': f'F{i}' for i in (2, 3, 4, 5, 6, 7, 9, 12)},
    **{f'num{i}': f'N{i}' for i in range(10)},
    'num_add': 'N+', 'num_sub': 'N-', 'num_mul': 'N*', 'num_div': 'N/', 'num_decimal': 'N.',
    'minus': '-', 'equals': '=', 'lbracket': '[', 'rbracket': ']',
    'semicolon': ';', 'apostrophe': "'", 'comma': ',', 'period': '.',
    'slash': '/', 'backslash': '\\', 'grave': '`',
}


def read_ini_keys(path=None):
    """Action -> primary key name from totk.ini at path (Path or str), or DEFAULT_KEYS when None or missing."""
    keys = dict(DEFAULT_KEYS)
    if path is None:
        return keys
    p = Path(path)
    if not p.is_file():
        return keys
    text = p.read_text(encoding='utf-8-sig')
    ini_binds = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith(('#', ';', '[')):
            continue
        if '=' not in line:
            continue
        k, v = line.split('=', 1)
        k = k.strip().lower()
        if k.startswith('totk_'):
            k = k[5:]
        v = v.strip()
        if len(v) >= 2 and v[0] == '"' and v[-1] == '"':
            v = v[1:-1]
        if not v:   # empty value leaves earlier/default value
            continue
        if k.startswith('bind_'):
            ini_binds[k[5:]] = v

    stick_actions = frozenset({'lstick_up', 'lstick_left', 'lstick_down', 'lstick_right'})
    for action, default_val in DEFAULT_KEYS.items():
        if action not in ini_binds:
            continue
        val_str = ini_binds[action]
        tokens = [t.strip() for t in val_str.split(',') if t.strip()]
        valid_entries = []
        for tok in tokens:
            low = tok.lower()
            if low == 'none':
                valid_entries.append(('none', None))
            elif low.startswith('key:'):
                kname = low[4:].strip()
                if kname == 'default':
                    valid_entries.append(('key_default', None))
                elif kname in KNOWN_KEYS:
                    valid_entries.append(('key', kname))
            elif low.startswith('pad:') and action not in stick_actions:
                pname = low[4:].strip()
                if pname == 'default' or pname in KNOWN_PADS:
                    valid_entries.append(('pad', pname))
        if not valid_entries:
            continue
        key_list = []
        for kind, kval in valid_entries:
            if kind == 'key_default':
                key_list.append(default_val)
            elif kind == 'key':
                key_list.append(kval)
        keys[action] = key_list[0] if key_list else 'none'
    return keys


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


def mouse_icon(button=None, look=False, advance=700):
    """Draw a mouse icon with advance 700. button: 'mouse_l', 'mouse_r', 'mouse_m', 'mouse_x1', 'mouse_x2', or None.
    look: True to draw left and right outward arrows flanking the mouse body."""
    def draw(pen):
        scale = (Y1 - Y0) / 640.0
        sx = lambda x: 350.0 + (x - 350.0) * scale
        sy = lambda y: Y0 + y * scale
        sr = lambda r: r * scale

        # 1. Outer mouse shell (CCW filled)
        rounded(pen, sx(150), sy(0), sx(550), sy(640), sr(180))

        # 2. Inner mouse cavity (CW hole via ReverseContourPen)
        rounded(ReverseContourPen(pen), sx(190), sy(45), sx(510), sy(595), sr(140))

        # 3. Horizontal button seam bar (CCW filled)
        rounded(pen, sx(199), sy(335), sx(501), sy(357), sr(8))

        # 4. Scroll wheel pill (larger when mouse_m)
        is_m = button == 'mouse_m'
        wx0, wx1 = (328, 372) if is_m else (337, 363)
        wy0, wy1 = (414, 532) if is_m else (428, 518)
        rounded(pen, sx(wx0), sy(wy0), sx(wx1), sy(wy1), sr(12))

        # 5. Upper/lower vertical divider segments split around seam & wheel so contours do not overlap
        rounded(pen, sx(339), sy(357), sx(361), sy(wy0), sr(5))
        rounded(pen, sx(339), sy(wy1), sx(361), sy(592), sr(5))

        # 6. Active button fill
        if button == 'mouse_l':
            rounded(pen, sx(190), sy(376), sx(328), sy(574), sr(48))
        elif button == 'mouse_r':
            rounded(pen, sx(372), sy(376), sx(510), sy(574), sr(48))

        # 7. Left-side thumb buttons (mouse_x1 / mouse_x2)
        if button in ('mouse_x1', 'mouse_x2'):
            rounded(pen, sx(91), sy(250), sx(138), sy(342), sr(16))
            rounded(pen, sx(91), sy(370), sx(138), sy(462), sr(16))
            if button == 'mouse_x1':
                rounded(ReverseContourPen(pen), sx(102), sy(382), sx(127), sy(450), sr(8))
            else:
                rounded(ReverseContourPen(pen), sx(102), sy(262), sx(127), sy(330), sr(8))

        # 8. Outward arrows flanking mouse body for camera look
        if look:
            left = [(35, 310), (85, 354), (85, 328), (126, 328), (126, 292), (85, 292), (85, 266)]
            right = [(665, 310), (615, 354), (615, 328), (574, 328), (574, 292), (615, 292), (615, 266)]
            polygon(pen, [(sx(x), sy(y)) for x, y in left[::-1]])
            polygon(pen, [(sx(x), sy(y)) for x, y in right])

    return advance, draw


def arrow_polygon(direction, cx=320, cy=CY):
    """Clockwise (hole) single-headed arrow centered at (cx, cy)."""
    pts = [
        (148, 14), (148, -14), (71, -90), (43, -62), (82, -23),
        (-145, -23), (-145, 23), (82, 23), (43, 62), (71, 90)
    ]
    if direction == 'right':
        rot = pts
    elif direction == 'up':
        rot = [(-y, x) for x, y in pts]
    elif direction == 'left':
        rot = [(-x, -y) for x, y in pts]
    elif direction == 'down':
        rot = [(y, -x) for x, y in pts]
    else:
        raise ValueError(f'unknown arrow direction: {direction}')
    return [(cx + x, cy + y) for x, y in rot]


def double_arrow(vertical=False, cx=320, cy=CY, length=155, head=75, barb=80, stem=24):
    """Clockwise (hole) double-headed arrow centered at (cx, cy)."""
    pts = [
        (length, 0), (head, -barb), (head, -stem), (-head, -stem), (-head, -barb),
        (-length, 0), (-head, barb), (-head, stem), (head, stem), (head, barb)
    ]
    if vertical:
        pts = [(-y, x) for x, y in pts]
    return [(cx + x, cy + y) for x, y in pts]


def keyboard_icons(g, keys=None):
    """Code point -> (advance, draw(pen)) for keyboard and mouse button prompts.
    g: Glyphs for the letters. keys: action -> primary key name (read_ini_keys())."""
    keys = read_ini_keys() if keys is None else keys

    def square_keycap(draw_inner, advance=640):
        def draw(pen):
            rounded(pen, 40, Y0, advance - 40, Y1, 90)
            draw_inner(pen)
        return advance, draw

    def wide_keycap(draw_inner, advance=880):
        def draw(pen):
            rounded(pen, 40, Y0, advance - 40, Y1, 90)
            draw_inner(pen)
        return advance, draw

    def key_glyph(label):
        if len(label) >= 3 or label.startswith('N'):
            return wide_keycap(lambda pen: g.hole(pen, label, 440, CY, 250, 660))
        return square_keycap(lambda pen: g.hole(pen, label, 320, CY, 290, 410))

    def arrow_glyph(direction):
        return square_keycap(lambda pen: polygon(pen, arrow_polygon(direction, 320, CY)))

    def unbound_glyph():
        return square_keycap(lambda pen: rounded(ReverseContourPen(pen), 160, CY - 28, 480, CY + 28, 20))

    def action_glyph(k):
        if k == 'none':
            return unbound_glyph()
        if k in ('mouse_l', 'mouse_r', 'mouse_m', 'mouse_x1', 'mouse_x2'):
            return mouse_icon(button=k)
        if k in ('up', 'down', 'left', 'right'):
            return arrow_glyph(k)
        return key_glyph(KEY_LABELS.get(k, k.upper()))

    # Movement labels (WASD or Move)
    u = KEY_LABELS.get(keys.get('lstick_up', 'w'), 'W')
    l = KEY_LABELS.get(keys.get('lstick_left', 'a'), 'A')
    d = KEY_LABELS.get(keys.get('lstick_down', 's'), 'S')
    r = KEY_LABELS.get(keys.get('lstick_right', 'd'), 'D')
    move_label = u + l + d + r if len(u) == 1 and len(l) == 1 and len(d) == 1 and len(r) == 1 else 'Move'
    move_glyph = wide_keycap(lambda pen: g.hole(pen, move_label, 440, CY, 250, 660))

    mouse_look = mouse_icon(look=True)

    v_arrow = square_keycap(lambda pen: polygon(pen, double_arrow(vertical=True, cx=320, cy=CY, length=155, head=75, barb=80, stem=24)))
    h_arrow = square_keycap(lambda pen: polygon(pen, double_arrow(vertical=False, cx=320, cy=CY, length=155, head=75, barb=80, stem=24)))
    all_dpad = square_keycap(lambda pen: [
        polygon(pen, double_arrow(vertical=False, cx=200, cy=CY, length=100, head=50, barb=52, stem=16)),
        polygon(pen, double_arrow(vertical=True, cx=440, cy=CY, length=100, head=50, barb=52, stem=16))
    ])

    out = {
        0xE040: action_glyph(keys.get('a', 'e')),
        0xE041: action_glyph(keys.get('b', 'lshift')),
        0xE042: action_glyph(keys.get('x', 'space')),
        0xE043: action_glyph(keys.get('y', 'mouse_l')),
        0xE044: action_glyph(keys.get('l', 'q')),
        0xE045: action_glyph(keys.get('r', 'r')),
        0xE046: action_glyph(keys.get('zl', 'mouse_r')),
        0xE047: action_glyph(keys.get('zr', 'f')),
        0xE04B: action_glyph(keys.get('plus', 'tab')),
        0xE04C: action_glyph(keys.get('minus', 'm')),
        0xE05E: action_glyph(keys.get('lclick', 'lctrl')),
        0xE05F: action_glyph(keys.get('rclick', 'c')),
        0xE060: action_glyph(keys.get('up', '2')),
        0xE061: action_glyph(keys.get('down', '4')),
        0xE062: action_glyph(keys.get('left', '1')),
        0xE063: action_glyph(keys.get('right', '3')),
        0xE064: v_arrow,
        0xE065: h_arrow,
        0xE049: all_dpad,
        0xE068: mouse_look,
    }
    for cp in range(0xE050, 0xE05E):
        out[cp] = move_glyph if cp % 2 == 0 else mouse_look
    return out


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


def bntx_redraw(data, base, textures, icons, optional=()):
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
    missing = sorted(set(textures) - set(done) - set(optional))
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
