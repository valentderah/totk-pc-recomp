"""Verify experiment correlations and profile snapshot isolation checks without game data."""
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from analyze_control_experiment import correlate, virtual_responses
from run_control_experiment import profile_fingerprint, validate_jump_contract


class Experiment(unittest.TestCase):
    def test_contract_matches_verified_build(self):
        probes = dict(experimental_action_channel=True, sites=[dict(address='0x1000', fields=[40, 56, 72])])
        self.assertEqual(validate_jump_contract('4096:40:56:72:1', probes), '0x1000:0x28:0x38:0x48:0x1')
        for contract in ('8192:40:56:72:1', '4096:40:56:80:1', '4096:40:40:72:1',
                         '4096:40:56:72:3', '4096:40:56:72:0', '4096:40:56:72:18446744073709551616'):
            with self.subTest(contract=contract), self.assertRaises(ValueError):
                validate_jump_contract(contract, probes)
        probes['experimental_action_channel'] = False
        with self.assertRaises(ValueError):
            validate_jump_contract('4096:40:56:72:1', probes)

    def test_virtual_position_window_and_actor_identity(self):
        def pos(t, actor, xyz): return dict(type='actor_position', world_us=t, site='0x100', actor=actor, position=xyz)
        rows = [pos(950000, 'one', [1, 2, 3]),
                dict(type='virtual_action', world_us=1000000, site='0x100', buttons=0, result=3),
                pos(1100000, 'one', [4, 4, 7]), pos(1200000, 'other', [100, 200, 300]),
                pos(2000000, 'one', [100, 200, 300])]
        response = virtual_responses(rows)[0]
        self.assertEqual(response['switch_buttons'], 0)
        self.assertEqual(response['position_response']['max_y_change_m'], 2)
        self.assertEqual(response['position_response']['max_horizontal_change_m'], 5)

    def test_edges_world_clock_and_windows(self):
        def inp(t, b): return dict(type='input', world_us=t, buttons=b)
        def mask(t, value): return dict(type='output_mask', world_us=t, site='0x100', object='0x200', id=40, result=value)
        rows = [inp(-1, 1), inp(-1, 0), inp(1000, 2), inp(2000, 2), mask(3000, 0x400),
                mask(4000, 0), mask(500000, 8), inp(600000, 0), inp(700000, 2)]
        result = correlate(rows)
        self.assertEqual(len(result), 2)
        self.assertEqual(result[0]['buttons'], ['B'])
        self.assertEqual(result[0]['outputs'][0]['values'], [0, 0x400])
        self.assertEqual(result[1]['outputs'], [])

    def test_fingerprint_detects_content_names_and_symlinks(self):
        with tempfile.TemporaryDirectory() as td:
            folder = Path(td)
            p = folder / 'save'; p.write_bytes(b'one')
            first = profile_fingerprint(folder)
            p.write_bytes(b'two')
            self.assertNotEqual(first, profile_fingerprint(folder))
            p.write_bytes(b'one'); p.rename(folder / 'other')
            self.assertNotEqual(first, profile_fingerprint(folder))
            (folder / 'link').symlink_to(folder / 'other')
            with self.assertRaises(ValueError): profile_fingerprint(folder)


if __name__ == '__main__':
    unittest.main()
