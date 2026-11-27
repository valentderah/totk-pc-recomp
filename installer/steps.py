"""The twelve setup steps of docs/installer.md: check, kit, fonts, extract, lift, compile, prepare, pc_prompts,
shapes, dlss, fsr, cleanup.

Extraction runs in this process through tools/extract.py (the package's sibling tools/, which ships with it); fonts
runs the vendored extract_shared_fonts.py on the firmware folder; lift, compile and prepare run the bundled Python on
recomp/gen.py, tools/build_game.py, tools/make_romfs_patches.py and tools/prepare_shaders.py, whose output goes to
<temp>/logs/<step>-<tool>.log and is read for progress. No Win32 call here: the host check is a small C program built
with the bundled clang (hostcheck.c).

Install folder: exefs/, romfs/ (romfs.bin, index.json), profile/, shared-fonts/ (Chinese and Korean text), totk.exe
and its DLLs, nvngx_dlss.dll (with --dlss), amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll (with
--fsr), romfs-index.bin, romfs-patches/, romfs-patches-xbox/,
romfs-patches-xbox-label/, romfs-patches-pc/, romfs-patches-<w>x<h>/ and romfs-patches-<style>-<w>x<h>/ (for the
screen shapes and the prompts style this player needs, installer/screens.py), totk_xlate.exe, the
shader state of totk.exe --prepare-shaders, totk.ini, totk-install.json, and on Windows build-kit/ (Kit): the release
folder the steps after the kit step run from, and the game's rebuild with them.
Temp folder (build-cache/work in the install folder on Windows, .setup-temp there on Linux, <temp>/<dest name>-<hash>
with --temp or in a repository; installer/layout.py temp_for): check/ (ExeFS of both inputs, the host check),
unpacked/ (the NSP of an NSZ or XCZ input until the extraction is done), gen/ (C), build/ (objects), logs/. The cleanup keeps gen/, build/ and
logs/: the next run lifts into the same gen/, where recomp/gen.py lifts again only the chunks whose inputs changed (its
lift cache, gen/<mod>/lift_cache.json) and rewrites only those whose C changed, and Ninja in build/ recompiles only
those. A change of the mods' code cheats then costs a few seconds of lift and the chunks holding its sites, not a full
lift or compile.

Install folder build-cache/: cheats.status (tools/cheats.py), exefs.status and exefs.data (tools/exefs_patches.py),
plugins.status (tools/exefs_plugins.py; plugins.stale is the game's list of plugins whose start differed) and
rebuild.txt, the command that starts this setup in its rebuild mode on the install folder (installer/gui.py --rebuild),
one argument per line, relative to the install folder for the build kit; totk.exe runs it at start when an enabled mod
has code cheats the build lacks (runtime/cheats.c).
"""
import hashlib
import http.client
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

if os.name == 'nt':
    import winreg
else:
    winreg = None

from . import ini, screens
from . import layout as layouts
from .layout import EXE, SUPPORTED, version_tag
from .runner import InstallError, Step

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
import extract  # noqa: E402
import ncz  # noqa: E402
import shader_list  # noqa: E402
from extract_exefs import (common_tickets, container_entries, load_keys, nca_ctr_key, nca_inventory,  # noqa: E402
                           program_nca)

GiB = 1 << 30
DEST_EXTRA = 4 * GiB      # beside the RomFS: shader and pipeline caches (about 2 GB), exefs, totk.exe, seed files
TEMP_NEED = 3 * GiB       # generated C (about 0.9 GB) and objects
KIT_NEED = 1 * GiB        # the build kit (Kit), about 0.65 GB
HOSTCHECK = Path(__file__).resolve().parent / 'hostcheck.c'
STAMP = '.install-key'
PLUGIN_ENTRY_ROUNDS = 4   # plugin lifts at most: each finds where an init called into main past a function start


# ---- helpers

def file_sha256(path, progress=None):
    """SHA-256 hex of a file; progress(done, total) after each block."""
    h = hashlib.sha256()
    total = path.stat().st_size
    done = 0
    with open(path, 'rb', buffering=0) as f:
        buf = bytearray(8 << 20)
        view = memoryview(buf)
        while n := f.readinto(buf):
            h.update(view[:n])
            done += n
            if progress:
                progress(done, total)
    return h.hexdigest()


def fingerprint(*parts):
    """SHA-256 over strings and the contents of files and directory trees (a missing path counts as missing)."""
    h = hashlib.sha256()
    for p in parts:
        if isinstance(p, Path):
            if p.is_dir():
                for f in sorted(p.rglob('*')):
                    if f.is_file() and '__pycache__' not in f.parts:
                        h.update(f.relative_to(p).as_posix().encode() + b'\0' + bytes.fromhex(file_sha256(f)))
            elif p.is_file():
                h.update(p.name.encode() + b'\0' + bytes.fromhex(file_sha256(p)))
            else:
                h.update(b'missing\0' + p.name.encode())
        else:
            h.update(str(p).encode())
        h.update(b'\1')
    return h.hexdigest()


def remove(path):
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
    elif path.exists() or path.is_symlink():
        path.unlink()


def replace_file(src, dst):
    """Copy src over dst through a temporary name, so a cancelled copy never leaves half a file under dst's name."""
    tmp = dst.with_name(dst.name + '.tmp')
    shutil.copy2(src, tmp)
    os.replace(tmp, dst)


def kill_tree(proc):
    if proc.poll() is not None:
        return
    if os.name == 'nt':
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(proc.pid)], capture_output=True)
        return
    try:   # run_tool starts each tool in a session of its own: the group holds ninja, clang and the game
        os.killpg(proc.pid, signal.SIGKILL)
    except OSError:
        proc.kill()


def parse_host(text):
    """The host check's JSON object, or None when it did not finish (a crash leaves the parts it flushed)."""
    try:
        host = json.loads(text)
    except ValueError:
        return None
    return host if isinstance(host, dict) and 'devices' in host else None


def exit_code(code):
    """A process exit code as Windows reports crashes (0xC0000005 for an access violation), else as a number."""
    if os.name == 'nt' and code is not None and (code < 0 or code >= 0xC0000000):
        return f'0x{code & 0xFFFFFFFF:08X}'
    return str(code)


def layer_manifests():
    """Implicit Vulkan layer manifests where the Khronos loader looks for them (its docs, LoaderLayerInterface.md,
    "Layer Discovery"): the folders in VK_IMPLICIT_LAYER_PATH or VK_ADD_IMPLICIT_LAYER_PATH, then on Windows the
    manifests HKLM and HKCU\\SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers enable with a DWORD 0, read in the 64-bit view
    the 64-bit probe and game use; elsewhere the implicit_layer.d folders. Layers a driver registers under its display
    adapter's key are not listed."""
    found = []
    for var in ('VK_IMPLICIT_LAYER_PATH', 'VK_ADD_IMPLICIT_LAYER_PATH'):
        for part in filter(None, os.environ.get(var, '').split(os.pathsep)):
            p = Path(part)
            found += sorted(p.glob('*.json')) if p.is_dir() else [p]
    if winreg:
        for root in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
            try:
                key = winreg.OpenKey(root, r'SOFTWARE\Khronos\Vulkan\ImplicitLayers', 0,
                                     winreg.KEY_READ | winreg.KEY_WOW64_64KEY)
            except OSError:
                continue
            with key:
                i = 0
                while True:
                    try:
                        path, enabled, _ = winreg.EnumValue(key, i)
                    except OSError:
                        break
                    i += 1
                    if enabled == 0:   # a DWORD 0 enables the manifest, anything else skips it
                        found.append(Path(path))
    else:
        home = Path.home()
        config = [os.environ.get('XDG_CONFIG_HOME') or home / '.config',
                  *(os.environ.get('XDG_CONFIG_DIRS') or '/etc/xdg').split(':'), '/usr/local/etc', '/etc']
        data = [os.environ.get('XDG_DATA_HOME') or home / '.local' / 'share',
                *(os.environ.get('XDG_DATA_DIRS') or '/usr/local/share:/usr/share').split(':')]
        for base in filter(None, config + data):
            found += sorted((Path(base) / 'vulkan' / 'implicit_layer.d').glob('*.json'))
    return found


def implicit_vulkan_layers():
    """(layer name, manifest path) of each readable implicit Vulkan layer manifest of layer_manifests(), for the
    search for the layer that crashes the host check. A manifest the loader cannot read loads no layer and is left
    out; a manifest listed twice counts once."""
    layers = []
    for path in dict.fromkeys(layer_manifests()):
        try:
            manifest = json.loads(path.read_text(encoding='utf-8-sig'))
        except (OSError, ValueError):
            continue
        entries = (manifest.get('layers') or [manifest.get('layer')]) if isinstance(manifest, dict) else []
        for entry in entries:
            name = entry.get('name') if isinstance(entry, dict) else None
            if isinstance(name, str) and (name, path) not in layers:
                layers.append((name, path))
    return layers


def linux_glibc():
    """(major, minor) of the C library this Linux process runs on, None elsewhere or when it is not glibc."""
    try:
        name = os.confstr('CS_GNU_LIBC_VERSION') if sys.platform.startswith('linux') else None
    except (ValueError, OSError):
        return None
    m = re.match(r'glibc (\d+)\.(\d+)', name or '')
    return (int(m.group(1)), int(m.group(2))) if m else None


# The bundled clang, lld and the cryptography wheel are built against glibc 2.34 (tools/make_release_linux.py
# GLIBC_FLOOR); on an older one they cannot start
GLIBC_NEEDED = (2, 34)

# The libraries the release puts next to the game (libSDL3.so.0 from the Steam Runtime sysroot) link system libraries
# that a minimal system may lack: libXss.so.1 on AnataseOS (#1315, #1428), libdecor-0.so.0 on Batocera (#1419). The
# loader then stops the game before it runs, after the half hour of lift and compile, as `totk --prepare-shaders`
# with exit code 127. The package that provides a common one, by distribution family.
LIBRARY_PACKAGES = {
    'libXss.so.1': ('libXScrnSaver', 'libxss1', 'libxss'),
    'libdecor-0.so.0': ('libdecor', 'libdecor-0-0', 'libdecor'),
    'libXi.so.6': ('libXi', 'libxi6', 'libxi'),
    'libXrandr.so.2': ('libXrandr', 'libxrandr2', 'libxrandr'),
    'libXcursor.so.1': ('libXcursor', 'libxcursor1', 'libxcursor'),
    'libXinerama.so.1': ('libXinerama', 'libxinerama1', 'libxinerama'),
    'libXext.so.6': ('libXext', 'libxext6', 'libxext'),
    'libXfixes.so.3': ('libXfixes', 'libxfixes3', 'libxfixes'),
    'libX11.so.6': ('libX11', 'libx11-6', 'libx11'),
    'libxkbcommon.so.0': ('libxkbcommon', 'libxkbcommon0', 'libxkbcommon'),
    'libwayland-client.so.0': ('libwayland-client', 'libwayland-client0', 'wayland'),
    'libasound.so.2': ('alsa-lib', 'libasound2', 'alsa-lib'),
}
LDD_NOT_FOUND = re.compile(r'^\s*(\S+) => not found', re.M)
LOADER_NOT_FOUND = re.compile(r'error while loading shared libraries: (\S+?): cannot open shared object file')


def library_packages(libraries):
    """The packages that provide the known ones of libraries, as " (Fedora: ..., Debian/Ubuntu: ..., Arch: ...)" for
    the message; "" when none is known."""
    known = [LIBRARY_PACKAGES[n] for n in libraries if n in LIBRARY_PACKAGES]
    if not known:
        return ''
    return ' (' + ', '.join(f'{family}: {" ".join(p[i] for p in known)}'
                            for i, family in enumerate(('Fedora', 'Debian/Ubuntu', 'Arch'))) + ')'


def loader_missing(log):
    """The libraries the system's loader could not find in a tool's log ("error while loading shared libraries"), in
    order, each once."""
    log = Path(log)
    text = log.read_text(encoding='utf-8', errors='replace') if log.is_file() else ''
    return list(dict.fromkeys(LOADER_NOT_FOUND.findall(text)))


def missing_libraries(paths):
    """Linux: the shared libraries `ldd` reports as not found for paths, in order, each once; [] elsewhere, when ldd
    cannot run, or when nothing is missing."""
    if not sys.platform.startswith('linux'):
        return []
    found = []
    for p in paths:
        try:
            r = subprocess.run(['ldd', str(p)], capture_output=True, text=True, errors='replace', timeout=60)
        except (OSError, subprocess.SubprocessError):
            return []
        found += [n for n in LDD_NOT_FOUND.findall(r.stdout) if n not in found]
    return found


def linux_kernel():
    """(major, minor) of the running Linux kernel, None elsewhere."""
    if not sys.platform.startswith('linux'):
        return None
    m = re.match(r'(\d+)\.(\d+)', os.uname().release)
    return (int(m.group(1)), int(m.group(2))) if m else None


def link_output_locked(log):
    """Whether a build failed because another program holds the exe the linker writes: ld.lld cannot write it and clang
    cannot remove it (Permission denied). A game started from the temp build folder does that, and so does an
    antivirus that flagged or quarantined the new totk.exe there (#1122)."""
    try:
        text = Path(log).read_text(encoding='utf-8', errors='replace')
    except OSError:
        return False
    return 'failed to write output' in text and 'Permission denied' in text


def run_tool(ctx, step, script, args, parse=None, cwd=None):
    """Run a tool script with the bundled Python. Its output goes to <temp>/logs/<step>-<script>.log and, line by line,
    to parse(line). Returns (exit code, log path)."""
    logs = ctx.temp / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    log = logs / f'{step}-{Path(script).stem}.log'
    command = [str(ctx.layout.python), '-c', extract.RUN_SCRIPT, str(script), *map(str, args)]
    ctx.reporter.log('run ' + ' '.join(command[3:]))
    env = dict(os.environ, PYTHONUNBUFFERED='1', PYTHONIOENCODING='utf-8')
    with open(log, 'w', encoding='utf-8', newline='\n') as out:
        proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=cwd or ctx.layout.root,
                                env=env, text=True, encoding='utf-8', errors='replace', bufsize=1,
                                start_new_session=os.name != 'nt')
        try:
            for line in proc.stdout:
                out.write(line)
                if parse:
                    parse(line.rstrip())
            code = proc.wait()
        except BaseException:
            kill_tree(proc)
            raise
    ctx.reporter.log(f'{Path(script).name}: exit code {code}; output in {log}')
    return code, log


def free_bytes(path):
    """Free bytes on the disk of path (its nearest existing parent) and that parent's device."""
    p = Path(path)
    # An unavailable drive or share has no existing ancestor: let disk_usage report its OS error at the root.
    while not p.exists() and p.parent != p:
        p = p.parent
    return shutil.disk_usage(p).free, os.stat(p).st_dev


def raise_if_damaged(error, name, path):
    """InstallError check.input_damaged when a reader's ValueError says the file's data fails its hashes or ends early
    (extract.DAMAGE_TEXT)."""
    if extract.DAMAGE_TEXT.search(str(error)):
        raise InstallError('check.input_damaged', input=name, path=str(path), detail=str(error)) from None


def game_file(ctx, name):
    """The file the extraction reads for the base or the update: the NSP that Check.unpack made from an NSZ or XCZ,
    else the chosen file itself."""
    return getattr(ctx, 'unpacked', {}).get(name) or Path(getattr(ctx.options, name))


def game_version(ctx):
    """The update this install is built from: what the check identified, else the recorded one. An install made before
    1.4.3 support records no version, and that install is 1.2.1: name it here rather than leaning on SUPPORTED's order."""
    return ctx.state.get('version', '1.2.1')


def expected_update(ctx):
    return extract.load_expected()[game_version(ctx)]


# Releases before 0.3.1 recorded the 0x20000 update as 1.1.1 (pull request #220); it is 1.1.0 (#371), with the same
# files, hashes and patch sites under new names.
RENAMED_VERSIONS = {'1.1.1': '1.1.0'}


def migrate_state(ctx):
    """Read a recorded update by its current name. The extraction it recorded stays: its record gets the key of the
    renamed expected file, and Extract.current and the full check of output another release kept
    (runner.Context.verify_kept) still test the files against the expected hashes."""
    old = ctx.state.get('version')
    if old not in RENAMED_VERSIONS:
        return
    ctx.state['version'] = RENAMED_VERSIONS[old]
    record = ctx.state['steps'].get('extract')
    if record:
        record['key'] = Extract().key(ctx)
    ctx.reporter.log(f'state: the update recorded as {old} is {ctx.state["version"]}')


def shader_list_text(layout):
    if layout.shader_list:
        return layout.shader_list.read_text(encoding='utf-8')
    try:
        return shader_list.history(layout.root / 'refs' / 'MacNativeGames')
    except subprocess.CalledProcessError:
        raise InstallError('prepare.shader_list_missing')


class Watch(threading.Thread):
    """Polls a measure() of bytes written while a long call runs and reports it as progress."""

    def __init__(self, reporter, step):
        super().__init__(daemon=True)
        self.reporter, self.step = reporter, step
        self.phase, self.measure, self.total = None, None, None
        self.stopped = threading.Event()

    def set(self, phase, measure=None, total=None):
        self.phase, self.measure, self.total = phase, measure, total
        self.reporter.progress(self.step, phase, 0 if total else None, total, 'bytes' if total else None, force=True)

    def run(self):
        while not self.stopped.wait(1.0):
            phase, measure, total = self.phase, self.measure, self.total
            if phase and measure:
                try:
                    done = min(measure(), total)
                except OSError:
                    continue
                self.reporter.progress(self.step, phase, done, total, 'bytes')

    def stop(self):
        self.stopped.set()


def size_of(paths):
    return sum(p.stat().st_size for p in paths if p.is_file())


# The Switch system shared fonts (runtime/ov_system.c): TotK falls back to them for Chinese, Korean and Traditional
# Chinese text. tools/vendor/macnativegames/extract_shared_fonts.py writes these names.
SHARED_FONTS = 'shared-fonts'
SHARED_FONT_FILES = ('0-standard.bfttf', '1-chinese-simplified.bfttf', '2-chinese-simplified-extension.bfttf',
                     '3-chinese-traditional.bfttf', '4-korean.bfttf', '5-nintendo-extension.bfttf')
FONTS_NEED = 32 << 20     # the six BFTTF files and their manifest


def requested_lang(ctx):
    """The language this run uses: --lang, else the one recorded for the install (Check canonicalizes it)."""
    return ini.canonical_language(getattr(ctx.options, 'lang', None) or ctx.state.get('lang'))


def firmware_dir(value):
    """The firmware NCA folder behind a --firmware path: the folder itself, or its registered/ child (the emulators'
    layout, Contents/registered)."""
    if not value:
        return None
    path = Path(value)
    return path / 'registered' if (path / 'registered').is_dir() else path


def firmware_has_ncas(folder):
    """Whether folder holds firmware NCAs: .nca files, or the hashed directories with a 00 file (registered/)."""
    try:
        entries = list(Path(folder).iterdir())
    except OSError:
        return False
    return any((e.is_file() and e.suffix.lower() == '.nca') or (e.is_dir() and (e / '00').is_file()) for e in entries)


def firmware_stamp(folder):
    """A cheap identity of the firmware NCAs: names, sizes and mtimes, not their contents (the folder is ~326 MB)."""
    try:
        entries = sorted(Path(folder).iterdir())
    except OSError:
        return 'missing'
    rows = []
    for entry in entries:
        if entry.is_dir():
            file = entry / '00'
            if not file.is_file():
                continue
        elif not entry.is_file() or entry.suffix.lower() != '.nca':
            continue
        else:
            file = entry
        st = file.stat()
        rows.append((entry.name, st.st_size, st.st_mtime_ns))
    return rows


def shared_fonts_ready(dest):
    """Whether dest/shared-fonts holds a complete set that the game can load."""
    folder = Path(dest) / SHARED_FONTS
    return (folder / 'manifest.json').is_file() and all((folder / name).is_file() for name in SHARED_FONT_FILES)


# ---- 1. check

UPDATE_TITLE_ID = '0100F2C0115B6800'


def base_misuse(records):
    """Name why a file chosen as the base game holds an update or a combined package, or return None.

    The Meta NCA (content type 1) carries the package's own title id: the update's id (...800) for an update NSP, and
    several Meta NCAs mean base, update and DLC repacked into one file.
    """
    metadata = [r['title_id'] for r, _ in records if r['content_type'] == 1]
    if len(metadata) > 1:
        return 'check.base_combined'
    if UPDATE_TITLE_ID in metadata:
        return 'check.base_is_update'
    return None


class Check(Step):
    id = 'check'
    always = True

    def key(self, ctx):
        # The compile step fingerprints the CPU level, and every key is computed before a step runs: probe the host
        # here, once, so both steps see the same answer (the check step itself is always run). A failed probe is
        # reported by run(), where the error names this step.
        try:
            self.host(ctx)
        except InstallError:
            pass
        return ''

    def run(self, ctx):
        layout, rep, dest = ctx.layout, ctx.reporter, ctx.dest
        missing = layout.missing(getattr(ctx, 'cpu', 'v3'))
        if missing:
            raise InstallError('check.release_incomplete', paths=missing[:5])
        if dest.exists() and not dest.is_dir():
            raise InstallError('check.dest_not_folder', path=str(dest))
        own = (ctx.temp, *ctx.temp.parents)   # a temp folder inside the install folder (installer/cli.py) is ours
        if ctx.mode == 'install' and dest.is_dir() and any(p not in own for p in dest.iterdir()):
            raise InstallError('check.dest_not_empty', path=str(dest))
        lang = requested_lang(ctx)
        if not lang:
            raise InstallError('check.lang_missing')
        if lang not in ini.LANGUAGES:
            raise InstallError('check.lang_unknown', lang=lang, known=', '.join(ini.LANGUAGES))
        firmware = firmware_dir(getattr(ctx.options, 'firmware', None))
        if firmware is not None:
            if not firmware.is_dir():
                raise InstallError('check.firmware_missing', path=str(firmware))
            if not firmware_has_ncas(firmware):
                raise InstallError('check.firmware_empty', path=str(firmware))
        if lang in ini.NEED_SHARED_FONTS:
            if firmware is None:
                rep.warning('warn.lang_needs_fonts', lang=lang)
            elif not shared_fonts_ready(dest):
                keys = getattr(ctx.options, 'keys', None)
                if not keys or not Path(keys).is_file():
                    raise InstallError('check.fonts_keys_needed')
                try:
                    key_values = load_keys(Path(keys))
                except (ValueError, OSError, UnicodeDecodeError):
                    raise InstallError('check.keys_unreadable') from None
                if 'header_key' not in key_values:
                    raise InstallError('check.keys_no_header_key')
        if (sys.platform == 'linux' and ctx.will_run('prepare')
                and not (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY'))):
            # totk --prepare-shaders opens a window and needs a present-capable queue family; without a display it
            # fails after lift and compile (~15 min), so stop here
            raise InstallError('check.no_display')
        ctx.lang = lang
        ctx.temp.mkdir(parents=True, exist_ok=True)

        rep.progress('check', 'host', force=True)
        self.host(ctx)
        rep.progress('check', 'verify', force=True)
        ctx.verify_kept('check')
        ncas = 0
        if ctx.will_run('extract'):
            rep.progress('check', 'inputs', force=True)
            ncas = self.inputs(ctx)
        self.space(ctx, ncas)
        if ctx.will_run('extract'):
            rep.progress('check', 'versions', force=True)
            self.versions(ctx)
        return {}

    def host(self, ctx):
        """Builds and runs the host check once per run: key() asks first and run() reports, so a failure is kept on
        ctx and raised again rather than a crashing probe and its retries running twice."""
        if getattr(ctx, 'host', None) is not None:
            return
        if getattr(ctx, 'host_error', None) is not None:
            raise ctx.host_error
        try:
            self.check_host(ctx)
        except InstallError as e:
            ctx.host_error = e
            raise

    def check_host(self, ctx):
        layout, work = ctx.layout, ctx.temp / 'check'
        glibc = linux_glibc()
        if glibc and glibc < GLIBC_NEEDED and not layout.dev:
            raise InstallError('check.glibc_old', version='.'.join(map(str, glibc)),
                               need='.'.join(map(str, GLIBC_NEEDED)))
        work.mkdir(parents=True, exist_ok=True)
        exe = work / ('hostcheck' + EXE)
        command = [str(layout.clang), '-O1', str(HOSTCHECK), '-o', str(exe)]
        if layout.deps:
            command += ['-I', str(layout.deps / 'include')]
        if layout.sysroot:
            command += [f'--sysroot={layout.sysroot}', '-fuse-ld=lld']
        if os.name != 'nt':
            command.append('-ldl')
        r = subprocess.run(command, capture_output=True, text=True, errors='replace')
        if r.returncode:
            ctx.reporter.log(r.stdout + r.stderr)
            raise InstallError('check.toolchain_broken', detail=(r.stderr or r.stdout).strip()[-400:])
        host = self.probe_host(ctx, exe)
        if not host['x86_64_v2']:
            raise InstallError('check.cpu_too_old')
        # Before lift and compile: the game would not start without these (LIBRARY_PACKAGES); a repository build links
        # the system's own libraries
        missing = [] if layout.dev else missing_libraries(layout.dlls)
        if missing:
            raise InstallError('check.libraries_missing', libraries=', '.join(missing),
                               packages=library_packages(missing))
        # v3 is the faster build; v2 runs on CPUs without AVX2/FMA (runtime/fma.c supplies the fused multiply-add).
        ctx.cpu = 'v3' if host['x86_64_v3'] else 'v2'
        if not host['loader']:
            raise InstallError('check.vulkan_missing')
        devices = host['devices']
        if not devices:
            raise InstallError('check.vulkan_no_device')
        if not any(tuple(d['api'][:2]) >= (1, 3) for d in devices):
            raise InstallError('check.vulkan_too_old',
                               devices=', '.join(f'{d["name"]} ({d["api"][0]}.{d["api"][1]})' for d in devices))
        kernel = linux_kernel()
        if kernel and kernel < (6, 7):
            # Without userfaultfd's asynchronous write protection nothing tracks the CPU's writes: the game copies whole
            # pools to the GPU at each submit and its texture checks see every page as changed (docs/building.md, Linux)
            ctx.reporter.warning('warn.kernel_old', version='.'.join(map(str, kernel)))
        ctx.host = host

    # Windows refuses to start a program a policy does not trust (issue #875): Smart App Control and other App Control
    # policies (4551 "An Application Control policy has blocked this file"; 4556-4559 and 4580-4583 the same with the
    # reputation verdict, among them 4559 "Unable to contact reputation service for unknown file"), group policy (1260
    # "This program is blocked by group policy") and an antivirus that flags or removes the file (225, 226). hostcheck
    # is the first program the setup builds on this PC; totk.exe is built the same way and unsigned too, so the install
    # cannot go on.
    BLOCKED_WINERRORS = {225, 226, 1260, 4551, 4556, 4557, 4558, 4559, 4580, 4581, 4582, 4583}
    LAYER_TRIES = 16   # implicit layers tried one by one in the search for the one that crashes the probe

    def probe_host(self, ctx, exe):
        """Runs the host check check_host() built and returns its JSON. A probe that crashes inside Vulkan is run again
        with the implicit Vulkan layers off (VK_LOADER_LAYERS_DISABLE=~implicit~, Vulkan loader 1.3.234 or later):
        overlays, recorders and tuning tools install such layers, and a broken one kills every Vulkan program at
        vkCreateInstance (issue #1345, exit code 0xC0000005 after "Checking the processor and the graphics card").
        When that run passes, each listed layer is turned off alone to name the one that crashes. The install stops
        either way: totk.exe --prepare-shaders in the prepare step and the game itself load the same layers
        (runtime/vk_device.c create_instance), so going on would fail after lift and compile."""
        rep = ctx.reporter
        r = self.run_probe(exe)
        host = parse_host(r.stdout)
        if host is not None:
            rep.log(f'host: {r.stdout.strip()}')
            return host
        code = exit_code(r.returncode)
        rep.log(f'host: the check stopped with exit code {code}; it printed {r.stdout.strip()!r}')
        if '"loader": true' not in r.stdout:
            raise InstallError('check.host_probe_failed', code=code)
        retry = self.run_probe(exe, dict(os.environ, VK_LOADER_LAYERS_DISABLE='~implicit~'))
        if parse_host(retry.stdout) is None:
            rep.log(f'host: without implicit Vulkan layers too: exit code {exit_code(retry.returncode)}, '
                    f'{retry.stdout.strip()!r}')
            raise InstallError('check.vulkan_crash', code=code)
        layers = implicit_vulkan_layers()
        rep.log(f'host: passes with the implicit Vulkan layers off; installed: '
                f'{", ".join(f"{n} ({p})" for n, p in layers) or "none listed"}')
        culprits = [(name, path) for name, path in layers[:self.LAYER_TRIES]
                    if parse_host(self.run_probe(exe, dict(os.environ, VK_LOADER_LAYERS_DISABLE=name)).stdout)]
        if culprits:
            rep.log(f'host: passes without {", ".join(n for n, _ in culprits)}')
            raise InstallError('check.vulkan_layer_crash', code=code,
                               layers=', '.join(f'{n} ({p})' for n, p in culprits),
                               disable=','.join(n for n, _ in culprits))
        raise InstallError('check.vulkan_layers_crash', code=code,
                           layers=', '.join(f'{n} ({p})' for n, p in layers) or '?')

    def run_probe(self, exe, env=None):
        try:
            return subprocess.run([str(exe)], capture_output=True, text=True, errors='replace', env=env)
        except OSError as e:
            if getattr(e, 'winerror', None) in self.BLOCKED_WINERRORS:
                raise InstallError('check.host_blocked', path=str(exe), detail=e.strerror or str(e)) from None
            raise InstallError('check.host_probe_failed',
                               code=f'{getattr(e, "winerror", None) or e.errno}: {e.strerror or e}') from None

    def inputs(self, ctx):
        """Open both files with the keys as the extraction will. Returns the bytes of both Program NCAs."""
        o = ctx.options
        for name in ('base', 'update', 'keys'):
            path = getattr(o, name)
            if not path:
                raise InstallError('check.inputs_needed')
            if not Path(path).is_file():
                raise InstallError('check.input_missing', input=name, path=str(path))
        try:
            keys = load_keys(Path(o.keys))
        except (ValueError, OSError, UnicodeDecodeError):
            raise InstallError('check.keys_unreadable') from None
        if 'header_key' not in keys:
            raise InstallError('check.keys_no_header_key')
        self.unpack(ctx)
        total = 0
        for name, is_update in (('base', False), ('update', True)):
            path = game_file(ctx, name)
            with path.open('rb') as f:
                total += self.open_input(f, path, keys, name, is_update)
        del keys
        return total

    def unpack(self, ctx):
        """Unpack an NSZ or XCZ (a game file compressed by the nsz tool) into <temp>/unpacked/<base|update>.nsp with
        tools/ncz.py, which checks every NCA against its hash; the extraction then reads that NSP. An earlier run's NSP
        of the same file is reused. The extract step removes the folder once it is done."""
        o, rep, folder = ctx.options, ctx.reporter, ctx.temp / 'unpacked'
        ctx.unpacked = {}
        jobs = []
        for name in ('base', 'update'):
            path = Path(getattr(o, name))
            try:
                found = ncz.compressed(path)
                if found is None:
                    continue
                if name == 'update' and found[0] != 'nsz':
                    raise InstallError('check.update_not_nsp')
                size = ncz.unpacked_size(path)
            except ValueError as e:
                raise_if_damaged(e, name, path)
                raise InstallError('check.not_game_file', input=name) from None
            st = path.stat()
            jobs.append((name, path, size, f'{path.resolve()}\n{st.st_size}\n{st.st_mtime_ns}\n{size}\n'))
        if not jobs:
            remove(folder)
            return
        done = []
        for name, path, size, tag in jobs:
            target, stamp = folder / f'{name}.nsp', folder / f'{name}.source'
            try:
                if target.stat().st_size == size and stamp.read_text(encoding='utf-8') == tag:
                    done.append(name)
                    ctx.unpacked[name] = target
            except OSError:
                pass
        need = sum(size for name, _, size, _ in jobs if name not in done)
        free, _ = free_bytes(folder)
        rep.log(f'unpack: {", ".join(name for name, *_ in jobs)}: {need / GiB:.1f} GiB to write, '
                f'{free / GiB:.1f} free')
        if need > free:
            raise InstallError('check.disk_space', path=str(folder), need_gb=round(need / 1e9, 1),
                               free_gb=round(free / 1e9, 1))
        for name, path, size, tag in jobs:
            if name in done:
                continue
            target, stamp = folder / f'{name}.nsp', folder / f'{name}.source'
            remove(stamp)
            try:
                folder.mkdir(parents=True, exist_ok=True)
                ncz.unpack(path, target, lambda d, t, n=name: rep.progress('check', 'unpack_' + n, d, t, 'bytes'))
                stamp.write_text(tag, encoding='utf-8')
            except ncz.DamagedFile as e:
                raise InstallError('check.input_damaged', input=name, path=str(e.path), detail=e.detail) from None
            except ValueError as e:
                raise_if_damaged(e, name, path)
                raise InstallError('check.unpack_failed', input=name, detail=str(e)) from None
            except OSError as e:
                raise InstallError('check.unpack_failed', input=name, detail=str(e)) from None
            ctx.unpacked[name] = target

    @staticmethod
    def open_input(f, path, keys, name, is_update):
        try:
            kind, entries = container_entries(f, path.stat().st_size)
        except ValueError as e:
            raise_if_damaged(e, name, path)
            raise InstallError('check.not_game_file', input=name) from None
        if is_update and kind != 'nsp':
            raise InstallError('check.update_not_nsp')
        try:
            tickets = common_tickets(f, entries)
        except ValueError as e:
            if 'Personalized' in str(e):
                raise InstallError('check.ticket_personalized', input=name) from None
            raise_if_damaged(e, name, path)
            raise InstallError('check.file_unreadable', input=name, detail=str(e)) from None
        try:
            records = nca_inventory(f, entries, keys)
        except ValueError as e:
            if 'NCA3' in str(e):
                raise InstallError('check.keys_wrong', input=name) from None
            raise_if_damaged(e, name, path)
            raise InstallError('check.file_unreadable', input=name, detail=str(e)) from None
        try:
            record, header = program_nca(records, update=is_update)
        except ValueError as e:
            if is_update and 'BKTR' in str(e):
                raise InstallError('check.update_is_base') from None
            if not is_update:
                reason = base_misuse(records)
                if reason:
                    raise InstallError(reason) from None
            raise InstallError('check.update_not_totk' if is_update else 'check.base_not_totk', detail=str(e)) from None
        try:
            nca_ctr_key(header, keys, tickets)
        except ValueError as e:
            if 'ticket' in str(e):
                raise InstallError('check.ticket_missing', input=name) from None
            raise InstallError('check.keys_too_old', input=name,
                               generation=max(header[0x206], header[0x220])) from None
        return record['size']

    def space(self, ctx, ncas):
        dest_need = temp_need = 0
        if ctx.will_run('extract'):
            # the update is identified after this estimate: the larger RomFS of the supported ones
            dest_need += max(extract.load_expected()[v]['romfs']['size'] for v in SUPPORTED) + ncas
            romfs = ctx.dest / 'romfs' / 'romfs.bin'
            if romfs.is_file():   # a resumed extraction replaces it
                dest_need -= romfs.stat().st_size
        if ctx.will_run('fonts'):
            dest_need += FONTS_NEED
        if ctx.will_run('kit') and layouts.uses_kit(ctx.layout):
            dest_need += KIT_NEED
        if ctx.will_run('prepare') or ctx.will_run('extract'):
            dest_need += DEST_EXTRA
        if ctx.will_run('lift') or ctx.will_run('compile'):
            temp_need += TEMP_NEED
        dest_free, dest_dev = free_bytes(ctx.dest)
        temp_free, temp_dev = free_bytes(ctx.temp)
        ctx.reporter.log(f'space: install folder needs {dest_need / GiB:.1f} GiB of {dest_free / GiB:.1f} free, '
                         f'temp folder {temp_need / GiB:.1f} GiB of {temp_free / GiB:.1f}')
        if dest_dev == temp_dev:
            dest_need += temp_need
            temp_need = 0
        ctx.reporter.emit('space', dest=str(ctx.dest), need=dest_need, free=dest_free, temp=str(ctx.temp),
                          temp_need=temp_need, temp_free=temp_free, same_disk=dest_dev == temp_dev)
        for path, need, free in ((ctx.dest, dest_need, dest_free), (ctx.temp, temp_need, temp_free)):
            if need > free:
                raise InstallError('check.disk_space', path=str(path), need_gb=round(need / 1e9, 1),
                                   free_gb=round(free / 1e9, 1))

    def versions(self, ctx):
        """Extract both ExeFS into the temp folder (verifying every block) and identify the versions by hash."""
        o, work = ctx.options, ctx.temp / 'check'
        logs = ctx.temp / 'logs'
        logs.mkdir(parents=True, exist_ok=True)
        versions = extract.load_expected()
        for name, is_update in (('base', False), ('update', True)):
            out = work / name
            remove(out)
            try:
                extract.extract_exefs(game_file(ctx, name), Path(o.keys), out, logs, update=is_update)
            except extract.DamagedFile as e:
                raise InstallError('check.input_damaged', input=name, path=str(getattr(o, name)),
                                   detail=e.detail) from None
            except ValueError:
                raise InstallError('check.exefs_failed', input=name, log=str(logs / f'extract_exefs-{name}.log')) \
                    from None
        problems = extract.exefs_problems(work / 'base' / 'exefs', versions['1.0.0'])
        if problems:
            raise InstallError('check.base_wrong_version', modules=', '.join(problems))
        try:
            found = extract.identify_update(work / 'update' / 'exefs', versions)['display_version']
        except ValueError:
            found = 'unknown'
        if found not in SUPPORTED:
            raise InstallError('check.update_wrong_version', found=found, wanted=' / '.join(SUPPORTED))
        if found != game_version(ctx):   # another update than the recorded one: every step builds again from it
            ctx.reporter.log(f'check: update {found} (recorded {game_version(ctx)})')
            ctx.state['version'] = found
            ctx.keys = {}
            ctx.plan(ctx.forced | {'extract', 'lift', 'compile', 'prepare'})
        ctx.state['version'] = found


# ---- 1b. build kit

class Kit(Step):
    """The build kit (installer/layout.py, Windows release): the release folder without TotK-Setup.exe, copied into
    <install>/build-kit. Every step after this one runs its tools from there (settle), and the game's rebuild for its
    mods starts the setup window from there, so the setup folder can go once the install is done and the install
    folder can move. A newer setup replaces the kit; the kit's own rebuild keeps it. The steps' keys fingerprint file
    contents, not paths, so the kit, a copy of the setup's files, keeps every step the setup recorded."""

    id = 'kit'

    def key(self, ctx):
        return fingerprint('kit-1', ctx.layout.release if layouts.uses_kit(ctx.layout) else 'none')

    def current(self, ctx, record):
        if not layouts.uses_kit(ctx.layout):
            return True
        kit = layouts.kit_folder(ctx.dest)
        if not layouts.is_release(kit):
            return False
        found = layouts.release_layout(kit)
        return found.release == ctx.layout.release and not found.missing(getattr(ctx, 'cpu', 'v3'))

    def run(self, ctx):
        layout, rep = ctx.layout, ctx.reporter
        if not layouts.uses_kit(layout):
            return {}
        kit = layouts.kit_folder(ctx.dest)
        if layout.root.resolve() == kit.resolve():   # the kit's own rebuild: it is in place
            return {}
        new, old = kit.with_name(layouts.KIT + '.new'), kit.with_name(layouts.KIT + '.old')
        try:
            remove(new)   # what a cancelled or failed run left
            remove(old)
        except OSError as e:
            raise InstallError('kit.in_use', path=str(e.filename or old)) from None
        files = []
        for name in layouts.KIT_ITEMS:
            src = layout.root / name
            if src.is_dir():
                files += [f for f in sorted(src.rglob('*')) if f.is_file() and '__pycache__' not in f.parts]
            elif src.is_file():
                files.append(src)
        total, done = size_of(files), 0
        rep.progress('kit', 'copy', 0, total, 'bytes', force=True)
        try:
            for f in files:
                dst = new / f.relative_to(layout.root)
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(f, dst)
                done += f.stat().st_size
                rep.progress('kit', 'copy', done, total, 'bytes')
        except OSError as e:
            raise InstallError('kit.copy_failed', path=str(e.filename or new), detail=str(e)) from None
        # An open rebuild window, or a layout generator the game's settings menu started, holds files of the old kit:
        # then nothing is replaced, and the message asks to close the game and run the setup again
        moved = False
        try:
            if kit.exists():
                os.replace(kit, old)
                moved = True
            os.replace(new, kit)
        except OSError:
            if moved and not kit.exists():
                os.replace(old, kit)   # the game keeps the kit it had
            raise InstallError('kit.in_use', path=str(kit)) from None
        try:
            remove(old)
        except OSError as e:
            rep.log(f'kit: the old kit stays until the next run: {e}')
        rep.log(f'kit: {len(files)} files, {total / GiB:.2f} GiB in {kit}')
        return {'bytes': total}

    def settle(self, ctx):
        if not layouts.uses_kit(ctx.layout):
            return
        kit = layouts.kit_folder(ctx.dest).resolve()
        if ctx.layout.root.resolve() != kit:
            ctx.layout = layouts.release_layout(kit)
            ctx.reporter.log(f'kit: the next steps run from {kit}')


def adopt_old_temp(layout, dest, temp):
    """An install by a setup before the build kit has its temp folder (generated C, lift cache, objects) in that
    setup's folder: <setup>/temp/<install name>-<hash>, the setup build-cache/rebuild.txt names. The first run of a
    setup with the build kit moves it into the install folder when both are on one disk, and the steps' keys decide
    what of it still holds. On another disk the new temp folder starts empty: an update recompiles everything anyway
    (the compiler's path is in every compile command), so a copy would save only part of the lift.
    Runs before the run's log opens in temp; returns the line for the log, None when there was nothing to move."""
    if not layouts.uses_kit(layout) or temp != dest / layouts.TEMP_IN_DEST or temp.exists():
        return None
    try:
        lines = (dest / 'build-cache' / 'rebuild.txt').read_text(encoding='utf-8').splitlines()
    except OSError:
        return None
    if len(lines) < 4 or lines[1] != '-c' or not os.path.isabs(lines[3]):
        return None
    old = layouts.temp_folder(Path(lines[3]) / 'temp', dest)
    if not (old / 'gen').is_dir():
        return None
    try:
        temp.parent.mkdir(parents=True, exist_ok=True)
        os.replace(old, temp)
        return f'temp: moved the temp folder of the setup in {lines[3]} to {temp}'
    except OSError as e:
        return f'temp: the temp folder {old} stays where it is ({e}); {temp} starts empty'


# ---- 2. fonts

class Fonts(Step):
    """The Switch system fonts for Chinese, Korean and Traditional Chinese text (tools/extract_fonts.py, which runs the
    vendored extract_shared_fonts.py). The firmware folder is optional: without it the setup warns and the game runs
    with those glyphs missing. The extraction verifies the firmware NCAs and writes the six BFTTF files the console
    puts in shared memory."""

    id = 'fonts'

    def key(self, ctx):
        lang = requested_lang(ctx)
        if lang not in ini.NEED_SHARED_FONTS:
            return fingerprint('fonts-1', 'not-needed')
        firmware = firmware_dir(getattr(ctx.options, 'firmware', None))
        if firmware is None:
            return fingerprint('fonts-1', 'no-firmware')
        return fingerprint('fonts-1', lang, str(firmware), firmware_stamp(firmware))

    def current(self, ctx, record):
        if requested_lang(ctx) not in ini.NEED_SHARED_FONTS or not getattr(ctx.options, 'firmware', None):
            return True
        return shared_fonts_ready(ctx.dest)

    def verify(self, ctx, record):
        # An update run may omit --firmware; the installed set still has to be there for these languages.
        if requested_lang(ctx) in ini.NEED_SHARED_FONTS:
            return shared_fonts_ready(ctx.dest)
        return True

    def run(self, ctx):
        lang = getattr(ctx, 'lang', None) or requested_lang(ctx)
        if lang not in ini.NEED_SHARED_FONTS:
            return {}
        firmware = firmware_dir(getattr(ctx.options, 'firmware', None))
        if firmware is None:
            return {}   # Check warned: the game runs, some text may not show
        keys = getattr(ctx.options, 'keys', None)
        if not keys:
            raise InstallError('check.fonts_keys_needed')
        out = ctx.temp / 'shared-fonts'
        remove(out)
        ctx.reporter.progress('fonts', 'extract', force=True)
        tool = ctx.layout.tools / 'vendor' / 'macnativegames' / 'extract_shared_fonts.py'
        code, log = run_tool(ctx, 'fonts', tool, ['--keys', keys, '--firmware', firmware, '--output', out])
        if code:
            detail = ''
            try:
                detail = log.read_text(encoding='utf-8', errors='replace').strip().splitlines()[-1][:300]
            except (OSError, IndexError):
                pass
            raise InstallError('fonts.failed', code=code, detail=detail, log=str(log))
        missing = [name for name in SHARED_FONT_FILES if not (out / name).is_file()]
        if missing or not (out / 'manifest.json').is_file():
            raise InstallError('fonts.incomplete', files=', '.join(missing) or 'manifest.json')
        ctx.reporter.progress('fonts', 'copy', force=True)
        try:
            ctx.dest.mkdir(parents=True, exist_ok=True)
            remove(ctx.dest / SHARED_FONTS)
            shutil.move(str(out), str(ctx.dest / SHARED_FONTS))
        except OSError as e:
            raise InstallError('fonts.copy_failed', path=str(ctx.dest / SHARED_FONTS), detail=str(e)) from None
        return {'files': len(SHARED_FONT_FILES)}


# ---- 3. extract

class Extract(Step):
    id = 'extract'

    def key(self, ctx):
        # The output is whatever matches the expected hashes, whichever files and tools produced it.
        return fingerprint('extract-1', ctx.layout.expected(game_version(ctx)))

    def current(self, ctx, record):
        # Another update file than the recorded one (1.2.1 -> 1.4.3) extracts again; the check then identifies it.
        update, known = getattr(ctx.options, 'update', None), ctx.state['inputs'].get('update')
        if update and known and Path(update).is_file():
            st = Path(update).stat()
            if (st.st_size, st.st_mtime_ns) != (known.get('size'), known.get('mtime_ns')) \
                    and file_sha256(Path(update)) != known.get('sha256'):
                return False
        expected = expected_update(ctx)
        romfs = ctx.dest / 'romfs'
        if extract.exefs_problems(ctx.dest / 'exefs', expected) or not extract.romfs_verified(romfs, expected):
            return False
        st = (romfs / 'romfs.bin').stat()
        return st.st_size == record.get('romfs_size') and st.st_mtime_ns == record.get('romfs_mtime_ns')

    def verify(self, ctx, record):
        image = ctx.dest / 'romfs' / 'romfs.bin'
        rep = ctx.reporter
        digest = file_sha256(image, lambda d, t: rep.progress('check', 'verify_romfs', d, t, 'bytes'))
        return digest == expected_update(ctx)['romfs']['sha256']

    def run(self, ctx):
        o, dest, rep = ctx.options, ctx.dest, ctx.reporter
        expected = expected_update(ctx)
        keys = Path(o.keys)
        inputs = {}
        for name, path in (('base', Path(o.base)), ('update', Path(o.update))):
            known = ctx.state['inputs'].get(name, {})
            st = path.stat()
            if known.get('size') == st.st_size and known.get('mtime_ns') == st.st_mtime_ns and known.get('sha256'):
                inputs[name] = known
                continue
            digest = file_sha256(path, lambda d, t, n=name: rep.progress('extract', 'hash_' + n, d, t, 'bytes'))
            inputs[name] = {'name': path.name, 'size': st.st_size, 'mtime_ns': st.st_mtime_ns, 'sha256': digest}
        ctx.state['inputs'] = inputs
        ctx.save()
        base, update = game_file(ctx, 'base'), game_file(ctx, 'update')

        dest.mkdir(parents=True, exist_ok=True)
        for stale in dest.glob('update-merge-*'):   # a killed merge leaves its NCA copies (and key copies) behind
            remove(stale)
        rep.progress('extract', 'exefs', force=True)
        staged = ctx.temp / 'check' / 'update' / 'exefs'
        if not staged.is_dir():
            remove(ctx.temp / 'check' / 'update')
            try:
                extract.extract_exefs(update, keys, ctx.temp / 'check' / 'update', ctx.temp / 'logs', update=True)
            except extract.DamagedFile as e:
                raise InstallError('check.input_damaged', input='update', path=str(o.update), detail=e.detail) from None
            except ValueError:
                raise InstallError('check.exefs_failed', input='update',
                                   log=str(ctx.temp / 'logs' / 'extract_exefs-update.log')) from None
        remove(dest / 'exefs')
        shutil.move(staged, dest / 'exefs')
        problems = extract.exefs_problems(dest / 'exefs', expected)
        if problems:
            raise InstallError('extract.exefs_mismatch', version=game_version(ctx), modules=', '.join(problems))
        if extract.sha256(dest / 'exefs' / 'main.npdm') != expected['exefs_sha256']['main.npdm']:
            rep.warning('warn.npdm_differs', version=game_version(ctx))

        romfs = dest / 'romfs'
        if self.id in ctx.forced:
            remove(romfs)   # the full hash failed: index.json still claims the extraction's hash
        if not extract.romfs_verified(romfs, expected):
            self.merge(ctx, base, update, keys, romfs, expected)
        report = json.loads((romfs / 'index.json').read_text())
        if report['romfs_size'] != expected['romfs']['size'] or report['romfs_sha256'] != expected['romfs']['sha256']:
            raise InstallError('extract.romfs_mismatch', version=game_version(ctx), size=report['romfs_size'],
                               sha256=report['romfs_sha256'][:16])
        (dest / 'profile').mkdir(exist_ok=True)
        remove(ctx.temp / 'unpacked')   # the NSPs unpacked from an NSZ or XCZ (Check.unpack)
        st = (romfs / 'romfs.bin').stat()
        return {'romfs_size': st.st_size, 'romfs_mtime_ns': st.st_mtime_ns}

    def merge(self, ctx, base, update, keys, romfs, expected):
        dest, logs = ctx.dest, ctx.temp / 'logs'
        ncas = extract.merge_space(base, update, keys)
        watch = Watch(ctx.reporter, 'extract')

        def phase(name):
            if name == 'copy':
                watch.set('copy', lambda: size_of(dest.glob('update-merge-*/*.nca')), ncas)
            elif name == 'merge':
                watch.set('merge', lambda: size_of(dest.glob('update-merge-*/result/romfs.bin')),
                          expected['romfs']['size'])
            else:
                watch.set(name)

        watch.start()
        try:
            extract.merge_update_romfs(base, update, keys, ctx.layout.hactool, romfs, logs, phase=phase)
        except extract.DamagedFile as e:
            name = 'base' if e.path == base else 'update'
            raise InstallError('check.input_damaged', input=name, path=str(getattr(ctx.options, name)),
                               detail=e.detail) from None
        except ValueError as e:
            raise InstallError('extract.romfs_merge_failed', detail=str(e), log=str(logs / 'merge_romfs-'
                                                                                     f'{dest.name}.log')) from None
        finally:
            watch.stop()


# ---- 4. lift

EMITTED = re.compile(r'^emitted (\d+)/(\d+) chunks')
DISCOVERED = re.compile(r'^(\w+): \d+ functions \(.*discovery')


CHEAT_PLACES = ('cheats', 'atmosphere/contents/0100F2C0115B6000/cheats', 'contents/0100F2C0115B6000/cheats',
                '0100F2C0115B6000/cheats')


def cheat_files(ctx):
    """The Atmosphere cheat files of the mods folder (<dest>/mods/<mod>/cheats/*.txt and the Atmosphere places, as
    tools/cheats.py finds them), for the lift key. All of them: the ones the build reads go by the build id of the
    installed main, and the keys are computed before Extract runs (runner plan), when a fresh install has no main and an
    update may still have another version's."""
    mods = ctx.dest / 'mods'
    if not mods.is_dir():
        return []
    return sorted(f for mod in mods.iterdir() if mod.is_dir() for where in CHEAT_PLACES
                  for f in ((mod / where).glob('*.txt') if (mod / where).is_dir() else ()))


EXEFS_PLACES = ('exefs', 'atmosphere/contents/0100F2C0115B6000/exefs', 'contents/0100F2C0115B6000/exefs',
                '0100F2C0115B6000/exefs')


def exefs_files(ctx):
    """The ExeFS patch files of the mods folder (.ips and .pchtxt in each mod's exefs places and its
    atmosphere/exefs_patches/<name>/, as tools/exefs_patches.py finds them), for the lift key."""
    mods = ctx.dest / 'mods'
    if not mods.is_dir():
        return []
    out = []
    for mod in (m for m in mods.iterdir() if m.is_dir()):
        folders = [mod / w for w in EXEFS_PLACES if (mod / w).is_dir()]
        patches = mod / 'atmosphere' / 'exefs_patches'
        folders += [d for d in patches.iterdir() if d.is_dir()] if patches.is_dir() else []
        out += [f for d in folders for f in d.iterdir() if f.is_file() and f.suffix.lower() in ('.ips', '.pchtxt')]
    return sorted(out)


def plugin_files(ctx):
    """The exlaunch plugins of the mods folder (each mod's exefs/subsdk*, in the first ExeFS place, as
    tools/exefs_plugins.py finds them), by mod name, for the lift key and the plugin stage."""
    mods = ctx.dest / 'mods'
    if not mods.is_dir():
        return []
    out = []
    for mod in sorted((m for m in mods.iterdir() if m.is_dir()), key=lambda m: m.name):
        exefs = next((mod / w for w in EXEFS_PLACES if (mod / w).is_dir()), None)
        if exefs:
            out += sorted((f for f in exefs.iterdir() if f.is_file() and f.name.lower().startswith('subsdk')),
                          key=lambda f: f.name.lower())
    return out


def plugins_status(ctx):
    """The build's plugin status file (tools/exefs_plugins.py --status), which runtime/mods.c reads."""
    return ctx.dest / 'build-cache' / 'plugins.status'


def plugins_stale(ctx):
    """True when the game listed a plugin whose start differed from the build (build-cache/plugins.stale, runtime/mods.c
    mods_plugin_off): the plugin's record comes from the lift's service run and the exe's patch sets from the compile,
    so both are out of date, while their keys, made of the files the steps read, are unchanged. Only Lift.run clears
    the file."""
    try:
        return bool((ctx.dest / 'build-cache' / 'plugins.stale').read_text(encoding='utf-8', errors='replace').strip())
    except OSError:
        return False


def built_plugins(status, files, mods):
    """The files of the plugins the status file lists as built (kind `plugin`), in the order of files. The key is
    FNV-1a 32 of "<mod folder>/<path in the mod>", as tools/exefs_plugins.py and runtime/mods.c compute it."""
    if not status.is_file():
        return []
    built = {line.split('\t')[0] for line in status.read_text(encoding='utf-8').splitlines()
             if len(line.split('\t')) >= 3 and line.split('\t')[2] == 'plugin'}

    def key(f):
        rel = f.relative_to(mods).as_posix()
        h = 0x811C9DC5
        for b in rel.encode():
            h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
        return f'{h:08X}'
    return [f for f in files if key(f) in built]


def exefs_status(ctx):
    """The build's ExeFS patch status file (tools/exefs_patches.py --status), which runtime/mods.c reads."""
    return ctx.dest / 'build-cache' / 'exefs.status'


def exefs_data(ctx):
    """The data writes of the mods' ExeFS patch files (tools/exefs_patches.py --data), which runtime/mods.c stores into
    main before the game runs."""
    return ctx.dest / 'build-cache' / 'exefs.data'


def cheats_status(ctx):
    """The build's cheat status file (tools/cheats.py --status), which runtime/cheats.c reads: in build-cache, not in a
    mod's own cheats folder, which the player (or TKMM's merge) may empty."""
    return ctx.dest / 'build-cache' / 'cheats.status'


class Lift(Step):
    id = 'lift'
    transient = True

    def key(self, ctx):
        # fingerprint takes a file's name and content only; the paths under mods/ go in too, because tools/cheats.py
        # and tools/exefs_patches.py name each patch set after its mod folder, so a renamed folder needs a lift
        # A mod plugin is built by running its init on a totk.exe of this build (tools/exefs_plugins.py), so the
        # runtime is in the key too when there is one, and this file, which runs that stage: a plugin stage that failed
        # (warn.plugins_skipped, the game built without the plugins) runs again once a release changes it.
        files = cheat_files(ctx) + exefs_files(ctx) + plugin_files(ctx)
        extra = [ctx.layout.tools / 'exefs_plugins.py', Path(__file__), runtime_key(ctx)] if plugin_files(ctx) else []
        return fingerprint('lift-5', ctx.keys['extract'], ctx.layout.recomp, ctx.layout.patches(game_version(ctx)),
                           ctx.layout.tools / 'cheats.py', ctx.layout.tools / 'exefs_patches.py', *extra, *files,
                           *(f.relative_to(ctx.dest / 'mods').as_posix() for f in files))

    def current(self, ctx, record):
        gen = ctx.temp / 'gen'
        stamp = gen / STAMP
        return ((gen / 'layout.json').is_file() and stamp.is_file() and stamp.read_text().strip() == ctx.keys['lift']
                and not plugins_stale(ctx))

    def run(self, ctx):
        # gen/ of the last run stays: gen.py lifts again only the chunks whose inputs changed
        # (gen/<mod>/lift_cache.json; TOTK_LIFT_CACHE=0 lifts all), rewrites only those whose C changed (and removes
        # the ones a smaller partition no longer has), so Ninja recompiles only those
        layout, rep, gen = ctx.layout, ctx.reporter, ctx.temp / 'gen'
        (gen / STAMP).unlink(missing_ok=True)
        rep.progress('lift', 'discover', force=True)

        def parse(line):
            m = EMITTED.match(line)
            if m:
                rep.progress('lift', 'emit', int(m.group(1)), int(m.group(2)), 'chunks')

        # The code cheats of the mods' cheat files and the mods' ExeFS patch files become patch sets and data writes
        # (tools/cheats.py, tools/exefs_patches.py); their status files tell the runtime what the build has. Both are
        # the player's downloads, so files the tools refuse cost those cheats or patches, not the install: the game is
        # built without them and the setup warns.
        plain = layout.patches(game_version(ctx))
        # (tool, patch file it writes, its outputs for the runtime: the status first, then the data file, the files it
        # reads, the warning when it fails)
        stages = [('cheats.py', 'patches-cheats.txt', [cheats_status(ctx)], cheat_files(ctx), 'warn.cheats_skipped'),
                  ('exefs_patches.py', 'patches-exefs.txt', [exefs_status(ctx), exefs_data(ctx)], exefs_files(ctx),
                   'warn.exefs_skipped')]
        patches, skipped = plain, {}

        def drop(outputs):   # no status: the runtime treats every such file as not built
            for f in outputs:
                f.unlink(missing_ok=True)

        for tool, out, outputs, inputs, warn in stages:
            drop(outputs)
            if not inputs:
                continue
            code, log = run_tool(ctx, 'lift', layout.tools / tool,
                                 ['--exefs', ctx.dest / 'exefs', '--mods', ctx.dest / 'mods', '--patches', patches,
                                  '--out', ctx.temp / out, '--status', outputs[0],
                                  *(['--data', outputs[1]] if len(outputs) > 1 else [])])
            if code:
                drop(outputs)
                skipped[warn] = log
            else:
                patches = ctx.temp / out
        args = ['--exefs', ctx.dest / 'exefs', '--out', gen]
        # The mods' exlaunch plugins (tools/exefs_plugins.py, after the design in #845 by itaskaev2): a lift with the
        # plugins in the layout, a totk.exe of it in build/ (the compile step's folder, so it recompiles only what
        # the hook sites change after), the plugin service run, and the plugins' patch sets for the lift below. A
        # refused plugin gets its reason in plugins.status; a failed stage costs the plugins, not the install.
        plugins = plugin_files(ctx)
        drop([plugins_status(ctx), ctx.dest / 'build-cache' / 'plugins.stale'])
        lift_plugins = []
        if plugins:
            listed = ['--plugins', ','.join(str(f) for f in plugins)]
            # An init that calls into main where the lift has no function start crashes its service run; the tool
            # names those addresses (--entries-out), and one more lift with them as `entry` lines lets the init go on
            # (Zonai Fieldworks calls the second word of a 1.4.3 function). Each round adds what the last one found.
            lift_patches, found = patches, []
            for _ in range(PLUGIN_ENTRY_ROUNDS):
                code, log = run_tool(ctx, 'lift', layout.recomp / 'gen.py', args + ['--patches', lift_patches] + listed,
                                     parse)
                if not code:
                    code, log = run_tool(ctx, 'lift', layout.tools / 'build_game.py', build_args(ctx, gen))
                if not code:
                    # the service run's totk.exe loads its DLLs from its own folder, as the installed one does
                    for dll in layout.dlls:
                        replace_file(dll, ctx.temp / 'build' / dll.name)
                    entries_file = ctx.temp / 'plugin-entries.txt'
                    entries_file.unlink(missing_ok=True)
                    code, log = run_tool(ctx, 'lift', layout.tools / 'exefs_plugins.py', [
                        '--exe', ctx.temp / 'build' / ('totk' + EXE), '--out', ctx.temp / 'plugins', '--mods',
                        ctx.dest / 'mods', '--exefs', ctx.dest / 'exefs', '--patches', lift_patches,
                        '--patches-out', ctx.temp / 'patches-plugins.txt', '--status', plugins_status(ctx),
                        '--entries-out', entries_file])
                    # exit 1 with a status file: refused plugins, their reasons in it; the rest goes on
                    if plugins_status(ctx).is_file() and (ctx.temp / 'patches-plugins.txt').is_file():
                        code = 0
                if code:
                    break
                new = [line for line in (entries_file.read_text(encoding='utf-8').splitlines()
                                         if entries_file.is_file() else []) if line and line not in found]
                if not new:
                    break
                found += new
                lift_patches = ctx.temp / 'patches-entries.txt'
                lift_patches.write_text(Path(patches).read_text(encoding='utf-8') + '\n' + '\n'.join(found) + '\n',
                                        encoding='utf-8')
            if code:
                drop([plugins_status(ctx)])
                skipped['warn.plugins_skipped'] = log
            else:
                patches = ctx.temp / 'patches-plugins.txt'
                # With one plugin built, the layout keeps every plugin of the service runs: the records hold addresses
                # of that layout. A refused plugin stays loaded at play, its init never runs (no patch set).
                if built_plugins(plugins_status(ctx), plugins, ctx.dest / 'mods'):
                    lift_plugins = plugins
        if lift_plugins:
            args += ['--plugins', ','.join(str(f) for f in lift_plugins)]
        code, log = run_tool(ctx, 'lift', layout.recomp / 'gen.py', args + ['--patches', patches], parse)
        if code and patches != plain:
            for tool, out, outputs, inputs, warn in stages:
                if inputs and warn not in skipped:
                    drop(outputs)
                    skipped[warn] = log
            if lift_plugins:
                drop([plugins_status(ctx)])
                skipped.setdefault('warn.plugins_skipped', log)
            args = ['--exefs', ctx.dest / 'exefs', '--out', gen]
            code, log = run_tool(ctx, 'lift', layout.recomp / 'gen.py', args + ['--patches', plain], parse)
        if code:
            raise InstallError('lift.failed', code=code, log=str(log))
        # The mods' data writes must stay off the bytes the lifted code reads as constants (its jump tables), which only
        # the lift knows: gen.py lists them in folded.txt, and a file that writes them becomes unsupported
        if exefs_data(ctx).is_file():
            code, log = run_tool(ctx, 'lift', layout.tools / 'exefs_patches.py',
                                 ['--exefs', ctx.dest / 'exefs', '--status', exefs_status(ctx), '--data',
                                  exefs_data(ctx), '--folded', gen / 'main' / 'folded.txt'])
            if code:
                drop([exefs_status(ctx), exefs_data(ctx)])
                skipped.setdefault('warn.exefs_skipped', log)
        for warn, log in skipped.items():
            rep.warning(warn, log=str(log))
        (gen / STAMP).write_text(ctx.keys['lift'] + '\n')
        return {}


# ---- 5. compile

NINJA_LINE = re.compile(r'^\[(\d+)/(\d+)\] ')


def runtime_key(ctx):
    """Fingerprint of what totk.exe is built from besides the lifted code: the runtime, the toolchain and the build
    tools. Shader preparation depends on these, not on the code cheats a lift adds."""
    layout, cpu = ctx.layout, getattr(ctx, 'cpu', 'v3')
    # without the lines that name clang's folder: the build kit of a moved install folder is the same toolchain
    clang = '\n'.join(line for line in subprocess.run([str(layout.clang), '--version'], capture_output=True,
                                                       text=True).stdout.splitlines()
                      if not line.startswith(('InstalledDir:', 'Configuration file:')))
    tools = [layout.tools / n for n in ('build_game.py', 'romfs_index.py', 'check_content.py', 'check_isa.py')]
    return fingerprint('runtime-1', cpu, layout.runtime_lib_for(cpu), layout.runtime, layout.deps or 'system',
                       layout.sysroot or 'host', *tools, clang, *[d.name for d in layout.dlls])


def build_args(ctx, gen):
    """tools/build_game.py's arguments for a totk.exe of the lifted C in gen, built in <temp>/build."""
    layout, dest = ctx.layout, ctx.dest
    cpu = getattr(ctx, 'cpu', 'v3')
    lib = layout.runtime_lib_for(cpu)
    pgo = layout.pgo(game_version(ctx))
    return ['--gen', gen, '--out', ctx.temp / 'build', '--toolchain', layout.llvm, '--cpu', cpu,
            *(['--deps', layout.deps] if layout.deps else []), *(['--sysroot', layout.sysroot] if layout.sysroot else []),
            '--runtime', layout.runtime, *(['--runtime-lib', lib] if lib else ()), *(['--pgo', pgo] if pgo else ()),
            '--ninja', layout.ninja,
            '--version', version_tag(game_version(ctx)), '--expected', layout.expected(game_version(ctx)),
            '--exefs', dest / 'exefs', '--romfs', dest / 'romfs',
            '--profile', dest / 'profile',
            # the build kit and the temp folder move with the install folder: Ninja's paths relative to the kit
            *(['--portable'] if layouts.uses_kit(layout) else [])]


class Compile(Step):
    id = 'compile'

    def key(self, ctx):
        # The release string too: build_game.py bakes the VERSION of the tools' folder into totk.exe (the menu's title
        # line), and a release that leaves the lift and the runtime alone would otherwise keep the old exe and its
        # old version.
        return fingerprint('compile-4', ctx.keys['lift'], runtime_key(ctx), ctx.layout.release,
                           ctx.layout.pgo(game_version(ctx)) or 'no profile')

    def current(self, ctx, record):
        exe = ctx.dest / ('totk' + EXE)
        if not exe.is_file() or not (ctx.dest / 'romfs-index.bin').is_file():
            return False
        if plugins_stale(ctx):   # the lift runs (Lift.current), and a transient step runs only with the step after it
            return False
        if not all((ctx.dest / d.name).is_file() for d in ctx.layout.dlls):
            return False
        return file_sha256(exe) == record.get('exe_sha256')

    def run(self, ctx):
        layout, rep, dest = ctx.layout, ctx.reporter, ctx.dest
        build = ctx.temp / 'build'
        phase = ['compile']

        def parse(line):
            m = NINJA_LINE.match(line)
            if m:
                rep.progress('compile', phase[0], int(m.group(1)), int(m.group(2)), 'objects')
            elif line.startswith('compile: '):
                phase[0] = 'link'
                rep.progress('compile', 'link', force=True)

        rep.progress('compile', 'compile', force=True)
        cpu = getattr(ctx, 'cpu', 'v3')
        code, log = run_tool(ctx, 'compile', layout.tools / 'build_game.py', build_args(ctx, ctx.temp / 'gen'), parse)
        exe = build / ('totk' + EXE)
        if code and link_output_locked(log):
            raise InstallError('compile.exe_locked', path=str(exe), log=str(log))
        if code or not exe.is_file():
            raise InstallError('compile.failed', code=code, log=str(log))
        try:
            replace_file(exe, dest / exe.name)
        except PermissionError:
            raise InstallError('compile.exe_in_use', path=str(dest / exe.name)) from None
        replace_file(build / 'romfs-index.bin', dest / 'romfs-index.bin')
        for dll in layout.dlls:
            replace_file(dll, dest / dll.name)
        return {'exe_sha256': file_sha256(dest / exe.name), 'cpu': cpu}


# ---- 6. prepare

TRANSLATED = re.compile(r'gfx_shader: prepare: (\d+) of (\d+) pairs')
# runtime/vk_pipeline.c lb_warm_prepared: variants the driver crashed on in an earlier run (tools/prepare_shaders.py
# runs the driver step again after such a crash)
SKIPPED = re.compile(r'prepare-shaders: (\d+) variants skipped: the graphics driver crashed')


ASPECT_SIZES = screens.ASPECT_SIZES


class Prepare(Step):
    id = 'prepare'

    def key(self, ctx):
        layout = ctx.layout
        xlate_id = layout.xlate.with_suffix('.id')
        tools = [layout.tools / n for n in ('prepare_shaders.py', 'shader_census.py', 'make_romfs_patches.py',
                                            'pack_shader_cache.py', 'ultrawide_layout.py', 'button_icons.py')]
        # not the compile key: a rebuild for the mods' code cheats keeps the prepared shaders
        return fingerprint('prepare-2', ctx.keys['extract'], runtime_key(ctx), layout.xlate, xlate_id,
                           shader_list_text(layout), *tools)

    def current(self, ctx, record):
        dest = ctx.dest
        xlate = dest / ctx.layout.xlate.name
        return ((dest / 'shader-prepared.bin').is_file() and (dest / 'romfs-patches' / 'manifest.tsv').is_file()
                and xlate.is_file() and file_sha256(xlate) == record.get('xlate_sha256'))

    def run(self, ctx):
        layout, rep, dest = ctx.layout, ctx.reporter, ctx.dest
        xlate = dest / layout.xlate.name
        if xlate.is_file() and file_sha256(xlate) != file_sha256(layout.xlate):
            # Another translator: its cache keys differ, so the old translations would only take space.
            for name in ('shader-cache', 'shader-prepared.bin'):
                remove(dest / name)
        # totk.exe starts totk_xlate.exe as a child while it prepares shaders; a preparation that was stopped from
        # outside leaves that child holding this file, and Windows then refuses to replace it (issue #174).
        try:
            replace_file(layout.xlate, xlate)
        except PermissionError:
            raise InstallError('prepare.xlate_in_use', path=str(xlate)) from None
        xlate_id = layout.xlate.with_suffix('.id')
        if xlate_id.is_file():
            try:
                replace_file(xlate_id, xlate.with_suffix('.id'))
            except PermissionError:
                raise InstallError('prepare.xlate_in_use', path=str(xlate.with_suffix('.id'))) from None
        else:
            remove(xlate.with_suffix('.id'))

        rep.progress('prepare', 'romfs_patches', force=True)
        remove(dest / 'romfs-patches')
        code, log = run_tool(ctx, 'prepare', layout.tools / 'make_romfs_patches.py',
                             ['--version', version_tag(game_version(ctx)), '--romfs', dest / 'romfs', '--out',
                              dest / 'romfs-patches'])
        if code:
            raise InstallError('prepare.romfs_patches_failed', code=code, log=str(log))
        # The same with Xbox and PlayStation button icons in the prompts, for prompts = xbox and playstation
        # (make_romfs_patches.py --prompts): romfs-patches-xbox for the Nintendo pad layout, romfs-patches-xbox-label
        # for xbox and auto, and romfs-patches-playstation / -playstation-xbox likewise (runtime/input_map.c
        # input_prompts_folder). All are written on every install, so the menu choices work without another run of
        # the setup. Optional: when one fails, the game keeps the Switch prompts with that layout and the menu says the
        # files are missing, so the setup warns once and goes on. The keyboard and mouse prompts are the PcPrompts
        # step's: they follow totk.ini's bindings.
        failed = None
        for style in screens.PAD_PROMPTS:
            out = dest / f'romfs-patches-{style}'
            remove(out)
            code, log = run_tool(ctx, f'prepare-{style}', layout.tools / 'make_romfs_patches.py',
                                 ['--version', version_tag(game_version(ctx)), '--romfs', dest / 'romfs', '--out', out,
                                  '--prompts', style])
            if code:
                remove(out)
                failed = failed or (code, log)
        if failed:
            rep.warning('warn.xbox_prompts_failed', code=failed[0], log=str(failed[1]))

        shader_list = layout.shader_list
        if not shader_list:
            shader_list = ctx.temp / 'shader_list.tsv'
            shader_list.write_text(shader_list_text(layout), encoding='utf-8', newline='\n')

        skipped = [0]

        def parse(line):
            m = TRANSLATED.search(line)
            if m:
                rep.progress('prepare', 'translate', int(m.group(1)), int(m.group(2)), 'shaders')
            elif SKIPPED.search(line):
                skipped[0] = int(SKIPPED.search(line).group(1))
            elif 'listed variants' in line:
                rep.progress('prepare', 'translate', force=True)
            elif 'gfx_shader: prepare:' in line and 'translated' in line and 'programs' in line:
                rep.progress('prepare', 'libraries', force=True)

        rep.progress('prepare', 'seed', force=True)
        code, log = run_tool(ctx, 'prepare', layout.tools / 'prepare_shaders.py',
                             [dest, '--version', version_tag(game_version(ctx)), '--romfs', dest / 'romfs', '--exefs',
                              dest / 'exefs', '--list', shader_list], parse)
        if code or not (dest / 'shader-prepared.bin').is_file():
            # The loader stopped the game before it ran: name the library rather than exit code 127
            missing = loader_missing(log)
            if missing:
                raise InstallError('prepare.libraries_missing', libraries=', '.join(missing),
                                   packages=library_packages(missing), log=str(log))
            raise InstallError('prepare.shaders_failed', code=code, log=str(log))
        if skipped[0]:
            rep.warning('warn.shaders_skipped', count=skipped[0], log=str(log))

        # One shader-cache.pack instead of four files per shader (~117,000): totk.exe reads the pack first.
        rep.progress('prepare', 'pack', force=True)
        code, log = run_tool(ctx, 'prepare', layout.tools / 'pack_shader_cache.py', [dest / 'shader-cache'])
        if code:
            raise InstallError('prepare.pack_failed', code=code, log=str(log))
        return {'xlate_sha256': file_sha256(xlate)}


# ---- 6b. Keyboard and mouse prompts

def bind_lines(dest):
    """The bind_ lines of the install's totk.ini (the Controls tab writes them), '' without the file."""
    try:
        text = (dest / 'totk.ini').read_text(encoding='utf-8-sig', errors='replace')
    except OSError:
        return ''
    # cfg.c accepts the TOTK_ prefix and quoted values. Keep the complete binding lines in the fingerprint and pass
    # the ini to the generator even when its only binding uses that prefix.
    return '\n'.join(line.strip() for line in text.splitlines()
                     if re.match(r'^\s*(?:totk_)?bind_\w+\s*=', line, re.IGNORECASE))


def pc_binds(dest):
    """make_romfs_patches.py's --binds argument for the keyboard and mouse prompts: totk.ini when it has bind_ lines."""
    return ['--binds', dest / 'totk.ini'] if bind_lines(dest) else []


class PcPrompts(Step):
    """romfs-patches-pc (16:9), for prompts = pc: the prompts drawn as the keys and mouse buttons of totk.ini's bind_
    lines (make_romfs_patches.py --prompts pc --binds), the keyboard defaults without them. Its own step, keyed by those
    lines, so running the setup again after a change of the bindings draws the prompts again and leaves the shaders
    alone. The shaped romfs-patches-pc-<w>x<h> are the Shapes step's, which follows this key. Optional like the Xbox
    prompts: a failure warns, and the game keeps the Switch prompts."""
    id = 'pc_prompts'

    def key(self, ctx):
        layout = ctx.layout
        tools = [layout.tools / n for n in ('make_romfs_patches.py', 'ultrawide_layout.py', 'button_icons.py')]
        return fingerprint('pc-prompts-1', ctx.keys['extract'], *tools, bind_lines(ctx.dest))

    def current(self, ctx, record):
        return record.get('failed') or (ctx.dest / 'romfs-patches-pc' / 'manifest.tsv').is_file()

    def run(self, ctx):
        layout, rep, dest = ctx.layout, ctx.reporter, ctx.dest
        rep.progress('pc_prompts', 'draw', force=True)
        plain = dest / 'romfs-patches-pc'
        remove(plain)
        code, log = run_tool(ctx, 'prepare-pc', layout.tools / 'make_romfs_patches.py',
                             ['--version', version_tag(game_version(ctx)), '--romfs', dest / 'romfs', '--out', plain,
                              '--prompts', 'pc', *pc_binds(dest)])
        if code:
            remove(plain)
            rep.warning('warn.xbox_prompts_failed', code=code, log=str(log))
            return {'failed': True}
        return {}


# ---- 6b. screen shapes

SHAPED_FOLDER = re.compile(r'^romfs-patches(?:-(?:%s))?-\d+x\d+(?:-hud(?:16|21)x9)?$'
                           % '|'.join(map(re.escape, screens.PROMPTS_STYLES)))
# make_romfs_patches.py writes <folder>.part-<pid> and renames it to <folder> when it is complete. A run that was
# stopped (the PC turned off under a generator the game started, runtime/romfs.c romfs_folders_prepare) leaves its
# folder behind: one whose files did not change for PART_STALE_S is such a leftover, a younger one may belong to a
# generator running now. SHAPED_FOLDER matches neither.
PART_FOLDER = re.compile(r'^romfs-patches[\w.-]*\.part-[\w-]+$')
PART_STALE_S = 3600


def stale_parts(dest, now=None):
    """The leftover .part folders of make_romfs_patches.py runs in dest."""
    now = time.time() if now is None else now
    found = []
    for folder in dest.iterdir() if dest.is_dir() else ():
        if not (folder.is_dir() and not folder.is_symlink() and PART_FOLDER.match(folder.name)):
            continue
        try:
            newest = max([folder.stat().st_mtime] + [f.stat().st_mtime for f in folder.rglob('*')])
        except OSError:
            continue
        if now - newest > PART_STALE_S:
            found.append(folder)
    return found


def hud_width(dest):
    """The selected HUD cap, with the same default as runtime/cfg.c."""
    try:
        text = (dest / 'totk.ini').read_text(encoding='utf-8-sig', errors='replace')
    except OSError:
        text = ''
    value = (screens.ini_value(text, 'hud_width') or 'full').lower()
    return value if value in ('16:9', '21:9') else 'full'


def shape_hud(size, hud):
    """The selected cap when at least 1% narrower than the shape, as make_romfs_patches.py requires."""
    w, h = (int(v) for v in size.split('x'))
    cap = {'16:9': 16 / 9, '21:9': 21 / 9}.get(hud)
    return hud if cap and cap < w / h * 0.99 else None


class Shapes(Step):
    """romfs-patches-<w>x<h> (the UI laid out for a shape, make_romfs_patches.py --aspect) and, with Xbox, PlayStation or
    PC prompts
    in totk.ini, romfs-patches-<style>-<w>x<h> (that layout with those prompts), for the shapes this player needs:
    those of the connected displays and of the totk.ini output_res, one folder per shape (installer/screens.py; the
    game maps its resolution to the folder in runtime/romfs.c overlay_init). A setup before this step wrote all 15
    sizes for every prompts style, 64 folders. The key holds the shapes, prompts style and selected HUD width, so a run after a
    monitor or settings change (or its rebuild mode) writes only these again; the game also starts the generator for a
    folder its settings need and the install lacks, at start and from its menu (runtime/romfs.c romfs_folders_prepare,
    make_romfs_patches.py --background), whose unfinished .part folders the clean-up below leaves alone."""

    id = 'shapes'

    def wanted(self, ctx):
        if not hasattr(ctx, 'shapes_wanted'):
            ctx.shapes_wanted = screens.wanted(ctx.dest)
        return ctx.shapes_wanted

    def key(self, ctx):
        sizes, style = self.wanted(ctx)
        # The pc prompts' shaped folders are drawn from the same bindings as romfs-patches-pc, so they follow its key
        prompts = ctx.keys.get('pc_prompts', '') if style == 'pc' else ''
        # Without a capped folder to write (hud_width full, or no shape wider than the cap) the key stays the one a
        # setup before hud_width wrote, so an existing install does not prepare its shaped folders again
        hud = hud_width(ctx.dest)
        if not any(shape_hud(size, hud) for size in sizes):
            return fingerprint('shapes-2', ctx.keys.get('prepare', ''), prompts, style or 'nintendo', *sizes)
        return fingerprint('shapes-3', ctx.keys.get('prepare', ''), prompts, style or 'nintendo', hud, *sizes)

    def current(self, ctx, record):
        return not record.get('failed') and all((ctx.dest / name / 'manifest.tsv').is_file()
                                               for name in record.get('folders', []))

    def run(self, ctx):
        layout, rep, dest = ctx.layout, ctx.reporter, ctx.dest
        sizes, style = self.wanted(ctx)
        rep.log(f'shapes: displays {screens.displays()}, layouts for {sizes or "none"}, prompts {style or "nintendo"}')
        for old in dest.iterdir() if dest.is_dir() else ():
            if old.is_dir() and SHAPED_FOLDER.match(old.name):
                remove(old)
        version = version_tag(game_version(ctx))
        plain = dest / f'romfs-patches-{style}' if style else None
        folders, failed = [], None
        hud = hud_width(dest)
        for i, size in enumerate(sizes):
            rep.progress('shapes', 'layouts', i, len(sizes), 'shapes', force=True)
            cap = shape_hud(size, hud)
            # Keep the full layout as a fallback, plus only the cap this player selected for this shape.
            for variant in (None, cap) if cap else (None,):
                suffix = '-hud' + variant.replace(':', 'x') if variant else ''
                hud_args = ['--hud', variant] if variant else []
                out = dest / f'romfs-patches-{size}{suffix}'
                code, log = run_tool(ctx, 'shapes', layout.tools / 'make_romfs_patches.py',
                                     ['--version', version, '--romfs', dest / 'romfs', '--out', out, '--aspect', size,
                                      *hud_args])
                if code:
                    raise InstallError('prepare.romfs_patches_failed', code=code, log=str(log))
                folders.append(out.name)
                # Reuse the selected prompts' fonts/messages/table; PC layouts retain the player's bindings.
                if not plain or not (plain / 'manifest.tsv').is_file():
                    continue
                out = dest / f'romfs-patches-{style}-{size}{suffix}'
                code, log = run_tool(ctx, f'shapes-{style}', layout.tools / 'make_romfs_patches.py',
                                     ['--version', version, '--romfs', dest / 'romfs', '--out', out, '--prompts', style,
                                      '--aspect', size, *hud_args, '--reuse', plain,
                                      *(pc_binds(dest) if style == 'pc' else ())])
                if code:
                    remove(out)
                    failed = failed or (code, log)
                else:
                    folders.append(out.name)
        rep.progress('shapes', 'layouts', len(sizes), len(sizes), 'shapes', force=True)
        if failed:
            rep.warning('warn.xbox_prompts_failed', code=failed[0], log=str(failed[1]))
        return {'folders': folders, 'sizes': sizes, 'style': style, 'hud': hud, 'failed': bool(failed)}


# ---- 7. NVIDIA DLSS

# NVIDIA's DLSS library, which NGX loads from the folder of totk.exe (runtime/vk_dlaa.c). The setup does not ship it:
# with --dlss it downloads NVIDIA's own copy from the DLSS SDK repository (NVIDIA RTX SDK license) at a pinned tag and
# checks its hash. The game runs without it; dlaa and the DLSS modes then do nothing.
DLSS_DLL = 'nvngx_dlss.dll'
DLSS_VERSION = '310.9.1'
DLSS_URL = f'https://raw.githubusercontent.com/NVIDIA/DLSS/v{DLSS_VERSION}/lib/Windows_x86_64/rel/{DLSS_DLL}'
DLSS_SIZE = 58956912
DLSS_SHA256 = '3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983'
DLSS_LICENSE = 'https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt'


def nvidia_rtx(host):
    """An NVIDIA RTX GPU among the host check's Vulkan devices: the GPUs DLSS runs on."""
    return any('NVIDIA' in d['name'].upper() and 'RTX' in d['name'].upper() for d in (host or {}).get('devices', []))


class Dlss(Step):
    """With --dlss on Windows and an NVIDIA RTX GPU: nvngx_dlss.dll next to totk.exe. A DLSS library the player put
    there (another version) is kept. A failed download warns and is tried again on the next run; the game runs
    without it."""

    id = 'dlss'

    def key(self, ctx):
        if not getattr(ctx.options, 'dlss', False):
            return fingerprint('dlss-1', 'off')
        return fingerprint('dlss-1', DLSS_VERSION, DLSS_SHA256)

    def current(self, ctx, record):
        if record.get('failed'):
            return False
        return not record.get('installed') or (ctx.dest / DLSS_DLL).is_file()

    def run(self, ctx):
        if not getattr(ctx.options, 'dlss', False):
            return {}
        if os.name != 'nt':
            return {'skipped': 'not_windows'}
        if not nvidia_rtx(getattr(ctx, 'host', None)):
            # The box is ticked by default, so most players without an RTX GPU never chose it: a log line, no warning
            ctx.reporter.log('dlss: no NVIDIA RTX GPU among the Vulkan devices, nothing downloaded')
            return {'skipped': 'no_rtx'}
        rep, target = ctx.reporter, ctx.dest / DLSS_DLL
        if target.is_file():
            if file_sha256(target) == DLSS_SHA256:
                return {'installed': DLSS_VERSION}
            rep.warning('warn.dlss_kept', path=str(target))
            return {'kept': True}
        part = ctx.temp / (DLSS_DLL + '.part')
        rep.progress('dlss', 'download', 0, DLSS_SIZE, 'bytes', force=True)
        rep.log(f'dlss: {DLSS_URL} (license {DLSS_LICENSE})')
        try:
            part.parent.mkdir(parents=True, exist_ok=True)
            h, done = hashlib.sha256(), 0
            with urllib.request.urlopen(DLSS_URL, timeout=60) as r, open(part, 'wb') as f:
                while block := r.read(1 << 20):
                    f.write(block)
                    h.update(block)
                    done += len(block)
                    rep.progress('dlss', 'download', min(done, DLSS_SIZE), DLSS_SIZE, 'bytes')
            if done != DLSS_SIZE or h.hexdigest() != DLSS_SHA256:
                raise OSError(f'{done} bytes, SHA-256 {h.hexdigest()[:16]}, not the expected file')
            replace_file(part, target)
            remove(part)
        # urllib's errors, timeouts and a bad file; a connection cut mid-file raises http.client.IncompleteRead,
        # which is not an OSError
        except (OSError, http.client.HTTPException) as e:
            remove(part)
            rep.warning('warn.dlss_download_failed', url=DLSS_URL, detail=str(e)[:300])
            return {'failed': True}
        return {'installed': DLSS_VERSION}


# ---- 8. AMD FSR

# AMD's FSR 4 for scene_upscaler = fsr4 (runtime/vk_ffx.c): the ffx-api loader and the upscaler effect, which the
# loader finds next to itself. AMD's FSR SDK license lets them be redistributed in binary form with its notice
# (THIRD_PARTY_NOTICES.md); the setup still does not ship them: with --fsr it downloads AMD's signed copies from the SDK
# repository at a pinned tag and checks their hashes. The game runs without them; scene_upscaler = fsr4 then does
# nothing.
FSR_VERSION = '2.4.0'   # the FSR SDK release; the upscaler DLL is FSR 4.1.2 (FSR 3.1.6 inside goes unused)
FSR_BASE = (f'https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v{FSR_VERSION}/'
            'Kits/FidelityFX/signedbin/')
FSR_FILES = (   # name, size, SHA-256
    ('amd_fidelityfx_loader_vk.dll', 39184, '38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9'),
    ('amd_fidelityfx_upscaler_vk.dll', 36135696, '4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7'),
)
FSR_LICENSE = f'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v{FSR_VERSION}/docs/license.md'


class Fsr(Step):
    """With --fsr on Windows: AMD's FSR loader and upscaler next to totk.exe; the game pins FSR 4 where they offer it
    for the GPU. Libraries the player put there (other versions) are kept. A failed download warns and is tried again
    on the next run; the game runs without them."""

    id = 'fsr'

    def key(self, ctx):
        if not getattr(ctx.options, 'fsr', False):
            return fingerprint('fsr-2', 'off')
        return fingerprint('fsr-2', FSR_VERSION, *(sha for _, _, sha in FSR_FILES))

    def current(self, ctx, record):
        if record.get('failed'):
            return False
        return not record.get('installed') or all((ctx.dest / name).is_file() for name, _, _ in FSR_FILES)

    def run(self, ctx):
        if not getattr(ctx.options, 'fsr', False):
            return {}
        if os.name != 'nt':
            return {'skipped': 'not_windows'}
        rep = ctx.reporter
        present = [ctx.dest / name for name, _, _ in FSR_FILES if (ctx.dest / name).is_file()]
        if present:
            if all((ctx.dest / name).is_file() and file_sha256(ctx.dest / name) == sha for name, _, sha in FSR_FILES):
                return {'installed': FSR_VERSION}
            # A set the player put there (or half of one): kept as it is, the loader and its effect belong together
            rep.warning('warn.fsr_kept', path=str(present[0]))
            return {'kept': True}
        total, done = sum(size for _, size, _ in FSR_FILES), 0
        rep.progress('fsr', 'download', 0, total, 'bytes', force=True)
        rep.log(f'fsr: {FSR_BASE} (license {FSR_LICENSE})')
        parts = []
        try:
            for name, size, sha in FSR_FILES:
                url, part = FSR_BASE + name, ctx.temp / (name + '.part')
                parts.append(part)
                part.parent.mkdir(parents=True, exist_ok=True)
                h, got = hashlib.sha256(), 0
                with urllib.request.urlopen(url, timeout=60) as r, open(part, 'wb') as f:
                    while block := r.read(1 << 20):
                        f.write(block)
                        h.update(block)
                        got += len(block)
                        rep.progress('fsr', 'download', min(done + got, total), total, 'bytes')
                if got != size or h.hexdigest() != sha:
                    raise OSError(f'{name}: {got} bytes, SHA-256 {h.hexdigest()[:16]}, not the expected file')
                done += size
            # both downloaded and checked: only then into the install folder, so it never holds half a set
            for (name, _, _), part in zip(FSR_FILES, parts):
                replace_file(part, ctx.dest / name)
                remove(part)
        # urllib's errors, timeouts and a bad file; a connection cut mid-file raises http.client.IncompleteRead,
        # which is not an OSError
        except (OSError, http.client.HTTPException) as e:
            for part in parts:
                remove(part)
            rep.warning('warn.fsr_download_failed', url=FSR_BASE, detail=str(e)[:300])
            return {'failed': True}
        return {'installed': FSR_VERSION}


# ---- 9. clean up

class Cleanup(Step):
    id = 'cleanup'
    always = True

    def run(self, ctx):
        rep, dest = ctx.reporter, ctx.dest
        rep.progress('cleanup', 'ini', force=True)
        try:
            ini.write(dest, ctx.lang, ctx.layout.ini_example)
        except OSError as e:
            raise InstallError('cleanup.ini_failed', detail=str(e)) from None
        ctx.state['lang'] = ctx.lang
        (dest / 'profile').mkdir(exist_ok=True)
        rep.progress('cleanup', 'temp', force=True)
        for stale in [*dest.glob('update-merge-*'), *stale_parts(dest)]:
            remove(stale)
        for name in ('check', 'unpacked', 'shader_list.tsv', 'shared-fonts'):   # gen/ and build/ stay for the next rebuild
            remove(ctx.temp / name)
        romfs_problem(dest / 'romfs', expected_update(ctx))
        write_rebuild_command(ctx)
        return {}


# Puts the release folder or the build kit (the argument after -c, made absolute: the kit's is relative to the install
# folder) on sys.path and runs installer.gui with the rest. `-m installer.gui` finds the package only through the
# release Python's ._pth: the Linux release's Python and the dev-mode embeddable one lack that entry, and totk.exe
# starts the command in the install folder.
REBUILD_BOOT = ('import os, runpy, sys; sys.path.insert(0, os.path.abspath(sys.argv.pop(1))); '
                "runpy.run_module('installer.gui', run_name='__main__', alter_sys=True)")


def rebuild_command(ctx):
    """The command that starts the setup window in its rebuild mode on this install folder (installer/gui.py
    --rebuild): pythonw beside the setup's Python on Windows, so no console opens. In a repository that is the Python
    running this setup: the tools' embeddable Python there has no tkinter.
    In the Linux AppImage the release folder is the AppImage's mount (/tmp/.mount_*), which goes away when the setup
    exits, so a path into it would fail to start later and the game would keep its old code. The command starts the
    AppImage file itself instead ($APPIMAGE, set by the AppImage runtime); its AppRun passes the arguments to
    installer.gui."""
    layout = ctx.layout
    appimage = os.environ.get('APPIMAGE', '')
    if not layout.dev and appimage and Path(appimage).is_file():
        return [str(Path(appimage).resolve()), '--rebuild', str(ctx.dest)]
    python = Path(sys.executable) if layout.dev else layout.python
    if os.name == 'nt' and python.with_name('pythonw.exe').is_file():
        python = python.with_name('pythonw.exe')
    root, dest = str(layout.root), str(ctx.dest)
    if layouts.uses_kit(layout) and layout.root.resolve() == layouts.kit_folder(ctx.dest).resolve():
        # the build kit: paths relative to the install folder, which totk.exe resolves against its own folder and
        # starts the command in, so a moved install folder keeps its rebuild
        python = Path(layouts.KIT) / python.relative_to(layout.root)
        root, dest = layouts.KIT, '.'
    return [str(python), '-c', REBUILD_BOOT, root, '--root', root, '--rebuild', dest]


def write_rebuild_command(ctx):
    """build-cache/rebuild.txt in the install folder: rebuild_command, one argument per line, for totk.exe."""
    cache = ctx.dest / 'build-cache'
    cache.mkdir(exist_ok=True)
    tmp = cache / 'rebuild.txt.tmp'
    tmp.write_text('\n'.join(rebuild_command(ctx)) + '\n', encoding='utf-8', newline='\n')
    os.replace(tmp, cache / 'rebuild.txt')


def romfs_problem(romfs, expected):
    """Raise cleanup.romfs_unreadable unless romfs/ still holds the verified image. The game opens romfs.bin at start;
    an install whose romfs/ went empty and unreadable after the extraction verified it (a folder Windows was still
    deleting, #107) reported success and the game stopped at its first file read."""
    try:
        ok = extract.romfs_verified(romfs, expected) and (romfs / 'romfs.bin').stat().st_size == expected['romfs']['size']
        if ok:
            with open(romfs / 'romfs.bin', 'rb') as f:
                f.read(1)
    except (OSError, ValueError):
        ok = False
    if not ok:
        raise InstallError('cleanup.romfs_unreadable', path=str(romfs))


def all_steps():
    return [Check(), Kit(), Fonts(), Extract(), Lift(), Compile(), Prepare(), PcPrompts(), Shapes(), Dlss(), Fsr(),
            Cleanup()]
