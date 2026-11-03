"""Build the game launcher (launcher/, docs/launcher.md) into build/launcher/launcher.exe.

    .venv\\Scripts\\python tools\\build_launcher.py              # build/launcher/launcher.exe
    .venv\\Scripts\\python tools\\build_launcher.py --out DIR    # DIR/launcher.exe
    python tools/build_launcher.py --toolchain llvm --deps runtime/deps --out DIR   # with a release's llvm-mingw

The launcher holds no game data, so one build serves every game and version. SDL3 is linked dynamically, as for
totk.exe: on Windows SDL3.dll and the libiconv-2.dll it imports (LGPL, unmodified) are copied next to launcher.exe, and
the C++ library is linked in. Off Windows it links the system's libSDL3. Objects of Dear ImGui are kept between builds;
the launcher's own files are compiled every time (they are small).

--toolchain DIR builds with an llvm-mingw tree (DIR/bin/clang++.exe, libc++ linked in) and --deps DIR takes the SDL3
headers and import library from DIR/include and DIR/lib, as tools/make_release.py does for the release folder, which
puts the launcher in its root next to a copy of its SDL3.dll; no DLL is copied then.
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


def compile_one(src, obj, flags, cxx):
    obj.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(cxx), *flags, '-c', str(src), '-o', str(obj)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return src, r.returncode, r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'launcher', help='output directory')
    ap.add_argument('--toolchain', type=Path, help='an llvm-mingw tree to build with (default: MSYS2 UCRT64 clang)')
    ap.add_argument('--deps', type=Path, help='include/ and lib/ with the SDL3 headers and import library')
    args = ap.parse_args()
    cxx, windres, libs, dlls = CXX, WINDRES, WIN_LIBS if WIN else UNIX_LIBS, WIN_DLLS if WIN else []
    if args.toolchain:
        bin_dir = args.toolchain.resolve() / 'bin'
        cxx, windres = bin_dir / ('clang++' + ('.exe' if WIN else '')), bin_dir / 'llvm-windres.exe'
        libs, dlls = ['-mwindows', '-static-libstdc++', '-static-libgcc', '-lSDL3'] if WIN else UNIX_LIBS, []
    if not Path(cxx).is_file() and not shutil.which(str(cxx)):
        sys.exit(f'{cxx} not found: install MSYS2 UCRT64 clang (docs/building.md)')
    out = args.out.resolve()
    obj_dir = out / 'obj'
    flags = ['-std=c++20', '-O2', '-Wall', f'-I{IMGUI}', f'-I{ROOT / "launcher" / "third_party"}',
             f'-DLAUNCHER_VERSION="{port_version()}"']
    if WIN:
        flags += ['-D_WIN32_WINNT=0x0A00']
    if args.deps:
        flags += [f'-I{args.deps.resolve() / "include"}']
        libs = [f'-L{args.deps.resolve() / "lib"}', *libs]
    stamp = obj_dir / 'compiler.txt'   # objects of another compiler are compiled again
    if not stamp.is_file() or stamp.read_text(encoding='utf-8') != str(cxx):
        shutil.rmtree(obj_dir, ignore_errors=True)
        obj_dir.mkdir(parents=True)
        stamp.write_text(str(cxx), encoding='utf-8')

    jobs = [(src, obj_dir / (src.stem + '.o')) for src in OWN]
    jobs += [(src, obj_dir / (src.stem + '.o')) for src in CACHED
             if not (obj_dir / (src.stem + '.o')).is_file()
             or (obj_dir / (src.stem + '.o')).stat().st_mtime < src.stat().st_mtime]
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        for src, code, log in pool.map(lambda j: compile_one(*j, flags, cxx), jobs):
            if log.strip():
                print(log.strip())
            if code:
                sys.exit(f'build_launcher: compiling {src.relative_to(ROOT)} failed')
    objs = [obj_dir / (src.stem + '.o') for src in OWN + CACHED]
    if WIN:
        res = obj_dir / 'launcher_rc.o'
        subprocess.run([str(windres), '-I', str(ROOT / 'runtime'), '-O', 'coff',
                        str(ROOT / 'launcher' / 'launcher.rc'), '-o', str(res)], check=True)
        objs.append(res)
    exe = out / ('launcher.exe' if WIN else 'launcher')
    r = subprocess.run([str(cxx), '-fuse-ld=lld', *map(str, objs), '-o', str(exe), *libs])
    if r.returncode:
        sys.exit(f'build_launcher: link failed ({exe} is the old one if a running launcher locks it)')
    for dll in dlls:
        shutil.copy2(dll, out / dll.name)
    print(f'build_launcher: {exe} ({exe.stat().st_size // 1024} KB)')


if __name__ == '__main__':
    main()
