"""Exercise actual binding inversion and cached glyph changes without game data."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class InputPromptTests(unittest.TestCase):
    def test_bindings_and_live_cache(self):
        with tempfile.TemporaryDirectory(prefix='totk-prompts-') as tmp:
            binary = Path(tmp) / 'prompts'
            sources = [ROOT / 'runtime/macos' / name for name in
                       ('input_prompts.c', 'prompt_raster.c', 'prompt_atlas.c')]
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', ROOT / 'runtime/macos',
                            *sources, ROOT / 'tests/macos_prompts_test.c', '-framework', 'CoreGraphics',
                            '-framework', 'CoreText', '-framework', 'CoreFoundation', '-o', binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
