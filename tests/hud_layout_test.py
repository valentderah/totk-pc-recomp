"""Synthetic HUD layout and generator CLI regressions, without game assets.

Run with .venv-py314/Scripts/python tests/hud_layout_test.py.
"""
import ast
import compression.zstd as zstd
import json
import os
import importlib.util
import math
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / 'tools/make_romfs_patches.py'
spec = importlib.util.spec_from_file_location('ultrawide_layout', ROOT / 'tools/ultrawide_layout.py')
ui = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ui)


def archive():
    """Build shipped pane fields and animation keys; no copyrighted archive data."""
    layouts = {name: {'RootPane': {}} for name in ui.KEEP_ROOT | ui.MARKER_ROOT.keys()}
    for name, panes in ui.MARKER_ROOT.items():
        for pane in panes: layouts[name][pane] = {}
    for name, pane, field, value in ui.edits(1, None):
        layouts.setdefault(name, {'RootPane': {}}).setdefault(pane, {})[field] = value
    data, members = bytearray(), {}
    for name, panes in sorted(layouts.items()):
        blob = bytearray(20)
        blob[:4] = b'FLYT'
        struct.pack_into('<H', blob, 6, 20)
        struct.pack_into('<H', blob, 16, len(panes))
        for pane, fields in panes.items():
            section = bytearray(80)
            section[:4] = b'pan1'
            struct.pack_into('<I', section, 4, 80)
            section[12:12 + len(pane)] = pane.encode()
            struct.pack_into('<2f', section, 68, 1, 1)
            for field, value in fields.items(): struct.pack_into('<f', section, ui.FIELD[field], value)
            blob += section
        members[f'blyt/{name}.bflyt'] = (len(data), len(data) + len(blob))
        data += blob
    for anim in {row[0] for row in ui.anim_edits(1, None)}:
        blob = bytearray(3600)
        for name, offset, shipped, _ in ui.anim_edits(1, None):
            if name == anim: struct.pack_into('<f', blob, offset, shipped)
        members[f'anim/{anim}.bflan'] = (len(data), len(data) + len(blob))
        data += blob
    return data, members


def value(data, members, name, pane, field):
    lo, hi = members[f'blyt/{name}.bflyt']
    return struct.unpack_from('<f', data, ui.panes(data, lo, hi)[pane] + ui.FIELD[field])[0]


class HudLayoutTest(unittest.TestCase):
    def test_caps_move_panes_and_animation_together(self):
        for cap, extension in ((None, 960), (21 / 9, 300), (16 / 9, 0)):
            with self.subTest(cap=cap):
                data, members = archive()
                ui.apply(data, members, 3840, 1080, cap)
                self.assertAlmostEqual(value(data, members, 'PlayerStatus_00', 'N_HeartPos_00', 'x'), -extension)
                self.assertAlmostEqual(value(data, members, 'EnvironmentInfo_00', 'RootPane', 'x'), extension / 2)
                lo, _ = members['anim/AppMap_00_MiniMap.bflan']
                for offset in (872, 880, 1628, 1636, 3264, 3272, 3468, 3476):
                    self.assertAlmostEqual(struct.unpack_from('<f', data, lo + offset)[0], 765 + extension)
                self.assertAlmostEqual(value(data, members, 'SaveLoadIcon_00', 'RootPane', 'x'), -extension / 2)
                self.assertEqual(value(data, members, 'GameOver_00', 'P_BG_00', 'scale_x'), 2)
                self.assertEqual(value(data, members, 'AttentionLockOn_00', 'RootPane', 'scale_x'), 1)
                self.assertEqual(value(data, members, 'AttentionLockOn_00', 'N_Offset_00', 'scale_x'), .5)

    def test_unnecessary_caps_equal_full(self):
        full, members = archive()
        ui.apply(full, members, 3440, 1440)
        for cap in (3440 / 1440, 4, 3440 / 1440 * .995):
            data, members = archive()
            ui.apply(data, members, 3440, 1440, cap)
            self.assertEqual(data, full)

    def test_invalid_parameters_do_not_mutate(self):
        for width, height, cap in ((0, 1080, None), (3840, 0, None), (3840, 1080, 1), (3840, 1080, math.nan)):
            data, members = archive()
            before = bytes(data)
            with self.assertRaises(ValueError): ui.apply(data, members, width, height, cap)
            self.assertEqual(data, before)

    def test_cli_rejects_invalid_before_output(self):
        with tempfile.TemporaryDirectory() as folder:
            out = Path(folder) / 'output'
            for arguments in (['--aspect', 'bad'], ['--aspect', '0x1440'], ['--aspect', '1920x1080'],
                              ['--hud', '16:9'], ['--aspect', '1280x800', '--hud', '16:9']):
                result = subprocess.run([sys.executable, str(TOOL), '--out', str(out), *arguments], capture_output=True, text=True)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn('error:', result.stderr)
                self.assertNotIn('Traceback', result.stderr)
                self.assertFalse(out.exists())

    def test_default_names_and_explicit_output(self):
        # Execute only argument handling; input loading and archive generation remain untouched.
        source = TOOL.read_text().split("index = json.loads", 1)[0]
        old = sys.argv
        try:
            for style in (None, 'xbox', 'xbox-label', 'pc'):
                for hud in ('full', '21:9', '16:9'):
                    argv = ['tool', '--aspect', '3840X1080', '--hud', hud]
                    if style: argv += ['--prompts', style]
                    sys.argv = argv
                    env = {'__file__': str(TOOL)}
                    exec(compile(source, str(TOOL), 'exec'), env)
                    expected = 'romfs-patches' + (f'-{style}' if style else '') + '-3840x1080'
                    if hud != 'full': expected += '-hud' + hud.replace(':', 'x')
                    self.assertEqual(env['args'].out.name, expected)
                    sys.argv = argv + ['--out', 'chosen']
                    exec(compile(source, str(TOOL), 'exec'), env)
                    self.assertEqual(env['args'].out, Path('chosen'))
        finally: sys.argv = old


@unittest.skipUnless(os.environ.get('HUD_TEST_ROMFS'), 'set HUD_TEST_ROMFS for local archive comparisons')
class RealArchiveTest(unittest.TestCase):
    def test_full_unchanged_and_caps_valid(self):
        folder = Path(os.environ['HUD_TEST_ROMFS'])
        index = json.loads((folder / 'index.json').read_text())
        files = {entry['path']: entry for entry in index['files']}
        tree = ast.parse(TOOL.read_text())
        functions = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in ('sarc_nodes', 'sarc_members')]
        namespace = {'struct': struct}
        exec(compile(ast.Module(body=functions, type_ignores=[]), str(TOOL), 'exec'), namespace)
        sarc = namespace['sarc_members']
        with (folder / 'romfs.bin').open('rb') as image:
            def read(path):
                entry = files[path]
                image.seek(entry['offset'])
                return image.read(entry['size'])
            pack = zstd.decompress(read('Pack/ZsDic.pack.zs'))
            dictionaries = {}
            for lo, hi in sarc(pack).values():
                dictionary = zstd.ZstdDict(pack[lo:hi])
                dictionaries[dictionary.dict_id] = dictionary
            path = max(path for path in files if path.startswith('UI/LayoutArchive/Common.Product.') and path.endswith('.blarc.zs'))
            raw = read(path)
        original = zstd.decompress(raw, zstd_dict=dictionaries[zstd.get_frame_info(raw).dictionary_id])
        members = sarc(original)
        # The dev implementation before the HUD port is the full-width compatibility reference.
        baseline_source = subprocess.check_output(['git', 'show', 'f85058070e955eae0e45c9adbbe903d340b05b32:tools/ultrawide_layout.py'], cwd=ROOT, text=True)
        baseline = {}
        exec(compile(baseline_source, '<dev layout>', 'exec'), baseline)
        for width, height in ((3840, 1080), (3440, 1440), (1280, 800)):
            full = bytearray(original)
            ui.apply(full, members, width, height)
            expected = bytearray(original)
            baseline['apply'](expected, members, width, height)
            # The map (its layout and the minimap animation's width keys), the Purah Pad's frame and the loading screen
            # changed on purpose after it (the maps fill the screen, the frame keeps its width); the rest is byte for byte.
            for member in ('blyt/AppMap_00.bflyt', 'blyt/AppMenuOverlay_00.bflyt', 'blyt/LoadingFade_00.bflyt',
                           'anim/AppMap_00_MiniMap.bflan', 'anim/LoadingFade_00_BarIn.bflan',
                           'anim/LoadingFade_00_BarOut.bflan'):
                lo, hi = members[member]
                expected[lo:hi] = full[lo:hi]
            self.assertEqual(full, expected)
        for cap, extension in ((16 / 9, 0), (21 / 9, 300)):
            data = bytearray(original)
            ui.apply(data, members, 3840, 1080, cap)
            self.assertAlmostEqual(value(data, members, 'PlayerStatus_00', 'N_HeartPos_00', 'x'), -extension)
            self.assertAlmostEqual(value(data, members, 'SaveLoadIcon_00', 'RootPane', 'x'), -extension / 2)
            for name in ui.KEEP_ROOT:
                if name == 'AppMenuOverlay_00': continue
                lo, hi = members[f'blyt/{name}.bflyt']
                self.assertEqual(data[lo:hi], original[lo:hi])


if __name__ == '__main__': unittest.main()
