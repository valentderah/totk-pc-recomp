"""Package a built totk.exe into a folder that runs from anywhere: build/<version>-portable by default.

The folder holds the exe, its DLLs, the shader translator (NativeAOT: no .NET), the RomFS index and patches, the
extracted ExeFS (the game code the exe loads), the shared fonts, the prepared shader and pipeline files. There is
no config file: totk.exe finds the exefs, romfs, profile and shared-fonts folders next to itself. Without --with-romfs
the 16 GB RomFS image stays out; copy build/extracted/<version>/romfs/romfs.bin to <folder>/romfs/ or set TOTK_ROMFS
(a path, relative ones to the exe's folder).

The folder contains game code and data derived from the game: for your own machines only, never publish or share it.
Saves go to <folder>/profile.

    .venv/Scripts/python tools/package_build.py [--version v121] [--build build/v121] [--exe PATH/totk.exe]
                                                [--out build/v121-portable] [--with-romfs] [--no-shader-cache]
"""
import argparse
import os
import shutil
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = '.exe' if os.name == 'nt' else ''
# A Linux source build links the system's SDL3, Opus and libvpx: the other machine needs them installed
DLLS = ('SDL3.dll', 'libopus-0.dll', 'libiconv-2.dll') if os.name == 'nt' else ()
# Prepared state next to the exe: translator answers, programs, the prepared list, pipeline cache and its keys, and the
# variant lists `totk.exe --prepare-shaders` reads (the driver keys its library cache by exe path, so each copy runs it).
STATE = ('romfs-index.bin', 'pipeline-cache.bin', 'pipeline-keys.bin', 'shader-answers.bin', 'shader-programs.bin',
         'shader-prepared.bin', 'seed-answers.bin', 'seed-programs.bin')

def copy(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def copy_tree(src, dst, workers=16):
    jobs = []
    for dirpath, _, files in os.walk(src):
        rel = Path(dirpath).relative_to(src)
        (dst / rel).mkdir(parents=True, exist_ok=True)
        jobs += [(Path(dirpath) / f, dst / rel / f) for f in files if not f.endswith('.tmp')]
    with ThreadPoolExecutor(workers) as pool:
        list(pool.map(lambda j: shutil.copy2(*j), jobs))
    return len(jobs)


def size_of(path):
    return sum(f.stat().st_size for f in Path(path).rglob('*') if f.is_file())


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--version', default='v121', help='game version (v121 or v143): the defaults below follow it')
    ap.add_argument('--build', type=Path, help='build dir with the prepared state (default build/<version>)')
    ap.add_argument('--exe', type=Path, help='totk.exe to package (default: <build>/totk.exe); its DLLs are taken from --build')
    ap.add_argument('--xlate', type=Path, default=ROOT / 'build' / 'xlate' / f'totk_xlate{EXE}')
    ap.add_argument('--extracted', type=Path, help='default build/extracted/<version>')
    ap.add_argument('--out', type=Path, help='default build/<version>-portable')
    ap.add_argument('--with-romfs', action='store_true', help='also copy romfs.bin (about 16 GB)')
    ap.add_argument('--no-shader-cache', action='store_true', help='leave out shader-cache (about 2 GB): translates again')
    a = ap.parse_args()
    a.build = a.build or ROOT / 'build' / a.version
    a.extracted = a.extracted or ROOT / 'build' / 'extracted' / a.version
    a.out = a.out or ROOT / 'build' / f'{a.version}-portable'

    build, out = a.build.resolve(), a.out.resolve()
    exe = (a.exe or build / f'totk{EXE}').resolve()
    fonts = a.extracted.resolve().parent / 'firmware' / 'shared-fonts'
    exefs = a.extracted.resolve() / 'exefs'
    romfs = a.extracted.resolve() / 'romfs' / 'romfs.bin'
    need = [exe, a.xlate, exefs, fonts, build / 'romfs-patches'] + [build / d for d in DLLS] + [build / 'romfs-index.bin']
    if a.with_romfs:
        need.append(romfs)
    missing = [str(p) for p in need if not p.exists()]
    if missing:
        sys.exit('package_build: missing: ' + ', '.join(missing))
    if out == build:
        sys.exit('package_build: --out must not be the build dir')
    if not str(out).startswith(str(ROOT / 'build')) and not str(out).startswith(str(ROOT / 'generated')):
        print('package_build: warning: the folder holds game code; keep it out of the repository')

    out.mkdir(parents=True, exist_ok=True)
    copy(exe, out / f'totk{EXE}')
    for d in DLLS:
        copy(build / d, out / d)
    copy(a.xlate, out / f'totk_xlate{EXE}')
    idf = a.xlate.with_suffix('.id')
    if idf.exists():
        copy(idf, out / 'totk_xlate.id')
    for f in STATE:
        if (build / f).exists():
            copy(build / f, out / f)
        else:
            print(f'package_build: {f} not in {build}, skipped')
    n = copy_tree(build / 'romfs-patches', out / 'romfs-patches')
    if (build / 'romfs-patches-xbox').is_dir():   # pad_layout = xbox_prompts (make_romfs_patches.py --xbox-prompts)
        n += copy_tree(build / 'romfs-patches-xbox', out / 'romfs-patches-xbox')
    n += copy_tree(exefs, out / 'exefs')
    n += copy_tree(fonts, out / 'shared-fonts')
    if not a.no_shader_cache and (build / 'shader-cache').is_dir():
        n += copy_tree(build / 'shader-cache', out / 'shader-cache')
    (out / 'profile').mkdir(exist_ok=True)
    if a.with_romfs:
        (out / 'romfs').mkdir(exist_ok=True)
        print('package_build: copying romfs.bin (16 GB) ...')
        shutil.copyfile(romfs, out / 'romfs' / 'romfs.bin')
    print(f'package_build: {out}: {n} files in folders, {size_of(out) / 2**30:.2f} GiB'
          + ('' if a.with_romfs else f'; no RomFS: put romfs.bin into romfs{os.sep} or set TOTK_ROMFS'))


if __name__ == '__main__':
    main()
