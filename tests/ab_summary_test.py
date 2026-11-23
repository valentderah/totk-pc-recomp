"""A/B log reports keep absolute timings when a percentage has a zero baseline."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def window(frame, interval, replay, b, n=60, world=20):
    return (f'perf: frame {frame}: interval avg {interval:.2f} ms max 100.00 ms ({1000 / interval:.1f} fps); '
            f'replay avg {replay:.2f} ms; ab {b}/{n}; world {world:.1f} s\n')


class AbSummaryTest(unittest.TestCase):
    def summarize(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / 'synthetic.log'
            log.write_text(text, encoding='utf-8')
            return subprocess.run([sys.executable, str(ROOT / 'tools' / 'ab_summary.py'), str(log)],
                                  capture_output=True, text=True)

    def test_no_replay_on_either_side_keeps_the_interval_comparison(self):
        run = self.summarize(window(60, 20, 0, 0) + window(120, 10, 0, 60))
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn('B - A: interval -10.00 ms (-50.0%), replay +0.00 ms (n/a: A is zero)', run.stdout)

    def test_replay_appearing_in_b_reports_its_absolute_cost(self):
        run = self.summarize(window(60, 10, 0, 0) + window(120, 12, 2, 60))
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn('replay +2.00 ms (n/a: A is zero)', run.stdout)

    def test_replay_disappearing_in_b_still_has_a_defined_percentage(self):
        run = self.summarize(window(60, 12, 2, 0) + window(120, 10, 0, 60))
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn('replay -2.00 ms (-100.0%)', run.stdout)

    def test_nonzero_means_remain_weighted_by_each_windows_frames(self):
        text = window(60, 10, 2, 0, n=30) + window(120, 20, 4, 0)
        text += window(180, 10, 2, 60)
        run = self.summarize(text)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn('interval -6.67 ms (-40.0%), replay -1.33 ms (-40.0%)', run.stdout)

    def test_load_and_mixed_windows_stay_excluded(self):
        text = window(60, 100, 100, 0, world=5) + window(120, 100, 100, 30)
        text += window(180, 10, 2, 0) + window(240, 12, 3, 60)
        run = self.summarize(text)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn('interval +2.00 ms (+20.0%), replay +1.00 ms (+50.0%)', run.stdout)

    def test_missing_side_keeps_the_diagnostic_and_failure_exit(self):
        run = self.summarize(window(60, 10, 2, 0))
        self.assertEqual(run.returncode, 1)
        self.assertIn('no whole A and B windows', run.stdout)
        self.assertEqual(run.stderr, '')


if __name__ == '__main__':
    unittest.main()
