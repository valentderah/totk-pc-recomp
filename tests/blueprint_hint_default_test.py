"""The blueprint import row's on/off default.

Two things have to hold for the review's request ("off by default, so a player who never imports pays
nothing") to be true, and this file checks both:

  1. The reader resolves an unset key to the table default. cfg_get returns NULL for a key that is
     neither in the environment nor in totk.ini, while cfg_value falls back to cfg.c's table; a check
     written as "not the string 0" therefore reads NULL as ON. That is how the row shipped enabled for
     everyone while the table said 0. blueprint_hint.cpp is linked and driven here to prove it reads
     through cfg_bool.
  2. The table actually says 0.

Run: .venv\\Scripts\\python tests\\blueprint_hint_default_test.py
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNTIME = ROOT / 'runtime'
CLANG = Path(r'C:\\msys64\\ucrt64\\bin\\clang.exe')
MAIN = ROOT / 'tests' / 'blueprint_hint_default_main.c'
STUBS = ROOT / 'tests' / 'blueprint_hint_default_stubs.c'
OUT = ROOT / 'build' / 'blueprint_hint_default_test.exe'

SOURCES = [RUNTIME / 'cfg.c']
INCLUDES = ['-I', str(RUNTIME), '-I', str(ROOT / 'generated' / 'v143')]


def source_checks():
    """The two facts a linked test cannot see: which accessor the row uses, and what the table says."""
    fails = 0
    hint = (RUNTIME / 'blueprint_hint.cpp').read_text(encoding='utf-8', errors='replace')
    cfg = (RUNTIME / 'cfg.c').read_text(encoding='utf-8', errors='replace')

    # The row must read the setting through cfg_bool (which goes via cfg_value to the table default).
    # Reading it with cfg_get and testing for "0" treats an unset key as on, whatever the table says.
    m = re.search(r'static int enabled\(void\)\s*\{(.*?)\n\}', hint, re.S)
    body = m.group(1) if m else ''
    if 'cfg_bool' not in body:
        print('FAIL: blueprint_hint.cpp does not read the setting through cfg_bool')
        fails += 1
    if 'cfg_get' in body:
        print('FAIL: blueprint_hint.cpp still reads the setting through cfg_get')
        fails += 1

    # The table default has to be 0, or "off by default" is only true of the reader.
    if not re.search(r'\{"BLUEPRINT_HINT", "0",', cfg):
        print('FAIL: cfg.c does not default BLUEPRINT_HINT to 0')
        fails += 1

    print('blueprint hint source: %d checks, %d failures' % (3, fails))
    return fails


def linked_checks():
    if not CLANG.exists():
        print('blueprint_hint_default_test: clang not found at %s' % CLANG)
        return 1
    OUT.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(CLANG), '-std=c11', '-O1', *INCLUDES, '-o', str(OUT),
           str(MAIN), str(STUBS), *[str(s) for s in SOURCES]]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print('blueprint_hint_default_test: compile failed')
        print(r.stdout)
        print(r.stderr)
        return 1
    run = subprocess.run([str(OUT)], capture_output=True, text=True)
    sys.stdout.write(run.stdout)
    if run.stderr:
        sys.stderr.write(run.stderr)
    return run.returncode


def main():
    return source_checks() + linked_checks()


if __name__ == '__main__':
    sys.exit(1 if main() else 0)
