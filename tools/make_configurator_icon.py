"""Draw runtime/configurator/totk-config.ico: the game's ring (runtime/totk.ico) with a gold wrench across its middle,
so the settings window and its exe tell themselves apart from the game in the task bar.

Each of totk.ico's images keeps its own pixels; the wrench is drawn at 1024 px from its shape (a capsule handle and a
round head with an open jaw), outlined dark so it reads on light and dark task bars, scaled down and laid over it.
The .ico is committed; run this again only to change the drawing (needs Pillow and NumPy):

    python tools/make_configurator_icon.py
"""
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / 'runtime' / 'totk.ico'
DST = ROOT / 'runtime' / 'configurator' / 'totk-config.ico'
BIG = 1024
GOLD = (240, 198, 84)
DARK = (13, 20, 17)


def wrench_mask(n):
    """Coverage of the wrench in an n x n square, 0..1: the handle to the bottom left, the jaw open to the top right."""
    y, x = np.mgrid[0:n, 0:n].astype(np.float32) / n - 0.5
    a = np.deg2rad(45.0)   # the local x axis points up and right on the screen
    u = (x * np.cos(a) - y * np.sin(a)) / 0.82
    v = (x * np.sin(a) + y * np.cos(a)) / 0.82
    # the handle: a capsule along u from -0.30 to 0.10, 0.055 wide each side
    uc = np.clip(u, -0.30, 0.10)
    handle = (u - uc) ** 2 + v ** 2 <= 0.058 ** 2
    head = (u - 0.19) ** 2 + v ** 2 <= 0.150 ** 2
    jaw = ((u >= 0.19) & (np.abs(v) <= 0.062)) | ((u - 0.19) ** 2 + v ** 2 <= 0.062 ** 2)
    return ((handle | head) & ~jaw).astype(np.float32)


def wrench_layer():
    """The wrench at BIG px: gold, with a dark outline around it."""
    m = Image.fromarray((wrench_mask(BIG) * 255).astype(np.uint8), 'L')
    ring = m.filter(ImageFilter.MaxFilter(41))   # the outline, about 2 % of the side
    layer = Image.new('RGBA', (BIG, BIG), DARK + (0,))
    layer.putalpha(ring.point(lambda p: p * 235 // 255))
    gold = Image.new('RGBA', (BIG, BIG), GOLD + (255,))
    gold.putalpha(m)
    return Image.alpha_composite(layer, gold)


def main():
    src = Image.open(SRC)
    sizes = sorted(src.info['sizes'])
    wrench = wrench_layer()
    frames = []
    for s in sizes:
        src.size = s
        src.load()
        base = src.convert('RGBA')
        frames.append(Image.alpha_composite(base, wrench.resize(s, Image.LANCZOS)))
    frames.sort(key=lambda f: f.size[0], reverse=True)
    frames[0].save(DST, format='ICO', sizes=[f.size for f in frames], append_images=frames[1:])
    print(f'{DST.relative_to(ROOT)}: {", ".join(str(f.size[0]) for f in frames)}')


if __name__ == '__main__':
    main()
