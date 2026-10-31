"""Combined-container selection, ticket transport and release dependencies."""
import ast
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from game_inputs import InputKeys, TITLE, UPDATE_TITLE, select_program


def record(update=False, title=TITLE):
    header = bytearray(0xc00)
    header[0x404] = 4 if update else 3
    return {'title_id': title, 'content_type': 0}, bytes(header)


class SelectionTests(unittest.TestCase):
    def test_combined_and_separate_containers(self):
        base, update = record(), record(True)
        for rows in ([base, update], [update, base]):
            self.assertEqual(select_program(rows), base)
            self.assertEqual(select_program(rows, update=True), update)
        self.assertEqual(select_program([base]), base)
        self.assertEqual(select_program([update], update=True), update)

    def test_ambiguous_and_foreign_content_is_rejected(self):
        for rows in ([record(), record()], [record(), record(True), record(True)],
                     [record(), record(title='another-title')], [record(title=UPDATE_TITLE)]):
            with self.assertRaises(ValueError):
                select_program(rows)

    def test_missing_requested_program_is_rejected(self):
        with self.assertRaises(ValueError):
            select_program([record(True)])
        with self.assertRaises(ValueError):
            select_program([record()], update=True)

    def test_external_encrypted_key_is_preserved_and_conflicts_fail(self):
        rights, encrypted = bytes(range(16)), bytes(range(16, 32))
        item, header = record(True)
        header = bytearray(header); header[0x230:0x240] = rights
        keys = InputKeys.__new__(InputKeys)
        keys.system = {}; keys.titles = {rights.hex(): encrypted}
        with patch('game_inputs.nca_inventory', return_value=[(item, bytes(header))]):
            with patch('game_inputs.common_tickets', return_value={}):
                self.assertEqual(keys.tickets(None, []), {rights: encrypted})
            with patch('game_inputs.common_tickets', return_value={rights: bytes(16)}):
                with self.assertRaisesRegex(ValueError, 'conflicts'):
                    keys.tickets(None, [])


class ReleaseTests(unittest.TestCase):
    def test_packaged_extractor_imports_without_repository_tools(self):
        tree = ast.parse((ROOT / 'tools/make_release.py').read_text())
        entries = {node.targets[0].id: ast.literal_eval(node.value) for node in tree.body
                   if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name)
                   and node.targets[0].id in ('TOOLS_FILES', 'TOOLS_DIRS')}
        with tempfile.TemporaryDirectory() as temporary:
            tools = Path(temporary) / 'tools'; tools.mkdir()
            for name in entries['TOOLS_FILES']:
                shutil.copy2(ROOT / 'tools' / name, tools / name)
            for name in entries['TOOLS_DIRS']:
                shutil.copytree(ROOT / 'tools' / name, tools / name)
            subprocess.run([sys.executable, '-c', 'import extract; import game_inputs'],
                           cwd=temporary, env=dict(os.environ, PYTHONPATH=str(tools)),
                           check=True, capture_output=True)


@unittest.skipUnless((ROOT / 'build/macos-arm64/hactool/hactool').is_file(), 'native hactool is not built')
class HactoolKeyTests(unittest.TestCase):
    def check_keys(self, text, title_data=''):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            keys = folder / 'fixture.keys'; keys.write_text(text)
            target = folder / 'invalid.nso'; target.write_bytes(bytes(128))
            result = subprocess.run([ROOT / 'build/macos-arm64/hactool/hactool', '--disablekeywarns',
                                     '--suppresskeys', '-k', keys, '-t', 'nso', target],
                                    input=title_data, env=dict(os.environ, MNG_TITLE_KEYS='-'),
                                    capture_output=True, text=True)
            return result.stdout + result.stderr

    def test_comments_trailing_nul_and_stdin_are_accepted(self):
        fixture = 'header_key = ' + bytes(32).hex() + ' # inline comment\n'
        fixture += 'master_kek_source_00 = ' + bytes(17).hex() + '\n'
        fixture += 'mariko_master_kek_source_00 = ' + bytes(17).hex() + '  '
        output = self.check_keys(fixture, bytes(range(16)).hex() + ' = ' + bytes(16).hex() + '\n')
        self.assertNotIn('Invalid key encoding', output)
        self.assertIn('Failed to read NSO0 header', output)

    def test_invalid_key_value_is_not_printed(self):
        private = 'synthetic-private-value'
        output = self.check_keys('header_key = ' + private + '\n')
        self.assertIn('Invalid key encoding', output)
        self.assertNotIn(private, output)


if __name__ == '__main__':
    unittest.main()
