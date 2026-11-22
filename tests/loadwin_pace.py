"""Build tests/loadwin_pace.c and run the loading window's estimate and hearts through every pair of emulated starts.

    python tests/loadwin_pace.py [--cc path/to/clang] [--csv DIR]

The harness compiles runtime/loadwin.c and runtime/loadwin_win.c as they are, so it runs on Windows only (the panel
draws with GDI); elsewhere it says so and passes. --csv writes each pair's trace (t, share, shown, quarters, part,
left).
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
FIRST = ['none', 'warm', 'cold', 'pipes', 'slowgame', 'slow4']
SECOND = ['warm', 'cold', 'pipes', 'slowgame', 'slow4']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cc', help='clang (default: from MSYS2 UCRT64 or PATH)')
    ap.add_argument('--csv', type=Path, help='folder for the traces')
    args = ap.parse_args()
    if os.name != 'nt':
        print('loadwin_pace: Windows only (the panel draws with GDI); nothing to run here')
        return 0
    env = dict(os.environ)
    if MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    cc = args.cc or shutil.which('clang', path=env['PATH'])
    if not cc:
        print('loadwin_pace: no clang')
        return 1
    work = ROOT / 'build' / 'test' / 'loadwin'
    work.mkdir(parents=True, exist_ok=True)
    exe = work / 'loadwin_pace.exe'
    subprocess.check_call([cc, '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'runtime'),
                           str(ROOT / 'tests' / 'loadwin_pace.c'), str(ROOT / 'runtime' / 'ui_text.c'), '-o', str(exe),
                           '-lgdi32', '-luser32', '-ldwmapi'], env=env)
    if args.csv:
        args.csv.mkdir(parents=True, exist_ok=True)
    failed = 0
    for a in FIRST:
        for b in SECOND:
            cmd = [str(exe), a, b] + ([str(args.csv / f'{a}_{b}.csv')] if args.csv else [])
            r = subprocess.run(cmd, env=env, capture_output=True, text=True)
            print(r.stdout, end='')
            failed += r.returncode != 0
    print(f'loadwin_pace: {len(FIRST) * len(SECOND) - failed} of {len(FIRST) * len(SECOND)} pairs ok')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
