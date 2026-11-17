"""Build and run the synthetic amiibo loader/IPC and Dear ImGui gamepad checks. No game files or keys required."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
MSYS = Path(r'C:\msys64\ucrt64\bin')


def compiler(name):
    candidate = MSYS / (name + '.exe')
    return str(candidate) if os.name == 'nt' and candidate.is_file() else shutil.which(name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=compiler('clang'), help='C compiler (default MSYS2 UCRT64 clang, else PATH)')
    parser.add_argument('--cxx', default=compiler('clang++'), help='C++ compiler (default MSYS2 UCRT64 clang++, else PATH)')
    args = parser.parse_args()
    if not args.cc or not args.cxx:
        parser.error('Clang and clang++ are required; use --cc and --cxx to specify their paths')
    work = ROOT / 'build' / 'test' / 'amiibo'
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    if os.name == 'nt':
        env['PATH'] = str(Path(args.cc).resolve().parent) + os.pathsep + env.get('PATH', '')
    common = ['-O1', '-Wall', '-fno-strict-aliasing', '-march=x86-64-v3', '-I', str(ROOT / 'runtime')]
    imgui = ROOT / 'runtime' / 'third_party' / 'imgui'
    for source, cc, flags, extra in [
        ('amiibo_check.c', args.cc, ['-std=gnu17'], []),
        ('amiibo_picker_check.cpp', args.cxx, ['-std=c++17', *(['-static'] if os.name == 'nt' else [])],
         [str(imgui / name) for name in ['imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp']]),
    ]:
        exe = work / (Path(source).stem + ('.exe' if os.name == 'nt' else ''))
        subprocess.run([cc, *common, *flags, '-I', str(imgui), str(ROOT / 'tests' / source), *extra, '-o', str(exe)],
                       check=True, cwd=ROOT, env=env)
        subprocess.run([str(exe)], check=True, cwd=ROOT, env=env)


if __name__ == '__main__':
    main()
