"""State reports must keep an unknown observation distinct from a clear flag or a selected ability."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_player_states import decode


class Reports(unittest.TestCase):
    def test_unknown_positive_and_unknown_slot_are_not_named(self):
        row = dict(status=2, flags=5, known=1, slots_known=1, verbs=[0, -1, 99, 0, 0], ability=1)
        config = {'verb_table': {'rows': [{'name': 'Idle'}]}}
        result = decode(row, config)
        self.assertNotIn('air', result['active_flags'])
        self.assertIn('air', result['unknown_flags'])
        self.assertEqual(result['verb_names'], ['Idle', None, None, None, None])
        self.assertNotIn('ability_in_use', result['active_flags'])
        row.update(status=0, known=0xffffffff)
        result = decode(row, config)
        self.assertEqual(result['verb_names'], [None] * 5)
        self.assertIn('air', result['unknown_flags'])


if __name__ == '__main__':
    unittest.main()
