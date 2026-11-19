"""The CAI blueprint validator, link-tested against the real runtime/blueprint_cai.c.

Every import passes through this parser, so the shapes it must accept and the ones it must refuse are worth pinning
down: a file that is not a blueprint has to be rejected before anything reaches the save data. The blobs are built byte
by byte here, so the test states what the format means instead of replaying a recorded file.

Run: .venv\\Scripts\\python tests\\blueprint_cai_test.py
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNTIME = ROOT / 'runtime'
CLANG = Path(r'C:\msys64\ucrt64\bin\clang.exe')
MAIN = ROOT / 'tests' / 'blueprint_cai_main.c'
OUT = ROOT / 'build' / 'blueprint_cai_test.exe'
INCLUDES = ['-I', str(RUNTIME), '-I', str(ROOT / 'generated' / 'v143')]


def main():
    if not CLANG.exists():
        print('blueprint_cai_test: clang not found at %s' % CLANG)
        return 1
    OUT.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(CLANG), '-std=c11', '-O1', *INCLUDES, '-o', str(OUT), str(MAIN),
           str(RUNTIME / 'blueprint_cai.c'), '-lm']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print('blueprint_cai_test: compile failed')
        print(r.stdout)
        print(r.stderr)
        return 1
    run = subprocess.run([str(OUT)], capture_output=True, text=True)
    sys.stdout.write(run.stdout)
    if run.stderr:
        sys.stderr.write(run.stderr)
    return run.returncode


if __name__ == '__main__':
    sys.exit(main())
