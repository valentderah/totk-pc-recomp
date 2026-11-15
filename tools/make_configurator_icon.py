"""Write configurator/totk-config.ico: the icon of totk.exe (runtime/totk.ico, the green ring) with a gold wrench across
it, for the settings configurator (configurator/, totk-config.exe).

    .venv\\Scripts\\python tools\\make_configurator_icon.py

Every size of runtime/totk.ico gets the wrench drawn at 4x that size and scaled down, so the small sizes stay clean.
The wrench is drawn here from circles and rectangles; nothing else goes in. Needs Pillow.
"""
import io
import struct
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / 'runtime' / 'totk.ico'
OUT = ROOT / 'configurator' / 'totk-config.ico'

GOLD = (237, 194, 84)           # the configurator's accent (configurator/src/theme.rs GOLD)
GOLD_LIGHT = (255, 230, 150)
OUTLINE = (12, 24, 18)          # the configurator's background, so the wrench reads on the ring and on any desktop


def read_ico(path):
    """[(size, PIL image)] of the PNG or BMP frames of an .ico."""
    data = path.read_bytes()
    count = struct.unpack_from('<HHH', data)[2]
    frames = []
    for i in range(count):
        w, h, _, _, _, _, size, off = struct.unpack_from('<BBBBHHII', data, 6 + 16 * i)
        img = Image.open(io.BytesIO(data[off:off + size])).convert('RGBA')
        frames.append((w or 256, img))
    return frames


def wrench_mask(n):
    """An open-end wrench, white on black, n x n, its handle along the diagonal from bottom left to top right."""
    big = n * 4
    m = Image.new('L', (big, big), 0)
    d = ImageDraw.Draw(m)
    # drawn horizontally around the centre, rotated afterwards
    c = big / 2
    length, handle = big * 0.80, big * 0.105
    left, right = c - length / 2, c + length / 2
    head = big * 0.145
    d.rounded_rectangle([left + head * 0.6, c - handle / 2, right - head * 0.6, c + handle / 2], radius=handle / 2,
                        fill=255)
    # open end on the right: a disc with a jaw cut into it
    d.ellipse([right - 2 * head, c - head, right, c + head], fill=255)
    jaw = head * 0.62
    d.rectangle([right - head * 0.9, c - jaw / 2, right + 1, c + jaw / 2], fill=0)
    # ring end on the left: a disc with a round hole
    d.ellipse([left, c - head * 0.9, left + 1.8 * head, c + head * 0.9], fill=255)
    hole = head * 0.42
    hx = left + 0.9 * head
    d.ellipse([hx - hole, c - hole, hx + hole, c + hole], fill=0)
    m = m.rotate(45, resample=Image.BICUBIC, center=(c, c))
    return m


def with_wrench(ring, n):
    big = n * 4
    mask = wrench_mask(n)
    # a dark outline a little wider than the wrench, then the gold body with a lighter upper edge
    grow = max(3, round(big * 0.035)) | 1
    outline = mask.filter(ImageFilter.MaxFilter(grow))
    layer = Image.new('RGBA', (big, big), OUTLINE + (0,))
    layer.putalpha(outline)
    body = Image.new('RGBA', (big, big), GOLD + (255,))
    shine = Image.linear_gradient('L').resize((big, big)).point(lambda v: 255 - v)
    body = Image.composite(Image.new('RGBA', (big, big), GOLD_LIGHT + (255,)), body,
                           ImageChops.multiply(shine, Image.new('L', (big, big), 140)))
    layer.alpha_composite(Image.composite(body, Image.new('RGBA', (big, big), (0, 0, 0, 0)), mask))
    out = ring.resize((big, big), Image.LANCZOS)
    out.alpha_composite(layer)
    return out.resize((n, n), Image.LANCZOS)


def write_ico(frames, path):
    """frames: [(size, image)], each stored as PNG."""
    blobs = []
    for n, img in frames:
        b = io.BytesIO()
        img.save(b, 'PNG', optimize=True)
        blobs.append((n, b.getvalue()))
    out = bytearray(struct.pack('<HHH', 0, 1, len(blobs)))
    off = 6 + 16 * len(blobs)
    for n, blob in blobs:
        out += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(blob), off)
        off += len(blob)
    for _, blob in blobs:
        out += blob
    path.write_bytes(bytes(out))


def main():
    frames = [(n, with_wrench(img, n)) for n, img in read_ico(SRC)]
    write_ico(frames, OUT)
    print(f'wrote {OUT.relative_to(ROOT)}: {", ".join(str(n) for n, _ in frames)} px')
    return 0


if __name__ == '__main__':
    sys.exit(main())
