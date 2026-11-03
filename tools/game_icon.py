"""Write a game's icon for the launcher (docs/launcher.md): the JPEG its Control NCA holds (icon_AmericanEnglish.dat).

    .venv-py314\\Scripts\\python tools\\game_icon.py --package <base game or update .nsp/.nsz/.xci> --keys <prod.keys> \\
        --out build\\extracted\\botw190\\icon.jpg

tools/extract.py writes build/extracted/<version>/icon.jpg this way after each update and tools/build_game.py copies it
next to totk.exe. The Control NCA is a plain NCA in an NSZ too (only the Program NCA is compressed). The icon is game
data: it stays in build/ like the rest. Key values are read from the keys file and never printed.
"""
import argparse
import struct
import sys
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import modes

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools' / 'vendor' / 'macnativegames'))
from extract_exefs import (common_tickets, container_entries, decrypt, load_keys, nca_ctr_key,  # noqa: E402
                           nca_inventory, read_at, record_offset, require)

CONTROL = 2           # NCA content type
ROMFS_CTR = b'\0\3\3'  # FS header: RomFS, IVFC hashes, AES-CTR
MAX_ROMFS = 64 << 20  # a Control RomFS holds the NACP and the icons: a few MB
LANGUAGES = ('AmericanEnglish', 'BritishEnglish', 'Japanese')   # the icon files to prefer, in order


def romfs_files(image):
    """{path: bytes} of a RomFS image (the header and file table of tools/vendor/macnativegames/extract_romfs.py
    index_romfs); a Control RomFS has its files in the root."""
    h = struct.unpack_from('<10Q', image, 0)
    require(h[0] == 80 and h[7] + h[8] <= len(image), 'Control RomFS header')
    table, files, pos = image[h[7]:h[7] + h[8]], {}, 0
    while pos + 32 <= len(table):
        _, _, data, size, _, n = struct.unpack_from('<IIQQII', table, pos)
        name = table[pos + 32:pos + 32 + n].decode('utf-8', 'replace')
        require(h[9] + data + size <= len(image), 'Control RomFS file bounds')
        files[name] = image[h[9] + data:h[9] + data + size]
        pos += 32 + (n + 3) // 4 * 4
    return files


def control_icon(package, keys_path):
    """The icon JPEG of the Control NCA in package (NSP, NSZ or XCI)."""
    keys = load_keys(keys_path)
    with package.open('rb') as f:
        _, entries = container_entries(f, package.stat().st_size)
        controls = [(r, h) for r, h in nca_inventory(f, entries, keys) if r['content_type'] == CONTROL]
        require(controls, f'{package.name} has no Control NCA')
        record, hdr = controls[0]
        ctr_key = nca_ctr_key(hdr, keys, common_tickets(f, entries))
        for i in range(4):
            start, end = struct.unpack_from('<II', hdr, 0x240 + i * 16)
            fs = hdr[0x400 + 512 * i:0x600 + 512 * i]
            if not start or fs[2:5] != ROMFS_CTR:
                continue
            offset, size = struct.unpack_from('<QQ', fs, 0x18 + 5 * 24)   # IVFC level 5: the RomFS itself
            require(size <= MAX_ROMFS and offset + size <= (end - start) * 512, 'Control RomFS bounds')
            absolute = start * 512 + offset
            aligned = absolute & ~15
            iv = fs[0x140:0x148][::-1] + (aligned // 16).to_bytes(8, 'big')
            data = read_at(f, record_offset(record) + aligned, size + absolute - aligned)
            files = romfs_files(decrypt(ctr_key, modes.CTR(iv), data)[absolute - aligned:])
            icons = {n[5:-4]: d for n, d in files.items() if n.startswith('icon_') and n.endswith('.dat')}
            require(icons, f'{package.name}: the Control NCA has no icon')
            icon = next((icons[lang] for lang in LANGUAGES if lang in icons), next(iter(icons.values())))
            require(icon[:3] == b'\xff\xd8\xff', f'{package.name}: the icon is no JPEG')
            return icon
    raise ValueError(f'{package.name}: the Control NCA has no RomFS section')


def write(package, keys_path, out):
    """Write the icon of package to out (a .jpg path); returns its size in bytes."""
    icon = control_icon(Path(package), Path(keys_path))
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(icon)
    return len(icon)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--package', type=Path, required=True, help='a base game or update package (.nsp, .nsz, .xci)')
    ap.add_argument('--keys', type=Path, required=True, help='prod.keys')
    ap.add_argument('--out', type=Path, required=True, help='the .jpg to write')
    args = ap.parse_args()
    print(f'{args.out}: {write(args.package, args.keys, args.out)} bytes')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        sys.exit(f'error: {error}')
