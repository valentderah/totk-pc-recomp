"""The seven setup steps of docs/installer.md: check, fonts, extract, lift, compile, prepare, cleanup.

Extraction runs in this process through tools/extract.py (the package's sibling tools/, which ships with it); fonts
runs the vendored extract_shared_fonts.py on the firmware folder; lift, compile and prepare run the bundled Python on
recomp/gen.py, tools/build_game.py, tools/make_romfs_patches.py and tools/prepare_shaders.py, whose output goes to
<temp>/logs/<step>-<tool>.log and is read for progress. No Win32 call here: the host check is a small C program built
with the bundled clang (hostcheck.c).

Install folder: exefs/, romfs/ (romfs.bin, index.json), profile/, shared-fonts/ (Chinese and Korean text), totk.exe
and its DLLs, romfs-index.bin, romfs-patches/, romfs-patches-xbox/, romfs-patches-xbox-label/, totk_xlate.exe, the
shader state of totk.exe --prepare-shaders, totk.ini, totk-install.json.
Temp folder (<temp>/<dest name>-<hash>): check/ (ExeFS of both inputs, the host check), gen/ (C), build/ (objects),
logs/. The cleanup keeps gen/, build/ and logs/: the next run lifts into the same gen/, where recomp/gen.py rewrites
only the chunks whose C changed, and Ninja in build/ recompiles only those. A change of the mods' code cheats then
costs the lift (about a minute) and the chunks it touches, not a full compile.

Install folder build-cache/: cheats.status (tools/cheats.py), exefs.status (tools/exefs_patches.py) and rebuild.txt, the command that starts this setup in
its rebuild mode on the install folder (installer/gui.py --rebuild), one argument per line; totk.exe runs it at start
when an enabled mod has code cheats the build lacks (runtime/cheats.c).
"""
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

from . import ini
from .layout import EXE, SUPPORTED, version_tag
from .runner import InstallError, Step

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
import extract  # noqa: E402
import shader_list  # noqa: E402
from extract_exefs import (common_tickets, container_entries, load_keys, nca_ctr_key, nca_inventory,  # noqa: E402
                           program_nca)

GiB = 1 << 30
DEST_EXTRA = 4 * GiB      # beside the RomFS: shader and pipeline caches (about 2 GB), exefs, totk.exe, seed files
TEMP_NEED = 3 * GiB       # generated C (about 0.9 GB) and objects
HOSTCHECK = Path(__file__).resolve().parent / 'hostcheck.c'
STAMP = '.install-key'


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


def linux_kernel():
    """(major, minor) of the running Linux kernel, None elsewhere."""
    if not sys.platform.startswith('linux'):
        return None
    m = re.match(r'(\d+)\.(\d+)', os.uname().release)
    return (int(m.group(1)), int(m.group(2))) if m else None


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
    while not p.exists():
        p = p.parent
    return shutil.disk_usage(p).free, os.stat(p).st_dev


def raise_if_damaged(error, name, path):
    """InstallError check.input_damaged when a reader's ValueError says the file's data fails its hashes or ends early
    (extract.DAMAGE_TEXT)."""
    if extract.DAMAGE_TEXT.search(str(error)):
        raise InstallError('check.input_damaged', input=name, path=str(path), detail=str(error)) from None


def game_version(ctx):
    """The update this install is built from: what the check identified, else the recorded one. An install made before
    1.4.3 support records no version, and that install is 1.2.1: name it here rather than leaning on SUPPORTED's order."""
    return ctx.state.get('version', '1.2.1')


def expected_update(ctx):
    return extract.load_expected()[game_version(ctx)]


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
        if getattr(ctx, 'host', None) is not None:
            return
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
        r = subprocess.run([str(exe)], capture_output=True, text=True, errors='replace')
        try:
            host = json.loads(r.stdout)
        except ValueError:
            raise InstallError('check.host_probe_failed', code=r.returncode) from None
        ctx.reporter.log(f'host: {r.stdout.strip()}')
        if not host['x86_64_v2']:
            raise InstallError('check.cpu_too_old')
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
        total = 0
        for name, is_update in (('base', False), ('update', True)):
            path = Path(getattr(o, name))
            with path.open('rb') as f:
                total += self.open_input(f, path, keys, name, is_update)
        del keys
        return total

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
                extract.extract_exefs(Path(getattr(o, name)), Path(o.keys), out, logs, update=is_update)
            except extract.DamagedFile as e:
                raise InstallError('check.input_damaged', input=name, path=str(e.path), detail=e.detail) from None
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
        base, update, keys = Path(o.base), Path(o.update), Path(o.keys)
        inputs = {}
        for name, path in (('base', base), ('update', update)):
            known = ctx.state['inputs'].get(name, {})
            st = path.stat()
            if known.get('size') == st.st_size and known.get('mtime_ns') == st.st_mtime_ns and known.get('sha256'):
                inputs[name] = known
                continue
            digest = file_sha256(path, lambda d, t, n=name: rep.progress('extract', 'hash_' + n, d, t, 'bytes'))
            inputs[name] = {'name': path.name, 'size': st.st_size, 'mtime_ns': st.st_mtime_ns, 'sha256': digest}
        ctx.state['inputs'] = inputs
        ctx.save()

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
                raise InstallError('check.input_damaged', input='update', path=str(e.path), detail=e.detail) from None
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
            raise InstallError('check.input_damaged', input='base' if e.path == base else 'update', path=str(e.path),
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


def exefs_status(ctx):
    """The build's ExeFS patch status file (tools/exefs_patches.py --status), which runtime/mods.c reads."""
    return ctx.dest / 'build-cache' / 'exefs.status'


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
        files = cheat_files(ctx) + exefs_files(ctx)
        return fingerprint('lift-4', ctx.keys['extract'], ctx.layout.recomp, ctx.layout.patches(game_version(ctx)),
                           ctx.layout.tools / 'cheats.py', ctx.layout.tools / 'exefs_patches.py', *files,
                           *(f.relative_to(ctx.dest / 'mods').as_posix() for f in files))

    def current(self, ctx, record):
        gen = ctx.temp / 'gen'
        stamp = gen / STAMP
        return (gen / 'layout.json').is_file() and stamp.is_file() and stamp.read_text().strip() == ctx.keys['lift']

    def run(self, ctx):
        # gen/ of the last run stays: gen.py rewrites only the chunks whose C changed (and removes the ones a smaller
        # partition no longer has), so Ninja recompiles only those
        layout, rep, gen = ctx.layout, ctx.reporter, ctx.temp / 'gen'
        (gen / STAMP).unlink(missing_ok=True)
        rep.progress('lift', 'discover', force=True)

        def parse(line):
            m = EMITTED.match(line)
            if m:
                rep.progress('lift', 'emit', int(m.group(1)), int(m.group(2)), 'chunks')

        # The code cheats of the mods' cheat files and the mods' ExeFS patch files become patch sets (tools/cheats.py,
        # tools/exefs_patches.py); their status files tell the runtime what the build has. Both are the player's
        # downloads, so files the tools refuse cost those cheats or patches, not the install: the game is built
        # without them and the setup warns.
        plain = layout.patches(game_version(ctx))
        stages = [('cheats.py', 'patches-cheats.txt', cheats_status(ctx), cheat_files(ctx), 'warn.cheats_skipped'),
                  ('exefs_patches.py', 'patches-exefs.txt', exefs_status(ctx), exefs_files(ctx), 'warn.exefs_skipped')]
        patches, skipped = plain, {}
        for tool, out, status, inputs, warn in stages:
            status.unlink(missing_ok=True)   # no status: the runtime treats every such file as not built
            if not inputs:
                continue
            code, log = run_tool(ctx, 'lift', layout.tools / tool,
                                 ['--exefs', ctx.dest / 'exefs', '--mods', ctx.dest / 'mods', '--patches', patches,
                                  '--out', ctx.temp / out, '--status', status])
            if code:
                status.unlink(missing_ok=True)
                skipped[warn] = log
            else:
                patches = ctx.temp / out
        args = ['--exefs', ctx.dest / 'exefs', '--out', gen]
        code, log = run_tool(ctx, 'lift', layout.recomp / 'gen.py', args + ['--patches', patches], parse)
        if code and patches != plain:
            for tool, out, status, inputs, warn in stages:
                if inputs and warn not in skipped:
                    status.unlink(missing_ok=True)
                    skipped[warn] = log
            code, log = run_tool(ctx, 'lift', layout.recomp / 'gen.py', args + ['--patches', plain], parse)
        if code:
            raise InstallError('lift.failed', code=code, log=str(log))
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
    clang = subprocess.run([str(layout.clang), '--version'], capture_output=True, text=True).stdout
    tools = [layout.tools / n for n in ('build_game.py', 'romfs_index.py', 'check_content.py')]
    return fingerprint('runtime-1', cpu, layout.runtime_lib_for(cpu), layout.runtime, layout.deps or 'system',
                       layout.sysroot or 'host', *tools, clang, *[d.name for d in layout.dlls])


class Compile(Step):
    id = 'compile'

    def key(self, ctx):
        return fingerprint('compile-3', ctx.keys['lift'], runtime_key(ctx))

    def current(self, ctx, record):
        exe = ctx.dest / ('totk' + EXE)
        if not exe.is_file() or not (ctx.dest / 'romfs-index.bin').is_file():
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
        lib = layout.runtime_lib_for(cpu)
        code, log = run_tool(ctx, 'compile', layout.tools / 'build_game.py', [
            '--gen', ctx.temp / 'gen', '--out', build, '--toolchain', layout.llvm, '--cpu', cpu,
            *(['--deps', layout.deps] if layout.deps else []), *(['--sysroot', layout.sysroot] if layout.sysroot else []),
            '--runtime', layout.runtime, *(['--runtime-lib', lib] if lib else ()), '--ninja', layout.ninja,
            '--version', version_tag(game_version(ctx)), '--expected', layout.expected(game_version(ctx)),
            '--exefs', dest / 'exefs', '--romfs', dest / 'romfs',
            '--profile', dest / 'profile'], parse)
        exe = build / ('totk' + EXE)
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


# The game resolutions of another shape than 16:9 that runtime/ui/menu.cpp row_output_res lists: 21:9 and 32:9 monitors,
# and 16:10 screens (the Steam Deck's 1280x800, handhelds and laptops)
ASPECT_SIZES = ('2560x1080', '3440x1440', '3840x1600', '5120x2160', '3840x1080', '5120x1440',
                '1280x800', '1680x1050', '1920x1200', '2560x1600')


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
        # The same overlay with the UI laid out for each game resolution of another shape the settings menu offers
        # (runtime/romfs.c overlay_init picks the one that matches; the camera's side is the patch set aspect,
        # recomp/patches-v<ver>.txt).
        for size in ASPECT_SIZES:
            remove(dest / f'romfs-patches-{size}')
            code, log = run_tool(ctx, 'prepare', layout.tools / 'make_romfs_patches.py',
                                 ['--version', version_tag(game_version(ctx)), '--romfs', dest / 'romfs', '--out',
                                  dest / f'romfs-patches-{size}', '--aspect', size])
            if code:
                raise InstallError('prepare.romfs_patches_failed', code=code, log=str(log))
        # The same with Xbox button icons in the prompts, for prompts = xbox (make_romfs_patches.py --prompts):
        # romfs-patches-xbox for the Nintendo pad layout, romfs-patches-xbox-label for xbox and auto (runtime/input_map.c
        # input_prompts_folder). Both are written on every install, so the menu choices work without another run of
        # the setup. Optional: when one fails, the game keeps the Switch prompts with that layout and the menu says the
        # files are missing, so the setup warns once and goes on.
        failed = None
        for style in ('xbox', 'xbox-label'):
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
            raise InstallError('prepare.shaders_failed', code=code, log=str(log))
        if skipped[0]:
            rep.warning('warn.shaders_skipped', count=skipped[0], log=str(log))

        # One shader-cache.pack instead of four files per shader (~117,000): totk.exe reads the pack first.
        rep.progress('prepare', 'pack', force=True)
        code, log = run_tool(ctx, 'prepare', layout.tools / 'pack_shader_cache.py', [dest / 'shader-cache'])
        if code:
            raise InstallError('prepare.pack_failed', code=code, log=str(log))
        return {'xlate_sha256': file_sha256(xlate)}


# ---- 7. clean up

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
        for stale in dest.glob('update-merge-*'):
            remove(stale)
        for name in ('check', 'shader_list.tsv', 'shared-fonts'):   # gen/ and build/ stay for the next rebuild
            remove(ctx.temp / name)
        romfs_problem(dest / 'romfs', expected_update(ctx))
        write_rebuild_command(ctx)
        return {}


# Puts the release folder (the argument after -c) on sys.path and runs installer.gui with the rest. `-m installer.gui`
# finds the package only through the release Python's ._pth: the Linux release's Python and the dev-mode embeddable one
# lack that entry, and totk.exe starts the command in the install folder.
REBUILD_BOOT = ('import runpy, sys; sys.path.insert(0, sys.argv.pop(1)); '
                "runpy.run_module('installer.gui', run_name='__main__', alter_sys=True)")


def rebuild_command(ctx):
    """The command that starts the setup window in its rebuild mode on this install folder (installer/gui.py
    --rebuild): pythonw beside the setup's Python on Windows, so no console opens. In a repository that is the Python
    running this setup: the tools' embeddable Python there has no tkinter."""
    layout = ctx.layout
    python = Path(sys.executable) if layout.dev else layout.python
    if os.name == 'nt' and python.with_name('pythonw.exe').is_file():
        python = python.with_name('pythonw.exe')
    return [str(python), '-c', REBUILD_BOOT, str(layout.root), '--root', str(layout.root), '--rebuild', str(ctx.dest)]


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
    return [Check(), Fonts(), Extract(), Lift(), Compile(), Prepare(), Cleanup()]
