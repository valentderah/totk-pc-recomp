"""Concurrent callbacks must drain every immutable record before trace shutdown."""
import csv
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/macos-arm64/probes-v143/totk-native'


class FrameTraceTests(unittest.TestCase):
    def capture(self, mode):
        with tempfile.TemporaryDirectory(dir=ROOT / 'build') as temporary:
            trace = Path(temporary) / 'frames.csv'
            env = dict(os.environ, TOTK_FRAME_TRACE=str(trace), TOTK_NATIVE_4K144='1')
            subprocess.run([BINARY, mode], env=env, check=True, capture_output=True, timeout=20)
            with trace.open() as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 1024)
            self.assertEqual({int(row['ticket']) for row in rows}, set(range(1024)))
            for row in rows:
                self.assertEqual(row['ticket'], row['frame'])
                self.assertAlmostEqual(float(row['present_seconds']), int(row['frame']) / 144, places=8)
                self.assertEqual(row['strict'], '1')
                for key in ('scene_width', 'output_width', 'drawable_width'):
                    self.assertEqual(row[key], '3840')
                for key in ('scene_height', 'output_height', 'drawable_height'):
                    self.assertEqual(row[key], '2160')

    def test_concurrent_callbacks_and_shutdown(self):
        self.capture('--probe-trace')

    def test_window_close_drains_trace_before_exit(self):
        self.capture('--probe-trace-exit')

    def test_unwritable_capture_fails(self):
        env = dict(os.environ, TOTK_FRAME_TRACE=str(ROOT / 'build'))
        result = subprocess.run([BINARY, '--probe-trace'], env=env, capture_output=True, timeout=20)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b'open frame trace', result.stderr)


if __name__ == '__main__':
    unittest.main()
