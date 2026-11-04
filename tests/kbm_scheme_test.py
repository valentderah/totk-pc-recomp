"""Build and run tests/kbm_scheme_test.c: the PC scheme's routing (runtime/kbm_scheme.c) against the cases the port
must hold.

Usage: kbm_scheme_test.py
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests' / 'diff'))
from common import MSYS_BIN, compiler  # noqa: E402


def main():
    # The toolchain's ld cannot write an output under a path with non-ASCII characters (this checkout's folder
    # name), so the test binary goes to the system temporary folder.
    work = Path(tempfile.gettempdir()) / 'totk-kbm-scheme-test'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / ('kbm_scheme_test.exe' if os.name == 'nt' else 'kbm_scheme_test')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    for opt in ('-O0', '-O1'):
        subprocess.check_call([compiler(), opt, '-fno-strict-aliasing', '-I', str(ROOT / 'runtime'),
                               str(ROOT / 'tests' / 'kbm_scheme_test.c'), str(ROOT / 'runtime' / 'kbm_scheme.c'),
                               '-o', str(exe)], env=env)
        print(opt, end=': ', flush=True)
        if subprocess.run([str(exe)], env=env).returncode:
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
