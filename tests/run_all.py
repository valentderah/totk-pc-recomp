"""Run every test in tests/ through one command, and say what each one needed.

The tests were written to be run one at a time (each header says how). This keeps that layout and adds the two
things it lacked: one entry point, and a report that separates a failure from a test that could not run here.

    python tests/run_all.py                 # the ones that need no game files: fast, safe in a loop
    python tests/run_all.py --all           # also the ones that read game files or a built totk.exe
    python tests/run_all.py --only ncz_test,spin_waits_test
    python tests/run_all.py --list

Each test declares its interpreter (.venv for the lifter, .venv-py314 for the extraction tools) and what it
needs. A need that cannot be met is SKIP with the reason, never a failure: a missing game dump is not a broken
build. Exit is 0 when nothing failed, 1 when something did.

No third-party imports: this runs before any venv is trusted.
"""

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WIN = os.name == 'nt'
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')     # clang/gcc for flags_check and excl_mt


def interp(venv):
    """.venv / .venv-py314, falling back to this interpreter when the venv is absent."""
    d = ROOT / venv
    p = d / ('Scripts/python.exe' if WIN else 'bin/python')
    return p if p.exists() else Path(sys.executable)


# name: (venv, needs, args, why the need is there)
#   needs: None | 'exefs' | 'exe' | 'embed' | 'gui'
TESTS = {
    # ---- no game files, no build ----
    'flags_check':              ('.venv', None, [], ''),
    'excl_mt':                  ('.venv', None, [], ''),
    'pack_shader_cache_test':   ('.venv', None, [], ''),
    'spin_waits_test':          ('.venv', None, [], ''),
    'worktree_test':            ('.venv', None, [], ''),
    'installer_test':           ('.venv-py314', None, [], ''),
    'ncz_test':                 ('.venv-py314', None, [], ''),
    'romfs_prompts_test':       ('.venv-py314', None, [], ''),
    # ---- read the game or a build ----
    'jumptables_check':         ('.venv', 'exefs', ['--exefs', '<exefs>'],
                                 'replays the modules through Unicorn; needs an unpacked exefs'),
    'mods_romfs_check':         ('.venv', 'exe', ['--exe', '<exe>', '--version', 'v143'],
                                 'runs the built totk.exe against a mod layout'),
    'installer_e2e':            ('.venv-py314', 'embed', [],
                                 'the release launcher runs installer on the embeddable Python'),
    'installer_gui_test':       ('.venv-py314', 'gui', [],
                                 'drives the installer window; needs an interactive desktop'),
}


def load_work_dir():
    """[work] dir from config.local.toml, so a game test finds the extracted files."""
    cfg = ROOT / 'config.local.toml'
    if not cfg.exists():
        return ROOT / 'build'
    try:
        import tomllib
        with open(cfg, 'rb') as f:
            d = tomllib.load(f).get('work', {}).get('dir')
        return Path(d) if d else ROOT / 'build'
    except Exception:
        return ROOT / 'build'


def find_exefs():
    work = load_work_dir()
    for c in (work / 'exefs-v143', work / 'extracted' / 'v143' / 'exefs',
              ROOT / 'build' / 'extracted' / 'v143' / 'exefs'):
        if (c / 'main').exists():
            return c
    return None


def find_exe():
    for c in (ROOT / 'build' / 'v143' / 'totk.exe',):
        if c.exists():
            return c
    return None


def find_embed():
    for c in (ROOT / 'build' / 'py314-embed',):
        if (c / ('python.exe' if WIN else 'bin/python3')).exists():
            return c
    return None


def resolve(needs):
    """(what the args take, or None when the need cannot be met here)."""
    if needs is None:
        return None, None
    if needs == 'exefs':
        p = find_exefs()
        return (None, 'no unpacked exefs (run tools/extract.py)') if p is None else (str(p), None)
    if needs == 'exe':
        p = find_exe()
        return (None, 'no built build/v143/totk.exe') if p is None else (str(p), None)
    if needs == 'embed':
        p = find_embed()
        return (None, 'no build/py314-embed (tools/make_release.py makes it)') if p is None else (str(p), None)
    if needs == 'gui':
        return None, 'drives a window; run it by hand on a desktop session'
    return None, f'unknown need {needs!r}'


def run_one(name, all_ok, timeout, verbose):
    venv, needs, args, why = TESTS[name]
    if not (ROOT / 'tests' / f'{name}.py').exists():
        return 'SKIP', 0.0, 'not on this branch', ''
    if needs is not None and not all_ok:
        return 'SKIP', 0.0, f'needs {needs}: {why} (--all)', ''
    value, reason = resolve(needs)
    if reason:
        return 'SKIP', 0.0, reason, ''
    argv = [str(interp(venv)), str(ROOT / 'tests' / f'{name}.py')]
    if value:
        argv += [a.replace('<exefs>', value).replace('<exe>', value) for a in args]
    env = dict(os.environ)
    if MSYS_BIN.is_dir():                       # flags_check and excl_mt compile C
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    t0 = time.time()
    try:
        r = subprocess.run(argv, cwd=str(ROOT), env=env, capture_output=True,
                           text=True, encoding='utf-8', errors='replace', timeout=timeout)
    except subprocess.TimeoutExpired:
        return 'FAIL', time.time() - t0, f'timed out after {timeout}s', ''
    dt = time.time() - t0
    out = (r.stdout or '') + (r.stderr or '')
    if r.returncode == 0:
        return 'PASS', dt, '', out if verbose else ''
    tail = '\n'.join(out.strip().splitlines()[-12:])
    return 'FAIL', dt, f'exit {r.returncode}', tail


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--all', action='store_true', help='also run the tests that read game files or a build')
    ap.add_argument('--only', help='comma-separated test names')
    ap.add_argument('--list', action='store_true', help='list the tests and their needs')
    ap.add_argument('--timeout', type=int, default=1800, help='per test, seconds (default 1800)')
    ap.add_argument('-v', '--verbose', action='store_true', help='print every test\'s output')
    a = ap.parse_args()

    if a.list:
        for n in TESTS:
            venv, needs, _, why = TESTS[n]
            print(f'{n:26} {venv:12} {needs or "-":6} {why}')
        return 0

    names = list(TESTS)
    if a.only:
        want = [s.strip() for s in a.only.split(',') if s.strip()]
        missing = [w for w in want if w not in TESTS]
        if missing:
            print(f'unknown test(s): {", ".join(missing)}', file=sys.stderr)
            return 2
        names = want

    print(f'{len(names)} test(s), cwd {ROOT}')
    rows, failed = [], 0
    for n in names:
        status, dt, note, out = run_one(n, a.all, a.timeout, a.verbose)
        failed += status == 'FAIL'
        rows.append((n, status, dt, note))
        line = f'  {status:4} {n:26} {dt:6.1f}s'
        print(line + (f'  {note}' if note else ''))
        if out:
            print('\n'.join('       ' + s for s in out.splitlines()))

    print()
    counts = {k: sum(1 for r in rows if r[1] == k) for k in ('PASS', 'FAIL', 'SKIP')}
    print(f'{counts["PASS"]} passed, {counts["FAIL"]} failed, {counts["SKIP"]} skipped')
    if failed:
        print('failed: ' + ', '.join(r[0] for r in rows if r[1] == 'FAIL'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
