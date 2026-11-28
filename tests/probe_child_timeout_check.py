"""Exercise production probe-child deadlines with real self-spawned synthetic children.

python tests/probe_child_timeout_check.py --cc clang [--deps /path/to/deps]
No game, graphical window, or external service is started.
"""
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
    with tempfile.TemporaryDirectory(prefix='totk-probe-child-') as temp:
        folder = Path(temp)
        executable = folder / ('check.exe' if os.name == 'nt' else 'check')
        command = [args.cc, '-O2', '-flto', '-march=x86-64-v3', '-ffunction-sections', '-fdata-sections',
                   '-Wl,--gc-sections', '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime/platform')]
        if args.deps:
            command += ['-I', str(args.deps / 'include'), '-L', str(args.deps / 'lib')]
        if os.name == 'nt':
            command += ['-D_WIN32_WINNT=0x0A00', '-Wl,--exclude-all-symbols']
        command += [str(Path(__file__).with_suffix('.c')), '-o', str(executable)]
        if os.name == 'nt':
            command += ['-lbcrypt', '-lpsapi', '-luser32', '-lwinmm', '-lole32', '-lshell32', '-luuid']
        else:
            command += ['-lpthread', '-ldl', '-lm']
        subprocess.run(command, check=True, timeout=120)
        environment = os.environ.copy()
        environment.pop('FIXTURE_CHILD_MODE', None)
        subprocess.run([str(executable)], env=environment, check=True, timeout=15)


if __name__ == '__main__':
    main()