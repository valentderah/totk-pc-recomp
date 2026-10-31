#!/usr/bin/env python3
"""Compile every runtime translation unit on its own, with the project's flags, without game files.

The generated C needs the game; the runtime does not. That makes this the one end-to-end check a machine without a
dump can run, and it catches what a real build only catches at the link: an include that does not exist on this OS, a
platform function used but not implemented here (with --undefined), a header that drifted.

    python tools/ci/compile_gate.py              # this OS's file set, the project's flags
    python tools/ci/compile_gate.py --undefined  # also fail when a plat_/rt_/gfx_/vk_ symbol is never defined

The file set and the per-OS exclusion are build_game's own, so the gate cannot drift from the build.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / 'runtime'
sys.path.insert(0, str(ROOT / 'tools'))
import build_game  # noqa: E402  RT_FLAGS, INSTALL_SIDE and the per-OS file rule live there

WIN = os.name == 'nt'
OTHER_OS = getattr(build_game, 'other_os', None)      # a file name -> True when this OS does not build it
NEEDS_GAME = set(build_game.INSTALL_SIDE)             # include romfs_config.h / patches.h: generated from the dump


def msys_prefix():
    """The MSYS2 prefix this Python lives in: C:/msys64/ucrt64 locally, D:/a/_temp/msys64/ucrt64 on a CI runner."""
    for cand in (Path(sys.prefix), Path(sys.base_prefix), Path('C:/msys64/ucrt64')):
        if 'ucrt64' in str(cand).replace('\\', '/').lower() and (cand / 'include').is_dir():
            return cand
    return Path('C:/msys64/ucrt64')


def compiler():
    if WIN:
        exe = msys_prefix() / 'bin' / 'clang.exe'
        if exe.is_file():
            return str(exe)
        return shutil.which('clang.exe')
    return shutil.which('clang')


def argv_path(flag, suffix=''):
    """A path from the command line, because vma_include() is asked before argparse has run."""
    argv = sys.argv
    if flag in argv:
        i = argv.index(flag)
        if i + 1 < len(argv):
            return Path(argv[i + 1]) / suffix if suffix else Path(argv[i + 1])
    return None


def args_sysroot_include():
    """The sysroot's own /usr/include, when one was asked for: it carries the SDL3 headers."""
    return argv_path('--sysroot', 'usr/include') or Path('/nonexistent')


def vma_include():
    deps = argv_path('--deps', 'include')
    prefix = msys_prefix() / 'include' if WIN else Path('/nonexistent')
    for d in ((deps / 'vma') if deps else Path('/nonexistent'),
              (prefix / 'vma') if WIN else Path('/nonexistent'), prefix,
              ROOT / 'toolchain' / 'vma' / 'include', ROOT / 'toolchain' / 'vma',
              Path('/usr/local/include/vma'), Path('/usr/include/vma'), Path('/usr/include'),
              args_sysroot_include()):
        if (d / 'vk_mem_alloc.h').is_file():
            return d
    return None


def sources():
    pats = ('*.c', '*.cpp', 'platform/*.c', 'ui/*.cpp', 'third_party/imgui/*.cpp',
            'third_party/imgui/backends/*.cpp')
    seen = []
    for pat in pats:
        for p in sorted(RUNTIME.glob(pat)):
            if p.is_file() and p not in seen:
                seen.append(p)
    return [p for p in seen if not (OTHER_OS and OTHER_OS(p.name))]


def run(cmd):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--undefined', action='store_true', help='also fail on undefined platform symbols')
    ap.add_argument('--gen', help='a generated/ folder, if one exists: its layout.h is used instead of a stub')
    ap.add_argument('--cc', help='the compiler to use (the release pins LLVM; a distro clang compiles the gate too, '
                                'until a file uses something only a newer clang has)')
    ap.add_argument('--deps', type=Path, help='runtime/deps of a release toolchain: its include/ carries zstd.h')
    ap.add_argument('--sysroot', type=Path,
                    help='Linux: compile against this sysroot instead of the host headers, as the release does '
                         '(tools/make_release_linux.py --fetch-sysroot prints the path)')
    ap.add_argument('--keep', action='store_true', help='keep the object folder')
    args = ap.parse_args()

    cc = args.cc or compiler()
    if not cc:
        sys.exit('compile gate: no clang found')
    flags = list(build_game.RT_FLAGS)
    # No -D_GNU_SOURCE: glibc's __USE_GNU turns bsearch into a five-argument macro and breaks nvn_procs.c. The
    # project's own flags are what a real build uses, and this gate exists to reproduce that build.
    if args.sysroot:
        root = args.sysroot.resolve()
        flags[:0] = ['--target=x86_64-linux-gnu', f'--sysroot={root}']
    inc = [f'-I{RUNTIME}', f'-I{RUNTIME / "third_party" / "imgui"}']
    if args.deps:
        inc.insert(0, f'-I{Path(args.deps).resolve() / "include"}')
    vma = vma_include()
    if vma:
        inc.append(f'-I{vma}')
    else:
        print('compile gate: no vk_mem_alloc.h found; every VMA user will fail')

    # A gate without the Vulkan headers cannot check the back end, and the failure looks like twenty broken files.
    cpath = [Path(d) for d in os.environ.get('CPATH', '').split(os.pathsep) if d]
    vk = any((Path(i[2:]) / 'vulkan' / 'vulkan.h').is_file() for i in inc if i.startswith('-I')) or \
        any((d / 'vulkan' / 'vulkan.h').is_file() for d in cpath) or \
        Path('/usr/include/vulkan/vulkan.h').is_file() or \
        (msys_prefix() / 'include' / 'vulkan' / 'vulkan.h').is_file()
    if not vk:
        sys.exit('compile gate: Vulkan headers not found: install libvulkan-dev (Ubuntu), vulkan-headers (Arch), '
                 'or point CPATH at a Vulkan-Headers checkout')

    objdir = Path(tempfile.mkdtemp(prefix='totk-gate-'))

    # runtime/memory.c, frame_rate.c, loader.c and nvn_procs.c include layout.h, which recomp/layout.py writes from the
    # dump. The runtime reads only MOD_COUNT and MOD_MAIN_BASE from it, so a stub keeps those four in the gate; a real
    # build always passes --gen and never sees this file.
    if args.gen:
        inc.insert(0, f'-I{Path(args.gen).resolve()}')
    else:
        stubgen = objdir / 'gen'
        stubgen.mkdir()
        (stubgen / 'layout.h').write_text(
            '// Stand-in for the layout.h recomp/layout.py writes from a dump. The compile gate has no game; the runtime'
            ' only reads MOD_COUNT and MOD_MAIN_BASE.\n'
            '#pragma once\n'
            '#define MOD_MAIN_BASE 0x7000000000ull\n'
            '#define MOD_MAIN_SIZE 0x200000ull\n'
            '#define MOD_COUNT 1\n'
            '#define MOD_LIST(X) X(main, MAIN)\n')
        inc.insert(0, f'-I{stubgen}')
        print('compile gate: no --gen, so layout.h is a stub (MOD_COUNT, MOD_MAIN_BASE, MOD_LIST)')
    files = sources()
    failed = []
    excluded = sorted(p.name for p in RUNTIME.glob('*') if OTHER_OS and OTHER_OS(p.name))
    for src in files:
        if src.name in NEEDS_GAME:
            print('skip %s: needs headers generated from the game' % src.relative_to(ROOT))
            continue
        obj = objdir / (str(src.relative_to(ROOT)).replace('/', '_') + '.o')
        r = run([cc, *flags, *inc, '-c', str(src), '-o', str(obj)])
        if r.returncode:
            failed.append((src, r.stderr))
            print('FAIL %s' % src.relative_to(ROOT))
        else:
            print('ok   %s' % src.relative_to(ROOT))
    print('\ncompile gate: %d files, %d failed, %d skipped (game headers)%s'
          % (len(files), len(failed), len(NEEDS_GAME),
             ', excluded on this OS: ' + ', '.join(excluded) if excluded else ''))

    undefined = []
    if args.undefined and not failed:
        nm = shutil.which('llvm-nm') or shutil.which('nm')
        if not nm:
            print('compile gate: no llvm-nm/nm, the undefined check is skipped')
        else:
            objs = [str(p) for p in objdir.glob('*.o')]
            undef = {line.split()[-1] for line in run([nm, '-u', *objs]).stdout.splitlines() if line.strip()}
            defined = {line.split()[-1] for line in run([nm, '--defined-only', *objs]).stdout.splitlines()
                       if len(line.split()) == 3}
            # plat_/os_ only: rt_* and the linked tables come from the generated modules.c, which needs the game.
            undefined = sorted(n for n in undef - defined if n.startswith(('plat_', 'os_')))
            print('undefined check: ' + (', '.join(undefined[:12]) if undefined
                                         else 'every plat_/os_ symbol used is defined'))

    if not args.keep:
        shutil.rmtree(objdir, ignore_errors=True)
    for src, err in failed:
        print('\n--- %s' % src.relative_to(ROOT))
        print('\n'.join(err.splitlines()[:8]))
    return 1 if (failed or undefined) else 0


if __name__ == '__main__':
    sys.exit(main())
