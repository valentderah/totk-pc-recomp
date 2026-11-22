"""Build command arguments through real Ninja and a POSIX shell, using only synthetic paths and source files."""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_game as build  # noqa: E402

NINJA, SHELL = shutil.which('ninja'), shutil.which('sh')


@unittest.skipUnless(NINJA, 'no Ninja on PATH')
class CommandTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='build command ')
        self.addCleanup(tmp.cleanup)
        self.folder = Path(tmp.name)
        self.probe = self.folder / 'arguments.py'
        self.probe.write_text('import json, pathlib, sys\n'
                              'pathlib.Path(sys.argv[1]).write_text(json.dumps(sys.argv[2:]))\n')

    def ninja(self, target, command, posix=False, bindings=''):
        if posix:
            # Ninja on Unix uses sh -c. An expanded response file lets Windows exercise those same shell bytes too.
            shell = subprocess.list2cmdline([SHELL]) if os.name == 'nt' else shlex.quote(SHELL)
            text = (f'rule probe\n  command = {shell.replace("$", "$$")} $out.rsp\n'
                    f'  rspfile = $out.rsp\n  rspfile_content = {command}\n')
        else:
            text = f'rule probe\n  command = {command}\n'
        (self.folder / 'build.ninja').write_text(bindings + text + f'build {target}: probe\n', encoding='utf-8')
        env = dict(os.environ, BUILD_QUOTE_TEST='expanded', MSYS2_ARG_CONV_EXCL='*')
        run = subprocess.run([NINJA, '-f', 'build.ninja', target], cwd=self.folder, env=env,
                             capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    @unittest.skipUnless(SHELL, 'no POSIX shell on PATH')
    def test_posix_arguments_survive_ninja_and_the_shell(self):
        values = ('ordinary/path', 'path with spaces', 'path/$BUILD_QUOTE_TEST/include',
                  'path with spaces/$BUILD_QUOTE_TEST/include', "path/O'Reilly/include",
                  'path/(parentheses)/include', 'path/double"quote/include',
                  'path/$(printf expanded)/include')
        for index, value in enumerate(values):
            with self.subTest(value=value), patch.object(build, 'WIN', False):
                target = f'posix-{index}.json'
                command = ' '.join(build.cmd_arg(arg) for arg in
                                   (Path(sys.executable).as_posix(), self.probe.as_posix(), target, value))
                self.ninja(target, command, posix=True)
                self.assertEqual(json.loads((self.folder / target).read_text()), [value])

    @unittest.skipUnless(os.name == 'nt', 'native Windows command parsing')
    def test_windows_paths_still_survive_native_ninja_commands(self):
        values = ['ordinary/path', 'path with spaces', 'path/$BUILD_QUOTE_TEST/include', "path/O'Reilly/include"]
        with patch.object(build, 'WIN', True):
            command = ' '.join(build.cmd_arg(arg) for arg in
                               (Path(sys.executable).as_posix(), self.probe.as_posix(), 'windows.json', *values))
        self.ninja('windows.json', command)
        self.assertEqual(json.loads((self.folder / 'windows.json').read_text()), values)

    @unittest.skipUnless(SHELL and (os.environ.get('CC') or shutil.which('clang')), 'needs a POSIX shell and clang')
    def test_compiler_finds_headers_in_a_path_with_shell_metacharacters(self):
        include = self.folder / "O'Reilly $BUILD_QUOTE_TEST (headers)"
        include.mkdir()
        (include / 'synthetic.h').write_text('#define SYNTHETIC_VALUE 42\n')
        source = self.folder / 'probe.c'
        source.write_text('#include <synthetic.h>\nint main(void) { return SYNTHETIC_VALUE != 42; }\n')
        compiler = os.environ.get('CC') or shutil.which('clang')
        target = 'probe.exe' if os.name == 'nt' else 'probe'
        with patch.object(build, 'WIN', False):
            command = f'{build.cmd_arg(Path(compiler).as_posix())} -I {build.cmd_path(include)} '
            command += f'{build.cmd_path(source)} -o {build.cmd_arg(target)}'
        self.ninja(target, command, posix=True)
        run = subprocess.run([str(self.folder / target)], timeout=10)
        self.assertEqual(run.returncode, 0)

    @unittest.skipUnless(SHELL and (os.environ.get('CC') or shutil.which('clang')), 'needs a POSIX shell and clang')
    def test_linker_writes_the_map_in_the_literal_build_directory(self):
        self.link_map(posix=True)

    @unittest.skipUnless(os.name == 'nt' and (os.environ.get('CC') or shutil.which('clang')), 'native Windows linker')
    def test_windows_linker_keeps_the_map_path_as_one_argument(self):
        self.link_map(posix=False)

    def link_map(self, posix):
        output = self.folder / "O'Reilly $BUILD_QUOTE_TEST (output), comma"
        output.mkdir()
        source = self.folder / 'link.c'
        source.write_text('int main(void) { return 0; }\n')
        compiler = os.environ.get('CC') or shutil.which('clang')
        target = 'link.exe' if os.name == 'nt' else 'link'
        with patch.object(build, 'WIN', not posix), patch.object(build, 'vma_include', return_value=self.folder), \
                patch.object(build, 'runtime_sources', return_value=[]):
            # Use production Ninja bindings, but link only our own tiny program, without runtime or game inputs.
            ninja, _, _ = build.ninja_file(self.folder, output, {}, 1)
            bindings = '\n'.join(line for line in ninja.splitlines()
                                 if line.startswith(('builddir = ', 'ldflags = '))) + '\n'
            command = (f'{build.cmd_arg(Path(compiler).as_posix())} {build.cmd_path(source)} '
                       f'$ldflags -o {build.cmd_arg(target)}')
        self.ninja(target, command, posix=posix, bindings=bindings)
        self.assertTrue((output / 'totk.map').is_file(), 'the linker must write the map at the requested path')
        self.assertIn('main', (output / 'totk.map').read_text())


if __name__ == '__main__':
    unittest.main(verbosity=2)
