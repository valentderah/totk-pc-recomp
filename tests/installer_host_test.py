"""Reject unmet runtime GPU requirements before extraction or compilation; no game inputs needed."""
import copy
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from installer import steps


class HostTest(unittest.TestCase):
    gpu = dict(name='fixture GPU', api=[1, 3, 0], extension_query_ok=True,
               missing_extensions=[], feature_query_ok=True, missing_features=[])

    def check(self, devices, **changes):
        host = dict(x86_64_v2=True, x86_64_v3=True, loader=True, devices=devices)
        host.update(changes)
        with tempfile.TemporaryDirectory() as tmp:
            layout = SimpleNamespace(clang=Path('clang'), deps=None, sysroot=None, dev=False)
            ctx = SimpleNamespace(layout=layout, temp=Path(tmp), reporter=SimpleNamespace(log=lambda _: None))
            with patch.object(steps, 'linux_glibc', return_value=None), \
                 patch.object(steps, 'linux_kernel', return_value=None), \
                 patch.object(steps.subprocess, 'run', side_effect=[
                     SimpleNamespace(returncode=0, stdout='', stderr=''),
                     SimpleNamespace(returncode=0, stdout=json.dumps(host), stderr='')]):
                steps.Check().host(ctx)
            return ctx

    def test_one_complete_device_is_sufficient(self):
        bad = dict(self.gpu, missing_features=['geometryShader'])
        ctx = self.check([bad, self.gpu])
        self.assertEqual(ctx.cpu, 'v3')
        self.assertEqual(ctx.host['devices'], [bad, self.gpu])
        self.assertEqual(self.check([self.gpu], x86_64_v3=False).cpu, 'v2')

    def test_each_missing_runtime_feature_is_rejected(self):
        features = ('timelineSemaphore', 'dynamicRendering', 'synchronization2',
                    'shaderStorageImageWriteWithoutFormat', 'depthClipControl', 'shaderInt64',
                    'samplerAnisotropy', 'independentBlend', 'geometryShader', 'textureCompressionBC')
        for name in features:
            with self.subTest(feature=name):
                with self.assertRaises(steps.InstallError) as caught:
                    self.check([dict(self.gpu, missing_features=[name])])
                self.assertEqual(caught.exception.id, 'check.gpu_requirements')
                self.assertIn(name, caught.exception.params['devices'])
                self.assertIn('fixture GPU', caught.exception.params['devices'])

    def test_each_missing_runtime_extension_is_rejected(self):
        for name in ('VK_KHR_swapchain', 'VK_KHR_push_descriptor', 'VK_EXT_depth_clip_control',
                     'VK_EXT_external_memory_host'):
            with self.subTest(extension=name):
                with self.assertRaises(steps.InstallError) as caught:
                    self.check([dict(self.gpu, missing_extensions=[name])])
                self.assertEqual(caught.exception.id, 'check.gpu_requirements')
                self.assertIn(name, caught.exception.params['devices'])

    def test_failed_or_absent_queries_are_rejected(self):
        for field in ('extension_query_ok', 'feature_query_ok'):
            for value in (False, None):
                with self.subTest(field=field, value=value):
                    with self.assertRaisesRegex(steps.InstallError, 'check.gpu_requirements'):
                        self.check([dict(self.gpu, **{field: value})])
            gpu = copy.deepcopy(self.gpu)
            del gpu[field]
            with self.assertRaisesRegex(steps.InstallError, 'check.gpu_requirements'):
                self.check([gpu])

    def test_requirements_cannot_be_combined_across_devices(self):
        old = dict(self.gpu, name='old GPU', api=[1, 2, 0])
        missing = dict(self.gpu, missing_features=['depthClipControl'])
        with self.assertRaisesRegex(steps.InstallError, 'check.gpu_requirements'):
            self.check([old, missing])

    def test_existing_cpu_loader_and_version_errors_are_preserved(self):
        cases = (([self.gpu], {'x86_64_v2': False}, 'check.cpu_too_old'),
                 ([], {'loader': False}, 'check.vulkan_missing'),
                 ([], {}, 'check.vulkan_no_device'),
                 ([dict(self.gpu, api=[1, 2, 0])], {}, 'check.vulkan_too_old'))
        for devices, changes, message in cases:
            with self.subTest(message=message):
                with self.assertRaisesRegex(steps.InstallError, message):
                    self.check(devices, **changes)


if __name__ == '__main__':
    unittest.main()
