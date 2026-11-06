import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from analyze_input_queries import read_jsonl, summarize


class Summary(unittest.TestCase):
    def test_transitions_counters_and_repeated_labels(self):
        rows = [{'type': 'input', 'ns': t, 'buttons': b} for t, b in [(0, 0), (1, 1), (2, 1), (3, 0), (5, 1)]]
        rows += [{'type': 'query', 'ns': t, 'site': 's', 'object': 'o', 'id': 2, 'caller': 'c',
                  'result': result, 'calls': calls} for t, result, calls in [(1, 0, 1), (2, 1, 9), (5, 0, 12)]]
        markers = [{'ns': t, 'label': label} for t, label in [(0, 'confirm'), (4, 'confirm'), (8, 'done')]]
        report = summarize(rows, markers)
        self.assertEqual(report['minimum_query_calls'], 12)
        self.assertEqual(report['positive_query_records'], 1)
        self.assertEqual(report['intervals'][0]['press_transitions'], {'A': 1})
        self.assertEqual(report['intervals'][1]['press_transitions'], {'A': 1})
        self.assertEqual(report['controller_names'], [])

    def test_partial_tail_and_corruption(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'test.jsonl'
            path.write_text('{"type":"session"}\n{"type":')
            rows, partial = read_jsonl(path)
            self.assertEqual(len(rows), 1)
            self.assertTrue(partial)
            path.write_text('{"type":\n{"type":"session"}\n')
            with self.assertRaises(ValueError):
                read_jsonl(path)


if __name__ == '__main__':
    unittest.main()
