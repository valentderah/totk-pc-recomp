import unittest
from analyze_present_cadence import summarize

class PresentCadenceTests(unittest.TestCase):
    def test_sixty_hz_and_slow_tail(self):
        rows=[dict(frame=i,ns=i*16666667) for i in range(1,202)]
        s=summarize(rows,121)
        self.assertAlmostEqual(s['mean_fps'],60,places=3)
        for index in (150,170,190):
            for r in rows[index:]:r['ns']+=100_000_000
        s=summarize(rows,121)
        self.assertGreater(s['p99_frame_ms'],50)
        self.assertLess(s['one_percent_low_fps'],20)
    def test_missing_or_nonmonotonic_frames_rejected(self):
        for rows in ([dict(frame=1,ns=10),dict(frame=3,ns=20)],[dict(frame=1,ns=20),dict(frame=2,ns=10)]):
            with self.assertRaises(ValueError):summarize(rows,1)
