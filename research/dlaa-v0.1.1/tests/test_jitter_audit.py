import unittest
import json
import numpy as np
import jitter_audit
from jitter_audit import camera_accesses, matrix_contract

PREFIX='''OpName %cam "fp_c4"
%int_0 = OpConstant %int 0
%int_3 = OpConstant %int 3
%int_7 = OpConstant %int 7
'''
class JitterAuditTests(unittest.TestCase):
    def test_constant_component_and_vector_access(self):
        r=camera_accesses(PREFIX+'''%a = OpAccessChain %ptr %cam %int_0 %int_7 %int_3
%b = OpAccessChain %vptr %cam %int_0 %int_3
''')
        self.assertEqual(r['byte_offsets'],[48,52,56,60,124]);self.assertEqual(r['unresolved'],[])
    def test_dynamic_index_does_not_become_constant(self):
        r=camera_accesses(PREFIX+'%a = OpAccessChain %ptr %cam %int_0 %runtime %int_3\n')
        self.assertEqual(r['byte_offsets'],[]);self.assertEqual(len(r['unresolved']),1)
    def test_camera_accesses_in_other_uniform_slots_are_identified(self):
        asm=PREFIX.replace('fp_c4','vp_c3')+'%a = OpAccessChain %ptr %cam %int_0 %int_7 %int_3\n'
        self.assertFalse(camera_accesses(asm)['camera_declared'])
        self.assertEqual(camera_accesses(asm,slot=3)['byte_offsets'],[124])
    def test_derived_pointer_is_followed(self):
        r=camera_accesses(PREFIX+'''%base = OpAccessChain %arrptr %cam %int_0
%leaf = OpAccessChain %ptr %base %int_7 %int_3
''')
        self.assertIn(124,r['byte_offsets'])
    def block(self):
        a=np.zeros((148,4),dtype='<f4');v=np.eye(4);v[2,3]=-12
        p=np.array([[1.5,0,0,0],[0,2.7,0,0],[0,0,-1.000008,-.2],[0,0,-1,0]])
        for vo,po,vpo in ((0,7,3),(21,28,24)):
            a[vo:vo+3]=v[:3];a[po:po+4]=p;a[vpo:vpo+4]=p@v
        a[11:14]=np.linalg.inv(v)[:3];a[18]=[2560,1440,1/2560,1/1440]
        return a
    def test_current_and_previous_matrix_relationships(self):
        r=matrix_contract(self.block().tobytes())
        self.assertTrue(r['valid']);self.assertLess(r['current_vp_error'],1e-5)
    def test_corrupt_or_incomplete_blocks_fail_closed(self):
        self.assertFalse(matrix_contract(b'\0'*176)['valid'])
        a=self.block();a[3,0]+=1
        self.assertFalse(matrix_contract(a.tobytes())['valid'])
    def test_degenerate_projection_and_inactive_anchor_are_rejected(self):
        a=self.block();a[7:11]=0;a[3:7]=0
        self.assertFalse(matrix_contract(a.tobytes())['valid'])
        a=self.block();data=a.tobytes();contract=matrix_contract(data)
        anchor=dict(active=0,current_vp=a[3:7].ravel().tolist())
        self.assertFalse(jitter_audit.main_camera_match(data,contract,anchor))
        anchor['active']=1
        self.assertTrue(jitter_audit.main_camera_match(data,contract,anchor))
    def test_binding_snapshots_reject_invalid_ranges(self):
        record=dict(event=1,frame=1250,stage='fs',shader='0123456789abcdef',size=16,offset=0,copied=16)
        r=jitter_audit.parse_bindings(json.dumps(record),b'1'*16)
        self.assertEqual(r[0]['data'],b'1'*16)
        record['offset']=8
        with self.assertRaises(ValueError):jitter_audit.parse_bindings(json.dumps(record),b'1'*16)
    def test_unavailable_binding_never_reads_live_or_previous_bytes(self):
        record=dict(event=1,frame=1250,stage='cs',shader='0123456789abcdef',size=2368,offset=0,copied=0)
        self.assertIsNone(jitter_audit.parse_bindings(json.dumps(record),b'1'*2368)[0]['data'])
