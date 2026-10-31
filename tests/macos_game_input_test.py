"""Exercise adapter preflight and camera ownership without game data."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GameInputTests(unittest.TestCase):
    def test_install_and_camera_owner(self):
        runtime = ROOT / 'build/macos-arm64/source/games/totk/runtime'
        self.assertTrue((runtime / 'runtime.h').is_file(), 'build the native source export first')
        with tempfile.TemporaryDirectory(prefix='totk-game-input-') as tmp:
            binary = Path(tmp) / 'input'
            subprocess.run(['xcrun', 'clang', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-DTOTK_GAME_V121=1',
                            '-I', runtime, '-I', ROOT / 'runtime/macos',
                            ROOT / 'tests/macos_game_input_test.c', '-o', binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
