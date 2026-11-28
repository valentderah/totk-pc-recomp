"""Run the real SDL text-window tests without game files, in a graphical desktop session.

python tests/swkbd_sdl_check.py --cc clang --deps /path/to/deps
On Windows, add --sdl-dll /path/to/SDL3.dll (or put its folder on PATH).
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--deps', type=Path, required=True, help='SDL3 include/ and lib/ directory')
    parser.add_argument('--sdl-dll', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='totk-swkbd-sdl-') as temp:
        folder = Path(temp)
        exe = folder / ('check.exe' if os.name == 'nt' else 'check')
        command = [args.cc, '-O2', '-march=x86-64-v3', '-ffunction-sections', '-fdata-sections',
                   '-Wl,--gc-sections', '-I', str(ROOT / 'runtime'), '-I', str(args.deps / 'include'),
                   str(ROOT / 'tests/swkbd_sdl_check.c'), '-L', str(args.deps / 'lib'), '-lSDL3', '-o', str(exe)]
        if os.name == 'nt':
            command += ['-Wl,--exclude-all-symbols']
        subprocess.run(command, check=True)
        if args.sdl_dll:
            shutil.copy2(args.sdl_dll, folder / 'SDL3.dll')
        subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == '__main__':
    main()