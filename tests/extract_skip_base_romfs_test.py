"""tools/extract.py --skip-base-romfs: the 17 GB base RomFS is not extracted, and nothing else stops being checked.

    python tests/extract_skip_base_romfs_test.py

The base game's ExeFS still decides that the file is TotK 1.0.0, and the update merge still verifies all six IVFC
levels of what it takes from that file, so the flag trades a check that runs before the merge for one that runs during
it, and saves the 17 GB image no tool in the repository reads.
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import extract  # noqa: E402

EXPECTED = {'version': 0, 'display_version': '1.0.0',
            'exefs_sha256': {**{name: '0' * 64 for name in extract.EXEFS_CODE}, 'main.npdm': '0' * 64},
            'romfs': {'sha256': '1' * 64, 'size': 17_069_139_640}}


class SkipBaseRomfsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.out = Path(self.tmp.name) / 'v0'
        (self.out / 'exefs').mkdir(parents=True)
        for name in extract.EXEFS_CODE:   # an ExeFS that matches EXPECTED, so no extraction is needed
            (self.out / 'exefs' / name).write_bytes(b'')
        self.patch = mock.patch.object(extract, 'sha256', lambda path: '0' * 64)
        self.patch.start()
        self.addCleanup(self.patch.stop)
        self.addCleanup(self.tmp.cleanup)

    def run_base(self, *, skip_romfs, romfs_present=False):
        """extract_base with the two RomFS steps recorded, and the log it printed."""
        if romfs_present:
            (self.out / 'romfs').mkdir()
            (self.out / 'romfs' / 'index.json').write_text(json.dumps(
                {'romfs_sha256': '1' * 64, 'romfs_size': 17_069_139_640, 'files': []}))
            (self.out / 'romfs' / 'romfs.bin').write_bytes(b'')
        calls = []
        with mock.patch.object(extract, 'extract_base_romfs', lambda *a, **k: calls.append('extract')), \
             mock.patch.object(extract, 'check_romfs', lambda *a, **k: calls.append('check')), \
             mock.patch.object(extract, 'say', lambda message: calls.append(message)):
            extract.extract_base(Path('base.xci'), Path('prod.keys'), self.out, Path('logs'), EXPECTED,
                                 skip_romfs=skip_romfs)
        return calls

    def test_skipped_romfs_is_neither_extracted_nor_checked(self):
        calls = self.run_base(skip_romfs=True)
        self.assertNotIn('extract', calls)
        self.assertNotIn('check', calls)
        self.assertIn('  RomFS skipped (--skip-base-romfs)', calls)

    def test_the_exefs_check_still_runs_when_the_romfs_is_skipped(self):
        """The flag drops the RomFS only: the ExeFS is what identifies the base game."""
        calls = self.run_base(skip_romfs=True)
        self.assertIn('  ExeFS 1.0.0: main, sdk, subsdk0, rtld match', calls)

    def test_without_the_flag_the_romfs_is_extracted_and_checked(self):
        calls = self.run_base(skip_romfs=False)
        self.assertIn('extract', calls)
        self.assertIn('check', calls)
        self.assertNotIn('  RomFS skipped (--skip-base-romfs)', calls)

    def test_without_the_flag_a_verified_romfs_is_reused(self):
        calls = self.run_base(skip_romfs=False, romfs_present=True)
        self.assertNotIn('extract', calls)   # already there and verified: nothing to extract
        self.assertIn('check', calls)        # and still checked

    def test_a_base_game_that_is_not_totk_1_0_0_is_refused_even_when_skipping(self):
        """The negative control: skipping the RomFS must not turn the base game check into a no-op."""
        (self.out / 'exefs' / 'main').write_bytes(b'')
        with mock.patch.object(extract, 'sha256', lambda path: 'f' * 64), \
             mock.patch.object(extract, 'extract_exefs', lambda *a, **k: None), \
             mock.patch.object(extract, 'say', lambda message: None):
            with self.assertRaises(ValueError) as raised:
                extract.extract_base(Path('base.xci'), Path('prod.keys'), self.out, Path('logs'), EXPECTED,
                                     skip_romfs=True)
        self.assertIn('Base game is not TotK 1.0.0', str(raised.exception))


if __name__ == '__main__':
    unittest.main()
