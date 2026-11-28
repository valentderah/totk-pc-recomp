"""Check Linux generator-child reaping with real processes and synthetic marker files.

python3 tests/linux_spawn_reap_check.py [--cc clang] [--deps /usr]
No game, graphical window or external service is started.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='clang')
    parser.add_argument('--deps', type=Path, default=Path('/usr'), help='SDL3 include/ prefix')
    args = parser.parse_args()
    if not sys.platform.startswith('linux'):
        print('SKIP: Linux child reaping uses /proc and waitpid')
        return
    with tempfile.TemporaryDirectory(prefix='totk-spawn-reap-') as temp:
        folder = Path(temp)
        executable = folder / 'check'
        command = [args.cc, '-O2', '-flto', '-march=x86-64-v3', '-ffunction-sections', '-fdata-sections',
                   '-Wl,--gc-sections', '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime/platform'),
                   '-I', str(args.deps / 'include'), str(Path(__file__).with_suffix('.c')),
                   '-lpthread', '-ldl', '-lm', '-o', str(executable)]
        subprocess.run(command, check=True, timeout=120)
        data = folder / 'files'
        data.mkdir()
        subprocess.run([str(executable), str(data)], check=True, timeout=15)


if __name__ == '__main__':
    main()