"""The Autobuild history rotation, link-tested against the real runtime/blueprint_import.c.

bp_rotate_ages is the rule a new creation follows: favourites keep their age, every other entry moves down one, and the
oldest free card is evicted to carry the new creation at age 0. It is pure, so it runs here without a game, a save or
guest memory - the writes around it cannot be exercised that way, but the arithmetic the player sees can.

Run: .venv\\Scripts\\python tests\\blueprint_rotation_test.py
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNTIME = ROOT / 'runtime'
CLANG = Path(r'C:\msys64\ucrt64\bin\clang.exe')
MAIN = ROOT / 'tests' / 'blueprint_rotation_main.c'
STUBS = ROOT / 'tests' / 'blueprint_rotation_stubs.c'
OUT = ROOT / 'build' / 'blueprint_rotation_test.exe'

# blueprint_import.c pulls in the blueprint reader, which pulls in the .txt decoder and its zlib.
SOURCES = [
    RUNTIME / 'blueprint_import.c',
    RUNTIME / 'blueprint_file.c',
    RUNTIME / 'blueprint_cai.c',
    RUNTIME / 'blueprint_txt.c',
    RUNTIME / 'blueprint_inflate.c',
]
INCLUDES = ['-I', str(RUNTIME), '-I', str(ROOT / 'generated' / 'v143')]


def main():
    if not CLANG.exists():
        print('blueprint_rotation_test: clang not found at %s' % CLANG)
        return 1
    OUT.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(CLANG), '-std=c11', '-O1', *INCLUDES, '-o', str(OUT), str(MAIN), str(STUBS),
           *[str(s) for s in SOURCES], '-lz']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print('blueprint_rotation_test: compile failed')
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
