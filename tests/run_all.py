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
import json
import os
import shutil
import subprocess
import sys
import time
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WIN = os.name == 'nt'
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')     # clang/gcc for flags_check and excl_mt


# The needs that only --all meets: game files, a build, the embeddable Python, a desktop. A compiler (cc) is not one.
GAME_NEEDS = {'exefs', 'exe', 'romfs', 'embed', 'gui'}


def interp(venv):
    """.venv / .venv-py314, falling back to this interpreter when the venv is absent."""
    d = ROOT / venv
    p = d / ('Scripts/python.exe' if WIN else 'bin/python')
    return p if p.exists() else Path(sys.executable)


# name: (venv, needs, args, why the need is there)
#   needs: None | needs joined by '+' (exefs | exe | romfs | embed | gui | cc)
#   name: the script below tests/, without .py (diff/simd is tests/diff/simd.py)
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
    'exefs_patches_test':       ('.venv', None, [], ''),
    'settings_doc_check':       ('.venv', None, [], ''),
    'mouse_direct_check':       ('.venv', 'cc', ['--cc', '<cc>'], 'compiles its checks with clang'),
    'cheats_vm_check':          ('.venv', 'cc', ['--cc', '<cc>'], 'compiles runtime/cheats.c into a harness with clang'),
    'diff/atomic':              ('.venv', None, [], ''),
    'diff/simd':                ('.venv', None, [], ''),
    # ---- read the game or a build ----
    'diff/lift':                ('.venv', 'exefs', ['--exefs', '<exefs>'],
                                 'samples instructions from the game modules; needs an unpacked exefs'),
    'diff/func':                ('.venv', 'exefs', ['--exefs', '<exefs>'],
                                 'lifts whole functions of the game modules; needs an unpacked exefs'),
    'jumptables_check':         ('.venv', 'exefs', ['--exefs', '<exefs>'],
                                 'replays the modules through Unicorn; needs an unpacked exefs'),
    'mods_romfs_check':         ('.venv', 'exe+romfs', ['--exe', '<exe>', '--version', 'v143', '--romfs', '<romfs>'],
                                 'runs the built totk.exe against a mod layout, on the extracted romfs'),
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
    """A built totk.exe: the plain v143 tree, else the newest variant (v143-heropath, ...)."""
    if (ROOT / 'build' / 'v143' / 'totk.exe').exists():
        return ROOT / 'build' / 'v143' / 'totk.exe'
    builds = sorted(ROOT.glob('build/v143-*/totk.exe'),
                    key=lambda p: p.stat().st_mtime, reverse=True)
    return builds[0] if builds else None


def find_romfs():
    """The extracted romfs of v143: its index.json names the real file paths."""
    cands = [ROOT / 'build' / 'extracted' / 'v143' / 'romfs', load_work_dir() / 'romfs']
    gui = ROOT / 'build' / 'build_gui.json'         # the GUI builder remembers where it built from
    if gui.exists():
        try:
            r = json.loads(gui.read_text(encoding='utf-8')).get('romfs')
            if r:
                cands.append(Path(r))
        except Exception:
            pass
    for c in cands:
        if (c / 'index.json').exists():
            return c
    return None


def find_embed():
    for c in (ROOT / 'build' / 'py314-embed',):
        if (c / ('python.exe' if WIN else 'bin/python3')).exists():
            return c
    return None


def find_cc():
    """clang for the tests that compile C: MSYS2 UCRT64's on Windows, else the one on PATH."""
    c = MSYS_BIN / 'clang.exe'
    if WIN and c.exists():
        return c
    w = shutil.which('clang')
    return Path(w) if w else None


def resolve(needs):
    """({placeholder: what the arg takes}, or the reasons the needs cannot be met here)."""
    if needs is None:
        return {}, None
    vals, missing = {}, []
    for need in needs.split('+'):
        if need == 'exefs':
            p = find_exefs()
            if p is None: missing.append('no unpacked exefs (run tools/extract.py)')
            else: vals['<exefs>'] = str(p)
        elif need == 'exe':
            p = find_exe()
            if p is None: missing.append('no built build/v143/totk.exe')
            else: vals['<exe>'] = str(p)
        elif need == 'romfs':
            p = find_romfs()
            if p is None: missing.append('no extracted romfs index (run tools/extract.py)')
            else: vals['<romfs>'] = str(p)
        elif need == 'embed':
            p = find_embed()
            if p is None: missing.append('no build/py314-embed (tools/make_release.py makes it)')
            else: vals['<embed>'] = str(p)
        elif need == 'cc':
            p = find_cc()
            if p is None: missing.append('no clang (MSYS2 UCRT64 on Windows)')
            else: vals['<cc>'] = str(p)
        elif need == 'gui':
            missing.append('drives a window; run it by hand on a desktop session')
        else:
            return {}, f'unknown need {need!r}'
    return vals, ('; '.join(missing) if missing else None)


def run_one(name, all_ok, timeout, verbose):
    venv, needs, args, why = TESTS[name]
    if not (ROOT / 'tests' / f'{name}.py').exists():
        return 'SKIP', 0.0, 'not on this branch', ''
    if needs is not None and not all_ok and set(needs.split('+')) & GAME_NEEDS:
        return 'SKIP', 0.0, f'needs {needs}: {why} (--all)', ''
    value, reason = resolve(needs)
    if reason:
        return 'SKIP', 0.0, reason, ''
    argv = [str(interp(venv)), str(ROOT / 'tests' / f'{name}.py')]
    for a in args:                              # fill each placeholder with its need's value
        s = a
        for k, v in value.items():
            s = s.replace(k, v)
        argv.append(s)
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
            print(f'{n:26} {venv:12} {needs or "-":9} {why}')
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
