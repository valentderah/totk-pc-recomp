import sys
from pathlib import Path
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'recomp'))
from layout import native_layout, FIRST_BASE, ADDRESS_LIMIT, ALIGN


class NativeLayoutTests(unittest.TestCase):
    def test_order_padding_and_ipc_limit(self):
        modules = native_layout({'sdk': 0x12001, 'subsdk0': 0x10000, 'main': 0x810001})
        self.assertEqual([m[0] for m in modules], ['main', 'subsdk0', 'sdk'])
        self.assertEqual(modules[0], ('main', FIRST_BASE, 0x818000))
        for (_, base, size), (_, next_base, _) in zip(modules, modules[1:]):
            self.assertEqual(base % ALIGN, 0)
            self.assertGreaterEqual(next_base, base + size)
        self.assertLess(modules[-1][1] + modules[-1][2], ADDRESS_LIMIT)

    def test_bad_images_rejected(self):
        for sizes in ({'main': 0}, {'main': -1}, {'other': 1}, {'main': ADDRESS_LIMIT}):
            with self.subTest(sizes=sizes), self.assertRaises(ValueError): native_layout(sizes)


if __name__ == '__main__': unittest.main()
