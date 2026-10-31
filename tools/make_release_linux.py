"""Assemble the Linux release folder of docs/installer.md ("Linux release") and pack it as an AppImage.

    .venv/bin/python tools/make_release_linux.py --out build/release/TotK-Setup-linux \\
        [--appimage build/release/TotK-Setup-x86_64.AppImage]
    .venv/bin/python tools/make_release_linux.py --out build/release/TotK-Setup-linux --prove

The same model as the Windows release (tools/make_release.py, whose download cache, unpacking and checks this reuses): the
folder carries the setup and a toolchain, never game code; the player's machine builds the game. Everything that runs on
the player's machine is built against the Steam Runtime 3 "sniper" SDK sysroot (glibc 2.31), so the AppImage and the
game it builds run on SteamOS and on any distribution at least as new. Needs network access on the first run, make,
the .NET 10 SDK and Python 3.12+ with Pillow (.venv). The folder holds:

    python/    python-build-standalone 3.14 (with tkinter) + the packages of tools/requirements-installer.txt; a .pth
               file puts the folder, tools/ and recomp/ on sys.path
    llvm/      LLVM 23 (clang, lld, llvm-ar, the clang resource headers) and llvm/sysroot: the files of the sniper SDK
               sysroot the game's compile and link read, nothing else (recorded while building the runtime library)
    runtime/   libtotkrt.a (build_game.py --make-runtime-lib --sysroot), the runtime headers, romfs.c, patches.c and
               ov_zstd.c (compiled per install), totk.png (the window icon), deps/ (Vulkan headers 1.4.363, VMA, zstd
               built static; SDL3, Opus and libvpx come from the sysroot)
    bin/       totk_xlate (+ .id), hactool, ninja, libSDL3.so.0 (the sysroot's; the game finds it beside itself)
    recomp/, tools/, data/, installer/, licenses/, THIRD_PARTY_NOTICES.md, totk.ini.example, VERSION
    AppRun, totk-setup.desktop, totk-setup.png   the AppImage entry: python/bin/python3 -m installer.gui

--appimage packs the folder with appimagetool and the static type-2 runtime (no libfuse2 on the host). --prove compiles
the generated C and links build/release-linux-check/totk with the folder's tools only, then runs the probes beside it.
Like tools/make_release.py, both need one game version extracted and lifted first (generated/v121, else v143).

The folder contains nothing derived from the game: check_content runs over it before the script finishes, then the
folder's own python runs a check-only installer pass and the window's --selftest.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_game  # noqa: E402
import make_release as mr  # noqa: E402

ROOT = mr.ROOT
MAIN_ROOT = mr.MAIN_ROOT
WORK = mr.WORK
CHECK = MAIN_ROOT / 'build' / 'release-linux-check'

LLVM_VERSION = '23.1.4'
LLVM_MAJOR = LLVM_VERSION.split('.')[0]
SNIPER = '3.0.20261021.266025'
SNIPER_URL = 'https://repo.steampowered.com/steamrt-images-sniper/snapshots'
PINS = {
    'llvm-linux': (f'https://github.com/llvm/llvm-project/releases/download/llvmorg-{LLVM_VERSION}/'
                   f'LLVM-{LLVM_VERSION}-Linux-X64.tar.xz',
                   '25d814fda50d5fb0084b42be09605d8b1595d49d5e2e03a3345d7e112ff6f9f3'),
    'llvm-license': (f'https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-{LLVM_VERSION}/llvm/LICENSE.TXT',
                     '8d85c1057d742e597985c7d4e6320b015a9139385cff4cbae06ffc0ebe89afee'),   # the binary release carries none
    'sniper-sdk': (f'{SNIPER_URL}/{SNIPER}/com.valvesoftware.SteamRuntime.Sdk-amd64%2Ci386-sniper-sysroot.tar.gz',
                   '8f71947035f97925a311b2ed753f22efcf2f13d6f1631945b8e537f8903ce2cf'),
    'python-linux': ('https://github.com/astral-sh/python-build-standalone/releases/download/20261026/'
                     'cpython-3.14.10%2B20261026-x86_64-unknown-linux-gnu-install_only.tar.gz',
                     '1b38024924eb7f4cf0cab45d7b871d1f1991354190fccd2625ac7529a43786e2'),
    'tcl-linux-src': ('https://github.com/tcltk/tcl/archive/refs/tags/core-9-0-4.tar.gz',
                      '8c01bac92a9a8ce271b63d3aa28f09cde2c6762d6719ca671cdc8aee25391234'),
    'tk-linux-src': ('https://github.com/tcltk/tk/archive/refs/tags/core-9-0-4.tar.gz',
                     'ab9bd89fcff2e1248b7a9c097035f817fa0725285028c95686fdb92db65bc148'),
    'ninja-linux': ('https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-linux.zip',
                    '5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6'),
    'zstd-src': ('https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz',
                 'eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3'),
    'vulkan-headers-src': ('https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.363.tar.gz',
                           'cbaf687d3c59b9666fe080e7f8396b6b4f4d344a768ee6b337c59852f4526b68'),
    'vma-header': build_game.VMA_FETCH,
    # appimagetool and the type-2 runtime from release tags, not "continuous", whose uploads change under the same
    # URL and would stop the build at the hash check
    'appimagetool': ('https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage',
                     'ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0'),
    'appimage-runtime': ('https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64',
                         '2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d'),
}
mr.PINS.update(PINS)   # one cache (toolchain/release-dl) and one fetch for both releases

# What clang, lld and the runtime build need from the LLVM release (the rest of its 8 GB stays packed).
LLVM_MEMBERS = [f'bin/clang-{LLVM_MAJOR}', 'bin/clang', 'bin/clang++', 'bin/lld', 'bin/ld.lld', 'bin/llvm-ar',
                'bin/llvm-ranlib', 'bin/llvm-objcopy', 'bin/llvm-strip', f'lib/clang/{LLVM_MAJOR}/include']
# The parts of the sniper SDK a C/C++ compile and link for x86_64 can read.
SNIPER_MEMBERS = ['usr/include', 'usr/share/pkgconfig', 'usr/lib/x86_64-linux-gnu', 'usr/lib/gcc/x86_64-linux-gnu',
                  'lib/x86_64-linux-gnu', 'lib64', 'usr/share/doc/libsdl3-0', 'usr/share/doc/libopus0',
                  'usr/share/doc/libvpx6', 'usr/share/doc/libc6', 'usr/share/doc/gcc-10-base',
                  'usr/share/doc/libvulkan1',
                  'usr/share/doc/libxft2', 'usr/share/doc/libxrender1']
# glibc's link libraries and their scripts (lib<name>.so, .a, _nonshared.a), and the GCC runtime pieces clang links.
GLIBC_LIBS = ['c', 'm', 'mvec', 'dl', 'pthread', 'rt', 'util']
GCC_LIBS = {'libgcc.a', 'libgcc_eh.a', 'libgcc_s.so', 'libstdc++.a', 'libsupc++.a'}
# The libraries of a release link (build_game.py LINK_LIBS with --deps: Opus, libvpx and zstd static).
RELEASE_LIBS = ['-lvulkan', '-lSDL3', '-l:libopus.a', '-l:libvpx.a', '-l:libzstd.a', '-l:libstdc++.a', '-lm',
                '-lpthread', '-ldl']


def run(cmd, **kw):
    return mr.run(cmd, **kw)


def tar_extract(name, dest, members, strip=0):
    """Extract only members (path prefixes inside the archive) of a pinned tarball with the system tar, once."""
    stamp = dest / '.unpacked'
    if stamp.is_file():
        return dest
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    path = mr.fetch(name)
    t0 = time.time()
    run(['tar', '-xf', path, '-C', dest, f'--strip-components={strip}', '--wildcards', '--no-wildcards-match-slash',
         *members])
    print(f'{name}: unpacked in {time.time() - t0:.0f} s')
    stamp.write_text('')
    return dest


# ---- llvm/ and the sysroot -------------------------------------------------------------------------------------------

def build_llvm(out):
    top = f'LLVM-{LLVM_VERSION}-Linux-X64'
    src = tar_extract('llvm-linux', WORK / 'llvm-linux', [f'{top}/{m}' for m in LLVM_MEMBERS], strip=1)
    dst = out / 'llvm'
    shutil.copytree(src, dst, symlinks=True, ignore=shutil.ignore_patterns('.unpacked'))
    strip(dst / 'bin', dst / 'bin' / 'llvm-strip')
    r = subprocess.run([str(dst / 'bin' / 'clang'), '--version'], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'{dst / "bin" / "clang"} does not start:\n{r.stderr}')
    print(r.stdout.splitlines()[0])
    return dst


def strip(where, tool):
    """Drop the debug symbols of the ELF files under where (the LLVM and Python releases ship them: clang 270 MB)."""
    for p in sorted(where.rglob('*')):
        if p.is_file() and not p.is_symlink() and p.read_bytes()[:4] == b'\x7fELF':
            subprocess.run([str(tool), '--strip-unneeded', str(p)], check=True)


def full_sysroot():
    """The sniper SDK's x86_64 compile and link files in toolchain/release-work/sniper, absolute symlinks made relative
    (the SDK points some at /lib and /usr/lib, which outside its container are the host's)."""
    root = WORK / 'sniper'
    if (root / '.relinked').is_file() and (root / 'usr/share/doc/libxft2/copyright').is_file():
        return root
    # Older cached SDK extractions omitted the font libraries' architecture-independent pkg-config files and the
    # libXft and libXrender copyright files.
    (root / '.unpacked').unlink(missing_ok=True)
    tar_extract('sniper-sdk', root, SNIPER_MEMBERS)
    for p in root.rglob('*'):
        if p.is_symlink() and os.readlink(p).startswith('/'):
            target = root / os.readlink(p).lstrip('/')
            p.unlink()
            p.symlink_to(os.path.relpath(target, p.parent))
    (root / '.relinked').write_text('')
    return root


def sysroot_cc(llvm, sysroot):
    return f'{llvm / "bin" / "clang"} --target=x86_64-linux-gnu --sysroot={sysroot} -fuse-ld=lld'


# ---- runtime/deps, bin/ --------------------------------------------------------------------------------------------

def build_deps(out, llvm, sysroot):
    d = out / 'runtime' / 'deps'
    (d / 'include' / 'vma').mkdir(parents=True, exist_ok=True)
    (d / 'lib').mkdir(parents=True, exist_ok=True)
    vk = tar_extract('vulkan-headers-src', WORK / 'vulkan-headers', ['Vulkan-Headers-1.4.363/include',
                                                                     'Vulkan-Headers-1.4.363/LICENSE.md'], strip=1)
    for sub in ('vulkan', 'vk_video'):
        shutil.copytree(vk / 'include' / sub, d / 'include' / sub)
    shutil.copy2(mr.fetch('vma-header'), d / 'include' / 'vma' / 'vk_mem_alloc.h')
    zs = tar_extract('zstd-src', WORK / 'zstd', ['zstd-1.5.7'], strip=1)
    lib = zs / 'lib'
    if not (lib / 'libzstd.a').is_file():
        run(['make', '-C', lib, f'-j{os.cpu_count() or 4}', 'libzstd.a', f'CC={sysroot_cc(llvm, sysroot)}',
             f'AR={llvm / "bin" / "llvm-ar"}', 'ZSTD_LEGACY_SUPPORT=0', 'MOREFLAGS=-O2'], stdout=subprocess.DEVNULL)
    shutil.copy2(lib / 'libzstd.a', d / 'lib' / 'libzstd.a')
    for name in ('zstd.h', 'zstd_errors.h'):
        shutil.copy2(lib / name, d / 'include' / name)


def build_bin(out, llvm, sysroot):
    b = out / 'bin'
    b.mkdir(parents=True, exist_ok=True)
    zip_dir = mr.unpack('ninja-linux')
    shutil.copy2(zip_dir / 'ninja', b / 'ninja')
    (b / 'ninja').chmod(0o755)
    hac = WORK / 'hactool-linux'
    run([sys.executable, ROOT / 'tools' / 'build_hactool.py', '--src', WORK / 'hactool-src', '--out', hac,
         '--cc', sysroot_cc(llvm, sysroot)])
    shutil.copy2(hac / 'hactool', b / 'hactool')
    xl = WORK / 'xlate-linux'
    run([sys.executable, ROOT / 'tools' / 'build_xlate.py', '--out', xl, '--llvm', llvm, '--sysroot', sysroot])
    shutil.copy2(xl / 'totk_xlate', b / 'totk_xlate')
    shutil.copy2(xl / 'totk_xlate.id', b / 'totk_xlate.id')
    sdl = (sysroot / 'usr' / 'lib' / 'x86_64-linux-gnu' / 'libSDL3.so.0').resolve()
    shutil.copy2(sdl, b / 'libSDL3.so.0')


# ---- runtime/ ----------------------------------------------------------------------------------------------------------

def runtime_lib(out, llvm, sysroot):
    build = CHECK / 'rtlib'
    shutil.rmtree(build, ignore_errors=True)
    version = mr.lifted_version()
    run([sys.executable, ROOT / 'tools' / 'build_game.py', '--make-runtime-lib', '--toolchain', llvm, '--sysroot', sysroot,
         '--deps', out / 'runtime' / 'deps', '--ninja', out / 'bin' / 'ninja', '--version', version,
         '--gen', MAIN_ROOT / 'generated' / version, '--out', build])
    rt = out / 'runtime'
    shutil.copy2(build / 'libtotkrt.a', rt / 'libtotkrt.a')
    for p in sorted((ROOT / 'runtime').rglob('*')):
        if p.suffix in mr.RUNTIME_SUFFIXES and p.is_file():
            (rt / p.relative_to(ROOT / 'runtime')).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, rt / p.relative_to(ROOT / 'runtime'))
    for name in build_game.INSTALL_SIDE:
        shutil.copy2(ROOT / 'runtime' / name, rt / name)
    from PIL import Image   # Tk loads no .ico off Windows: the window and the AppImage take a PNG
    with Image.open(ROOT / 'runtime' / 'totk.ico') as ico:
        ico.size = max(ico.ico.sizes())
        ico.convert('RGBA').save(rt / 'totk.png')
    return build


def used_sysroot_files(out, llvm, sysroot, rtlib_build):
    """The sysroot files a game build reads: the headers of the runtime's compile (its .d files), of the install-side
    sources and of hostcheck, and every library and object of a link (lld --trace) of the runtime library with the
    flags and libraries build_game.py uses, its references to the generated code left unresolved."""
    used = set()
    for dep in rtlib_build.rglob('*.d'):
        used.update(re.findall(r'\S+', dep.read_text(encoding='utf-8', errors='replace').replace('\\\n', ' ')))
    probe = CHECK / 'sysroot-probe'
    shutil.rmtree(probe, ignore_errors=True)
    probe.mkdir(parents=True)
    cc = [llvm / 'bin' / 'clang', '--target=x86_64-linux-gnu', f'--sysroot={sysroot}', '-I', out / 'runtime',
          '-I', out / 'runtime' / 'deps' / 'include', '-I', CHECK / 'rtlib']
    for i, src in enumerate([*(out / 'runtime' / n for n in sorted(build_game.INSTALL_SIDE)),
                             ROOT / 'installer' / 'hostcheck.c']):
        # romfs.c needs the per-install romfs_config.h: whatever it reads up to there still lands in the .d file
        subprocess.run([str(c) for c in [*cc, '-fsyntax-only', '-MD', '-MF', probe / f'{i}.d', src]],
                       capture_output=True)
        if (probe / f'{i}.d').is_file():
            used.update(re.findall(r'\S+', (probe / f'{i}.d').read_text().replace('\\\n', ' ')))
    trace = subprocess.run([str(c) for c in [
        llvm / 'bin' / 'clang', '--target=x86_64-linux-gnu', f'--sysroot={sysroot}', *build_game.LINK_FLAGS[:3],
        '-Wl,--trace', '-Wl,--unresolved-symbols=ignore-all', '-Wl,--whole-archive', out / 'runtime' / 'libtotkrt.a',
        '-Wl,--no-whole-archive', '-L', out / 'runtime' / 'deps' / 'lib',
        *RELEASE_LIBS, '-o', probe / 'trace.out']],
        capture_output=True, text=True)
    if trace.returncode:
        sys.exit(f'sysroot trace link failed:\n{trace.stderr[-3000:]}')
    used.update(line.split('(')[0] for line in trace.stdout.splitlines())
    root = str(sysroot.resolve())
    files = set()
    for p in used:
        path = Path(p)
        if not path.is_absolute():
            continue
        for q in (path, path.resolve()):   # the name read and, through symlinks, the file
            if str(q).startswith(root + '/') and q.exists():
                files.add(q)
    # The C library and kernel headers whole: which of them a compile reads depends on its flags (at -O1 glibc's inline
    # variants pull in bits/stdio.h and others), and the setup compiles the generated code, hostcheck and the
    # install-side sources with flags of its own
    inc = sysroot / 'usr' / 'include'
    arch = inc / 'x86_64-linux-gnu'
    files.update(p for p in [*inc.glob('*.h'), *arch.glob('*.h')] if p.is_file())
    for d in (arch / 'bits', arch / 'sys', arch / 'gnu', arch / 'asm', inc / 'linux', inc / 'asm-generic'):
        files.update(p for p in d.rglob('*') if p.is_file() or p.is_symlink())
    # The C runtime whole, for PIE links (hostcheck) as well as the game's: lld's trace names the libraries a link
    # script loads but not the script (libc.so, libm.so, libgcc_s.so), and clang's GCC detection needs the crt objects
    lib = sysroot / 'usr' / 'lib' / 'x86_64-linux-gnu'
    for name in GLIBC_LIBS:
        files.update(p for p in lib.glob(f'lib{name}[._]*') if p.suffix in ('.so', '.a') or '.so' in p.name)
        files.update(p for p in (sysroot / 'lib' / 'x86_64-linux-gnu').glob(f'lib{name}.so*'))
    files.update(lib.glob('*crt*.o'))
    files.update((sysroot / 'lib64').glob('ld-linux*'))
    files.update((sysroot / 'lib' / 'x86_64-linux-gnu').glob('ld-linux*'))
    files.update((sysroot / 'lib' / 'x86_64-linux-gnu').glob('libgcc_s.so*'))
    gcc = next((sysroot / 'usr' / 'lib' / 'gcc' / 'x86_64-linux-gnu').iterdir())
    files.update(p for p in gcc.iterdir() if p.name.startswith('crt') or p.name in GCC_LIBS)
    return files


def link_chain(path):
    """path and every link on the way to its file: libvulkan.so -> libvulkan.so.1 -> libvulkan.so.1.4.309."""
    hops = [path]
    while hops[-1].is_symlink() and len(hops) < 16:
        hops.append(hops[-1].parent / os.readlink(hops[-1]))
    return hops


def build_sysroot(out, sysroot, files):
    dst = out / 'llvm' / 'sysroot'
    files = {Path(os.path.normpath(h)) for f in files for h in link_chain(f)}
    for f in sorted(files):
        rel = f.relative_to(sysroot)
        (dst / rel).parent.mkdir(parents=True, exist_ok=True)
        if f.is_symlink():
            if not (dst / rel).is_symlink():
                (dst / rel).symlink_to(os.readlink(f))
        elif not (dst / rel).exists():
            shutil.copy2(f, dst / rel)
    # a symlink copied above may name a file the build reads only through it
    for link in [p for p in dst.rglob('*') if p.is_symlink()]:
        target = (sysroot / link.relative_to(dst)).resolve()
        mirrored = dst / target.relative_to(sysroot.resolve())
        if not mirrored.exists():
            mirrored.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(target, mirrored)
    print(f'sysroot: {sum(1 for p in dst.rglob("*") if p.is_file())} files, {mr.mib(mr.dir_size(dst))}')


# ---- python/ ---------------------------------------------------------------------------------------------------------

def build_tk_xft(out, llvm, sysroot):
    """Replace standalone Python's no-Xft Tcl/Tk with the same version using system TrueType fonts (#227)."""
    prefix = WORK / 'tcltk-xft'
    env = dict(os.environ, CC=sysroot_cc(llvm, sysroot),
               PKG_CONFIG_SYSROOT_DIR=str(sysroot),
               PKG_CONFIG_LIBDIR=os.pathsep.join(str(sysroot / p) for p in
                   ('usr/lib/x86_64-linux-gnu/pkgconfig', 'usr/share/pkgconfig')),
               PKG_CONFIG_PATH='')
    sources = {}
    for name in ('tcl', 'tk'):
        src = mr.unpack(f'{name}-linux-src') / f'{name}-core-9-0-4'
        sources[name] = src
        build = src / 'unix'
        options = [f'--prefix={prefix}', '--enable-shared', '--disable-zipfs', '--disable-rpath']
        if name == 'tk':
            options += [f'--with-tcl={prefix / "lib"}', '--enable-xft', '--disable-libcups', '--disable-xss']
        run([build / 'configure', *options], cwd=build, env=env)
        run(['make', f'-j{os.cpu_count() or 4}'], cwd=build, env=env)
        run(['make', 'install'], cwd=build, env=env)
    lib = prefix / 'lib'
    # A successful GUI selftest alone did not catch missing glyphs. Reject a no-Xft build before packaging it.
    symbols = subprocess.run(['nm', '-D', str(lib / 'libtcl9tk9.0.so')],
                             capture_output=True, text=True, check=True).stdout
    if not re.search(r'\bU Xft', symbols):
        sys.exit('Tcl/Tk build has no Xft font rendering')
    dst = out / 'python' / 'lib'
    for name in ('libtcl9.0.so', 'libtcl9tk9.0.so'):
        shutil.copy2(lib / name, dst / name)
    # libXft is not on every desktop (the old Tk did not need it) and without it _tkinter fails to import, so the
    # window would not open at all. python's RPATH ($ORIGIN/../lib) finds these beside the Tk library.
    sys_lib = sysroot / 'usr' / 'lib' / 'x86_64-linux-gnu'
    for pattern in ('libXft.so.2*', 'libXrender.so.1*'):
        for f in sys_lib.glob(pattern):
            shutil.copy2(f, dst / f.name, follow_symlinks=False)
    for name in ('tcl9.0', 'tk9.0'):
        shutil.rmtree(dst / name)
        shutil.copytree(lib / name, dst / name, symlinks=True)
    licenses = out / 'licenses'
    licenses.mkdir(exist_ok=True)
    for name, src in sources.items():
        shutil.copy2(src / 'license.terms', licenses / f'{name}-license.terms')


def build_python(out, llvm, sysroot):
    src = mr.unpack('python-linux') / 'python'
    py = out / 'python'
    shutil.copytree(src, py, symlinks=True,
                    ignore=shutil.ignore_patterns('test', 'idlelib', 'turtledemo', '__pycache__', '*.a'))
    exe = py / 'bin' / 'python3'
    env = dict(os.environ, PIP_DISABLE_PIP_VERSION_CHECK='1')
    run([exe, '-m', 'pip', 'install', '--no-compile', '--only-binary', ':all:', '-r',
         ROOT / 'tools' / 'requirements-installer.txt'], env=env, stdout=subprocess.DEVNULL)
    run([exe, '-m', 'pip', 'uninstall', '-y', 'pip'], stdout=subprocess.DEVNULL)   # the setup installs nothing
    build_tk_xft(out, llvm, sysroot)
    strip(py, out / 'llvm' / 'bin' / 'llvm-strip')
    site = next(py.glob('lib/python3.*/site-packages'))
    # The release folder, tools/ and recomp/ on sys.path, as the Windows python's ._pth does: `python -m installer` and
    # the tools importing their siblings work from any directory
    (site / 'totk-release.pth').write_text('../../../..\n../../../../tools\n../../../../recomp\n', encoding='utf-8')
    check = subprocess.run([str(exe), '-c', 'import tkinter, numpy, lz4.block, cryptography, cffi, pycparser; '
                            'print("python ok: Tcl", tkinter.Tcl().eval("info patchlevel"), "numpy", numpy.__version__)'],
                           cwd='/', capture_output=True, text=True)
    if check.returncode:
        sys.exit('bundled python check failed:\n' + check.stderr)
    print(check.stdout.strip())


# ---- licences, AppImage entry ------------------------------------------------------------------------------------------

def collect_licenses(out, sysroot):
    lic = out / 'licenses'
    lic.mkdir(parents=True, exist_ok=True)
    doc = sysroot / 'usr' / 'share' / 'doc'
    pairs = [
        (mr.fetch('llvm-license'), 'llvm-LICENSE.txt'),
        (doc / 'libsdl3-0' / 'copyright', 'SDL3-copyright.txt'),
        (doc / 'libopus0' / 'copyright', 'opus-copyright.txt'),
        (doc / 'libvpx6' / 'copyright', 'libvpx-copyright.txt'),
        (doc / 'libc6' / 'copyright', 'glibc-copyright.txt'),
        (doc / 'gcc-10-base' / 'copyright', 'libstdc++-copyright.txt'),
        (doc / 'libvulkan1' / 'copyright', 'vulkan-loader-copyright.txt'),
        (doc / 'libxft2' / 'copyright', 'libXft-copyright.txt'),
        (doc / 'libxrender1' / 'copyright', 'libXrender-copyright.txt'),
        (WORK / 'vulkan-headers' / 'LICENSE.md', 'vulkan-headers-LICENSE.txt'),
        (WORK / 'zstd' / 'LICENSE', 'zstd-LICENSE.txt'),
        (out / 'python' / 'lib' / f'python{sys_version(out)}' / 'LICENSE.txt', 'python-LICENSE.txt'),
        (ROOT / 'runtime' / 'shaders' / 'ffx' / 'LICENSE.txt', 'FidelityFX-FSR-LICENSE.txt'),
        (WORK / 'hactool-linux' / 'LICENSE', 'hactool-LICENSE.txt'),
        (MAIN_ROOT / 'refs' / 'MacNativeGames' / 'LICENSE', 'MacNativeGames-LICENSE.txt'),
    ]
    for src, name in pairs:
        shutil.copy2(src, lic / name)
    import tarfile   # Debian's libvpx copyright carries no patent grant: the WebM PATENTS text from the pinned source tag
    with tarfile.open(mr.fetch('libvpx-src')) as t:
        (lic / 'libvpx-PATENTS.txt').write_bytes(t.extractfile('libvpx-1.17.0/PATENTS').read())
    head = (out / 'runtime' / 'deps' / 'include' / 'vma' / 'vk_mem_alloc.h').read_text(encoding='utf-8').split('*/')[0]
    (lic / 'vulkan-memory-allocator-LICENSE.txt').write_text(head.lstrip('/*') + '\n', encoding='utf-8')


def sys_version(out):
    return next((out / 'python' / 'lib').glob('python3.*')).name.removeprefix('python')


APPRUN = '''#!/bin/sh
# The AppImage entry: the setup window, run by the bundled Python from the read-only mount (no bytecode written there).
HERE="$(dirname "$(readlink -f "$0")")"
export PYTHONDONTWRITEBYTECODE=1
export TCL_LIBRARY="$HERE/python/lib/tcl9.0"
export TK_LIBRARY="$HERE/python/lib/tk9.0"
exec "$HERE/python/bin/python3" -m installer.gui "$@"
'''

DESKTOP = '''[Desktop Entry]
Type=Application
Name=TotK Recomp Setup
Comment=Builds Tears of the Kingdom for this PC from your own game files
Exec=AppRun
Icon=totk-setup
Categories=Game;
Terminal=false
'''


def build_entry(out):
    (out / 'AppRun').write_text(APPRUN, encoding='utf-8')
    (out / 'AppRun').chmod(0o755)
    (out / 'totk-setup.desktop').write_text(DESKTOP, encoding='utf-8')
    shutil.copy2(out / 'runtime' / 'totk.png', out / 'totk-setup.png')
    (out / '.DirIcon').symlink_to('totk-setup.png')


def write_appimage(out, path):
    tool = mr.fetch('appimagetool')
    tool.chmod(0o755)
    runtime = mr.fetch('appimage-runtime')
    path.parent.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, ARCH='x86_64', APPIMAGE_EXTRACT_AND_RUN='1')
    run([tool, '--no-appstream', '--runtime-file', runtime, out, path], env=env)
    print(f'{path}: {mr.mib(path.stat().st_size)}')


# ---- checks -------------------------------------------------------------------------------------------------------------

# The newest glibc any program or library in the folder may need: the official LLVM binaries and the cryptography
# wheel are built against 2.34 (Ubuntu 22.04, Debian 12, SteamOS 3 have it); the game itself links the sysroot's 2.31.
GLIBC_FLOOR = (2, 34)


def check_glibc(out):
    """Every ELF file in the folder (llvm/sysroot aside: glibc itself, read by the link) needs glibc GLIBC_FLOOR or
    older; prints which files set the floor."""
    newest, by = (0, 0), []
    for p in sorted(out.rglob('*')):
        if not p.is_file() or p.is_symlink() or out / 'llvm' / 'sysroot' in p.parents:
            continue
        with open(p, 'rb') as f:
            if f.read(4) != b'\x7fELF':
                continue
        text = subprocess.run(['readelf', '-V', str(p)], capture_output=True, text=True).stdout
        need = max((tuple(map(int, v.split('.'))) for v in re.findall(r'GLIBC_(\d+\.\d+(?:\.\d+)?)', text)),
                   default=(0, 0))
        if need > newest:
            newest, by = need, []
        if need == newest:
            by.append(str(p.relative_to(out)))
    floor = '.'.join(map(str, GLIBC_FLOOR))
    if newest > GLIBC_FLOOR:
        sys.exit(f'glibc check: {", ".join(by[:8])} need glibc {".".join(map(str, newest))}, above {floor}')
    print(f'glibc check: the folder needs glibc {".".join(map(str, newest))} (floor {floor}), set by '
          f'{", ".join(by[:6])}{" and others" if len(by) > 6 else ""}')


def check_bundle(out):
    """The folder's own python runs the installer without game files and the window's selftest."""
    import json
    chk = CHECK / 'bundle'
    shutil.rmtree(chk, ignore_errors=True)
    py = out / 'python' / 'bin' / 'python3'
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
    r = subprocess.run([str(py), '-m', 'installer', '--lang', 'en-US', '--dest', str(chk / 'install'), '--temp',
                        str(chk / 'temp'), '--root', str(out)], cwd='/', env=env, capture_output=True, text=True,
                       timeout=600)
    events = [json.loads(line) for line in r.stdout.splitlines() if line.startswith('{')]
    errors = [e['id'] for e in events if e['type'] == 'error']
    started = any(e['type'] == 'start' and e.get('release') == (out / 'VERSION').read_text().strip() for e in events)
    steps = [e['phase'] for e in events if e['type'] == 'progress']
    if errors != ['check.inputs_needed'] or not started or 'inputs' not in steps:
        sys.exit(f'bundle check: exit {r.returncode}, errors {errors}, release read {started}\n{r.stdout[-2000:]}\n'
                 f'{r.stderr[-2000:]}')
    print(f'bundle check: the installer ran the host check from the folder and stopped at the game files '
          f'(exit {r.returncode})')
    run([py, '-c', 'import tkinter as tk; from tkinter import font; '
         'root = tk.Tk(); root.withdraw(); '
         'fonts = [font.nametofont("TkDefaultFont"), font.Font(family="DejaVu Sans"), '
         'font.Font(family="DejaVu Sans Mono")]; '
         'missing = [(f.actual("family"), c) for f in fonts for c in "УстановкаЁёЙй" if f.measure(c) <= 0]; '
         'root.destroy(); assert not missing, f"Cyrillic glyphs missing: {missing}"'], cwd='/', env=env)
    r = subprocess.run([str(py), '-m', 'installer.gui', '--selftest', '--root', str(out)], cwd='/', env=env,
                       capture_output=True, text=True, timeout=300)
    if r.returncode:
        sys.exit(f'bundle check: window selftest exit {r.returncode}\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}')
    print('bundle check: the window built every screen in both languages and closed')


def prove(out):
    """Compile the generated C and link totk with the folder's tools only, then run the probes beside it."""
    out = out.resolve()
    version = mr.lifted_version()
    gen = MAIN_ROOT / 'generated' / version
    ext = MAIN_ROOT / 'build' / 'extracted' / version
    chk = CHECK / 'game'
    env = dict(os.environ, PATH=os.pathsep.join([str(out / 'llvm' / 'bin'), str(out / 'bin'), '/usr/bin', '/bin']))
    t0 = time.time()
    run([out / 'python' / 'bin' / 'python3', out / 'tools' / 'build_game.py', '--toolchain', out / 'llvm',
         '--sysroot', out / 'llvm' / 'sysroot', '--deps', out / 'runtime' / 'deps', '--runtime', out / 'runtime',
         '--runtime-lib', out / 'runtime' / 'libtotkrt.a', '--ninja', out / 'bin' / 'ninja', '--version', version,
         '--gen', gen, '--out', chk, '--exefs', ext / 'exefs', '--romfs', ext / 'romfs', '--profile', chk / 'profile'],
        env=env)
    print(f'prove: build {time.time() - t0:.0f} s')
    shutil.copy2(out / 'bin' / 'libSDL3.so.0', chk / 'libSDL3.so.0')
    needed = subprocess.run(['readelf', '-d', str(chk / 'totk')], capture_output=True, text=True).stdout
    print('prove: needs', ', '.join(re.findall(r'\[(lib[^\]]+)\]', needed)))
    glibc = sorted(set(re.findall(r'GLIBC_(\d+\.\d+)', subprocess.run(['readelf', '-V', str(chk / 'totk')],
                                                                         capture_output=True, text=True).stdout)),
                   key=lambda v: tuple(map(int, v.split('.'))))
    print(f'prove: newest glibc symbol version {glibc[-1] if glibc else "none"}')
    failed = []
    for probe in ['--selftest', *mr.PROBES]:
        r = subprocess.run([str(chk / 'totk'), probe], cwd=chk, capture_output=True, text=True, timeout=900)
        tail = (r.stdout.strip().splitlines() or [''])[-1]
        print(f'{probe}: exit {r.returncode}: {tail}')
        if r.returncode:
            failed.append(probe)
    if failed:
        sys.exit(f'probes failed: {failed}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', type=Path, required=True, help='release folder (replaced)')
    ap.add_argument('--appimage', type=Path, help='also pack the folder as this AppImage')
    ap.add_argument('--prove', action='store_true', help='only run the compile-and-probe check on an existing folder')
    args = ap.parse_args()
    out = args.out.resolve()
    if args.prove:
        prove(out)
        return
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    llvm = build_llvm(out)
    sysroot = full_sysroot()
    build_deps(out, llvm, sysroot)
    build_bin(out, llvm, sysroot)
    rtlib = runtime_lib(out, llvm, sysroot)
    build_sysroot(out, sysroot, used_sysroot_files(out, llvm, sysroot, rtlib))
    build_python(out, llvm, sysroot)
    mr.build_sources(out)
    collect_licenses(out, sysroot)
    build_entry(out)
    mr.check_folder(out)
    check_glibc(out)
    check_bundle(out)
    mr.sizes(out)
    if args.appimage:
        write_appimage(out, args.appimage.resolve())


if __name__ == '__main__':
    main()
