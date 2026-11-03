#!/usr/bin/env python3
"""Profile-guided optimisation: merge the counters an instrumented build wrote into the .profdata an optimised build reads.

    python tools/build_game.py --version v143 --pgo gen     build/v143-pgo-gen/totk.exe, instrumented
    <play: the scenes that should be fast; each run adds its own file to pgo-raw/>
    python tools/pgo.py merge --version v143                pgo/totk-v143.profdata
    python tools/build_game.py --version v143 --pgo use     build/v143-pgo/totk.exe, compiled with the profile

Where the counters go: LLVM_PROFILE_FILE, or profile.profraw in the working directory when it is unset. Set it once per
run so the runs do not overwrite each other:

    set LLVM_PROFILE_FILE=<game folder>\\pgo-raw\\run1-%p.profraw

LLVM matches a function to its profile by name and control-flow hash: code that changed since the training (another game
version, a generator change, any edit to runtime/) is compiled without profile data and only loses the gain. So train and
use on the same version, and merge again after changing the generator.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def llvm_profdata() -> Path:
    """llvm-profdata: LLVM_PROFDATA, then the MSYS2 installs, then PATH, then the LLVM installer's directory."""
    env = os.environ.get('LLVM_PROFDATA')
    if env and Path(env).is_file():
        return Path(env)
    names = ['llvm-profdata.exe', 'llvm-profdata']
    for d in (r'C:\msys64\clang64\bin', r'C:\msys64\ucrt64\bin', r'C:\msys64\mingw64\bin',
              r'D:\msys64\clang64\bin', r'D:\msys64\ucrt64\bin', r'D:\msys64\mingw64\bin',
              r'C:\Program Files\LLVM\bin'):
        for n in names:
            p = Path(d) / n
            if p.is_file():
                return p
    p = shutil.which('llvm-profdata')
    if p:
        return Path(p)
    sys.exit('pgo: llvm-profdata not found: set LLVM_PROFDATA to it (it ships with clang)')


def raw_files(version: str, given: list[Path]) -> list[Path]:
    """The .profraw files: those named (files or folders), else the training build's pgo-raw folders."""
    dirs = given or [ROOT / 'build' / f'{version}-pgo-gen' / 'pgo-raw', ROOT / 'build' / 'pgo-raw']
    out: list[Path] = []
    for d in dirs:
        if d.is_file():
            out.append(d)
        elif d.is_dir():
            out += sorted(d.rglob('*.profraw'))
    return [f for f in out if f.stat().st_size > 0]


def profdata_show(tool: Path, data: Path, top: int) -> str:
    r = subprocess.run([str(tool), 'show', '--counts', f'--topn={top}', str(data)], capture_output=True, text=True)
    return (r.stdout + r.stderr).strip()


def merge(args) -> int:
    files = raw_files(args.version, args.raw)
    if not files:
        where = ', '.join(str(p) for p in (args.raw or [ROOT / 'build' / f'{args.version}-pgo-gen' / 'pgo-raw']))
        sys.exit(f'pgo: no .profraw files in {where}: build with --pgo gen and play that build first')
    out = (args.out or ROOT / 'pgo' / f'totk-{args.version}.profdata').resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix('.profdata.tmp')
    tool = llvm_profdata()
    # --failure-mode=all: a run killed while it was writing its file is skipped with a note instead of failing the merge
    cmd = [str(tool), 'merge', '--failure-mode=all', '-o', str(tmp), *map(str, files)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        tmp.unlink(missing_ok=True)
        sys.exit(f'pgo: llvm-profdata merge failed: {(r.stdout + r.stderr).strip()[-1500:]}')
    os.replace(tmp, out)
    size = sum(f.stat().st_size for f in files)
    print(f'pgo: merged {len(files)} run(s), {size / 2**20:.1f} MiB of counters -> {below(out)} '
          f'({out.stat().st_size / 2**20:.1f} MiB)')
    if args.quiet:
        return 0
    show = profdata_show(tool, out, 8)
    for line in show.splitlines():
        if line.startswith(('Total functions', 'Total count', 'Maximum function count', 'Maximum internal block count')):
            print(f'pgo: {line}')
    top = [l.strip() for l in show.splitlines() if ':' in l and l.strip().split(':')[0].isdigit()]
    if top:
        print('pgo: hottest functions (name: count):')
        for line in top[:8]:
            print(f'  {line}')
    print(f'pgo: use it with: python tools/build_game.py --version {args.version} --pgo use')
    return 0


def show(args) -> int:
    data = (args.data or ROOT / 'pgo' / f'totk-{args.version}.profdata').resolve()
    if not data.is_file():
        sys.exit(f'pgo: {below(data)} not found')
    print(profdata_show(llvm_profdata(), data, args.top))
    return 0


def below(p: Path) -> str:
    try:
        return p.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return str(p)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('merge', help='merge the runs of a --pgo gen build into one .profdata')
    m.add_argument('--version', default='v143', help='game version the profile is for (default v143)')
    m.add_argument('--raw', type=Path, action='append', default=[], help='a .profraw file or a folder of them (repeatable)')
    m.add_argument('--out', type=Path, help='default pgo/totk-<version>.profdata')
    m.add_argument('--quiet', action='store_true', help='no summary of the merged profile')
    s = sub.add_parser('show', help='what a .profdata holds: functions, counts, the hottest ones')
    s.add_argument('--version', default='v143', help='game version the profile is for (default v143)')
    s.add_argument('--data', type=Path, help='default pgo/totk-<version>.profdata')
    s.add_argument('--top', type=int, default=25, help='how many of the hottest functions to list (default 25)')
    args = ap.parse_args(argv)
    return merge(args) if args.cmd == 'merge' else show(args)


if __name__ == '__main__':
    sys.exit(main())
