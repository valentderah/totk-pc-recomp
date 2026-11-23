"""The performance log summary reports total frames divided by elapsed time.

    python tests/perf_summary_test.py
"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def window(frame, fps, count=60):
    # Synthetic text in runtime/perf.c perf_present's format; no game log is read.
    line = (f'perf: frame {frame}: interval avg {1000 / fps:.2f} ms max 50.00 ms ({fps:.1f} fps); '
            'render thread blocked: pacing 0% fence 0% drain 0% pipeline build 0% shader xlate 0% texture 0%')
    return line + (f'; replay avg 1.00 ms; ab 0/{count}; world 20.0 s' if count is not None else '') + '\n'


class PerfSummaryTest(unittest.TestCase):
    def summarize(self, text, lo=0, hi=1000):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / 'perf.log'
            log.write_text(text, encoding='utf-8')
            result = subprocess.run([sys.executable, str(ROOT / 'tools' / 'perf_summary.py'),
                                     str(log), str(lo), str(hi)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_equal_frame_windows_use_their_combined_duration(self):
        # 120 frames in 0.6 + 2.4 seconds = 40 fps, not (100 + 25) / 2.
        out = self.summarize(window(60, 100) + window(120, 25))
        self.assertIn('mean fps 40.0, min window fps 25.0, max frame 50.0 ms', out)

    def test_partial_window_uses_its_logged_frame_count(self):
        # 30 frames in 0.3 seconds and 60 in 2.4 seconds = 33.3 fps.
        out = self.summarize(window(60, 100, 30) + window(120, 25))
        self.assertIn('mean fps 33.3,', out)

    def test_older_logs_without_counts_keep_equal_window_weights(self):
        out = self.summarize(window(60, 100, None) + window(120, 25, None))
        self.assertIn('mean fps 40.0,', out)

    def test_frame_bounds_only_include_the_selected_windows(self):
        text = window(60, 100) + window(120, 50) + window(180, 25) + window(240, 200)
        out = self.summarize(text, lo=60, hi=180)
        self.assertIn('SUMMARY frames 60-180: mean fps 33.3, min window fps 25.0,', out)
        self.assertNotIn('perf 60:', out)
        self.assertNotIn('perf 240:', out)

    def test_constant_rate_is_unchanged(self):
        out = self.summarize(window(60, 60) + window(120, 60))
        self.assertIn('mean fps 60.0, min window fps 60.0,', out)

    def test_no_perf_windows_does_not_print_a_frame_summary(self):
        self.assertNotIn('SUMMARY frames', self.summarize('unrelated diagnostic\n'))


if __name__ == '__main__':
    unittest.main()
