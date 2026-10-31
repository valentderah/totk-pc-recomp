"""Validate prompt archive parsing and guarded layout edits with synthetic data."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from macos.prompts import PROMPT_TEXTURES, patch_action_layout, replace_member, texture_rows
from shader_census import sarc


def textures():
    data = bytearray(0x8000)
    data[:4] = b'BNTX'
    data[12:14] = b'\xff\xfe'
    struct.pack_into('<IQ', data, 0x24, len(PROMPT_TEXTURES), 0x40)
    for i, name in enumerate(PROMPT_TEXTURES):
        pos, text, pixels, level = 0x100 + i * 0x80, 0x2000 + i * 80, 0x4000 + i * 64, 0x3800 + i * 8
        struct.pack_into('<Q', data, 0x40 + i * 8, pos)
        data[pos:pos+4] = b'BRTI'
        struct.pack_into('<H', data, pos + 0x16, 1)
        struct.pack_into('<I4xII', data, pos + 0x1c, 0x201, 72, 72)
        struct.pack_into('<I', data, pos + 0x50, 64)
        struct.pack_into('<Q', data, pos + 0x60, text)
        struct.pack_into('<Q', data, pos + 0x70, level)
        struct.pack_into('<Q', data, level, pixels)
        struct.pack_into('<H', data, text, len(name))
        data[text+2:text+2+len(name)] = name.encode()
        data[pixels:pixels+64] = bytes([i]) * 64
    return data


def layout():
    data = bytearray(20)
    data[:6] = b'FLYT\xff\xfe'
    struct.pack_into('<H', data, 6, 20)
    struct.pack_into('<H', data, 16, 17)
    data += b'fnl1' + struct.pack('<IHHI', 32, 1, 0, 4) + b'Normal_00.fcpx\0\0'
    for i in range(16):
        pane = bytearray(176)
        struct.pack_into('<4sI', pane, 0, b'txt1', len(pane))
        letter = 'ABXY'[i % 4] if i < 15 else 'A'
        name = f'T_{letter}_{i:02}' if i < 15 else 'Title'
        pane[12:12+len(name)] = name.encode()
        struct.pack_into('<I', pane, 0x64, 0xa8)
        pane[0xa8:0xac] = (letter + '\0').encode('utf-16le')
        data += pane
    struct.pack_into('<I', data, 12, len(data))
    return data


def archive():
    data = bytearray(0x1000 + 3 * 256)
    struct.pack_into('<4sHHIIHH', data, 0, b'SARC', 20, 0xfeff, len(data), 0x1000, 0x100, 0)
    struct.pack_into('<4sHHI', data, 20, b'SFAT', 12, 3, 0x65)
    struct.pack_into('<4sHH', data, 80, b'SFNT', 8, 0)
    for i, name in enumerate(('before', 'layout', 'after')):
        struct.pack_into('<4I', data, 32 + i * 16, i, 0x1000000 + i * 4, i * 256, i * 256 + 16)
        data[88 + i*16:88 + i*16 + len(name)] = name.encode()
        data[0x1000+i*256:0x1000+i*256+16] = bytes([i]) * 16
    return data


class PromptAssetsTests(unittest.TestCase):
    def test_texture_identities_cover_exact_names(self):
        rows = texture_rows(textures())
        self.assertEqual({r['code'] for r in rows}, set(PROMPT_TEXTURES.values()))
        self.assertEqual(len({r['digest'] for r in rows}), len(rows))

    def test_duplicate_cannot_hide_missing_texture(self):
        data = textures()
        struct.pack_into('<Q', data, 0x40 + 8, 0x100)
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            texture_rows(data)

    def test_reject_truncated_pixels(self):
        data = textures()
        struct.pack_into('<Q', data, 0x3800, len(data) - 1)
        with self.assertRaises(ValueError):
            texture_rows(data)

    def test_only_positional_button_letters_change(self):
        data = layout()
        changed = patch_action_layout(data)
        expected = bytearray(data)
        for i in range(15):
            struct.pack_into('<H', expected, 52 + i * 176 + 0xa8, 0)
        self.assertEqual(changed, expected)
        self.assertEqual(changed[-8:-4], b'A\0\0\0')
        self.assertNotIn(b'External_00.fcpx', changed)

    def test_unknown_layout_fails_without_mutating_input(self):
        data = layout()
        data[52 + 0xa8] = ord('Z')
        before = data[:]
        with self.assertRaises(ValueError):
            patch_action_layout(data)
        self.assertEqual(data, before)

    def test_archive_replacement_preserves_other_members_and_offsets(self):
        data = bytes(archive())
        replacement = bytes(range(16))
        changed = replace_member(data, 'layout', replacement)
        expected = sarc(data)
        expected['layout'] = replacement
        self.assertEqual(sarc(changed), expected)
        self.assertEqual(len(changed), len(data))
        self.assertEqual(changed[:0x1100], data[:0x1100])
        self.assertEqual(changed[0x1110:], data[0x1110:])

    def test_archive_rejects_growth_before_mutating_input(self):
        data = archive()
        before = data[:]
        with self.assertRaisesRegex(ValueError, 'preserve its size'):
            replace_member(data, 'layout', bytes(17))
        self.assertEqual(data, before)


if __name__ == '__main__':
    unittest.main()
