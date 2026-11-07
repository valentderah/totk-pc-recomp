"""ExeFS patches of the mods folder to code patch sets and data writes: IPS, IPS32 and IPSwitch (.pchtxt) files for
the game's main.

Lifted code cannot be byte-patched when the game runs, so a patch file's writes into main's code become `site` lines
(recomp/patches.py), one patch set per file, which the runtime switches on while its mod is on (runtime/mods.c). The
writes go through the same rules as the mods' code cheats (tools/cheats.py convert): an instruction replaced in place,
a branch into a code cave the file writes into main's zero padding inlined at the branch. Writes into main's data
(.rodata, .data, .bss) need no lifting: the runtime stores them into the loaded module before the game runs, as
emulators and Atmosphere apply a patch file to the module before its dynamic linker runs (data_runs).

  python tools/exefs_patches.py --exefs <dir> --mods <mods folder> --patches <patch file> --out <patch file>
                                --status <file> --data <file>
  python tools/exefs_patches.py --exefs <dir> --status <file> --data <file> --folded <gen>/main/folded.txt

The files are <mod>/exefs/*.ips and *.pchtxt (or the Atmosphere places of the exefs folder, as runtime/mods.c finds
them) and <mod>/atmosphere/exefs_patches/<name>/*.ips. A file is for main when its build id matches main's: an IPS
file's name, a pchtxt file's @nsobid line (trailing zeros may be left out). Offsets are as emulators read them: IPS
offsets count the NSO header (0x100 bytes) before main's image; a pchtxt offset adds its `@flag offset_shift` first.
A pchtxt patch line applies between @enabled and @disabled; @stop ends the file.

--out gets the --patches file followed by one set per file with sites (`mod_<hash>`, switch TOTK_MODPATCH_<HASH>,
default off) and its sites. --status gets one line per file, mods by name, tab separated:

  <file hash> <content hash> <kind> <built-in sets> <note>

file hash: FNV-1a 32 of "<mod folder name>/<path in the mod>" (forward slashes); content hash: the same over the file's
bytes, so the runtime sees a file that changed after the build. kind:
  code         a set of sites, and data writes when the file has some
  data         data writes only
  other        for another game version or module: not used
  empty        no write into main for this version (or only the game's own bytes)
  unsupported  <note> says why: a write the site rules cannot express, a write outside main's image, into the dynamic
               linker's tables or into a pointer it binds lazily, or (after the lift) into bytes the lifted code reads
               as constants
built-in sets: the port's own sets (the --patches file's sets that are no cheats) with a site this file patches too,
comma separated, or "-": the runtime switches them off while the mod is on, so the mod's code runs. The note of a code
or data file with data writes is `data <bytes>`, the bytes its data lines hold: the runtime keeps the mod's patches off
when the data file does not hold them all.

--data gets one line per run of data bytes of the code and data files, tab separated (runtime/mods.c reads it):

  <file hash> <content hash> <offset in main, hex> <bytes, hex> [base]

`base` marks a DT_RELR word: 8 bytes, little-endian, the word before relocation; the runtime adds main's base, as the
dynamic linker does. Without --data a data write makes its file unsupported.

The lifted code reads some of main's .text and .rodata at lift time instead of at run time: the jump tables
(recomp/jumptables.py), whose targets become labels of the generated C. A data write over such bytes would not reach
the lifted code. recomp/gen.py lists the ranges it read in <gen>/<module>/folded.txt; the second form above, run after
the lift, makes every file with a data write into those ranges unsupported (its status line says so and its data lines
go). Without that list, every data write into .rodata counts as one into read bytes.

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
from nso import (DT_JMPREL, DT_NULL, DT_PLTRELSZ, DT_RELA, DT_RELASZ, DT_RELR, DT_RELRSZ, DT_STRSZ,  # noqa: E402
                 DT_STRTAB, DT_SYMTAB, R_AARCH64_JUMP_SLOT)

NSO_HEADER = 0x100
EXEFS_PLACES = ('exefs', f'atmosphere/contents/{TITLE}/exefs', f'contents/{TITLE}/exefs', f'{TITLE}/exefs')
RUN_MAX = 256   # bytes per line of the data file (runtime/mods.c reads lines of at most 1024 characters)


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


# ---- writes into data

def relocated(n):
    """({offset: type} of the words main's explicit-addend relocations (DT_RELA, DT_JMPREL) fill, {offset} of its
    DT_RELR words): the 8-byte words the dynamic linker writes at start (runtime/loader.c relocate_table,
    relocate_relr)."""
    rela = {}
    for table, size in ((DT_RELA, DT_RELASZ), (DT_JMPREL, DT_PLTRELSZ)):
        if table in n.dyn:
            for i in range(n.dyn[size] // 24):
                off, info, _ = struct.unpack_from('<QQq', n.image, n.dyn[table] + 24 * i)
                rela[off] = info & 0xFFFFFFFF
    relr, where = set(), 0
    for i in range(n.dyn.get(DT_RELRSZ, 0) // 8):
        e = struct.unpack_from('<Q', n.image, n.dyn[DT_RELR] + 8 * i)[0]
        if not e & 1:   # an address; the bitmap after it covers the 63 words that follow
            relr.add(e)
            where = e + 8
        else:
            relr.update(where + 8 * k for k in range(63) if e >> (k + 1) & 1)
            where += 63 * 8
    return rela, relr


def linker_tables(n):
    """[(start, end)] of main's .dynamic, relocation, symbol and string tables: the dynamic linker reads them at start,
    so a write into them would change what it does, which no store into the loaded module can follow."""
    end = n.dynamic
    while struct.unpack_from('<Q', n.image, end)[0] != DT_NULL:
        end += 16
    out = [(n.dynamic, end + 16)]
    for table, size in ((DT_RELA, DT_RELASZ), (DT_JMPREL, DT_PLTRELSZ), (DT_RELR, DT_RELRSZ), (DT_STRTAB, DT_STRSZ)):
        if table in n.dyn and size in n.dyn:
            out.append((n.dyn[table], n.dyn[table] + n.dyn[size]))
    if DT_SYMTAB in n.dyn:
        out.append((n.dyn[DT_SYMTAB], n.dyn[DT_SYMTAB] + 24 * n.symbol_count()))
    return out


class MainData:
    """What a write into main's data meets: main's image from .rodata to the end of .bss, the words the dynamic linker
    fills and the tables it reads."""

    def __init__(self, main):
        n = main.nso
        self.image, self.lo, self.size, self.data = main.image, main.code_end, n.size, n.data[0]
        self.rela, self.relr = relocated(n)
        self.linker = linker_tables(n)


def data_runs(md, writes):
    """The data part of one file ({offset: byte} outside main's code), applied as emulators and Atmosphere apply a patch
    file: to the module before its dynamic linker runs. Returns ([(offset, bytes, rebase)], dropped):

    - a byte no relocation covers is stored as it is (runs of consecutive bytes, at most RUN_MAX long);
    - a DT_RELR word holds its addend: the word with the file's bytes in it, rebase set (main's base gets added);
    - the linker overwrites a word an explicit-addend relocation fills whatever the file wrote there: such bytes
      change nothing and only count in dropped.

    Bytes equal to main's are left out. ValueError for a write outside main's image (below .rodata: the NSO header),
    into the dynamic linker's tables, or into a function pointer it binds lazily (JUMP_SLOT): on the Switch it holds
    the file's value plus main's base until the first call binds it, which no store before the start can reproduce."""
    plain, words, dropped = {}, {}, 0
    for off in sorted(writes):
        if not md.lo <= off < md.size:
            raise ValueError(f'it writes outside main\'s image at main+{off:X}')
        if any(a <= off < b for a, b in md.linker):
            raise ValueError(f'it writes the dynamic linker\'s tables at main+{off:X}')
        w = next((w for w in range(off - 7, off + 1) if w in md.rela or w in md.relr), None)
        if w is None:
            plain[off] = writes[off]
        elif w in md.relr:
            words.setdefault(w, bytearray(md.image[w:w + 8]))[off - w] = writes[off]
        elif md.rela[w] == R_AARCH64_JUMP_SLOT:
            raise ValueError(f'it writes a function pointer the dynamic linker binds lazily at main+{w:X}')
        else:
            dropped += 1
    runs = []
    for off in sorted(o for o, b in plain.items() if b != md.image[o]):
        last = runs[-1] if runs else None
        if last and last[0] + len(last[1]) == off and len(last[1]) < RUN_MAX:
            runs[-1] = (last[0], last[1] + bytes([plain[off]]), False)
        else:
            runs.append((off, bytes([plain[off]]), False))
    runs += [(w, bytes(b), True) for w, b in words.items() if bytes(b) != md.image[w:w + 8]]
    return sorted(runs), dropped


def data_line(key, content, run):
    off, data, rebase = run
    return f'{key}\t{content}\t{off:x}\t{data.hex()}' + ('\tbase' if rebase else '')


def builtin_sites(text):
    """{offset in main: [set name]} of the patch file's sets that are no cheats or mod patches."""
    out = {}
    for line in text.splitlines():
        f = line.split('#', 1)[0].split()
        if len(f) >= 4 and f[0] == 'site' and f[2] == 'main' and not f[1].startswith(('cheat_', 'mod_')):
            out.setdefault(int(f[3], 16), []).append(f[1])
    return out


def classify(m, md, bid, path, raw):
    """(kind, note, sites, runs, dropped) of one patch file: path gives its name and suffix, raw its bytes, bid main's
    build id; md None: no data file, so a write into main's data makes the file unsupported."""
    try:
        if path.suffix.lower() == '.ips':
            for_main = same_build(path.stem, bid)
            writes = parse_ips(raw) if for_main else {}
        else:
            pid, writes = parse_pchtxt(raw.decode('utf-8-sig', errors='replace'))
            for_main = pid is not None and same_build(pid, bid)
        if not for_main:
            return 'other', '', {}, [], 0
        code = {o: b for o, b in writes.items() if m.in_code(o - o % 4)}
        data = {o: b for o, b in writes.items() if o not in code}
        if data and md is None:
            raise ValueError(f'it writes main\'s data at main+{min(data):X} (only code patches are supported)')
        sites = convert(m, word_writes(m, code)) if code else {}
        runs, dropped = data_runs(md, data) if data else ([], 0)
        return 'code' if sites else 'data' if runs else 'empty', '', sites, runs, dropped
    except ValueError as e:
        return 'unsupported', str(e), {}, [], 0


DATA_HEADER = '# file hash, content hash, offset in main, bytes[, base] (tools/exefs_patches.py)'


def build(args):
    m = Main(args.exefs / 'main')
    md = MainData(m) if args.data else None
    bid = m.nso.build_id
    text = args.patches.read_text()
    builtin = builtin_sites(text)
    files = patch_files(args.mods)
    sets, sites, status, data_lines, kinds = [], [], [], [], {}
    for mod, rel, f in files:
        raw = f.read_bytes()
        key, content = fnv(f'{mod}/{rel}'.encode()), fnv(raw)
        kind, note, file_sites, runs, dropped = classify(m, md, bid, f, raw)
        over = []
        if file_sites:
            name = f'mod_{key.lower()}'
            label = f'{mod}/{rel}'.encode('ascii', 'replace').decode()
            sets.append(f'set {name} TOTK_MODPATCH_{key} 0   # {label}')
            for a, repl in sorted(file_sites.items()):
                sites.append(f'site {name} main {a:x} {m.word(a):08x} {" ".join(f"{w:08x}" for w in repl)}')
                over += [s for s in builtin.get(a, []) if s not in over]
        data_lines += [data_line(key, content, r) for r in runs]
        data_bytes = sum(len(r[1]) for r in runs)
        kinds[kind] = kinds.get(kind, 0) + 1
        status_note = f'data {data_bytes}' if runs else note.replace(chr(9), ' ')
        status.append(f'{key}\t{content}\t{kind}\t{",".join(over) or "-"}\t{status_note}')
        parts = [note] if note else []
        if file_sites:
            parts.append(f'{len(file_sites)} sites' + (f', over {", ".join(over)}' if over else ''))
        if runs:
            parts.append(f'{data_bytes} bytes of main\'s data')
        if dropped:
            parts.append(f'{dropped} bytes into words the dynamic linker fills, which change nothing')
        print(f'exefs: {kind:11} {mod}/{rel}' + (f' ({"; ".join(parts)})' if parts else ''))
    if sets:
        text += ('\n# ExeFS patches of the mods folder (tools/exefs_patches.py)\n' + '\n'.join(sets) + '\n'
                 + '\n'.join(sites) + '\n')
    args.out.write_text(text)
    args.status.parent.mkdir(parents=True, exist_ok=True)
    args.status.write_text('\n'.join(status) + ('\n' if status else ''), encoding='utf-8')
    if args.data:
        args.data.parent.mkdir(parents=True, exist_ok=True)
        args.data.write_text('\n'.join([DATA_HEADER] + data_lines) + '\n', encoding='utf-8')
    print(f'exefs: {len(files)} patch files: ' + ', '.join(f'{n} {k}' for k, n in sorted(kinds.items()))
          + f'; {len(sites)} sites, {len(data_lines)} data runs')


def read_ranges(path):
    """[(start, end)] of a folded.txt (recomp/gen.py): hex module offsets, one range per line, '#' comments."""
    out = []
    for line in path.read_text().splitlines():
        f = line.split('#', 1)[0].split()
        if len(f) == 2:
            out.append((int(f[0], 16), int(f[1], 16)))
    return sorted(out)


def data_offset(main_path):
    """The offset of main's .data segment in its image, from the NSO header (no decompression)."""
    with open(main_path, 'rb') as f:
        head = f.read(0x40)
    return struct.unpack_from('<III', head, 0x30)[1]


def recheck(args):
    """After the lift: every file with a data write into bytes the lifted code read at lift time becomes unsupported."""
    if not args.status.is_file() or not args.data.is_file():
        print('exefs: no status or data file: nothing to check')
        return
    folded = read_ranges(args.folded) if args.folded.is_file() else None
    data_off = data_offset(args.exefs / 'main')
    lines = args.data.read_text(encoding='utf-8').splitlines()
    bad = {}
    for line in lines:
        f = line.split('\t')
        if line.startswith('#') or len(f) < 4 or f[0] in bad:
            continue
        a = int(f[2], 16)
        b = a + len(f[3]) // 2
        if folded is None and a < data_off:
            bad[f[0]] = (f'it writes main+{a:X} in .rodata, and the lift left no list of the bytes it reads as '
                         f'constants')
        elif folded is not None:
            hit = next((s for s, e in folded if s < b and a < e), None)
            if hit is not None:
                bad[f[0]] = (f'it writes main+{max(a, hit):X}, which the lifted code reads when the game is built '
                             f'(a jump table): the change would not reach it')
    if not bad:
        print(f'exefs: data writes checked against {"the bytes the lift read" if folded is not None else ".rodata"}: '
              f'none meets them')
        return
    status = []
    for line in args.status.read_text(encoding='utf-8').splitlines():
        f = line.split('\t')
        if len(f) >= 5 and f[0] in bad:
            line = '\t'.join([f[0], f[1], 'unsupported', '-', bad[f[0]]])
            print(f'exefs: unsupported file {f[0]} ({bad[f[0]]})')
        status.append(line)
    args.status.write_text('\n'.join(status) + '\n', encoding='utf-8')
    kept = [line for line in lines if line.split('\t')[0] not in bad]
    args.data.write_text('\n'.join(kept) + '\n', encoding='utf-8')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exefs', type=Path, required=True)
    ap.add_argument('--mods', type=Path, help='the mods folder next to totk.exe')
    ap.add_argument('--patches', type=Path, help='the patch file so far')
    ap.add_argument('--out', type=Path, help='the patch file with the mods\' ExeFS patches added')
    ap.add_argument('--status', type=Path, required=True)
    ap.add_argument('--data', type=Path, help='the data writes of the mods\' patch files (runtime/mods.c reads them)')
    ap.add_argument('--folded', type=Path, help='after the lift: <gen>/main/folded.txt; checks --status and --data '
                                                'against it instead of building')
    args = ap.parse_args()
    sys.stdout.reconfigure(errors='replace')
    if args.folded:
        if not args.data:
            ap.error('--folded needs --data')
        recheck(args)
    else:
        if not (args.mods and args.patches and args.out):
            ap.error('--mods, --patches and --out are needed to build')
        build(args)


if __name__ == '__main__':
    main()
