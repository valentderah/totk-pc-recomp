"""PGO tool discovery honors an explicit toolchain, local toolchains, then PATH."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import pgo  # noqa: E402


class PgoToolTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='pgo tools ')
        self.addCleanup(tmp.cleanup)
        self.folder = Path(tmp.name)
        self.root = self.folder / 'checkout'
        self.path_bin = self.folder / 'system tools'
        self.root.mkdir()
        self.path_bin.mkdir()
        self.root_patch = patch.object(pgo, 'ROOT', self.root)
        self.root_patch.start()
        self.addCleanup(self.root_patch.stop)
        self.path_patch = patch.dict(os.environ, PATH=str(self.path_bin))
        self.path_patch.start()
        self.addCleanup(self.path_patch.stop)

    def tool(self, folder):
        folder.mkdir(parents=True, exist_ok=True)
        tool = folder / f'llvm-profdata{pgo.EXE}'
        tool.touch()
        tool.chmod(0o755)
        return tool

    def test_path_tool_is_found_when_no_local_toolchain_exists(self):
        tool = self.tool(self.path_bin)
        self.assertEqual(pgo.llvm_profdata(None), tool)

    def test_explicit_toolchain_takes_precedence_over_path(self):
        self.tool(self.path_bin)
        explicit = self.folder / 'chosen toolchain'
        tool = self.tool(explicit / 'bin')
        self.assertEqual(pgo.llvm_profdata(explicit), tool)

    def test_newest_local_toolchain_takes_precedence_over_path(self):
        self.tool(self.path_bin)
        self.tool(self.root / 'toolchain' / 'llvm-mingw-20261001' / 'llvm-mingw-old' / 'bin')
        tool = self.tool(self.root / 'toolchain' / 'llvm-mingw-20261020' / 'llvm-mingw-new' / 'bin')
        self.assertEqual(pgo.llvm_profdata(None), tool)

    def test_missing_explicit_tool_does_not_silently_use_another_compiler_version(self):
        self.tool(self.path_bin)
        explicit = self.folder / 'missing toolchain'
        with self.assertRaises(SystemExit) as error:
            pgo.llvm_profdata(explicit)
        self.assertIn(str(explicit / 'bin' / f'llvm-profdata{pgo.EXE}'), str(error.exception))
        self.assertIn('pass --toolchain', str(error.exception))

    def test_missing_default_tool_keeps_the_diagnostic(self):
        with self.assertRaises(SystemExit) as error:
            pgo.llvm_profdata(None)
        self.assertIn(f'llvm-profdata{pgo.EXE} not found: pass --toolchain', str(error.exception))


if __name__ == '__main__':
    unittest.main()
