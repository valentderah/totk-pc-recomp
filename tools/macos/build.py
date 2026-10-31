"""Build the pinned ARM64/Metal runtime with the repository's macOS integration.

The reference checkout is read-only. Sources, patches, generated game tables and
objects live under build/macos-arm64; no build command runs inside refs/.
"""
from __future__ import annotations

import argparse
import filecmp
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import tomllib
import fcntl
from contextlib import contextmanager
from functools import cache
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from build_xlate import mng_pin
from check_content import GENERATED_MARKER

REF = ROOT / 'refs/MacNativeGames'
INTEGRATION = ROOT / 'runtime/macos'
BUILD = ROOT / 'build/macos-arm64'
VERSIONS = ('v121', 'v143')
RUNTIME_SOURCES = '''loader.c game_patches.c world_survey.c game_slot.c services.c romfs.c threads.c
gate.S hle_stack.S hle_stack.c thread_enter.S gfx_bridge.c frontend.m account.c input.c hid_feedback.c
motion_fusion.c autoload.c gamepad.m gamepad_feedback.m gfx_probe.c vpool_probe.m gpu_path_probe.m
memory_probe.c shared_font.c ipc.c audio_renderer.c audio_mixer.c audio_reverb.c audio_effects.c fs_host.c
opus_codec.c movie_host.c time_host.c applet_host.c swkbd_host.c'''.split()
PROBE_SOURCES = {'gfx_probe.c', 'vpool_probe.m', 'gpu_path_probe.m', 'memory_probe.c'}
FRAMEWORKS = '''Metal MetalFX Foundation QuartzCore ImageIO CoreGraphics CoreText UniformTypeIdentifiers
Cocoa CoreVideo CoreFoundation AudioToolbox GameController CoreHaptics IOKit'''.split()


def run(args, **kwargs):
    subprocess.run([str(x) for x in args], check=True, **kwargs)


def digest(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def write(path, text):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def prerequisites(need_metal_compiler=True):
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        raise RuntimeError('macos-arm64 requires an Apple Silicon Mac running natively')
    if int(platform.mac_ver()[0].split('.')[0]) < 26:
        raise RuntimeError('Metal 4 requires macOS 26 or later')
    for command in ('ninja', 'xcrun'):
        if not shutil.which(command):
            raise RuntimeError(f'{command} is required')
    run(['xcrun', '--find', 'clang'], stdout=subprocess.DEVNULL)
    if need_metal_compiler:
        run(['xcrun', '--find', 'metal'], stdout=subprocess.DEVNULL)


@contextmanager
def source_lock():
    BUILD.mkdir(parents=True, exist_ok=True)
    with (BUILD / 'source.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        yield


def stage_sources():
    with source_lock():
        return _stage_sources()


def preserve_source_times(tree, previous):
    """Keep unchanged inputs incremental; changed archive files must look new."""
    for new in tree.rglob('*'):
        if not new.is_file():
            continue
        old = previous / new.relative_to(tree)
        if old.is_file() and filecmp.cmp(new, old, shallow=False):
            stat = old.stat()
            os.utime(new, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        else:
            # A removed patch restores the upstream commit's OLD archive time.
            # Touch changed bytes so Ninja cannot reuse a newer, patched object.
            os.utime(new, None)


def _stage_sources():
    """Export committed inputs, ignoring local modifications in the reference checkout."""
    pin = mng_pin()
    if not REF.is_dir():
        raise RuntimeError('refs/MacNativeGames is missing; run tools/fetch_refs.sh')
    patches = sorted((INTEGRATION / 'patches').glob('*.patch'))
    identity = hashlib.sha256(pin.encode())
    for p in patches:
        identity.update(p.name.encode())
        identity.update(p.read_bytes())
    identity.update(b'macos-source-export-v2')
    source = BUILD / 'source'
    stamp = source / '.source-id'
    if stamp.is_file() and stamp.read_text() == identity.hexdigest():
        return source / 'games/totk'
    # Replace only the export; objects and the player's extracted data are elsewhere.
    with tempfile.TemporaryDirectory(dir=BUILD, prefix='source-') as temporary:
        temp = Path(temporary)
        archive = temp / 'source.tar'
        with archive.open('wb') as out:
            run(['git', '-C', REF, 'archive', pin, 'games/totk', 'LICENSE'], stdout=out)
        with tarfile.open(archive) as tar:
            tar.extractall(temp / 'tree', filter='data')
        tree = temp / 'tree'
        for patch in patches:
            run(['git', 'apply', '--unsafe-paths', '--directory', str(tree / 'games/totk'), patch], cwd=ROOT)
        # Preserve Ninja's dependencies when another patched file changes.
        # git archive gives unmodified sources stable mtimes; patched files need
        # this byte comparison because git apply otherwise touches all of them.
        preserve_source_times(tree, source)
        write(tree / '.source-id', identity.hexdigest())
        if source.exists():
            shutil.rmtree(source)
        (tree).rename(source)
    return source / 'games/totk'


def quote(value):
    return shlex.quote(str(value)).replace('$', '$$')


def ninja_path(value):
    return str(value).replace('$', '$$').replace(' ', '$ ').replace(':', '$:')


@cache
def library_prefix(name, archive):
    """Reject dependencies whose deployment target exceeds our macOS minimum."""
    preferred = Path(subprocess.check_output(['brew', '--prefix', name], text=True).strip())
    cellar = Path(subprocess.check_output(['brew', '--cellar', name], text=True).strip())
    candidates = [preferred, *sorted(cellar.glob('*'), reverse=True)]
    for prefix in candidates:
        lib = prefix / 'lib' / archive
        if not lib.is_file():
            continue
        info = subprocess.check_output(['otool', '-l', lib], text=True)
        versions = re.findall(r'\bminos\s+(\d+(?:\.\d+)*)', info)
        if versions and all(tuple((list(map(int, v.split('.'))) + [0, 0])[:3]) <= (26, 0, 0) for v in versions):
            run(['lipo', '-verify_arch', 'arm64', lib], stdout=subprocess.DEVNULL)
            return prefix
    raise RuntimeError(f'{name}: need an arm64 {archive} built with MACOSX_DEPLOYMENT_TARGET=26.0')


def probe_headers(out):
    """Explicit empty fixtures: a probe build refuses to enter the game."""
    marker = f'// {GENERATED_MARKER}\n'
    write(out / 'runtime_tables.h', marker + '''#define TOTK_PROBE_BUILD 1
static Module modules[1] = {{.name="probe-only"}};
#define PROBE_PLT 0ULL
#define PROBE_SLOT 0ULL
''')
    write(out / 'romfs_config.h', marker + '#define ROMFS_IMAGE_SIZE 0ULL\n'
          '#define ROMFS_IMAGE_SHA256 ""\n#define ROMFS_INDEX_SHA256 ""\n')
    write(out / 'shared_fonts_config.h', marker + '#define SHARED_FONT_COUNT 1\n'
          '#define SHARED_FONT_ROWS {"",0,0,""}\n')
    write(out / 'native_layout.h', marker + 'static const struct { const char *name; uint64_t base, size; } '
          'native_modules[] = {{"probe-only", 0, 0}};\n')
    write(out / 'native_prompt_textures.h', marker +
          'static const NativePromptTextureIdentity prompt_textures[] = {{0,0,0,0,{0}}};\n')


def compile_runtime(tree, out, version, jobs, probes):
    out.mkdir(parents=True, exist_ok=True)
    if probes:
        probe_headers(out)
    else:
        proof = out / 'preparation.json'
        if not proof.is_file():
            raise RuntimeError('Verified game tables are missing; run with --prepare-game first')
        prepared = json.loads(proof.read_text())
        required = {'runtime_tables.h','romfs_config.h','shared_fonts_config.h','native_layout.h','native_prompt_textures.h'}
        if (prepared.get('version') != version or
            prepared.get('source_id') != (tree.parent.parent / '.source-id').read_text() or
            prepared.get('layout_sha256') != digest(ROOT / 'recomp/layout.py') or
            set(prepared.get('headers', {})) != required or
            any(not (out / name).is_file() or digest(out / name) != sha for name, sha in prepared['headers'].items())):
            raise RuntimeError('Native preparation is stale or changed; run --prepare-game again')
    includes = [tree / 'runtime', tree / 'third_party/d2r_gfx/gfx/nvn/include', tree / 'third_party/d2r_gfx/gfx/nvn', out, INTEGRATION]
    libs = []
    for name, inc in [('opus', 'include/opus'), ('libvpx', 'include')]:
        archive = 'libopus.a' if name == 'opus' else 'libvpx.a'
        prefix = library_prefix(name, archive)
        includes.append(prefix / inc)
        libs.append(prefix / 'lib' / archive)
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    sdk_build = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-build-version'], text=True).strip()
    flags = ['-arch', 'arm64', '-isysroot', sdk, '-mmacosx-version-min=26.0', '-std=gnu11', '-O3', '-flto=thin', '-g',
             '-fno-strict-aliasing', '-Wall', '-Wextra', '-Wno-deprecated-declarations',
             '-Wno-unused-function', '-Wno-unused-parameter', '-fobjc-arc', f'-DTOTK_GAME_{version.upper()}=1']
    flags += [f'-I{p}' for p in includes]
    if probes:
        flags.append('-DTOTK_PROBE_BUILD=1')
    sources = [tree / 'runtime' / ('game_patches_v143.c' if s == 'game_patches.c' and version == 'v143' else s)
               for s in RUNTIME_SOURCES if probes or s not in PROBE_SOURCES]
    gfx = tree / 'third_party/d2r_gfx/gfx'
    sources += sorted(p for d in ('nvn', 'tex') for p in (gfx / d).iterdir() if p.suffix in ('.c', '.m', '.S'))
    sources += sorted(p for p in INTEGRATION.iterdir() if p.suffix in ('.c', '.m') and p.name != 'launcher.m')
    objects = [out / 'obj' / f'{p.name}.o' for p in sources]
    if len(set(objects)) != len(objects):
        raise RuntimeError('Native source basenames must be unique')
    compiler = subprocess.check_output(['xcrun', 'clang', '--version'], text=True).strip()
    compiler_stamp = out / 'compiler.id'
    write(compiler_stamp, compiler + '\n' + sdk + '\n' + sdk_build + '\n')
    lto_cache = out / 'lto-cache'
    lto_cache.mkdir(exist_ok=True)
    lines = ['ninja_required_version = 1.10',
             f'flags = {" ".join(quote(x) for x in flags)}',
             'rule cc', '  command = xcrun clang $flags -MMD -MF $out.d -c $in -o $out',
             '  depfile = $out.d', '  deps = gcc', '  description = CC $in', 'rule link',
             '  command = xcrun clang -arch arm64 -isysroot ' + quote(sdk) + ' -mmacosx-version-min=26.0 -flto=thin -Wl,-dead_strip ' +
             quote(f'-Wl,-cache_path_lto,{lto_cache}') + ' $in ' +
             ' '.join(quote(p) for p in libs) + ' ' +
             ' '.join(f'-framework {f}' for f in FRAMEWORKS) +
             ' -o $out && codesign --force --sign - $out', '  description = LINK $out']
    for src, obj in zip(sources, objects):
        obj.parent.mkdir(parents=True, exist_ok=True)
        lines.append(f'build {ninja_path(obj)}: cc {ninja_path(src)} | {ninja_path(compiler_stamp)}')
    for obsolete in (out / 'obj').glob('*.o'):
        if obsolete not in objects:
            obsolete.unlink()
    binary = out / 'totk-native'
    lines += [f'build {ninja_path(binary)}: link ' + ' '.join(ninja_path(p) for p in objects) +
              ' | ' + ' '.join(ninja_path(p) for p in libs),
              f'default {ninja_path(binary)}']
    write(out / 'build.ninja', '\n'.join(lines) + '\n')
    run(['ninja', '-f', out / 'build.ninja', '-j', jobs], cwd=out)
    inputs = {str(p.relative_to(ROOT)): digest(p) for folder in (INTEGRATION, ROOT / 'tools/macos')
              for p in sorted(folder.rglob('*')) if p.is_file() and '__pycache__' not in p.parts}
    inputs['recomp/layout.py'] = digest(ROOT / 'recomp/layout.py')
    write(out / 'build-info.json', json.dumps({'target': 'macos-arm64', 'version': version, 'inputs': inputs,
          'libraries': {str(p): digest(p) for p in libs},
          'compiler': compiler.splitlines()[0], 'sdk': sdk, 'sdk_build': sdk_build,
          'upstream': mng_pin(), 'source_id': (tree.parent.parent / '.source-id').read_text(),
          'probe_only': probes, 'binary_sha256': digest(binary)}, indent=2) + '\n')
    return binary


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--version', choices=VERSIONS, default='v143')
    ap.add_argument('--jobs', type=int, default=min(os.cpu_count() or 4, 16))
    ap.add_argument('--probes', action='store_true', help='compile without game files; boot is disabled')
    ap.add_argument('--prepare-game', action='store_true', help='verify and rewrite extracted game modules')
    ap.add_argument('--extract', action='store_true', help='extract config.local.toml inputs before preparing the game')
    ap.add_argument('--config', type=Path, default=ROOT / 'config.local.toml')
    ap.add_argument('--package', action='store_true', help='build a signed local .app')
    args = ap.parse_args(argv)
    if args.extract:
        args.prepare_game = True
    if args.jobs < 1 or (args.probes and args.prepare_game):
        ap.error('positive --jobs required; --probes and --prepare-game are mutually exclusive')
    prerequisites(need_metal_compiler=not args.probes)
    BUILD.mkdir(parents=True, exist_ok=True)
    extracted = ROOT / 'build/extracted'
    if args.extract:
        if sys.version_info < (3, 14):
            raise RuntimeError('Extraction requires Python 3.14 or later')
        if not args.config.is_file():
            raise RuntimeError('Create config.local.toml from config.macos.example.toml and set your local input paths')
        config = tomllib.loads(args.config.read_text())
        for key in ('base', 'keys', 'firmware'):
            value = config.get('input', {}).get(key)
            if not value or not Path(value).expanduser().exists():
                raise RuntimeError(f'config.local.toml: input.{key} must name an existing local path')
        if not config.get('input', {}).get('updates'):
            raise RuntimeError('Set input.updates to the supported cumulative update files')
        if any(not Path(p).expanduser().is_file() for p in config['input']['updates']):
            raise RuntimeError('config.local.toml: an input.updates file is missing')
        extracted = (ROOT / config.get('work', {}).get('dir', 'build') / 'extracted').resolve()
        if not extracted.is_relative_to(ROOT / 'build'):
            raise RuntimeError('Native extraction output must stay under build/')
        run([sys.executable, ROOT / 'tools/build_hactool.py', '--src', BUILD / 'hactool-src', '--out', BUILD / 'hactool'])
        run([sys.executable, ROOT / 'tools/extract.py', '--config', args.config, '--hactool', BUILD / 'hactool/hactool'])
        if config.get('input', {}).get('firmware'):
            run([sys.executable, ROOT / 'tools/extract_fonts.py', '--config', args.config])
    tree = stage_sources()
    out = BUILD / (f'probes-{args.version}' if args.probes else args.version)
    if args.prepare_game:
        from macos.prepare import prepare_game
        prepare_game(tree, out, args.version, args.jobs, extracted)
    binary = compile_runtime(tree, out, args.version, args.jobs, args.probes)
    if args.package:
        from macos.package import package
        print(package(tree, out, args.version, args.probes))
    print(binary)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from None
