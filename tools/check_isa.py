"""Refuse instructions above x86-64-v2 in objects built for --cpu v2.

The v2 build is for CPUs without AVX, AVX2, FMA, BMI1/2, F16C, LZCNT and MOVBE (a Pentium Gold G6400, an older Core
i7). The compiler keeps generated code inside that set by -march=x86-64-v2, but inline assembly, target attributes and
intrinsics pass through: kern_ap_stub (runtime/kernel.c) saved YMM registers with vmovdqa in the v2 library of 0.3.1
and crashed such a CPU with an illegal instruction the first time a core was interrupted. tools/build_game.py --cpu v2
runs this over every runtime object before it links or archives them.

A function that holds such an instruction behind a runtime CPU check goes in DISPATCHED with that check named.

    python tools/check_isa.py [--objdump PATH] FILE...   (objects, archives or executables)

Standard library only.
"""

import argparse
import collections
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

# Mnemonics outside x86-64-v2 that are not VEX/EVEX encoded, or VEX encoded but without a v prefix (BMI1/2).
# lzcnt and tzcnt decode as bsr and bsf on older CPUs and give other results; tzcnt is left out because the
# compiler emits it on any x86-64 level where bsf would do (a nonzero input, or one it checks first).
NON_V2 = {'andn', 'bextr', 'blsi', 'blsmsk', 'blsr', 'bzhi', 'shlx', 'shrx', 'sarx', 'rorx', 'pdep', 'pext', 'mulx',
          'lzcnt', 'movbe', 'adcx', 'adox', 'sha1rnds4', 'sha1nexte', 'sha1msg1', 'sha1msg2', 'sha256rnds2',
          'sha256msg1', 'sha256msg2', 'aesenc', 'aesenclast', 'aesdec', 'aesdeclast', 'aesimc', 'aeskeygenassist',
          'pclmulqdq', 'rdrand', 'rdseed'}
PREFIXES = {'rep', 'repe', 'repz', 'repne', 'repnz', 'lock', 'notrack', 'data16', 'cs', 'ds', 'es', 'ss', 'fs', 'gs'}
WIDE = re.compile(r'%[yz]mm\d|%k[0-7]\b')

# Function -> the runtime check that keeps it off CPUs without the instructions.
DISPATCHED = {
    'blocks_ni': 'runtime/sha256.c sha256_blocks: __builtin_cpu_supports("sha") && __builtin_cpu_supports("sse4.1")',
}


def find_objdump(hint=None):
    """llvm-objdump next to the compiler in use, else on PATH, else GNU objdump; None when there is none."""
    for p in ([Path(hint)] if hint else []):
        if p.is_file():
            return str(p)
    for name in ('llvm-objdump', 'objdump'):
        found = shutil.which(name)
        if found:
            return found
    return None


def mnemonic(line):
    """The mnemonic of one disassembly line (llvm-objdump or GNU objdump, AT&T syntax), or None."""
    s = line.strip()
    m = re.match(r'^[0-9a-f]+:\s*', s)
    if m:
        s = s[m.end():]
    words = s.split()
    while words and words[0] in PREFIXES:
        words = words[1:]
    return words[0] if words else None


def is_non_v2(line, mn):
    if mn.startswith('v') and mn not in ('verr', 'verw'):
        return True   # VEX or EVEX: AVX, AVX2, FMA, F16C, AVX-512
    if mn.startswith('k') and WIDE.search(line):
        return True   # AVX-512 mask registers
    return mn in NON_V2 or bool(WIDE.search(line))


def scan_one(path, objdump):
    """{(file, function): Counter of mnemonics} of one file."""
    hits = collections.defaultdict(collections.Counter)
    r = subprocess.run([objdump, '-d', '--no-show-raw-insn', str(path)], capture_output=True, text=True,
                       errors='replace')
    if r.returncode:
        sys.exit(f'check_isa: {objdump} failed on {path}: {r.stderr.strip()[:300]}')
    obj = str(path)
    func = None
    for line in r.stdout.splitlines():
        if 'file format' in line:
            obj = line.split(':\tfile format')[0].split(':     file format')[0].strip()
            continue
        m = re.match(r'^[0-9a-f]*\s*<(.+)>:$', line)
        if m:
            func = m.group(1)
            continue
        if not line[:1].isspace():
            continue
        mn = mnemonic(line)
        if mn and func not in DISPATCHED and is_non_v2(line, mn):
            hits[(obj, func)][mn] += 1
    return hits


def scan(paths, objdump):
    """{(file, function): Counter of mnemonics} for every function outside DISPATCHED with a non-v2 instruction."""
    hits = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(16, os.cpu_count() or 1)) as pool:
        for h in pool.map(lambda p: scan_one(p, objdump), paths):
            hits.update(h)
    return hits


def check_objects(paths, objdump, record):
    """Scan the objects changed since the last clean scan (their sizes and mtimes in the JSON file record); return
    the failure text, or None. Without an objdump it warns and passes: the check guards the build, it is no part
    of it."""
    if not objdump:
        print('check_isa: no llvm-objdump or objdump; the v2 instruction check is skipped', file=sys.stderr)
        return None
    try:
        seen = json.loads(record.read_text())
    except (OSError, ValueError):
        seen = {}
    stamp = {str(p): [p.stat().st_size, p.stat().st_mtime_ns] for p in paths}
    todo = [p for p in paths if seen.get(str(p)) != stamp[str(p)]]
    hits = scan(todo, objdump)
    if hits:
        record.unlink(missing_ok=True)
        return report(hits)
    record.write_text(json.dumps(stamp))
    print(f'check_isa: {len(todo)} of {len(paths)} runtime objects scanned, none above x86-64-v2')
    return None


def report(hits):
    lines = [f'  {f} in {o}: ' + ', '.join(f'{k} x{v}' for k, v in c.most_common(6))
             for (o, f), c in sorted(hits.items())]
    return ('instructions above x86-64-v2 in a --cpu v2 build (a CPU without AVX/BMI stops on them with an illegal '
            'instruction); use SSE4.2 code under #ifndef __AVX__, or a runtime CPU check and an entry in '
            'tools/check_isa.py DISPATCHED:\n' + '\n'.join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('files', nargs='+', type=Path)
    ap.add_argument('--objdump', help='llvm-objdump or objdump to use (default: PATH)')
    args = ap.parse_args()
    objdump = find_objdump(args.objdump)
    if not objdump:
        sys.exit('check_isa: no llvm-objdump or objdump found')
    hits = scan(args.files, objdump)
    if hits:
        sys.exit(report(hits))
    print(f'check_isa: {len(args.files)} files, no instructions above x86-64-v2')


if __name__ == '__main__':
    main()
