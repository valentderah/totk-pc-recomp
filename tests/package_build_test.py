"""The real portable-packaging CLI on synthetic files and completed overlay folders; no game files or build needed.

Run: python tests/package_build_test.py
"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import package_build as package  # noqa: E402


class PackageTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.build, self.out = self.root / 'build', self.root / 'portable'
        self.extracted, self.xlate = self.root / 'extracted' / 'v143', self.root / 'translator'
        self.fonts = self.extracted.parent / 'firmware' / 'shared-fonts'
        for path in [self.build / f'totk{package.EXE}', self.xlate, self.build / 'romfs-index.bin',
                     self.extracted / 'exefs' / 'module',
                     self.fonts / 'font',
                     *(self.build / name for name in package.DLLS)]:
            self.put(path, b'synthetic input')
        self.overlay('romfs-patches')

    @staticmethod
    def put(path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def overlay(self, name, complete=True):
        folder = self.build / name
        self.put(folder / 'UI' / 'layout.bin', ('synthetic ' + name).encode())
        if complete:
            self.put(folder / 'manifest.tsv', b'# synthetic completed overlay\n')
        return folder

    def run_package(self, *extra, expected_returncode=0):
        command = [sys.executable, str(ROOT / 'tools' / 'package_build.py'), '--version', 'v143', '--build',
                   str(self.build), '--extracted', str(self.extracted), '--xlate', str(self.xlate), '--out',
                   str(self.out), *extra]
        result = subprocess.run(command, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, expected_returncode, result.stdout + result.stderr)
        return result

    def test_all_completed_prompt_and_shape_folders_are_preserved(self):
        names = ('romfs-patches-pc', 'romfs-patches-xbox', 'romfs-patches-xbox-label',
                 'romfs-patches-playstation', 'romfs-patches-playstation-xbox',
                 'romfs-patches-1280x800', 'romfs-patches-3440x1440-hud16x9',
                 'romfs-patches-pc-2560x1080', 'romfs-patches-playstation-xbox-3440x1440-hud21x9')
        for name in names:
            self.overlay(name)
        self.run_package()
        for name in ('romfs-patches', *names):
            with self.subTest(name=name):
                self.assertTrue((self.out / name / 'manifest.tsv').is_file())
                self.assertEqual((self.out / name / 'UI' / 'layout.bin').read_bytes(),
                                 (self.build / name / 'UI' / 'layout.bin').read_bytes())

    def test_partial_or_unpublished_folders_are_not_packaged(self):
        names = ('romfs-patches-pc.part-123', 'romfs-patches-3440x1440.part-stale-456')
        for name in names:
            self.overlay(name)   # a generator can write its manifest immediately before publishing
        self.overlay('romfs-patches-2560x1080', complete=False)
        self.put(self.build / 'romfs-patches-note.txt', b'not a directory')
        self.run_package()
        for name in (*names, 'romfs-patches-2560x1080', 'romfs-patches-note.txt'):
            self.assertFalse((self.out / name).exists(), name)

    def test_optional_cache_and_romfs_flags_are_respected(self):
        self.put(self.build / 'shader-cache' / 'entry', b'synthetic cache')
        self.put(self.extracted / 'romfs' / 'romfs.bin', b'synthetic image')
        self.run_package('--with-romfs', '--no-shader-cache')
        self.assertEqual((self.out / 'romfs' / 'romfs.bin').read_bytes(), b'synthetic image')
        self.assertFalse((self.out / 'shader-cache').exists())

    def test_shared_fonts_are_optional_when_they_were_not_extracted(self):
        (self.fonts / 'font').unlink()
        self.fonts.rmdir()
        self.run_package()
        self.assertTrue((self.out / f'totk{package.EXE}').is_file())
        self.assertTrue((self.out / 'exefs' / 'module').is_file())
        self.assertFalse((self.out / 'shared-fonts').exists())

    def test_repack_refreshes_layouts_and_keeps_the_profile(self):
        self.overlay('romfs-patches-pc')
        self.put(self.out / 'romfs-patches-pc' / 'UI' / 'layout.bin', b'old layout')
        self.put(self.out / 'profile' / 'save', b'kept profile')
        self.run_package()
        self.assertEqual((self.out / 'romfs-patches-pc' / 'UI' / 'layout.bin').read_bytes(),
                         b'synthetic romfs-patches-pc')
        self.assertEqual((self.out / 'profile' / 'save').read_bytes(), b'kept profile')

    def test_output_cannot_be_a_layout_or_inside_it(self):
        folder = self.overlay('romfs-patches-pc')
        before = {p.relative_to(folder): p.read_bytes() for p in folder.rglob('*') if p.is_file()}
        for out in (folder, folder / 'portable'):
            with self.subTest(out=out):
                self.out = out
                result = self.run_package(expected_returncode=1)
                self.assertIn('--out must not copy a source folder into itself', result.stderr)
                self.assertEqual({p.relative_to(folder): p.read_bytes() for p in folder.rglob('*') if p.is_file()},
                                 before)

    def test_every_tree_source_is_checked_before_any_copy(self):
        cache = self.build / 'shader-cache'
        self.put(cache / 'entry', b'synthetic cache')
        for source in (self.extracted / 'exefs', self.fonts, cache):
            for out in (source, source / 'portable', source.parent):
                with self.subTest(source=source, out=out):
                    command = ['package_build.py', '--build', str(self.build), '--extracted', str(self.extracted),
                               '--xlate', str(self.xlate), '--out', str(out)]
                    # Stop an unfixed run before it can recursively copy a fixture into itself.
                    with patch.object(sys, 'argv', command), \
                         patch.object(package, 'copy', side_effect=AssertionError('copy ran before overlap check')):
                        with self.assertRaisesRegex(SystemExit, '--out.*(?:source|build dir)'):
                            package.main()

    def test_cli_rejects_output_inside_each_tree_source_without_writing(self):
        cache = self.build / 'shader-cache'
        self.put(cache / 'entry', b'synthetic cache')
        for source in (self.extracted / 'exefs', self.fonts, cache):
            with self.subTest(source=source):
                before = {p.relative_to(source): p.read_bytes() for p in source.rglob('*') if p.is_file()}
                self.out = source / 'portable'
                result = self.run_package(expected_returncode=1)
                self.assertIn('--out must not copy a source folder into itself', result.stderr)
                self.assertFalse(self.out.exists())
                self.assertEqual({p.relative_to(source): p.read_bytes() for p in source.rglob('*') if p.is_file()}, before)

    def test_output_in_an_excluded_cache_is_allowed(self):
        cache = self.build / 'shader-cache'
        self.put(cache / 'entry', b'synthetic cache')
        self.out = cache / 'portable'
        self.run_package('--no-shader-cache')
        self.assertTrue((self.out / 'exefs' / 'module').is_file())
        self.assertFalse((self.out / 'shader-cache').exists())


if __name__ == '__main__':
    unittest.main()
