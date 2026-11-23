"""The two shape scans of runtime/blueprint_import.c, link-tested against the real code.

find_index_by_shape and find_fav_by_shape find the Autobuild history arrays by their shape. A shape is not an
identity, so an ambiguous match has to be refused rather than resolved to the nearest candidate: the import rotates
whichever array the scan hands it, and rotating an array nothing reads is a silently failed import. The same goes for
the favourite mask, where reading the wrong one would let an import evict a card the player starred.

The scans are static, so blueprint_import.c exposes them under BP_IMPORT_TEST; the stubs in
tests/blueprint_import_scan_stubs.c make guest memory a buffer this test owns, which is what lets the scans walk it.

Run: .venv\\Scripts\\python tests\\blueprint_import_scan_test.py
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNTIME = ROOT / 'runtime'
CLANG = Path(r'C:\\msys64\\ucrt64\\bin\\clang.exe')
MAIN = ROOT / 'tests' / 'blueprint_import_scan_main.c'
STUBS = ROOT / 'tests' / 'blueprint_import_scan_stubs.c'
OUT = ROOT / 'build' / 'blueprint_import_scan_test.exe'

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
        print('blueprint_import_scan_test: clang not found at %s' % CLANG)
        return 1
    OUT.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(CLANG), '-std=c11', '-O1', '-DBP_IMPORT_TEST=1', *INCLUDES, '-o', str(OUT),
           str(MAIN), str(STUBS), *[str(s) for s in SOURCES], '-lz']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print('blueprint_import_scan_test: compile failed')
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
