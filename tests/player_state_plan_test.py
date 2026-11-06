"""Local state contracts must match the executable's verb order and evidence before C generation."""
import copy
import hashlib
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from player_state_plan import FIELDS, source, validate


class Plans(unittest.TestCase):
    def setUp(self):
        image = bytearray(256)
        image[120:130] = b'Idle\0Jump\0'
        self.module = SimpleNamespace(image=bytes(image), base=0x1000,
                                      nso=SimpleNamespace(relocs=[(64, 1027, 0, 120), (72, 1027, 0, 125)]))
        self.config = dict(fields=dict.fromkeys(FIELDS, 0), global_offset=32,
                           evidence=[dict(offset=0, size=8, sha256=hashlib.sha256(bytes(8)).hexdigest())],
                           verb_table=dict(offset=64, invalid=2, rows=[dict(name='Idle', body=0, any=0),
                                                                    dict(name='Jump', body=4, any=0)]))
        self.config['fields'].update(swim_mask=16, climb_mask=256)

    def test_matching_contract_generates_numeric_config(self):
        validate(self.config, self.module)
        text = source(self.config, self.module, 0x1234, 'GENERATED_TEST')
        self.assertIn('GENERATED_TEST', text)
        self.assertIn('.global_slot = 4128ull', text)
        self.assertNotIn('Jump', text)

    def test_changed_name_order_hash_and_bounds_rejected(self):
        configs = []
        for key, value in [('name', 'Other'), ('body', 1 << 31), ('any', -1)]:
            c = copy.deepcopy(self.config); c['verb_table']['rows'][1][key] = value; configs.append(c)
        for key, value in [('offset', 255), ('invalid', 1), ('invalid', True)]:
            c = copy.deepcopy(self.config); c['verb_table'][key] = value; configs.append(c)
        for key, value in [('actor_components', -1), ('actor_components', True), ('swim_mask', 3)]:
            c = copy.deepcopy(self.config); c['fields'][key] = value; configs.append(c)
        c = copy.deepcopy(self.config); c['evidence'][0]['sha256'] = '0' * 64; configs.append(c)
        c = copy.deepcopy(self.config); c['global_offset'] = 33; configs.append(c)
        for c in configs:
            with self.subTest(config=c), self.assertRaises(ValueError):
                validate(c, self.module)


if __name__ == '__main__':
    unittest.main()
