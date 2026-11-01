import unittest
from objects import parse_objects, unique_pairs

HEADER='draw 1 frame 2399 fence 42 pass 7 vs ab12 fs cd34 area 2560x1440 depth 0x1000\n'
VB=' vbuf 0 addr 0x2000 size 0x40\n'

class ObjectTests(unittest.TestCase):
    def test_binary_offsets_must_stay_inside_file(self):
        text=HEADER+VB+' ubo c7 addr 0x3000 size 0x40 offset 12 copied 64 source snapshot\n'
        with self.assertRaises(ValueError):parse_objects(text,bytes(64))

    def test_unavailable_is_not_a_zero_filled_transform(self):
        text=HEADER+VB+' ubo c7 addr 0x3000 size 0x40 offset 0 copied 0 source unavailable\n'
        records=parse_objects(text,b'')
        self.assertIsNone(records[0]['uniforms'][7]['data'])

    def test_duplicate_geometry_is_not_silently_matched(self):
        text=HEADER+VB+HEADER.replace('draw 1','draw 2')+VB+HEADER.replace('frame 2399','frame 2400')+VB
        records=parse_objects(text,b'')
        pairs,ambiguous=unique_pairs(records)
        self.assertEqual(pairs,[])
        self.assertEqual(ambiguous,1)

    def test_unique_geometry_keeps_both_frame_payloads(self):
        text=HEADER+VB+' ubo c7 addr 0x3000 size 0x4 offset 0 copied 4 source snapshot\n'
        text+=HEADER.replace('frame 2399','frame 2400')+VB+' ubo c7 addr 0x4000 size 0x4 offset 4 copied 4 source snapshot\n'
        pairs,ambiguous=unique_pairs(parse_objects(text,b'abcdWXYZ'))
        self.assertEqual(ambiguous,0)
        self.assertEqual(pairs[0][0]['uniforms'][7]['data'],b'abcd')
        self.assertEqual(pairs[0][1]['uniforms'][7]['data'],b'WXYZ')
