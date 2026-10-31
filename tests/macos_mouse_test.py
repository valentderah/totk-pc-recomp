"""Check frame-rate independent camera input, click edges and focus clearing."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MouseInputTests(unittest.TestCase):
    def test_native_input(self):
        with tempfile.TemporaryDirectory(prefix='totk-mouse-') as tmp:
            binary = Path(tmp) / 'mouse-test'
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', ROOT / 'runtime/macos',
                            ROOT / 'runtime/macos/mouse_input.c', ROOT / 'tests/macos_mouse_test.c',
                            '-o', binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
