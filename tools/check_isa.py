"""Refuse instructions above x86-64-v2 in objects built for --cpu v2, and above AMD K10 in objects built for --cpu k10.

The v2 build is for CPUs without AVX, AVX2, FMA, BMI1/2, F16C, LZCNT and MOVBE (a Pentium Gold G6400, an older Core
i7). The compiler keeps generated code inside that set by -march=x86-64-v2, but inline assembly, target attributes and
intrinsics pass through: kern_ap_stub (runtime/kernel.c) saved YMM registers with vmovdqa in the v2 library of 0.3.1
and crashed such a CPU with an illegal instruction the first time a core was interrupted. tools/build_game.py --cpu v2
runs this over every runtime object before it links or archives them.

The k10 build is for AMD family 10h (Phenom II X4/X6, Athlon II): SSE3, SSE4a, POPCNT and LZCNT, but none of SSSE3,
SSE4.1 or SSE4.2. Its check refuses everything the v2 check does except lzcnt (K10 has ABM), plus the SSSE3/SSE4
mnemonics in NON_K10.

A function that holds such an instruction behind a runtime CPU check goes in DISPATCHED with that check named.

    python tools/check_isa.py [--level v2|k10] [--objdump PATH] FILE...   (objects, archives or executables)

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
# SSSE3, SSE4.1 and SSE4.2 (all inside x86-64-v2), which AMD K10 lacks. pextrw/pinsrw with a register operand are SSE2
# and stay allowed; the SSE4.1 memory form of pextrw is rare enough to leave out.
NON_K10 = (NON_V2 - {'lzcnt'}) | {
    'pshufb', 'phaddw', 'phaddd', 'phaddsw', 'phsubw', 'phsubd', 'phsubsw', 'pmaddubsw', 'pmulhrsw', 'psignb',
    'psignw', 'psignd', 'pabsb', 'pabsw', 'pabsd', 'palignr',
    'blendps', 'blendpd', 'blendvps', 'blendvpd', 'pblendw', 'pblendvb', 'roundps', 'roundpd', 'roundss', 'roundsd',
    'dpps', 'dppd', 'insertps', 'extractps', 'mpsadbw', 'phminposuw', 'pmulld', 'pmuldq', 'pminsb', 'pminsd',
    'pminuw', 'pminud', 'pmaxsb', 'pmaxsd', 'pmaxuw', 'pmaxud', 'pextrb', 'pextrd', 'pextrq', 'pinsrb', 'pinsrd',
    'pinsrq', 'ptest', 'packusdw', 'pcmpeqq', 'movntdqa',
    'pmovsxbw', 'pmovsxbd', 'pmovsxbq', 'pmovsxwd', 'pmovsxwq', 'pmovsxdq',
    'pmovzxbw', 'pmovzxbd', 'pmovzxbq', 'pmovzxwd', 'pmovzxwq', 'pmovzxdq',
    'pcmpgtq', 'crc32', 'crc32b', 'crc32w', 'crc32l', 'crc32q', 'pcmpestri', 'pcmpestrm', 'pcmpistri', 'pcmpistrm'}
LEVEL_SET = {'v2': NON_V2, 'k10': NON_K10}
LEVEL_NAME = {'v2': 'x86-64-v2', 'k10': 'AMD K10 (SSE3/SSE4a)'}
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


def strip_suffix(mn):
    """AT&T size suffixes (crc32l, pextrdl...) are not in the sets; try the bare mnemonic too."""
    return mn[:-1] if mn[-1:] in 'bwlq' else mn


def is_non_v2(line, mn, level='v2'):
    if mn.startswith('v') and mn not in ('verr', 'verw'):
        return True   # VEX or EVEX: AVX, AVX2, FMA, F16C, AVX-512
    if mn.startswith('k') and WIDE.search(line):
        return True   # AVX-512 mask registers
    bad = LEVEL_SET[level]
    return mn in bad or strip_suffix(mn) in bad or bool(WIDE.search(line))


def scan_one(path, objdump, level='v2'):
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
            # ThinLTO renames a promoted static to <name>.llvm.<hash>; DISPATCHED names the source function
            func = re.sub(r'\.llvm\.\d+$', '', m.group(1))
            continue
        if not line[:1].isspace():
            continue
        mn = mnemonic(line)
        if mn and func not in DISPATCHED and is_non_v2(line, mn, level):
            hits[(obj, func)][mn] += 1
    return hits


def scan(paths, objdump, level='v2'):
    """{(file, function): Counter of mnemonics} for every function outside DISPATCHED with an instruction above level."""
    hits = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(16, os.cpu_count() or 1)) as pool:
        for h in pool.map(lambda p: scan_one(p, objdump, level), paths):
            hits.update(h)
    return hits


def check_objects(paths, objdump, record, level='v2'):
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
    hits = scan(todo, objdump, level)
    if hits:
        record.unlink(missing_ok=True)
        return report(hits, level)
    record.write_text(json.dumps(stamp))
    print(f'check_isa: {len(todo)} of {len(paths)} runtime objects scanned, none above {LEVEL_NAME[level]}')
    return None


def report(hits, level='v2'):
    lines = [f'  {f} in {o}: ' + ', '.join(f'{k} x{v}' for k, v in c.most_common(6))
             for (o, f), c in sorted(hits.items())]
    if level == 'k10':
        return ('instructions above AMD K10 in a --cpu k10 build (a Phenom II stops on SSSE3/SSE4.1/SSE4.2/AVX with an '
                'illegal instruction); use SSE2/SSE3 code under #ifndef __SSSE3__, or a runtime CPU check and an entry '
                'in tools/check_isa.py DISPATCHED:\n' + '\n'.join(lines))
    return ('instructions above x86-64-v2 in a --cpu v2 build (a CPU without AVX/BMI stops on them with an illegal '
            'instruction); use SSE4.2 code under #ifndef __AVX__, or a runtime CPU check and an entry in '
            'tools/check_isa.py DISPATCHED:\n' + '\n'.join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('files', nargs='+', type=Path)
    ap.add_argument('--objdump', help='llvm-objdump or objdump to use (default: PATH)')
    ap.add_argument('--level', choices=('v2', 'k10'), default='v2', help='CPU level to check against (default v2)')
    args = ap.parse_args()
    objdump = find_objdump(args.objdump)
    if not objdump:
        sys.exit('check_isa: no llvm-objdump or objdump found')
    hits = scan(args.files, objdump, args.level)
    if hits:
        sys.exit(report(hits, args.level))
    print(f'check_isa: {len(args.files)} files, no instructions above {LEVEL_NAME[args.level]}')


if __name__ == '__main__':
    main()
