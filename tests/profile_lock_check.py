"""Check shared-profile startup exclusion with private save transactions and real OS processes."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--deps', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='totk-profile-') as temp:
        folder = Path(temp)
        source = (ROOT / 'runtime/fs_host.c').read_text(encoding='utf-8')
        marker = '// ---- mounts and paths'
        if source.count(marker) != 1:
            raise RuntimeError('save startup source boundary changed')
        (folder / 'save_startup.c').write_text(source[:source.index(marker)], encoding='utf-8')
        exe = folder / ('check.exe' if os.name == 'nt' else 'check')
        command = [args.cc, '-O2', '-flto', '-march=x86-64-v3', '-ffunction-sections', '-fdata-sections',
                   '-Wl,--gc-sections', '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime/platform'),
                   '-I', str(folder)]
        if 'plat_profile_lock(' in (ROOT / 'runtime/platform/platform.h').read_text():
            command += ['-DPROFILE_LOCK_AVAILABLE']
        if args.deps:
            command += ['-I', str(args.deps / 'include'), '-L', str(args.deps / 'lib')]
        if os.name == 'nt':
            command += ['-D_WIN32_WINNT=0x0A00', '-Wl,--exclude-all-symbols']
        command += [str(ROOT / 'tests/profile_lock_check.c'), '-o', str(exe)]
        if os.name == 'nt':
            command += ['-lbcrypt', '-lpsapi', '-luser32', '-lwinmm', '-lole32', '-lshell32', '-luuid']
        else:
            command += ['-lpthread', '-ldl', '-lm']
        subprocess.run(command, check=True)
        profile = folder / 'profile'
        owner = subprocess.Popen([str(exe), str(profile), 'owner'], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while not (profile / 'owner-ready').exists():
                if owner.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('owner did not initialize its working copy')
                time.sleep(.02)
            contender = subprocess.run([str(exe), str(profile), 'recover'], capture_output=True, timeout=30)
            print('second startup exit:', contender.returncode, flush=True)
            print(contender.stdout.decode(errors='replace'), end='', flush=True)
            intact = (profile / 'save-tx/work/pending.txt').read_bytes() == b'uncommitted' if (profile / 'save-tx/work/pending.txt').exists() else False
            print('active working copy preserved:', intact, flush=True)
            if contender.returncode == 73:
                alias = str(profile) + os.sep + '..' + os.sep + profile.name
                result = subprocess.run([str(exe), alias, 'recover'], capture_output=True, timeout=30)
                assert result.returncode == 73, result.stdout
                subprocess.run([str(exe), str(folder / 'other-profile'), 'recover'], check=True, timeout=30)
                lock_file = profile / '.totk.lock'
                assert lock_file.read_bytes() == b''
                shutil.copyfile(lock_file, folder / 'lock-copy')
                if os.name == 'nt':
                    subprocess.run([str(exe), str(profile), 'copy'], check=True, timeout=30)
                print('alternate spelling refused; separate profile and lock-file copy allowed', flush=True)
            output, _ = owner.communicate(b'\n', timeout=10)
            print(output.decode(errors='replace'), end='', flush=True)
            if not intact or owner.returncode or contender.returncode != 73:
                raise SystemExit(1)
            subprocess.run([str(exe), str(profile), 'recover'], check=True, timeout=30)
            print('same profile available after owner exits', flush=True)
            (profile / 'owner-ready').unlink()
            owner = subprocess.Popen([str(exe), str(profile), 'owner'], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 10
            while not (profile / 'owner-ready').exists():
                if owner.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('crash fixture did not initialize')
                time.sleep(.02)
            owner.kill()
            owner.communicate(timeout=10)
            subprocess.run([str(exe), str(profile), 'recover'], check=True, timeout=30)
            print('same profile available after forced owner termination', flush=True)
            blocked = folder / 'file-instead-of-profile'
            blocked.write_text('keep')
            result = subprocess.run([str(exe), str(blocked), 'recover'], capture_output=True, timeout=30)
            assert result.returncode == 74, result.stdout
            assert blocked.read_text() == 'keep'
            print('invalid profile reports an I/O error without modifying it', flush=True)
        finally:
            if owner.poll() is None:
                owner.kill()
                owner.communicate(timeout=10)


if __name__ == '__main__':
    main()
