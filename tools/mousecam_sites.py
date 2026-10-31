#!/usr/bin/env python3
"""Turns the `bl` callers of the camera function into patch lines for recomp/patches-vNNN.txt.

    python tools/find_calls.py --main build/extracted/v121/exefs/main 0xa70604 | python tools/mousecam_sites.py --main-size 0x... 

Input (stdin or file args): lines OFFSET:WORD, the format find_calls.py prints (module offset in hex, the instruction word
the module has there, a `bl`). Lines that do not match are ignored.

Output: `site mousecam main <offset> <original> <bl to the host function>` for each caller, preceded by the set line.
The host function lives on the patch page at +0xfd0 (runtime/mousecam.c HOST_CAM). The patch page is the first 4 KiB page
after main's image: give --main-size (main's image size in hex, what recomp/layout.py uses; the page is main_size rounded up
to 0x1000) or --page-off (the page's offset from main's base, e.g. 0x3adf000 for 1.4.3).
"""
import argparse
import re
import sys

HOST_OFF = 0xFD0


def bl(pc, target):
    d = target - pc
    if d % 4 or not -(1 << 27) <= d < (1 << 27):
        raise SystemExit(f'bl at {pc:#x}: target {target:#x} out of range (+-128 MiB) or unaligned')
    return 0x94000000 | ((d >> 2) & 0x3FFFFFF)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('files', nargs='*')
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--main-size', type=lambda s: int(s, 0))
    g.add_argument('--page-off', type=lambda s: int(s, 0))
    a = ap.parse_args()
    page = a.page_off if a.page_off is not None else (a.main_size + 0xFFF) & ~0xFFF
    target = page + HOST_OFF
    text = ''.join(open(f).read() for f in a.files) if a.files else sys.stdin.read()
    print('set mousecam TOTK_MOUSECAM 1')
    n = 0
    for m in re.finditer(r'\b([0-9a-fA-F]+):([0-9a-fA-F]{8})\b', text):
        off, word = int(m.group(1), 16), int(m.group(2), 16)
        if word >> 26 != 0x25:   # bl
            print(f'# {off:x}: {word:08x} is not a bl, skipped', file=sys.stderr)
            continue
        print(f'site mousecam main {off:x} {word:08x} {bl(off, target):08x}   # bl camera -> host')
        n += 1
    print(f'# {n} call sites', file=sys.stderr)


if __name__ == '__main__':
    main()
