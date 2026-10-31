"""Ninja must rebuild reverted patches and preserve unchanged source objects."""
import os
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from macos.build import BUILD, compile_runtime, digest, preserve_source_times, probe_headers, stage_sources
from macos.prepare import llvm_bin


class PreparationTests(unittest.TestCase):
    def test_explicit_llvm_directory_with_spaces(self):
        with tempfile.TemporaryDirectory(prefix='llvm tools ') as temporary:
            directory = Path(temporary)
            for name in ('llvm-objcopy', 'llvm-objdump'):
                tool = directory / name
                tool.write_text('#!/bin/sh\nexit 0\n')
                tool.chmod(0o755)
            with patch.dict(os.environ, LLVM_BIN=str(directory)):
                self.assertEqual(llvm_bin(), directory.resolve())

    def test_incomplete_explicit_llvm_fails_without_fallback(self):
        with tempfile.TemporaryDirectory() as temporary:
            with patch.dict(os.environ, LLVM_BIN=temporary):
                with self.assertRaisesRegex(RuntimeError, 'LLVM tools are missing'):
                    llvm_bin()

class SourceExportTests(unittest.TestCase):
    def check_export(self, removed=False):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            root = Path(temporary)
            source, exported = root / 'source', root / 'exported'
            source.mkdir(); exported.mkdir()
            for name, text in [('a.c', 'patched'), ('b.c', 'unchanged')]:
                (source / name).write_text(text)
            (root / 'build.ninja').write_text(
                'rule copy\n  command = cp $in $out\n'
                'build a.o: copy source/a.c\nbuild b.o: copy source/b.c\n')
            subprocess.run(['ninja'], cwd=root, check=True, capture_output=True)
            unchanged_time = (root / 'b.o').stat().st_mtime_ns
            (exported / 'a.c').write_text('original')
            (exported / 'b.c').write_text('unchanged')
            for file in exported.iterdir():
                os.utime(file, (946684800, 946684800))  # historical git archive time
            if removed:
                shutil.rmtree(source)
            preserve_source_times(exported, source)
            if source.exists():
                shutil.rmtree(source)
            exported.rename(source)
            result = subprocess.run(['ninja'], cwd=root, check=True, capture_output=True, text=True)
            self.assertIn('[1/2]' if removed else '[1/1]', result.stdout)
            self.assertEqual((root / 'a.o').read_text(), 'original')
            if not removed:
                self.assertEqual((root / 'b.o').stat().st_mtime_ns, unchanged_time)
            result = subprocess.run(['ninja'], cwd=root, check=True, capture_output=True, text=True)
            self.assertIn('no work to do', result.stdout)

    def test_patch_removal_rebuilds_only_changed_source(self):
        self.check_export()

    def test_deleted_export_cannot_reuse_stale_objects(self):
        self.check_export(removed=True)


class NativeLinkTests(unittest.TestCase):
    def test_game_host_links_without_diagnostic_objects(self):
        tree = stage_sources()
        # Empty, explicitly marked tables check the production linker graph.
        # They are never packaged, booted or retained as a playable build.
        with tempfile.TemporaryDirectory(dir=BUILD, prefix='link-fixture-') as temporary:
            out = Path(temporary)
            probe_headers(out)
            tables = out / 'runtime_tables.h'
            tables.write_text(tables.read_text().replace('#define TOTK_PROBE_BUILD 1\n', ''))
            headers = ('runtime_tables.h', 'romfs_config.h', 'shared_fonts_config.h', 'native_layout.h', 'native_prompt_textures.h')
            (out / 'preparation.json').write_text(json.dumps({
                'fixture': 'link-only, no game data', 'version': 'v143',
                'source_id': (tree.parent.parent / '.source-id').read_text(),
                'layout_sha256': digest(ROOT / 'recomp/layout.py'),
                'headers': {name: digest(out / name) for name in headers},
            }))
            binary = compile_runtime(tree, out, 'v143', 3, False)
            symbols = subprocess.check_output(['nm', str(binary)], text=True)
            self.assertNotIn('_runtime_gpu_path_probe', symbols)
            self.assertNotIn('_probe_pool_cache', symbols)
            # Runtime-discovered native entry tables must survive dead stripping.
            sections = subprocess.check_output(['otool', '-l', binary], text=True)
            self.assertIn('sectname totkgfx', sections)
            self.assertIn('sectname totknvnx', sections)


if __name__ == '__main__':
    unittest.main()
