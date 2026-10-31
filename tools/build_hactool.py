"""Build native hactool: toolchain/hactool/hactool[.exe] from pinned SciresM sources.

Fetches the pinned commit into toolchain/src/hactool, resets it (local changes and build output there are discarded),
applies portable.patch (MacNativeGames' extraction changes and our stdin/key parsing fixes) and unicode.patch,
and builds a static exe
with MSYS2 UCRT64 gcc; mbedtls comes with hactool's tree. The exe and hactool's LICENSE go to toolchain/hactool, where
tools/extract.py and tools/make_release.py take them. macOS uses Apple Clang ARM64,
the bundled mbedtls, and only portable.patch; the Unicode resource patch is Windows-only.

  python tools/build_hactool.py [--src DIR] [--out DIR]
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
URL = 'https://github.com/SciresM/hactool.git'
COMMIT = '1d64a83450e025622f3468c28fc4164dad2c5ef6'
PATCHES = [ROOT / 'tools' / 'hactool' / n for n in ('portable.patch', 'unicode.patch')]
MSYS_BASH = Path(r'C:\msys64\usr\bin\bash.exe')
CONFIG_MK = ('CC = gcc\nEXEEXT = .exe\nCFLAGS = -O2 -Wall -Wextra -pedantic -std=gnu11\n'
             'LDFLAGS = -lmbedtls -lmbedx509 -lmbedcrypto -static\n')


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
    ap.add_argument('--out', type=Path, default=ROOT / 'toolchain' / 'hactool', help='where hactool.exe goes')
    args = ap.parse_args()
    native_mac = sys.platform == 'darwin'
    if not native_mac and not MSYS_BASH.is_file():
        sys.exit(f'{MSYS_BASH} not found: install MSYS2 with the UCRT64 gcc and make (docs/building.md)')
    src, out = args.src.resolve(), args.out.resolve()

    fetch(src)
    patches = PATCHES[:1] if native_mac else PATCHES
    for p in patches:
        git(src, 'apply', '--whitespace=nowarn', str(p))
    config = ('CC = clang\nEXEEXT =\nCFLAGS = -O2 -arch arm64 -mmacosx-version-min=26.0 -std=gnu11\n'
              'LDFLAGS = -lmbedtls -lmbedx509 -lmbedcrypto\n') if native_mac else CONFIG_MK
    (src / 'config.mk').write_text(config, encoding='utf-8', newline='\n')

    env = dict(os.environ, MSYSTEM='UCRT64', CHERE_INVOKING='1')
    command = ['make', f'-j{min(os.cpu_count() or 4, 16)}'] if native_mac else [str(MSYS_BASH), '-lc', f'make -j{os.cpu_count() or 4}']
    r = subprocess.run(command, cwd=src, env=env,
                       capture_output=True, text=True)
    (src / 'build.log').write_text(r.stdout + r.stderr, encoding='utf-8')
    if r.returncode:
        sys.exit(f'make failed ({r.returncode}); log: {src / "build.log"}')

    out.mkdir(parents=True, exist_ok=True)
    name = 'hactool' if native_mac else 'hactool.exe'
    shutil.copy2(src / name, out / name)
    shutil.copy2(src / 'LICENSE', out / 'LICENSE')
    banner = subprocess.run([str(out / name)], capture_output=True, text=True)
    if 'hactool (c) SciresM' not in banner.stdout + banner.stderr:
        sys.exit(f'{out / name} does not start')
    print(f'{out / name}: {COMMIT[:7]} + {", ".join(p.name for p in patches)}')


if __name__ == '__main__':
    main()
