"""ExeFS patches of the mods folder to code patch sets: IPS, IPS32 and IPSwitch (.pchtxt) files for the game's main.

Lifted code cannot be byte-patched when the game runs, so a patch file's writes into main's code become `site` lines
(recomp/patches.py), one patch set per file, which the runtime switches on while its mod is on (runtime/mods.c). The
writes go through the same rules as the mods' code cheats (tools/cheats.py convert): an instruction replaced in place,
a branch into a code cave the file writes into main's zero padding inlined at the branch.

  python tools/exefs_patches.py --exefs <dir> --mods <mods folder> --patches <patch file> --out <patch file> --status <file>

The files are <mod>/exefs/*.ips and *.pchtxt (or the Atmosphere places of the exefs folder, as runtime/mods.c finds
them) and <mod>/atmosphere/exefs_patches/<name>/*.ips. A file is for main when its build id matches main's: an IPS
file's name, a pchtxt file's @nsobid line (trailing zeros may be left out). Offsets are as emulators read them: IPS
offsets count the NSO header (0x100 bytes) before main's image; a pchtxt offset adds its `@flag offset_shift` first.
A pchtxt patch line applies between @enabled and @disabled; @stop ends the file.

--out gets the --patches file followed by one set per usable file (`mod_<hash>`, switch TOTK_MODPATCH_<HASH>, default
off) and its sites. --status gets one line per file, mods by name, tab separated:

  <file hash> <content hash> <kind> <built-in sets> <note>

file hash: FNV-1a 32 of "<mod folder name>/<path in the mod>" (forward slashes); content hash: the same over the file's
bytes, so the runtime sees a file that changed after the build. kind:
  code         a set of sites
  other        for another game version or module: not used
  empty        no write into main for this version (or only the game's own words)
  unsupported  <note> says why: a write into main's data, a write the site rules cannot express
built-in sets: the port's own sets (the --patches file's sets that are no cheats) with a site this file patches too,
comma separated, or "-": the runtime switches them off while the mod is on, so the mod's code runs.

A file the rules cannot express is unsupported as a whole: half of a patch is worse than none.

Standard library, recomp/ and tools/cheats.py only.
"""
import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cheats import TITLE, Main, convert, fnv  # noqa: E402

NSO_HEADER = 0x100
EXEFS_PLACES = ('exefs', f'atmosphere/contents/{TITLE}/exefs', f'contents/{TITLE}/exefs', f'{TITLE}/exefs')


def patch_files(mods):
    """[(mod folder name, path in the mod, file)] of the folders of mods, by name: the .ips and .pchtxt files of the
    first exefs place that exists (runtime/mods.c find_root) and of atmosphere/exefs_patches/<name>/."""
    out = []
    for mod in sorted((d for d in mods.iterdir() if d.is_dir()), key=lambda d: d.name) if mods.is_dir() else []:
        folders = []
        exefs = next((mod / w for w in EXEFS_PLACES if (mod / w).is_dir()), None)
        if exefs:
            folders.append(exefs)
        patches = mod / 'atmosphere' / 'exefs_patches'
        if patches.is_dir():
            folders += sorted(d for d in patches.iterdir() if d.is_dir())
        for folder in folders:
            for f in sorted(folder.iterdir()):
                if f.is_file() and f.suffix.lower() in ('.ips', '.pchtxt'):
                    out.append((mod.name, f.relative_to(mod).as_posix(), f))
    return out


def same_build(name, build_id):
    """1 when name (hex, any case, trailing zeros left out or not) is main's build id (hex of its 32 bytes)."""
    name = name.lower()
    return bool(re.fullmatch(r'[0-9a-f]{1,64}', name)) and name.ljust(64, '0') == build_id.ljust(64, '0')


# ---- the formats: {offset in main's image: byte}

def parse_ips(data):
    """IPS (PATCH ... EOF, 3-byte offsets) or IPS32 (IPS32 ... EEOF, 4-byte offsets); ValueError when it is neither."""
    if data[:5] == b'PATCH':
        width, end, at = 3, b'EOF', 5
    elif data[:5] == b'IPS32':
        width, end, at = 4, b'EEOF', 5
    else:
        raise ValueError('not an IPS or IPS32 file')
    writes = {}
    while True:
        if data[at:at + len(end)] == end and at + len(end) >= len(data) - 3:
            return writes
        if at + width + 2 > len(data):
            raise ValueError('the IPS file is cut short')
        off = int.from_bytes(data[at:at + width], 'big')
        size = int.from_bytes(data[at + width:at + width + 2], 'big')
        at += width + 2
        if size:
            body = data[at:at + size]
            at += size
        else:   # a run: 2-byte length, 1 byte
            run = int.from_bytes(data[at:at + 2], 'big')
            body = data[at + 2:at + 3] * run
            at += 3
        if len(body) != (size or len(body)):
            raise ValueError('the IPS file is cut short')
        for i, b in enumerate(body):
            writes[off - NSO_HEADER + i] = b


def pchtxt_value(token):
    """The bytes of a pchtxt value: hex digits in byte order, or a quoted string with C escapes."""
    if token.startswith('"'):
        if not token.endswith('"') or len(token) < 2:
            raise ValueError(f'a string without its closing quote: {token}')
        return token[1:-1].encode('utf-8').decode('unicode_escape').encode('latin-1')
    if len(token) % 2 or not re.fullmatch(r'[0-9A-Fa-f]+', token):
        raise ValueError(f'not a hex value: {token}')
    return bytes.fromhex(token)


def parse_pchtxt(text):
    """(build id or None, {offset in main's image: byte}) of an IPSwitch file."""
    bid, shift, enabled, writes = None, 0, False, {}
    for raw in text.splitlines():
        line = raw.split('//', 1)[0].strip()
        if not line:
            continue
        if line.startswith('@stop'):
            break
        if line.startswith('@nsobid-'):
            bid = line[len('@nsobid-'):].strip()
        elif line.startswith('@enabled'):
            enabled = True
        elif line.startswith('@disabled'):
            enabled = False
        elif line.startswith('@flag'):
            f = line.split()
            if len(f) >= 3 and f[1] == 'offset_shift':
                shift = int(f[2], 0)
        elif line.startswith('@') or not enabled:
            continue
        else:
            m = re.fullmatch(r'([0-9A-Fa-f]+)\s+(".*"|\S+)', line)
            if not m:
                continue   # a title in brackets or a line of text
            off = int(m.group(1), 16) + shift - NSO_HEADER
            for i, b in enumerate(pchtxt_value(m.group(2))):
                writes[off + i] = b
    return bid, writes


# ---- writes to sites

def word_writes(main, writes):
    """{offset: word} of byte writes into main's code (.text and its padding), each word with the bytes the file does
    not write kept; ValueError for a write into main's data."""
    words = {}
    for off in sorted(writes):
        if not main.in_code(off - off % 4):
            raise ValueError(f'it writes main\'s data at main+{off:X} (only code patches are supported)')
        a = off - off % 4
        w = words.get(a, main.word(a))
        shift = (off % 4) * 8
        words[a] = (w & ~(0xFF << shift) | writes[off] << shift) & 0xFFFFFFFF
    return words


def builtin_sites(text):
    """{offset in main: [set name]} of the patch file's sets that are no cheats or mod patches."""
    out = {}
    for line in text.splitlines():
        f = line.split('#', 1)[0].split()
        if len(f) >= 4 and f[0] == 'site' and f[2] == 'main' and not f[1].startswith(('cheat_', 'mod_')):
            out.setdefault(int(f[3], 16), []).append(f[1])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exefs', type=Path, required=True)
    ap.add_argument('--mods', type=Path, required=True, help='the mods folder next to totk.exe')
    ap.add_argument('--patches', type=Path, required=True, help='the patch file so far')
    ap.add_argument('--out', type=Path, required=True, help='the patch file with the mods\' ExeFS patches added')
    ap.add_argument('--status', type=Path, required=True)
    args = ap.parse_args()
    sys.stdout.reconfigure(errors='replace')
    m = Main(args.exefs / 'main')
    raw = (args.exefs / 'main').read_bytes()
    bid = raw[0x40:0x60].hex()
    text = args.patches.read_text()
    builtin = builtin_sites(text)
    files = patch_files(args.mods)
    sets, sites, status, kinds = [], [], [], {}
    for mod, rel, f in files:
        data = f.read_bytes()
        key, content = fnv(f'{mod}/{rel}'.encode()), fnv(data)
        kind, note, over, file_sites = 'code', '', [], {}
        try:
            if f.suffix.lower() == '.ips':
                for_main = same_build(f.stem, bid)
                writes = parse_ips(data) if for_main else {}
            else:
                pid, writes = parse_pchtxt(data.decode('utf-8-sig', errors='replace'))
                for_main = pid is not None and same_build(pid, bid)
            if not for_main:
                kind = 'other'
            else:
                file_sites = convert(m, word_writes(m, writes))
                if not file_sites:
                    kind = 'empty'
        except ValueError as e:
            kind, note, file_sites = 'unsupported', str(e), {}
        if file_sites:
            name = f'mod_{key.lower()}'
            label = f'{mod}/{rel}'.encode('ascii', 'replace').decode()
            sets.append(f'set {name} TOTK_MODPATCH_{key} 0   # {label}')
            for a, repl in sorted(file_sites.items()):
                sites.append(f'site {name} main {a:x} {m.word(a):08x} {" ".join(f"{w:08x}" for w in repl)}')
                over += [s for s in builtin.get(a, []) if s not in over]
        kinds[kind] = kinds.get(kind, 0) + 1
        status.append(f'{key}\t{content}\t{kind}\t{",".join(over) or "-"}\t{note.replace(chr(9), " ")}')
        print(f'exefs: {kind:11} {mod}/{rel}' + (f' ({note})' if note else '')
              + (f' ({len(file_sites)} sites, over {", ".join(over)})' if over else
                 f' ({len(file_sites)} sites)' if file_sites else ''))
    if sets:
        text += ('\n# ExeFS patches of the mods folder (tools/exefs_patches.py)\n' + '\n'.join(sets) + '\n'
                 + '\n'.join(sites) + '\n')
    args.out.write_text(text)
    args.status.parent.mkdir(parents=True, exist_ok=True)
    args.status.write_text('\n'.join(status) + ('\n' if status else ''), encoding='utf-8')
    print(f'exefs: {len(files)} patch files: ' + ', '.join(f'{n} {k}' for k, n in sorted(kinds.items()))
          + f'; {len(sites)} sites')


if __name__ == '__main__':
    main()
