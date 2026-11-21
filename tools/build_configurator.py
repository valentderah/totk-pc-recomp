"""Build totk-config, the settings window outside the game (runtime/configurator, docs/configurator.md).

    python tools/build_configurator.py [--out build/configurator] [--toolchain DIR] [--deps DIR] [--sysroot DIR]

It compiles runtime/configurator/*.c, the settings table tools/gen_configurator.py writes, and the runtime files the
window shares with the game (cfg.c, ui_text.c, input_map.c, platform/), and links totk-config.exe (totk-config on
Linux) against SDL3. Nuklear, the widget library, is fetched once into toolchain/nuklear (pinned, hash-checked). The clang is the game's: MSYS2 UCRT64 on Windows and the one on PATH elsewhere, or --toolchain's
(an llvm-mingw or LLVM directory) with --deps' SDL3 headers and libraries, as tools/build_game.py takes them.
tools/build_game.py calls build() after it links totk.exe, so the window lands next to the game; the release builds it
into bin/ (tools/make_release.py, tools/make_release_linux.py) and the setup copies it into the install folder.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import gen_configurator   # noqa: E402

WIN = os.name == 'nt'
UCRT = Path(r'C:\msys64\ucrt64\bin')
EXE = 'totk-config.exe' if WIN else 'totk-config'
SHARED = ['cfg.c', 'ui_text.c', 'input_map.c', 'platform/common.c', 'platform/win32.c' if WIN else 'platform/linux.c']
WIN_LIBS = ['-lSDL3', '-lbcrypt', '-lwinmm', '-lshell32', '-lole32', '-luuid', '-luser32', '-lgdi32', '-ldwmapi',
            '-limm32', '-lpsapi']
POSIX_LIBS = ['-lSDL3', '-lm', '-lpthread', '-ldl']
# Nuklear 4.13.4 (MIT or public domain, THIRD_PARTY_NOTICES.md): its one header is over the repository's file size limit
# (tools/check_content.py), so it is fetched once into toolchain/nuklear and checked against these hashes, as
# tools/build_game.py fetches vk_mem_alloc.h. The SDL3 renderer is Nuklear's own demo back end.
NUKLEAR_URL = 'https://raw.githubusercontent.com/Immediate-Mode-UI/Nuklear/v4.13.4/'
NUKLEAR_FILES = {
    'nuklear.h': ('nuklear.h', '96f1383bebb3c0a7eb167a79d3b614c1550dc280f6ba810d3a99f6f9f87409c1'),
    'nuklear_sdl3_renderer.h': ('demo/sdl3_renderer/nuklear_sdl3_renderer.h',
                                '52b9f925ce61640c59a28792747c209300ea46364a8a9c53110c1d6a4959c5dd'),
    'LICENSE': ('LICENSE', 'b7c67c24445f3d5c16b6cd7aea2924d23209a416742a619d61bd38b05cbf64f9'),
}
NUKLEAR_DIR = (ROOT / 'build').resolve().parent / 'toolchain' / 'nuklear'   # the main checkout's, also from a worktree


class BuildError(Exception):
    pass


def compiler(toolchain):
    if toolchain:
        return Path(toolchain) / 'bin' / ('clang.exe' if WIN else 'clang')
    if WIN:
        return UCRT / 'clang.exe'
    found = shutil.which('clang')
    return Path(found) if found else Path('/usr/bin/clang')


def nuklear_dir():
    """toolchain/nuklear with the pinned Nuklear files, fetched on first use."""
    NUKLEAR_DIR.mkdir(parents=True, exist_ok=True)
    for name, (path, sha) in NUKLEAR_FILES.items():
        dst = NUKLEAR_DIR / name
        if dst.is_file() and hashlib.sha256(dst.read_bytes()).hexdigest() == sha:
            continue
        print(f'build_configurator: fetching Nuklear {path} into {NUKLEAR_DIR}')
        try:
            data = urllib.request.urlopen(NUKLEAR_URL + path, timeout=60).read()
        except OSError as e:
            raise BuildError(f'fetching {NUKLEAR_URL + path} failed ({e}); put the file in {dst}')
        if hashlib.sha256(data).hexdigest() != sha:
            raise BuildError(f'{NUKLEAR_URL + path} does not match the pinned hash')
        dst.write_bytes(data)
    return NUKLEAR_DIR


def newest(paths):
    return max((p.stat().st_mtime_ns for p in paths if p.is_file()), default=0)


def build(out, runtime=None, toolchain=None, deps=None, sysroot=None, cpu='v2', jobs=None):
    """Builds out/totk-config(.exe) and returns its path; BuildError when a compile or the link fails."""
    runtime = Path(runtime or ROOT / 'runtime').resolve()
    out = Path(out).resolve()
    obj = out / 'obj' / 'configurator'
    obj.mkdir(parents=True, exist_ok=True)
    rows = obj / 'cfgr_rows.c'
    table, errors = gen_configurator.build()
    if errors:
        raise BuildError('; '.join(errors))
    text = gen_configurator.emit(table)
    if not rows.is_file() or rows.read_text(encoding='utf-8') != text:
        rows.write_text(text, encoding='utf-8', newline='\n')
    cc = compiler(toolchain)
    if not cc.is_file():
        raise BuildError(f'{cc} not found')
    version = (ROOT / 'VERSION').read_text(encoding='utf-8').strip() if (ROOT / 'VERSION').is_file() else ''
    nk = nuklear_dir()
    flags = ['-std=gnu11', '-O2', '-Wall', '-fno-strict-aliasing', f'-march=x86-64-{cpu}', f'-DCR_VERSION="{version}"',
             '-I', str(runtime), '-I', str(nk)]
    link = ['-fuse-ld=lld']
    if WIN:
        flags.append('-D_WIN32_WINNT=0x0A00')
        link += ['-mwindows']
        libs = WIN_LIBS
    else:
        link += ["-Wl,-rpath,$ORIGIN"]   # a libSDL3.so.0 next to the exe first, as the game has it
        libs = POSIX_LIBS
    if deps:
        flags += ['-I', str(Path(deps) / 'include')]
        link += ['-L', str(Path(deps) / 'lib')]
    if sysroot and not WIN:
        target = ['--target=x86_64-linux-gnu', f'--sysroot={Path(sysroot).resolve()}']
        flags[:0] = target
        link[:0] = target
    sources = sorted((runtime / 'configurator').glob('*.c')) + [runtime / s for s in SHARED] + [rows]
    headers = newest(list(runtime.glob('*.h')) + list((runtime / 'configurator').glob('*.h')) +
                     list((runtime / 'platform').glob('*.h')) + list(nk.glob('*.h')) +
                     [runtime / 'ui' / 'fonts' / 'Roboto-Medium.ttf', ROOT / 'VERSION'])
    env = dict(os.environ)
    env['PATH'] = str(cc.parent) + os.pathsep + env.get('PATH', '')

    def compile_one(src):
        o = obj / (src.stem + '.o')
        if o.is_file() and o.stat().st_mtime_ns >= max(src.stat().st_mtime_ns, headers):
            return o, None
        r = subprocess.run([str(cc), *flags, '-c', str(src), '-o', str(o)], capture_output=True, text=True, env=env)
        return o, (f'{src.name}:\n{r.stdout}{r.stderr}' if r.returncode else None)

    with ThreadPoolExecutor(jobs or os.cpu_count() or 4) as pool:
        done = list(pool.map(compile_one, sources))
    failed = [e for _, e in done if e]
    if failed:
        raise BuildError('\n'.join(failed))
    exe = out / EXE
    r = subprocess.run([str(cc), *link, *[str(o) for o, _ in done], *libs, '-o', str(exe)], capture_output=True,
                       text=True, env=env)
    if r.returncode:
        raise BuildError(f'link:\n{r.stdout}{r.stderr}')
    return exe


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'configurator')
    ap.add_argument('--runtime', type=Path, help='runtime source directory (default runtime)')
    ap.add_argument('--toolchain', type=Path, help='llvm-mingw (Windows) or LLVM (Linux) directory with bin/clang')
    ap.add_argument('--deps', type=Path, help='include/ and lib/ with SDL3 (the release\'s runtime/deps)')
    ap.add_argument('--sysroot', type=Path, help='Linux: the glibc headers and libraries to build against')
    ap.add_argument('--cpu', choices=('v2', 'v3'), default='v2',
                    help='target CPU level (default v2: the window runs on every x86-64 the game supports)')
    args = ap.parse_args()
    try:
        exe = build(args.out, args.runtime, args.toolchain, args.deps, args.sysroot, args.cpu)
    except BuildError as e:
        sys.exit(f'build_configurator: {e}')
    print(f'{exe}: {exe.stat().st_size // 1024} KiB')


if __name__ == '__main__':
    main()
