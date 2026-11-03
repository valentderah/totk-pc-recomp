"""Build the game launcher (launcher/, docs/launcher.md) into build/launcher/launcher.exe.

    .venv\\Scripts\\python tools\\build_launcher.py              # build/launcher/launcher.exe
    .venv\\Scripts\\python tools\\build_launcher.py --out DIR    # DIR/launcher.exe

The launcher holds no game data, so one build serves every game and version. SDL3 is linked dynamically, as for
totk.exe: on Windows SDL3.dll and the libiconv-2.dll it imports (LGPL, unmodified) are copied next to launcher.exe, and
the C++ library is linked in. Off Windows it links the system's libSDL3. Objects of Dear ImGui are kept between builds;
the launcher's own files are compiled every time (they are small).
"""
import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WIN = os.name == 'nt'
UCRT = Path('C:/msys64/ucrt64/bin')
CXX = UCRT / 'clang++.exe' if WIN else Path(shutil.which('clang++') or 'clang++')
WINDRES = UCRT / 'windres.exe'
IMGUI = ROOT / 'runtime' / 'third_party' / 'imgui'
OWN = [ROOT / 'launcher' / 'main.cpp', ROOT / 'launcher' / 'games.cpp']
CACHED = [IMGUI / n for n in ('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp')] + \
         [ROOT / 'launcher' / 'third_party' / n for n in ('imgui_impl_sdl3.cpp', 'imgui_impl_sdlrenderer3.cpp')]
# SDL3 from its DLL (a static libSDL3.a of MSYS2 would pull LGPL libiconv into the exe); the C++ runtime linked in
WIN_LIBS = ['-mwindows', '-static-libstdc++', '-static-libgcc', '-Wl,-Bstatic', '-lwinpthread', '-Wl,-Bdynamic',
            '-lSDL3']
WIN_DLLS = [UCRT / 'SDL3.dll', UCRT / 'libiconv-2.dll']
UNIX_LIBS = ['-lSDL3']


def port_version():
    path = ROOT / 'VERSION'
    return path.read_text(encoding='utf-8').strip() if path.is_file() else 'unknown'


def compile_one(src, obj, flags):
    obj.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(CXX), *flags, '-c', str(src), '-o', str(obj)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return src, r.returncode, r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'launcher', help='output directory')
    args = ap.parse_args()
    if not CXX.is_file() and not shutil.which(str(CXX)):
        sys.exit(f'{CXX} not found: install MSYS2 UCRT64 clang (docs/building.md)')
    out = args.out.resolve()
    obj_dir = out / 'obj'
    flags = ['-std=c++20', '-O2', '-Wall', f'-I{IMGUI}', f'-I{ROOT / "launcher" / "third_party"}',
             f'-DLAUNCHER_VERSION="{port_version()}"']
    if WIN:
        flags += ['-D_WIN32_WINNT=0x0A00']

    jobs = [(src, obj_dir / (src.stem + '.o')) for src in OWN]
    jobs += [(src, obj_dir / (src.stem + '.o')) for src in CACHED
             if not (obj_dir / (src.stem + '.o')).is_file()
             or (obj_dir / (src.stem + '.o')).stat().st_mtime < src.stat().st_mtime]
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        for src, code, log in pool.map(lambda j: compile_one(*j, flags), jobs):
            if log.strip():
                print(log.strip())
            if code:
                sys.exit(f'build_launcher: compiling {src.relative_to(ROOT)} failed')
    objs = [obj_dir / (src.stem + '.o') for src in OWN + CACHED]
    if WIN:
        res = obj_dir / 'launcher_rc.o'
        subprocess.run([str(WINDRES), '-I', str(ROOT / 'runtime'), '-O', 'coff',
                        str(ROOT / 'launcher' / 'launcher.rc'), '-o', str(res)], check=True)
        objs.append(res)
    exe = out / ('launcher.exe' if WIN else 'launcher')
    r = subprocess.run([str(CXX), '-fuse-ld=lld', *map(str, objs), '-o', str(exe), *(WIN_LIBS if WIN else UNIX_LIBS)])
    if r.returncode:
        sys.exit(f'build_launcher: link failed ({exe} is the old one if a running launcher locks it)')
    for dll in WIN_DLLS if WIN else []:
        shutil.copy2(dll, out / dll.name)
    print(f'build_launcher: {exe} ({exe.stat().st_size // 1024} KB)')


if __name__ == '__main__':
    main()
