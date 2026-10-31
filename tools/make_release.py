"""Assemble the release folder of docs/installer.md ("Release layout") from pinned downloads.

    .venv-py314\\Scripts\\python tools\\make_release.py --out build/release/TotK-Setup [--zip build/release/TotK-Setup.zip]
    .venv-py314\\Scripts\\python tools\\make_release.py --out build/release/TotK-Setup --prove

Needs Python 3.14 (tarfile reads the MSYS2 .zst packages), uv on PATH and network access on the first run. Downloads are
cached in toolchain/release-dl/ and checked against the sha256 in PINS; a mismatch stops the build. The folder holds:

    python/    python.org embeddable 3.14.9 + the packages of tools/requirements-installer.txt + tkinter (the Tcl/Tk 9.0
               files come out of the python.org installer's own component MSI, unpacked with msiexec /a and expand.exe;
               the installer is never run)
    llvm/      llvm-mingw, x86_64 only, the files clang and lld need to compile and link totk.exe
    runtime/   libtotkrt.a (build_game.py --make-runtime-lib with llvm/), the runtime headers, romfs.c, patches.c and
               ov_zstd.c (compiled per install), deps/ (headers and import libraries of SDL3, Opus, libvpx, Vulkan and VMA)
    bin/       totk_xlate.exe (+ .id), hactool.exe, SDL3.dll (upstream), libopus-0.dll, ninja.exe
    recomp/, tools/, data/, installer/, licenses/, THIRD_PARTY_NOTICES.md, totk.ini.example
    VERSION    the repository's VERSION plus the commit, 0.1.0+973c626; the installer's release string
    TotK-Setup.exe  installer/launcher/launcher.c built with llvm/ (-mwindows): starts python/pythonw.exe -m installer.gui

--prove compiles the generated C and links build/release-check/totk.exe with nothing but the folder's python/, llvm/,
runtime/ and bin/ (PATH holds the Windows system directory and those), then runs the four probes there.

Both need one game version extracted and lifted first (tools/extract.py, recomp/gen.py): generated/v121 or, without
it, generated/v143, and for --prove its build/extracted/<version> (they are read, nothing is written to them). The
runtime library reads only what both versions' layout.h share (MOD_COUNT, MOD_MAIN_BASE), so either serves.

The folder contains nothing derived from the game: check_content runs over it before the script finishes. Then the
folder's own python runs a check-only installer pass (no game files: it must get through the release, toolchain and host
checks and stop at the missing game files) and the window's --selftest.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_content  # noqa: E402
import shader_list  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
MAIN_ROOT = (ROOT / 'build').resolve().parent   # build/, generated/, toolchain/ and refs/ are junctions in a worktree
DL = MAIN_ROOT / 'toolchain' / 'release-dl'
WORK = MAIN_ROOT / 'toolchain' / 'release-work'   # unpacked downloads

MSYS2 = 'https://repo.msys2.org/mingw/ucrt64/'
SDL_VERSION = '3.4.18'
LLVM_VERSION = '20261020'
MNG_COMMIT = 'b75be8cfa704df3701e86130c8ced268e79d6b11'   # MacNativeGames, the shader list (refs/MacNativeGames)
# name: (url, sha256)
PINS = {
    'python-embed': ('https://www.python.org/ftp/python/3.14.9/python-3.14.9-embed-amd64.zip',
                     '3c01d7cfc666f746a6f064f60fb2156ab0ec464d3d738ab796371a46a9bb24a2'),
    'python-installer': ('https://www.python.org/ftp/python/3.14.9/python-3.14.9-amd64.exe',
                         'a247a0b612244a803f507b39fb7121e3602e4160d59c3b99d98e735289c53394'),
    'llvm-mingw': (f'https://github.com/mstorsjo/llvm-mingw/releases/download/{LLVM_VERSION}/'
                   f'llvm-mingw-{LLVM_VERSION}-ucrt-x86_64.zip',
                   'fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd'),
    'sdl3-dll': (f'https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/SDL3-{SDL_VERSION}-win32-x64.zip',
                 '75c2c0fc74e7d1206aaedc22893a35887332f4f85f5c8b74fddc10d177985f2c'),
    'sdl3-devel': (f'https://github.com/libsdl-org/SDL/releases/download/release-{SDL_VERSION}/SDL3-devel-{SDL_VERSION}-mingw.tar.gz',
                   '049b9dd711e3fb3f8d84feb998dfec54a75e17cf539c4a80ff4d058d839771cf'),
    'ninja': ('https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip',
              '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65'),
    'opus': (MSYS2 + 'mingw-w64-ucrt-x86_64-opus-1.6.1-2-any.pkg.tar.zst',
             'bb114bcf26225f50f874497ebfd6b8224fff2a20bda3f8ee45cd07ce5f02657b'),
    'libvpx': (MSYS2 + 'mingw-w64-ucrt-x86_64-libvpx-1.17.0-1-any.pkg.tar.zst',
               '8ff6919dbb88ea26ce28d22fd81d6f0d6e200cf56a4dd81ffc2d33124fe98024'),
    'libvpx-src': ('https://github.com/webmproject/libvpx/archive/refs/tags/v1.17.0.tar.gz',   # LICENSE and PATENTS only
                   '1020f184046187baa2985dbde38e0691f49c44088bca7a1842b0236c6081dc0a'),
    'vulkan-headers': (MSYS2 + 'mingw-w64-ucrt-x86_64-vulkan-headers-1~1.4.363.0-1-any.pkg.tar.zst',
                       'be602b2657cace531316e78b1eda4087e8f7f84ebb877f08fba198a2db06406a'),
    'vulkan-loader': (MSYS2 + 'mingw-w64-ucrt-x86_64-vulkan-loader-1~1.4.363.0-1-any.pkg.tar.zst',   # import library only
                      '065076fe13d39ab97eea4740596426d5ae741b5044e48e1c9495d04835c92dbe'),
    'vma': (MSYS2 + 'mingw-w64-ucrt-x86_64-vulkan-memory-allocator-3.4.0-1-any.pkg.tar.zst',
            '44c81162337915afecda3185f66fd98268a54454580b2717080c9dfef57fe781'),
    'zstd': (MSYS2 + 'mingw-w64-ucrt-x86_64-zstd-1.5.7-2-any.pkg.tar.zst',   # static libzstd.a (runtime/resolution.c)
             'dbdb8427280046a2b41697780aa4c52983b708082b0da4755951dc3bea96ca89'),
}
# The component MSI inside the python.org installer that carries Tcl/Tk and tkinter (Burn names its payloads a0..a27).
TCLTK_PAYLOAD = ('a12', 'ac87609db3f715404dd41a989c2f1706ea1512f3d86217f938d11c63af8eb505')
SDL3_DLL_SHA256 = '2b4b128d8d510cc2d442693cf848c050d700d43dc667afe9e5fd82b0570d667e'   # SDL3.dll inside the SDL zip

# llvm-mingw: what clang and lld need to compile and link totk.exe for x86_64-w64-mingw32 (dynamic LLVM, libc++, compiler-rt).
LLVM_BIN = {'clang.exe', 'clang++.exe', 'clang-23.exe', 'ld.lld.exe', 'llvm-ar.exe', 'llvm-ranlib.exe', 'llvm-nm.exe',
            'llvm-objdump.exe', 'llvm-objcopy.exe', 'llvm-strip.exe', 'llvm-readobj.exe', 'llvm-dlltool.exe',
            'llvm-rc.exe', 'llvm-cvtres.exe', 'llvm-windres.exe', 'llvm-wrapper.exe', 'mingw32-common.cfg',
            'x86_64-w64-windows-gnu.cfg', 'x86_64-pc-windows-gnu.cfg', 'x86_64-w64-mingw32-clang.exe',
            'x86_64-w64-mingw32-clang++.exe', 'libLLVM-23.dll', 'libclang-cpp.dll', 'libc++.dll', 'libunwind.dll',
            'libwinpthread-1.dll', 'libffi-8.dll'}
LLVM_SKIP_DIRS = {'aarch64-w64-mingw32', 'armv7-w64-mingw32', 'i686-w64-mingw32', 'busybox', 'python', 'share'}
LLVM_SKIP_SUFFIXES = {'.idl', '.tlb'}

# tools/ files the installer steps run (and what they import), copied from the repo.
TOOLS_FILES = ['build_game.py', 'build_shaders.py', 'button_icons.py', 'check_content.py', 'extract.py', 'hostmem.py',
               'make_romfs_patches.py', 'pack_shader_cache.py', 'prepare_shaders.py', 'romfs_index.py', 'shader_census.py',
               'shader_list.py', 'ultrawide_layout.py']
TOOLS_DIRS = ['vendor/macnativegames', 'data']   # data/: expected hashes
RUNTIME_SUFFIXES = {'.h', '.inc', '.ico'}   # .ico: the installer window icon

PROBES = ['--probe-kernel', '--probe-vkexec', '--probe-nvn-tex', '--probe-shader']


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def fetch(name):
    """The pinned download, from the cache or the network, checked against its sha256."""
    url, want = PINS[name]
    DL.mkdir(parents=True, exist_ok=True)
    path = DL / Path(url.split('?')[0]).name.replace('%7E', '~')
    if name == 'libvpx-src':
        path = DL / 'libvpx-1.17.0.tar.gz'
    if not path.is_file():
        print(f'download {url}')
        with urllib.request.urlopen(url) as r, open(path, 'wb') as f:
            shutil.copyfileobj(r, f)
    got = sha256_file(path)
    if got != want:
        sys.exit(f'{path}: sha256 {got}, pinned {want}')
    return path


def unpack(name, member_filter=None):
    """Extract a pinned archive into toolchain/release-work/<name> (once) and return that directory."""
    dest = WORK / name
    stamp = dest / '.unpacked'
    if stamp.is_file():
        return dest
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    path = fetch(name)
    if path.suffix == '.zip':
        with zipfile.ZipFile(path) as z:
            z.extractall(dest)
    else:
        with tarfile.open(path) as t:
            t.extractall(dest, filter='data')
    stamp.write_text('')
    return dest


def copy_tree(src, dst, skip=lambda rel: False):
    """Copy src into dst file by file (sorted), leaving out what skip(relative posix path) names."""
    for p in sorted(src.rglob('*')):
        rel = p.relative_to(src)
        if p.is_dir() or skip(rel.as_posix()):
            continue
        (dst / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, dst / rel)


def clean_env(*path_dirs):
    """A process environment without MSYS2 and the user's tools: the Windows system directory and path_dirs only."""
    system = os.environ.get('SystemRoot', r'C:\Windows')
    keep = ('SYSTEMROOT', 'SYSTEMDRIVE', 'TEMP', 'TMP', 'USERPROFILE', 'LOCALAPPDATA', 'APPDATA', 'COMSPEC', 'PATHEXT',
            'NUMBER_OF_PROCESSORS')   # os.environ keys are upper case on Windows
    env = {k: v for k, v in os.environ.items() if k.upper() in keep}
    env['PATH'] = os.pathsep.join([*(str(Path(d)) for d in path_dirs), rf'{system}\System32', system])
    return env


def run(cmd, **kw):
    print('+', ' '.join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


def mib(n):
    return f'{n / (1 << 20):.1f} MiB'


def dir_size(p):
    return sum(f.stat().st_size for f in Path(p).rglob('*') if f.is_file())


# ---- python/ -------------------------------------------------------------------------------------------------------

def tcltk_files():
    """Directory holding the python.org installer's tcltk payload, unpacked (not installed) into toolchain/release-work."""
    dest = WORK / 'tcltk'
    if (dest / 'DLLs' / '_tkinter.pyd').is_file():
        return dest
    exe = fetch('python-installer')
    data = exe.read_bytes()
    cabs = []
    pos = data.find(b'MSCF\0\0\0\0')
    while pos >= 0:
        cabs.append((struct.unpack_from('<I', data, pos + 8)[0], pos))
        pos = data.find(b'MSCF\0\0\0\0', pos + 1)
    size, pos = max(cabs)   # the attached container holds all component MSIs
    shutil.rmtree(WORK / 'tcltk-src', ignore_errors=True)
    src = WORK / 'tcltk-src'
    src.mkdir(parents=True)
    (src / 'container.cab').write_bytes(data[pos:pos + size])
    name, want = TCLTK_PAYLOAD
    expand = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'expand.exe'
    run([expand, '-F:' + name, src / 'container.cab', src], stdout=subprocess.DEVNULL)
    got = sha256_file(src / name)
    if got != want:
        sys.exit(f'{src / name}: sha256 {got}, pinned {want} (not the tcltk component of python 3.14.9)')
    msi = src / (name + '.msi')
    (src / name).rename(msi)
    shutil.rmtree(dest, ignore_errors=True)
    # Administrative image: copies the MSI's files to TARGETDIR, installs nothing and shows no window.
    run(['msiexec', '/a', msi, '/qn', f'TARGETDIR={dest}'])
    if not (dest / 'DLLs' / '_tkinter.pyd').is_file():
        sys.exit(f'{dest}: no _tkinter.pyd after msiexec /a')
    return dest


def build_python(out):
    py = out / 'python'
    with zipfile.ZipFile(fetch('python-embed')) as z:
        z.extractall(py)
    (py / 'python314._pth').write_text('python314.zip\n.\nLib/site-packages\n..\n../tools\n../recomp\n', encoding='utf-8')
    # The embeddable Python ignores the script directory and PYTHONPATH once a ._pth file exists, so the folder's own code
    # (installer/, and tools/*.py and recomp/*.py importing their siblings) has to be on the list.
    env = dict(os.environ, UV_CACHE_DIR=str(MAIN_ROOT / 'toolchain' / 'uv-cache'))
    run(['uv', 'pip', 'install', '--target', py / 'Lib' / 'site-packages', '--python', sys.executable,
         '--python-platform', 'windows', '--python-version', '3.14', '--only-binary', ':all:',
         '-r', ROOT / 'tools' / 'requirements-installer.txt'], env=env)
    shutil.rmtree(py / 'Lib' / 'site-packages' / 'bin', ignore_errors=True)   # numpy/cffi console scripts
    tk = tcltk_files()
    for dll in ('_tkinter.pyd', 'tcl90.dll', 'tcl9tk90.dll', 'zlib1.dll'):
        shutil.copy2(tk / 'DLLs' / dll, py / dll)
    copy_tree(tk / 'Lib' / 'tkinter', py / 'tkinter', lambda r: r.startswith('__pycache__'))
    copy_tree(tk / 'tcl', py / 'tcl', lambda r: r.endswith(('.lib', '.sh')) or r.startswith('nmake'))
    check = subprocess.run([str(py / 'python.exe'), '-I', '-c',
                            'import tkinter, numpy, lz4.block, cryptography, cffi, pycparser, fontTools.ttLib; '
                            'print("python ok: Tcl", tkinter.Tcl().eval("info patchlevel"), "numpy", numpy.__version__)'],
                           env=clean_env(py), capture_output=True, text=True)
    if check.returncode:
        sys.exit('bundled python check failed:\n' + check.stderr)
    print(check.stdout.strip())


# ---- llvm/ ---------------------------------------------------------------------------------------------------------

def build_llvm(out):
    src = unpack('llvm-mingw') / f'llvm-mingw-{LLVM_VERSION}-ucrt-x86_64'
    dst = out / 'llvm'

    def skip(rel):
        top = rel.split('/')[0]
        if top in LLVM_SKIP_DIRS:
            return True
        if top == 'bin':
            return rel not in {f'bin/{n}' for n in LLVM_BIN}
        if top == 'lib':   # lib/clang/<major>/include and the x86_64 Windows builtins; no sanitizers, no other targets
            if rel.startswith('lib/clang/') and '/include/' in rel:
                return False
            return not rel.endswith('/lib/windows/libclang_rt.builtins-x86_64.a')
        return Path(rel).suffix.lower() in LLVM_SKIP_SUFFIXES or rel.startswith('include/ddk/')

    copy_tree(src, dst, skip)
    return dst


# ---- bin/ and deps ---------------------------------------------------------------------------------------------------

def build_bin(out):
    b = out / 'bin'
    b.mkdir(parents=True, exist_ok=True)
    for f in ('totk_xlate.exe', 'totk_xlate.id'):
        shutil.copy2(MAIN_ROOT / 'build' / 'xlate' / f, b / f)
    shutil.copy2(MAIN_ROOT / 'toolchain' / 'hactool' / 'hactool.exe', b / 'hactool.exe')
    dll = unpack('sdl3-dll') / 'SDL3.dll'
    if sha256_file(dll) != SDL3_DLL_SHA256:
        sys.exit(f'{dll}: not the pinned SDL3.dll')
    shutil.copy2(dll, b / 'SDL3.dll')
    shutil.copy2(unpack('opus') / 'ucrt64' / 'bin' / 'libopus-0.dll', b / 'libopus-0.dll')
    shutil.copy2(unpack('ninja') / 'ninja.exe', b / 'ninja.exe')


def build_deps(out):
    """runtime/deps: the headers and import libraries build_game.py --deps expects (include/, lib/)."""
    d = out / 'runtime' / 'deps'
    (d / 'include').mkdir(parents=True, exist_ok=True)
    (d / 'lib').mkdir(parents=True, exist_ok=True)
    sdl = unpack('sdl3-devel') / f'SDL3-{SDL_VERSION}' / 'x86_64-w64-mingw32'
    copy_tree(sdl / 'include', d / 'include')
    shutil.copy2(sdl / 'lib' / 'libSDL3.dll.a', d / 'lib')
    for pkg, dirs, libs in (('opus', ['opus'], ['libopus.dll.a']), ('libvpx', ['vpx'], ['libvpx.a']),
                            ('vulkan-headers', ['vulkan', 'vk_video'], []), ('vulkan-loader', [], ['libvulkan-1.dll.a']),
                            ('vma', ['vma'], [])):
        root = unpack(pkg) / 'ucrt64'
        for inc in dirs:
            copy_tree(root / 'include' / inc, d / 'include' / inc)
        for lib in libs:
            shutil.copy2(root / 'lib' / lib, d / 'lib')
    zstd = unpack('zstd') / 'ucrt64'
    for name in ('zstd.h', 'zstd_errors.h'):
        shutil.copy2(zstd / 'include' / name, d / 'include')
    shutil.copy2(zstd / 'lib' / 'libzstd.a', d / 'lib')


def collect_licenses(out):
    lic = out / 'licenses'
    lic.mkdir(parents=True, exist_ok=True)
    pairs = [
        (unpack('opus') / 'ucrt64' / 'share' / 'licenses' / 'opus' / 'COPYING', 'opus-COPYING.txt'),
        (unpack('vulkan-headers') / 'ucrt64' / 'share' / 'licenses' / 'vulkan-headers' / 'LICENSE', 'vulkan-headers-LICENSE.txt'),
        (unpack('vulkan-loader') / 'ucrt64' / 'share' / 'licenses' / 'vulkan-loader' / 'LICENSE', 'vulkan-loader-LICENSE.txt'),
        (unpack('vma') / 'ucrt64' / 'share' / 'licenses' / 'VulkanMemoryAllocator' / 'LICENSE', 'vulkan-memory-allocator-LICENSE.txt'),
        (unpack('sdl3-dll') / 'LICENSE.txt', 'SDL3-LICENSE.txt'),
        (unpack('zstd') / 'ucrt64' / 'share' / 'licenses' / 'zstd' / 'LICENSE', 'zstd-LICENSE.txt'),
        (ROOT / 'runtime' / 'shaders' / 'ffx' / 'LICENSE.txt', 'FidelityFX-FSR-LICENSE.txt'),
        (MAIN_ROOT / 'toolchain' / 'hactool' / 'LICENSE', 'hactool-LICENSE.txt'),
        (MAIN_ROOT / 'refs' / 'MacNativeGames' / 'LICENSE', 'MacNativeGames-LICENSE.txt'),
    ]
    for src, name in pairs:
        shutil.copy2(src, lic / name)
    with tarfile.open(fetch('libvpx-src')) as t:   # the MSYS2 package carries no licence text; the notice needs both
        for member, name in (('libvpx-1.17.0/LICENSE', 'libvpx-LICENSE.txt'), ('libvpx-1.17.0/PATENTS', 'libvpx-PATENTS.txt')):
            (lic / name).write_bytes(t.extractfile(member).read())
    for name in ('LICENSE.TXT',):
        shutil.copy2(out / 'llvm' / name, lic / 'llvm-mingw-LICENSE.txt')


# ---- runtime/, recomp/, tools/, data/ ---------------------------------------------------------------------------------

def lifted_version():
    """The game version whose generated C the runtime library and --prove build against: v121, else v143."""
    for version in ('v121', 'v143'):
        if (MAIN_ROOT / 'generated' / version / 'layout.json').is_file():
            return version
    sys.exit('neither generated/v121 nor generated/v143 exists: extract a game version and lift it first '
             '(tools/extract.py, recomp/gen.py; docs/building.md)')


def runtime_lib(out, llvm):
    """Build libtotkrt.a with the folder's llvm/ and copy it, the headers and the install-side sources into runtime/."""
    build = MAIN_ROOT / 'build' / 'release-check' / 'rtlib'
    version = lifted_version()
    gen = MAIN_ROOT / 'generated' / version
    run([sys.executable, ROOT / 'tools' / 'build_game.py', '--make-runtime-lib', '--toolchain', llvm, '--version', version,
         '--deps', out / 'runtime' / 'deps', '--ninja', out / 'bin' / 'ninja.exe', '--gen', gen, '--out', build],
        env=clean_env(llvm / 'bin', out / 'bin'))
    rt = out / 'runtime'
    shutil.copy2(build / 'libtotkrt.a', rt / 'libtotkrt.a')
    for p in sorted((ROOT / 'runtime').rglob('*')):
        if p.suffix in RUNTIME_SUFFIXES and p.is_file():
            (rt / p.relative_to(ROOT / 'runtime')).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, rt / p.relative_to(ROOT / 'runtime'))
    for name in ('romfs.c', 'patches.c', 'ov_zstd.c'):   # build_game.py INSTALL_SIDE
        shutil.copy2(ROOT / 'runtime' / name, rt / name)


def build_sources(out):
    copy_tree(ROOT / 'recomp', out / 'recomp', lambda r: not r.endswith(('.py', '.txt')) or '__pycache__' in r)
    for f in TOOLS_FILES:
        (out / 'tools').mkdir(exist_ok=True)
        shutil.copy2(ROOT / 'tools' / f, out / 'tools' / f)
    for d in TOOLS_DIRS:
        copy_tree(ROOT / 'tools' / d, out / 'tools' / d, lambda r: '__pycache__' in r or r.endswith('.md'))
    (out / 'data').mkdir(exist_ok=True)
    text = shader_list.history(MAIN_ROOT / 'refs' / 'MacNativeGames', MNG_COMMIT)
    (out / 'data' / 'shader_list.tsv').write_text(text, encoding='utf-8', newline='\n')
    copy_tree(ROOT / 'installer', out / 'installer', lambda r: '__pycache__' in r or r.startswith('launcher/'))
    shutil.copy2(ROOT / 'totk.ini.example', out / 'totk.ini.example')
    shutil.copy2(ROOT / 'THIRD_PARTY_NOTICES.md', out / 'THIRD_PARTY_NOTICES.md')
    (out / 'VERSION').write_text(version() + '\n', encoding='utf-8')


def version():
    """The port version from VERSION with the commit as build metadata: 0.1.0+973c626. The commit keeps two releases of
    one version apart, so the installer reruns the steps an earlier release made."""
    r = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True)
    if r.returncode or not r.stdout.strip():
        sys.exit('no git version: not a git checkout')
    return (ROOT / 'VERSION').read_text(encoding='utf-8').strip() + '+' + r.stdout.strip()


# ---- TotK-Setup.exe --------------------------------------------------------------------------------------------------

def c_wide(text):
    """A C wide string literal of text: ASCII as is, everything else as a universal character name."""
    bs = chr(92)
    out = []
    for ch in text:
        if ch in (bs, '"'):
            out.append(bs + ch)
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        elif ord(ch) < 0x10000:
            out.append(f'{bs}u{ord(ch):04x}')
        else:
            sys.exit(f'launcher text: {ch!r} is outside the BMP')
    return 'L"' + ''.join(out) + '"'


def launcher_header(path):
    """launcher_text.h: the launcher's two messages in English and Russian, taken from the installer's catalog."""
    lines = ['/* generated by tools/make_release.py from installer/text/{en,ru}.json */']
    for lang in ('en', 'ru'):
        texts = json.loads((ROOT / 'installer' / 'text' / f'{lang}.json').read_text(encoding='utf-8'))
        for key in ('title', 'python_missing'):
            lines.append(f'#define LAUNCHER_{key.upper()}_{lang.upper()} {c_wide(texts["launcher." + key])}')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(lines) + '\n', encoding='ascii')


def build_launcher(out, llvm):
    work = MAIN_ROOT / 'build' / 'release-check' / 'launcher'
    launcher_header(work / 'launcher_text.h')
    env = clean_env(llvm / 'bin')
    res = work / 'launcher_rc.o'
    run([llvm / 'bin' / 'llvm-windres.exe', '-I', ROOT / 'runtime', '-O', 'coff',
         ROOT / 'installer' / 'launcher' / 'launcher.rc', '-o', res], env=env)
    run([llvm / 'bin' / 'clang.exe', '-O2', '-mwindows', '-Wall', '-Wl,-s', '-I', work,
         ROOT / 'installer' / 'launcher' / 'launcher.c', res, '-o', out / 'TotK-Setup.exe'], env=env)


# ---- checks ----------------------------------------------------------------------------------------------------------

def check_folder(out):
    """check_content over every file of the release folder; a hit stops the build."""
    bad = []
    for p in sorted(out.rglob('*')):
        if p.is_file():
            bad += [f'{p.relative_to(out)}: {problem}' for problem in check_content.problems_for(p, allow_large=True)]
    if bad:
        sys.exit('game-derived content in the release folder:\n' + '\n'.join(bad))
    print(f'check_content: {sum(1 for p in out.rglob("*") if p.is_file())} files, no hit')


def check_bundle(out):
    """The folder's own python runs the installer without game files and the window's selftest."""
    chk = MAIN_ROOT / 'build' / 'release-check' / 'bundle'
    shutil.rmtree(chk, ignore_errors=True)
    env = clean_env(out / 'python')
    py = out / 'python' / 'python.exe'
    r = subprocess.run([py, '-m', 'installer', '--lang', 'en-US', '--dest', chk / 'install', '--temp', chk / 'temp'],
                       cwd=out, env=env, capture_output=True, text=True, timeout=600)
    events = [json.loads(line) for line in r.stdout.splitlines() if line.startswith('{')]
    errors = [e['id'] for e in events if e['type'] == 'error']
    started = any(e['type'] == 'start' and e.get('release') == (out / 'VERSION').read_text().strip() for e in events)
    steps = [e['phase'] for e in events if e['type'] == 'progress']
    if errors != ['check.inputs_needed'] or not started or 'inputs' not in steps:
        sys.exit(f'bundle check: exit {r.returncode}, errors {errors}, release read {started}\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}')
    print(f'bundle check: the installer ran the host check from the folder and stopped at the game files (exit {r.returncode})')
    r = subprocess.run([py, '-m', 'installer.gui', '--selftest'], cwd=out, env=env, capture_output=True, text=True, timeout=300)
    if r.returncode:
        sys.exit(f'bundle check: window selftest exit {r.returncode}\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}')
    print('bundle check: the window built every screen in both languages and closed')


def sizes(out):
    rows = [(p.name if p.is_dir() else p.name, dir_size(p) if p.is_dir() else p.stat().st_size) for p in sorted(out.iterdir())]
    for name, n in rows:
        print(f'  {name:26} {mib(n):>12}')
    print(f'  {"total":26} {mib(sum(n for _, n in rows)):>12}')


def write_zip(out, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for p in sorted(out.rglob('*')):
            if p.is_file():
                info = zipfile.ZipInfo((Path(out.name) / p.relative_to(out)).as_posix(), (1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o644 << 16
                z.writestr(info, p.read_bytes(), compresslevel=6)
    print(f'{path}: {mib(path.stat().st_size)}')


def prove(out):
    """Compile the generated C and link totk.exe with the folder's tools only, then run the probes next to it."""
    out = out.resolve()
    version = lifted_version()
    gen = MAIN_ROOT / 'generated' / version
    ext = MAIN_ROOT / 'build' / 'extracted' / version
    chk = MAIN_ROOT / 'build' / 'release-check'
    env = clean_env(out / 'python', out / 'llvm' / 'bin', out / 'bin')
    t0 = time.time()
    run([out / 'python' / 'python.exe', out / 'tools' / 'build_game.py', '--toolchain', out / 'llvm',
         '--deps', out / 'runtime' / 'deps', '--runtime', out / 'runtime', '--runtime-lib', out / 'runtime' / 'libtotkrt.a',
         '--ninja', out / 'bin' / 'ninja.exe', '--version', version, '--gen', gen, '--out', chk, '--exefs', ext / 'exefs',
         '--romfs', ext / 'romfs', '--profile', chk / 'profile'], env=env)
    print(f'prove: build {time.time() - t0:.0f} s')
    for dll in ('SDL3.dll', 'libopus-0.dll'):
        shutil.copy2(out / 'bin' / dll, chk / dll)
    failed = []
    for probe in PROBES:
        r = subprocess.run([str(chk / 'totk.exe'), probe], cwd=chk, env=clean_env(chk), capture_output=True, text=True,
                           timeout=900)
        tail = (r.stdout.strip().splitlines() or [''])[-1]
        print(f'{probe}: exit {r.returncode}: {tail}')
        if r.returncode:
            failed.append(probe)
    if failed:
        sys.exit(f'probes failed: {failed}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', type=Path, required=True, help='release folder (replaced)')
    ap.add_argument('--zip', type=Path, help='also write the folder as this zip')
    ap.add_argument('--prove', action='store_true', help='only run the compile-and-probe check on an existing folder')
    args = ap.parse_args()
    out = args.out.resolve()
    if args.prove:
        prove(out)
        return
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    build_python(out)
    llvm = build_llvm(out)
    build_bin(out)
    build_deps(out)
    runtime_lib(out, llvm)
    build_sources(out)
    build_launcher(out, llvm)
    collect_licenses(out)
    check_folder(out)
    check_bundle(out)
    sizes(out)
    if args.zip:
        write_zip(out, args.zip.resolve())


if __name__ == '__main__':
    main()
