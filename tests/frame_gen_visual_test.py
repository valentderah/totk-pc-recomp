"""Game-backed visual regressions for frame generation and DLSS.

The fast tests check the image and log readers themselves. The game tests run only when a complete build directory is
available, either through --exe or TOTK_FG_VISUAL_BUILD:

    python tests/frame_gen_visual_test.py --exe build/v143/totk.exe

The build must contain the extracted game data and the built shader caches. TOTK_FG_VISUAL_SCRIPT overrides the
camera sweep (tools/input/camera_sweep.txt), and TOTK_FG_VISUAL_SAVE names a folder under build/save-backup. Artifacts
stay under build/frame-gen-visual.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from PIL import Image, ImageStat

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build' / 'frame-gen-visual'


def mean_luma(path):
    with Image.open(path) as image:
        return ImageStat.Stat(image.convert('L')).mean[0]


def brightness_ratio(real, generated):
    real_mean = sum(mean_luma(p) for p in real) / len(real)
    generated_mean = sum(mean_luma(p) for p in generated) / len(generated)
    if real_mean <= 0:
        raise AssertionError('real frames are black; the run did not reach a rendered scene')
    return generated_mean / real_mean


def dlss_camera_resets(log):
    matches = re.findall(r'resets: \d+ size, (\d+) camera, \d+ target', log)
    if not matches:
        raise AssertionError('the log has no final DLSS reset line')
    return int(matches[-1])


def visual_build(exe=None):
    if exe:
        return exe.resolve().parent
    value = os.environ.get('TOTK_FG_VISUAL_BUILD')
    return Path(value).resolve() if value else None


def options():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--exe', type=Path)
    return parser.parse_known_args()


_OPTIONS, _UNITTEST_ARGS = options()
BUILD = visual_build(_OPTIONS.exe)
SCRIPT = Path(os.environ.get('TOTK_FG_VISUAL_SCRIPT', ROOT / 'tools' / 'input' / 'camera_sweep.txt'))


class ReaderTest(unittest.TestCase):
    def test_brightness_reader_accepts_matching_frames(self):
        with tempfile.TemporaryDirectory() as d:
            real, generated = [], []
            for name, value in (('real', 64), ('gen', 66)):
                path = Path(d) / f'{name}.png'
                Image.new('RGB', (2, 2), (value, value, value)).save(path)
                (real if name == 'real' else generated).append(path)
            self.assertAlmostEqual(brightness_ratio(real, generated), 66 / 64, places=2)

    def test_brightness_reader_rejects_double_encoded_frames(self):
        with tempfile.TemporaryDirectory() as d:
            paths = {}
            for name, value in (('real', 64), ('gen', 128)):
                paths[name] = Path(d) / f'{name}.png'
                Image.new('RGB', (2, 2), (value, value, value)).save(paths[name])
            self.assertGreater(brightness_ratio([paths['real']], [paths['gen']]), 1.35)

    def test_reset_reader_uses_the_last_dlaa_summary(self):
        log = 'resets: 1 size, 6 camera, 2 target\nresets: 2 size, 3 camera, 1 target\n'
        self.assertEqual(dlss_camera_resets(log), 3)


@unittest.skipUnless(BUILD, 'set TOTK_FG_VISUAL_BUILD or pass --exe for the game-backed visual checks')
class GameVisualTest(unittest.TestCase):
    def run_game(self, name, seconds, env, capture_at=None):
        if not (BUILD / 'totk.exe').is_file():
            self.skipTest(f'{BUILD / "totk.exe"} is missing')
        if not SCRIPT.is_file():
            self.skipTest(f'{SCRIPT} is missing')
        case = OUT / f'{name}-{os.getpid()}'
        shots = case / 'shots'
        shots.mkdir(parents=True)
        log = case / 'run.log'
        env = dict(env)
        if capture_at is not None:
            env['TOTK_FG_SHOT'] = f's{capture_at}'
            env['TOTK_FG_SHOT_DIR'] = str(shots)
        command = [sys.executable, str(ROOT / 'tools' / 'run_game.py'), str(BUILD), '--seconds', str(seconds),
                   '--tag', f'fgvisual-{name}-{os.getpid()}', '--log', str(log), '--script', str(SCRIPT)]
        save = os.environ.get('TOTK_FG_VISUAL_SAVE')
        if save:
            command += ['--save', save]
        for key, value in env.items():
            command += ['--env', f'{key}={value}']
        result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
        if result.returncode:
            self.fail(f'{name} exited {result.returncode}\n{result.stdout[-2000:]}\n{result.stderr[-4000:]}')
        text = log.read_text(encoding='utf-8', errors='replace')
        if 'frame step: world clock starts' not in text:
            self.fail(f'{name} did not reach the world\n{text[-4000:]}')
        return text, shots

    def test_fsr_generated_frames_have_real_frame_brightness(self):
        text, shots = self.run_game('fsr3-brightness', 12, {
            'TOTK_FRAME_GEN': 'fsr3',
            'TOTK_SCENE_UPSCALER': 'off',
            'TOTK_HDR': '0',
            'TOTK_FPS_LIMIT': '60',
        }, capture_at=8)
        real = sorted(shots.glob('swap_*_real.png'))
        generated = sorted(shots.glob('swap_*_gen.png'))
        self.assertTrue(real and generated, f'no frame captures in {shots}\n{text[-2000:]}')
        ratio = brightness_ratio(real, generated)
        self.assertGreaterEqual(ratio, 0.7, f'generated frames are too dark: {ratio:.3f}')
        self.assertLessEqual(ratio, 1.35, f'generated frames are too bright: {ratio:.3f}')

    def test_dlss_without_frame_generation_keeps_camera_resets(self):
        text, _ = self.run_game('dlss-camera', 32, {
            'TOTK_FRAME_GEN': 'off',
            'TOTK_SCENE_UPSCALER': 'dlss',
            'TOTK_DLSS': 'quality',
            'TOTK_FPS_LIMIT': '60',
        })
        # One reset of variation is expected across GLB loading and the first camera sweep; the regression doubled it.
        expected = int(os.environ.get('TOTK_FG_VISUAL_MAX_DLSS_CAMERA_RESETS', '4'))
        resets = dlss_camera_resets(text)
        self.assertLessEqual(resets, expected, f'DLSS history reset on camera {resets} times, expected at most {expected}')


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *_UNITTEST_ARGS])
