"""Run-profile copying and CLI rejection with disposable marker files; never launches a game."""
import contextlib
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import run_game as run  # noqa: E402


class ProfileTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.folder = Path(tmp.name)
        self.profile = self.folder / 'profile'
        self.profile.mkdir()
        (self.profile / 'marker').write_text('original profile')
        source = patch.object(run, 'PROFILE', self.profile)
        source.start()
        self.addCleanup(source.stop)

    def test_overlapping_copies_fail_before_cleanup_or_copying(self):
        for target in (self.profile, self.profile.parent, self.profile / 'nested'):
            with self.subTest(target=target), \
                 patch.object(run.shutil, 'rmtree', side_effect=AssertionError('cleanup ran before overlap check')), \
                 patch.object(run.shutil, 'copytree', side_effect=AssertionError('copy ran before overlap check')):
                with self.assertRaisesRegex(ValueError, 'profile'):
                    run.profile_copy(target, None)
                self.assertEqual((self.profile / 'marker').read_text(), 'original profile')

    def test_separate_copy_refreshes_only_the_disposable_destination(self):
        target = self.folder / 'build' / 'prof-run'
        target.mkdir(parents=True)
        (target / 'stale').write_text('old copy')
        run.profile_copy(target, None)
        self.assertEqual((self.profile / 'marker').read_text(), 'original profile')
        self.assertEqual((target / 'marker').read_text(), 'original profile')
        self.assertFalse((target / 'stale').exists())

    def test_path_like_tags_are_rejected_before_files_or_locks_are_touched(self):
        for tag in ('../escape', '../../profile', '../../../profile', '..\\escape', 'nested/run', '/absolute'):
            with self.subTest(tag=tag), \
                 patch.object(sys, 'argv', ['run_game.py', str(self.folder / 'build'), '--seconds', '1', '--tag', tag]), \
                 patch.object(Path, 'mkdir', side_effect=AssertionError('created a folder before checking tag')), \
                 patch.object(run, 'take_slot', side_effect=AssertionError('took a slot before checking tag')), \
                 contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    run.main()
                self.assertEqual(error.exception.code, 2)

    def test_build_inside_the_original_profile_is_rejected_before_writes(self):
        with patch.object(sys, 'argv', ['run_game.py', str(self.profile), '--seconds', '1']), \
             patch.object(Path, 'mkdir', side_effect=AssertionError('created a folder before overlap check')), \
             contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as error:
                run.main()
            self.assertEqual(error.exception.code, 2)
        self.assertEqual((self.profile / 'marker').read_text(), 'original profile')

    def test_normal_run_cleans_its_copy_and_keeps_the_source(self):
        build = self.folder / 'build'
        proc = unittest.mock.Mock()
        proc.wait.return_value = 0
        proc.poll.return_value = 0
        with patch.object(sys, 'argv', ['run_game.py', str(build), '--seconds', '1', '--tag', 'bench-1', '--show']), \
             patch.object(run, 'take_slot', return_value=None), patch.object(run, 'count', return_value=0), \
             patch.object(run.subprocess, 'Popen', return_value=proc) as launch, \
             contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(SystemExit) as error:
                run.main()
            self.assertEqual(error.exception.code, 0)
        self.assertEqual((self.profile / 'marker').read_text(), 'original profile')
        self.assertFalse((build / 'prof-bench-1').exists())
        self.assertIn('rc=0', (build / 'logs' / 'bench-1.log').read_text())
        launch.assert_called_once()
        proc.terminate.assert_not_called()

    def test_interrupted_wait_stops_the_owned_child_before_cleanup(self):
        child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        proxy = unittest.mock.Mock(wraps=child)
        interrupted = False

        def wait(*args, **kwargs):
            nonlocal interrupted
            if not interrupted:
                interrupted = True
                raise KeyboardInterrupt
            return child.wait(*args, **kwargs)
        proxy.wait.side_effect = wait
        build = self.folder / 'build'
        remove = run.shutil.rmtree

        def remove_after_exit(path, *args, **kwargs):
            if Path(path) == build / 'prof-run':
                self.assertIsNotNone(child.poll(), 'profile cleanup ran while the child was alive')
            return remove(path, *args, **kwargs)
        try:
            with patch.object(sys, 'argv', ['run_game.py', str(build), '--seconds', '1', '--show']), \
                 patch.object(run, 'take_slot', return_value=None), patch.object(run, 'count', return_value=0), \
                 patch.object(run.shutil, 'rmtree', side_effect=remove_after_exit), \
                 patch.object(run.subprocess, 'Popen', return_value=proxy):
                with self.assertRaises(KeyboardInterrupt):
                    run.main()
            self.assertIsNotNone(child.poll(), 'interrupted run left its child alive')
            self.assertFalse((build / 'prof-run').exists())
            self.assertEqual((self.profile / 'marker').read_text(), 'original profile')
        finally:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=5)

    def test_unresponsive_child_is_killed_after_termination_timeout(self):
        proc = unittest.mock.Mock()
        proc.poll.return_value = None
        proc.wait.side_effect = [subprocess.TimeoutExpired('owned child', 5), 0]
        run.stop_process(proc)
        proc.terminate.assert_called_once()
        proc.kill.assert_called_once()
        self.assertEqual(proc.wait.call_args_list, [unittest.mock.call(timeout=5), unittest.mock.call()])


if __name__ == '__main__':
    unittest.main(verbosity=2)
