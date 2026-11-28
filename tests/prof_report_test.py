"""The sampling report resolves C++ symbols from an lld COFF map, not input-file rows."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import prof_report  # noqa: E402


def row(address, size, align, columns):
    return f'{address:08x} {size:08x} {align:5d} {columns}\n'


def symbol(address, name):
    # lld COFF LLDMapFile.cpp: two empty eight-character columns before the symbol.
    return row(address, 0, 0, ' ' * 16 + name)


class ProfReportTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.folder = Path(tmp.name)
        self.map = self.folder / 'synthetic.map'

    def write_map(self, text):
        self.map.write_text('Address  Size     Align Out     In      Symbol\n' + text, encoding='utf-8')

    def test_demangled_names_keep_parentheses_spaces_and_templates(self):
        names = ['sample_add(int)', 'sample_pair(int, int)', 'helpers::visit(std::pair<int, int> const&)']
        self.write_map(''.join(symbol(0x1000 + i * 0x100, name) for i, name in enumerate(names)))
        self.assertEqual(prof_report.load_map(self.map), ([0x1000, 0x1100, 0x1200], names))

    def test_section_and_object_rows_are_not_symbols_and_first_alias_wins(self):
        self.write_map(row(0x1000, 0x200, 4096, '.text') +
                       row(0x1000, 0x100, 16, ' ' * 8 + 'objects/a (copy).o:(.text)') +
                       symbol(0x1000, 'first(int)') + symbol(0x1000, 'alias(int)') +
                       row(0x1100, 0x100, 16, ' ' * 8 + 'b.o:(.text)') +
                       symbol(0x1100, 'plain_c_function'))
        self.assertEqual(prof_report.load_map(self.map),
                         ([0x1000, 0x1100], ['first(int)', 'plain_c_function']))

    def test_cli_attributes_samples_and_chains_to_the_cpp_functions(self):
        self.write_map(symbol(0x1000, 'runtime_helper') + symbol(0x1100, 'sample_add(int)') +
                       symbol(0x1200, 'sample_pair(int, int)'))
        profile = self.folder / 'synthetic.prof'
        profile.write_text('totk-prof 1\nbase 140000000\nsamples 10\n'
                           'module 140000000 4000 synthetic.exe\n'
                           'rip 140001104 6\nrip 140001204 4\n'
                           'stack 6 140001104 140001204\nstack 4 140001204\n', encoding='utf-8')
        run = subprocess.run([sys.executable, str(ROOT / 'tools' / 'prof_report.py'),
                              str(profile), str(self.map)], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn('60.00%         6  sample_add(int)', run.stdout)
        self.assertIn('40.00%         4  sample_pair(int, int)', run.stdout)
        self.assertIn('100.00%        10  sample_pair(int, int)', run.stdout)
        self.assertNotIn('runtime_helper', run.stdout)


if __name__ == '__main__':
    unittest.main()
