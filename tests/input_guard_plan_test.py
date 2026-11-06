"""Guard consumer patch validation on synthetic instructions, without game data."""
import copy
import hashlib
import struct
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from input_guard_plan import apply


class GuardPlan(unittest.TestCase):
    def setup_case(self):
        import numpy as np
        words = [0x52800041, 0x39400108, 0x37100008, 0xd65f03c0]
        image = struct.pack('<4I', *words)
        module = SimpleNamespace(base=0x1000, lo=0x1000, hi=0x1010, image=image,
                                 words=np.frombuffer(image, dtype='<u4'), _cache={'original': True},
                                 in_text=lambda a: 0x1000 <= a < 0x1010 and a % 4 == 0)
        config = {'bit': 27, 'from_bit': 2, 'sites': [
            {'offset': 0, 'before': words[0], 'after': 0x52800361, 'kind': 'query'},
            {'offset': 4, 'before': words[1], 'after': 0x39400d08, 'kind': 'byte'},
            {'offset': 8, 'before': words[2], 'after': 0x37180008, 'kind': 'test'}],
            'evidence': [{'offset': 0, 'end': 16, 'sha256': hashlib.sha256(image).hexdigest()}]}
        return module, config

    def test_independent_disassembly(self):
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        module, config = self.setup_case()
        patched, _ = apply(module,config,[0x1000])
        before=list(Cs(CS_ARCH_ARM64,CS_MODE_ARM).disasm(module.image,module.base))
        after=list(Cs(CS_ARCH_ARM64,CS_MODE_ARM).disasm(patched.image,module.base))
        self.assertEqual((after[0].mnemonic,after[0].op_str),('mov','w1, #0x1b'))
        self.assertEqual((after[1].mnemonic,after[1].op_str),('ldrb','w8, [x8, #3]'))
        self.assertEqual(after[2].mnemonic,before[2].mnemonic)
        self.assertEqual(after[2].op_str.split(',')[-1],before[2].op_str.split(',')[-1])
        self.assertIn('#3',after[2].op_str)

    def test_copy_and_validation(self):
        module, config = self.setup_case()
        patched, affected = apply(module, config, [0x1000])
        self.assertEqual(affected, {0x1000})
        self.assertEqual(module._cache, {'original': True})
        self.assertNotEqual(patched.image, module.image)
        self.assertEqual(int(patched.words[0]), 0x52800361)
        self.assertFalse(patched._cache)
        for change in ('fingerprint', 'word', 'kind', 'duplicate', 'bit', 'boundary'):
            bad = copy.deepcopy(config)
            if change == 'fingerprint': bad['evidence'][0]['sha256'] = '0'*64
            if change == 'word': bad['sites'][0]['after'] ^= 1
            if change == 'kind': bad['sites'][0]['kind'] = 'other'
            if change == 'duplicate': bad['sites'].append(bad['sites'][0])
            if change == 'bit': bad['bit'] = 2
            if change == 'boundary': bad['evidence'][0]['end'] = 12
            with self.subTest(change=change), self.assertRaises(ValueError):
                apply(module, bad, [0x1000])


if __name__ == '__main__':
    unittest.main()
