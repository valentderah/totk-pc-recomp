"""Check playtime preservation with real platform I/O and a Windows sharing lock."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--deps', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='totk-playtime-') as temp:
        folder = Path(temp)
        exe = folder / ('check.exe' if os.name == 'nt' else 'check')
        command = [args.cc, '-O2', '-flto', '-march=x86-64-v3', '-ffunction-sections', '-fdata-sections',
                   '-Wl,--gc-sections', '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime/platform')]
        if args.deps:
            command += ['-I', str(args.deps / 'include'), '-L', str(args.deps / 'lib')]
        if os.name == 'nt':
            command += ['-D_WIN32_WINNT=0x0A00', '-Wl,--exclude-all-symbols']
        command += [str(ROOT / 'tests/playtime_read_check.c'), '-o', str(exe)]
        if os.name == 'nt':
            command += ['-lbcrypt', '-lpsapi', '-luser32', '-lwinmm', '-lole32', '-lshell32', '-luuid']
        else:
            command += ['-lpthread', '-ldl', '-lm']
        subprocess.run(command, check=True)
        subprocess.run([str(exe), str(folder / 'data')], check=True, timeout=30)


if __name__ == '__main__':
    main()
