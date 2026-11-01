"""Build hactool: toolchain/hactool/hactool.exe (SciresM/hactool at a pinned commit + tools/hactool/*.patch).

Fetches the pinned commit into toolchain/src/hactool, resets it (local changes and build output there are discarded),
applies portable.patch (MacNativeGames' Windows-relevant changes) and unicode.patch (ours), and builds a static exe
with MSYS2 UCRT64 gcc; mbedtls comes with hactool's tree. The exe and hactool's LICENSE go to toolchain/hactool, where
tools/extract.py and tools/make_release.py take them. Run with .venv. On Linux: the system's cc and make build
toolchain/hactool/hactool (unicode.patch only adds a Windows manifest, under EXEEXT .exe).

  python tools/build_hactool.py [--src DIR] [--out DIR]
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
URL = 'https://github.com/SciresM/hactool.git'
COMMIT = '1d64a83450e025622f3468c28fc4164dad2c5ef6'
PATCHES = [ROOT / 'tools' / 'hactool' / n for n in ('portable.patch', 'unicode.patch', 'mbedtls-asm-clobbers.patch')]
WIN = os.name == 'nt'
MSYS_BASH = Path(r'C:\msys64\usr\bin\bash.exe')
EXE = 'hactool.exe' if WIN else 'hactool'
CONFIG_MK = ('CC = gcc\nEXEEXT = .exe\nCFLAGS = -O2 -Wall -Wextra -pedantic -std=gnu11\n'
             'LDFLAGS = -lmbedtls -lmbedx509 -lmbedcrypto -static\n') if WIN else \
            ('CC = cc\nEXEEXT =\nCFLAGS = -O2 -Wall -Wextra -pedantic -std=gnu11\n'
             'LDFLAGS = -lmbedtls -lmbedx509 -lmbedcrypto\n')


def git(src, *args):
    subprocess.run(['git', '-C', str(src), *args], check=True)


def fetch(src):
    """src holds COMMIT with no local changes or untracked files."""
    if not (src / '.git').is_dir():
        src.mkdir(parents=True, exist_ok=True)
        git(src, 'init', '-q')
        git(src, 'remote', 'add', 'origin', URL)
    head = subprocess.run(['git', '-C', str(src), 'rev-parse', 'HEAD'], capture_output=True, text=True).stdout.strip()
    if head != COMMIT:
        git(src, 'fetch', '-q', '--depth', '1', 'origin', COMMIT)
    git(src, 'checkout', '-q', '-f', COMMIT)
    git(src, 'clean', '-q', '-fdx')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--src', type=Path, default=ROOT / 'toolchain' / 'src' / 'hactool', help='source checkout')
    ap.add_argument('--out', type=Path, default=ROOT / 'toolchain' / 'hactool', help='where hactool(.exe) goes')
    ap.add_argument('--cc', help='Linux: the C compiler command for hactool and its bundled mbedtls, for example '
                                 '"clang --sysroot=DIR" (tools/make_release_linux.py); default cc')
    args = ap.parse_args()
    if WIN and not MSYS_BASH.is_file():
        sys.exit(f'{MSYS_BASH} not found: install MSYS2 with the UCRT64 gcc and make (docs/building.md)')
    if not WIN and not (shutil.which('make') and (args.cc or shutil.which('cc'))):
        sys.exit('make and cc not found: install a C compiler and make (docs/building.md)')
    src, out = args.src.resolve(), args.out.resolve()

    fetch(src)
    for p in PATCHES:
        git(src, 'apply', '--whitespace=nowarn', str(p))
    config = CONFIG_MK.replace('CC = cc\n', f'CC = {args.cc}\n') if args.cc else CONFIG_MK
    (src / 'config.mk').write_text(config, encoding='utf-8', newline='\n')

    jobs = f'-j{os.cpu_count() or 4}'
    if WIN:
        env = dict(os.environ, MSYSTEM='UCRT64', CHERE_INVOKING='1')
        r = subprocess.run([str(MSYS_BASH), '-lc', f'make {jobs}'], cwd=src, env=env, capture_output=True, text=True)
    else:
        r = subprocess.run(['make', jobs, *([f'CC={args.cc}'] if args.cc else [])], cwd=src, capture_output=True,
                           text=True)   # CC= on the command line: the bundled mbedtls's own make reads it too
    (src / 'build.log').write_text(r.stdout + r.stderr, encoding='utf-8')
    if r.returncode:
        sys.exit(f'make failed ({r.returncode}); log: {src / "build.log"}')

    out.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src / EXE, out / EXE)
    shutil.copy2(src / 'LICENSE', out / 'LICENSE')
    # hactool reads ~/.switch/prod.keys before it prints the banner and exits on a malformed key there, so the check
    # runs with an empty home: it tests the binary, not the builder's keys.
    with tempfile.TemporaryDirectory() as home:
        env = dict(os.environ, HOME=home, USERPROFILE=home)
        banner = subprocess.run([str(out / EXE)], capture_output=True, text=True, env=env)
    if 'hactool (c) SciresM' not in banner.stdout + banner.stderr:
        sys.exit(f'{out / EXE} does not start')
    print(f'{out / EXE}: {COMMIT[:7]} + {", ".join(p.name for p in PATCHES)}')


if __name__ == '__main__':
    main()
