"""Fault injection against the actual APFS save publisher, without game data."""
import ctypes
import errno
import os
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Store(ctypes.Structure):
    _fields_ = [(name, ctypes.c_char * 1024) for name in ('committed', 'root', 'work', 'stage', 'previous')]


@unittest.skipUnless(platform.system() == 'Darwin', 'APFS/macOS test')
class NativeSaveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        out = ROOT / 'build/macos-tests'
        out.mkdir(parents=True, exist_ok=True)
        lib = out / 'save_store.dylib'
        subprocess.run(['xcrun', 'clang', '-dynamiclib', '-arch', 'arm64', '-std=c11', '-O2',
                        '-Wall', '-Wextra', '-Werror', str(ROOT / 'runtime/macos/save_store.c'),
                        '-o', str(lib)], check=True)
        cls.lib = ctypes.CDLL(str(lib), use_errno=True)
        cls.lib.native_save_open.argtypes = [ctypes.POINTER(Store), ctypes.c_char_p, ctypes.c_char_p]
        cls.lib.native_save_commit.argtypes = [ctypes.POINTER(Store)]

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT / 'build/macos-tests')
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.store = Store()
        self.open()

    def open(self):
        self.assertEqual(self.lib.native_save_open(ctypes.byref(self.store),
                         os.fsencode(self.base / 'committed'), os.fsencode(self.base / 'transaction')), 0)
        self.work = Path(os.fsdecode(self.store.work))
        self.committed = Path(os.fsdecode(self.store.committed))

    def write(self, value):
        (self.work / 'slot').mkdir(exist_ok=True)
        (self.work / 'slot/progress').write_text(value)
        (self.work / 'slot/caption').write_text(value)

    def commit(self):
        return self.lib.native_save_commit(ctypes.byref(self.store))

    def assert_generation(self, value):
        self.assertEqual((self.committed / 'slot/progress').read_text(), value)
        self.assertEqual((self.committed / 'slot/caption').read_text(), value)

    def test_commit_and_copy_on_write(self):
        self.write('old'); self.assertEqual(self.commit(), 0)
        self.write('new'); self.assert_generation('old')
        self.assertEqual(self.commit(), 0); self.assert_generation('new')

    def test_failures_preserve_commit_and_retry(self):
        self.write('old'); self.assertEqual(self.commit(), 0)
        for step in ('copy', 'flush', 'publish'):
            with self.subTest(step=step):
                self.write(step)
                os.environ['TOTK_SAVE_FAIL_AT'] = step
                try:
                    self.assertEqual(self.commit(), -1)
                    self.assertEqual(ctypes.get_errno(), errno.ENOSPC)
                finally:
                    del os.environ['TOTK_SAVE_FAIL_AT']
                self.assert_generation('old')
        self.assertEqual(self.commit(), 0); self.assert_generation('publish')

    def test_post_publish_failure_is_not_acknowledged(self):
        for step in ('published','barrier'):
            with self.subTest(step=step):
                self.write('old'); self.assertEqual(self.commit(), 0); self.write('new')
                os.environ['TOTK_SAVE_FAIL_AT'] = step
                try: self.assertEqual(self.commit(), -1)
                finally: del os.environ['TOTK_SAVE_FAIL_AT']
                self.assert_generation('new')
                self.assertEqual(self.commit(), 0)

    def test_process_crash_has_complete_generation(self):
        for step in ('copy', 'flush', 'publish', 'published'):
            with self.subTest(step=step):
                self.write('old'); self.assertEqual(self.commit(), 0); self.write('new')
                child = os.fork()
                if child == 0:
                    os.environ['TOTK_SAVE_CRASH_AT'] = step
                    self.commit()
                    os._exit(99)
                _, status = os.waitpid(child, 0)
                self.assertEqual(os.waitstatus_to_exitcode(status), 86)
                self.open()
                self.assert_generation('new' if step == 'published' else 'old')

    def test_clean_restarts_do_not_accumulate_recoveries(self):
        self.write('saved'); self.assertEqual(self.commit(), 0)
        for _ in range(5): self.open()
        self.assertEqual(list((self.base / 'transaction').glob('recovery-*')), [])

    def test_interrupted_work_is_preserved(self):
        self.write('old'); self.assertEqual(self.commit(), 0); self.write('uncommitted')
        self.open(); self.assert_generation('old')
        recoveries = list((self.base / 'transaction').glob('recovery-*/work/slot/progress'))
        self.assertEqual([p.read_text() for p in recoveries], ['uncommitted'])

    def test_symlink_is_rejected_without_modifying_target(self):
        self.write('old'); self.assertEqual(self.commit(), 0)
        target = self.base / 'outside'; target.write_text('untouched')
        (self.work / 'link').symlink_to(target)
        self.assertEqual(self.commit(), -1)
        self.assert_generation('old'); self.assertEqual(target.read_text(), 'untouched')


if __name__ == '__main__': unittest.main()
