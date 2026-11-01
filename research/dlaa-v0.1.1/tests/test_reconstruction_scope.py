import copy,unittest
from analyze_projection_reconstruction import reconstruction_scope

class ReconstructionScopeTests(unittest.TestCase):
    def records(self):
        return [dict(reconstruction_probe_mode=True,other_camera_bytes_unchanged=True,program_overflow=0,bindings=4,vs_bindings=2,fs_bindings=2,extent_mask=7,programs=[dict(id=i,vs_slot_mask=8,fs_slot_mask=16) for i in (1,2)])]
    passes='vs 91fe9ba82ba38c82 fs d88166141a033b44 prog 1\nvs f93d393cea3eea0b fs 8756fd5dd052d836 prog 2'
    def test_known_ray_pairs(self):
        self.assertEqual(len(reconstruction_scope(self.records(),self.passes)['ray_programs']),2)
    def test_missing_secondary_slot_rejected(self):
        r=self.records();r[0]['programs'][1]['vs_slot_mask']=16
        with self.assertRaises(ValueError):reconstruction_scope(r,self.passes)
    def test_unreviewed_fragment_pair_rejected(self):
        with self.assertRaises(ValueError):reconstruction_scope(self.records(),self.passes.replace('8756fd5dd052d836','0000000000000001'))
    def test_variants_cannot_donate_slots(self):
        r=self.records();r[0]['programs'][0].update(vs='91fe9ba82ba38c82',fs='d88166141a033b44',fs_slot_mask=0)
        r.append(copy.deepcopy(r[0]));r[1]['programs'][0].update(vs_slot_mask=0,fs_slot_mask=16,fs='0000000000000001')
        with self.assertRaises(ValueError):reconstruction_scope(r,self.passes)
    def test_missing_extent_or_unrelated_bytes_rejected(self):
        for key,value in [('extent_mask',3),('other_camera_bytes_unchanged',False),('program_overflow',1),('bindings',3)]:
            r=self.records();r[0][key]=value
            with self.assertRaises(ValueError):reconstruction_scope(r,self.passes)
