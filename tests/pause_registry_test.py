"""Hash vectors and registry interpretation using synthetic config data."""
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from resolve_pause_registry import murmur3, resolve


class Registry(unittest.TestCase):
    def test_vectors(self):
        self.assertEqual(murmur3(b''), 0)
        self.assertEqual(murmur3(b'hello'), 0x248bfa47)
        self.assertEqual(murmur3(b'foo'), 0xf6a5c420)

    def test_rows_and_deduplicated_transitions(self):
        c = {'CellList': {'a': dict(RowKey='hello', ColumnKey='Menu', IsPause=True),
                          'b': dict(RowKey='hello', ColumnKey='Idle')}}
        events = [dict(type='context_entry', id=murmur3(b'hello'), result=35)]
        events += [dict(type='context_word', id=1, result=v, world_us=t) for t, v in [(0, 0), (1, 8), (2, 8), (3, 0)]]
        result = resolve(c, events)[0]
        self.assertEqual(result['configured_pause_reasons'], ['Menu'])
        self.assertEqual(result['observed_transitions'], [dict(world_us=0, paused=False), dict(world_us=1, paused=True),
                                                         dict(world_us=3, paused=False)])


if __name__ == '__main__':
    unittest.main()
