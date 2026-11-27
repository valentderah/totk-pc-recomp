"""Only a successfully exited host probe can pass or identify a failing Vulkan layer.

HOSTCHECK_TEST_SOURCE_ROOT selects an unchanged checkout for the same before/after tests.
HOSTCHECK_TEST_CC selects clang for the optional Windows native subprocess fixture.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ.get('HOSTCHECK_TEST_SOURCE_ROOT', HERE.parent))
sys.path.insert(0, str(ROOT))
from installer import steps

GOOD = json.dumps(dict(x86_64_v2=True, x86_64_v3=True, loader=True, instance=0,
                       devices=[dict(name='synthetic', api=[1, 3, 0], type=1)]))
PARTIAL = '{"x86_64_v2": true, "x86_64_v3": true, "loader": true, '
CRASH = 0xc0000005


class ProbeExitTest(unittest.TestCase):
    def run_probe(self, answers, layers=()):
        results = [subprocess.CompletedProcess([], code, text, '') for code, text in answers]
        self.logs = []
        ctx = SimpleNamespace(reporter=SimpleNamespace(log=self.logs.append))
        with patch.object(steps.Check, 'run_probe', side_effect=results) as run, \
                patch.object(steps, 'implicit_vulkan_layers', return_value=layers):
            try:
                return steps.Check().probe_host(ctx, Path('hostcheck.exe'))
            finally:
                self.calls = run.call_count

    def test_complete_json_does_not_hide_initial_crash(self):
        with self.assertRaises(steps.InstallError) as caught:
            self.run_probe([(CRASH, GOOD), (CRASH, GOOD)])
        self.assertEqual(caught.exception.id, 'check.vulkan_crash')
        self.assertEqual(self.calls, 2)

    def test_crashed_retry_does_not_prove_implicit_layers_are_the_cause(self):
        with self.assertRaises(steps.InstallError) as caught:
            self.run_probe([(CRASH, PARTIAL), (CRASH, GOOD)])
        self.assertEqual(caught.exception.id, 'check.vulkan_crash')
        self.assertEqual(self.calls, 2)

    def test_crashed_candidate_does_not_name_an_innocent_layer(self):
        layers = [('VK_LAYER_TEST_first', Path('first.json')), ('VK_LAYER_TEST_second', Path('second.json'))]
        with self.assertRaises(steps.InstallError) as caught:
            self.run_probe([(CRASH, PARTIAL), (0, GOOD), (CRASH, GOOD), (0, GOOD)], layers)
        self.assertEqual(caught.exception.id, 'check.vulkan_layer_crash')
        self.assertEqual(caught.exception.params['disable'], 'VK_LAYER_TEST_second')
        self.assertEqual(self.calls, 4)

    def test_nonzero_exit_without_vulkan_does_not_pass(self):
        with self.assertRaises(steps.InstallError) as caught:
            self.run_probe([(1, '{"loader": false, "devices": []}')])
        self.assertEqual(caught.exception.id, 'check.host_probe_failed')
        self.assertEqual(self.calls, 1)


@unittest.skipUnless(os.name == 'nt', 'native fixture uses a Windows DLL')
class NativeProbeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = os.environ.get('HOSTCHECK_TEST_CC') or shutil.which('clang')
        if not cc:
            raise unittest.SkipTest('clang is required for the native fixture')
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.folder = Path(cls.temp.name)
        cls.exe = cls.folder / 'hostcheck.exe'
        for source, out, extra in ((ROOT / 'installer/hostcheck.c', cls.exe, []),
                                  (HERE / 'hostcheck_loader_fixture.c', cls.folder / 'vulkan-1.dll', ['-shared'])):
            compiled = subprocess.run([cc, '-O1', '-Wall', '-Wextra', '-Werror', *extra, str(source), '-o', str(out)],
                                      capture_output=True, text=True)
            if compiled.returncode:
                raise AssertionError(compiled.stdout + compiled.stderr)

    def test_healthy_loader_passes(self):
        ctx = SimpleNamespace(reporter=SimpleNamespace(log=lambda text: None))
        with patch.dict(os.environ, HOSTCHECK_FIXTURE='healthy'):
            host = steps.Check().probe_host(ctx, self.exe)
        self.assertEqual(host['devices'][0]['name'], 'Synthetic GPU')

    def test_actual_cleanup_crash_is_rejected_despite_complete_json(self):
        with patch.dict(os.environ, HOSTCHECK_FIXTURE='crash-destroy'):
            result = steps.Check().run_probe(self.exe)
            self.assertEqual(result.returncode & 0xffffffff, CRASH)
            self.assertEqual(json.loads(result.stdout)['devices'][0]['name'], 'Synthetic GPU')
            ctx = SimpleNamespace(reporter=SimpleNamespace(log=lambda text: None))
            with self.assertRaises(steps.InstallError) as caught:
                steps.Check().probe_host(ctx, self.exe)
        self.assertEqual(caught.exception.id, 'check.vulkan_crash')


if __name__ == '__main__':
    unittest.main()
