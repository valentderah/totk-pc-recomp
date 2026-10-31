import unittest
import json
import numpy as np
from analyze_projection_jitter import validate_records

class ProjectionJitterOutputTests(unittest.TestCase):
    def record(self):
        p=np.array([[1.5,0,0,0],[0,8/3,0,0],[0,0,-1.000008,-.2],[0,0,-1,0]],dtype=np.float32)
        v=np.eye(4);v[2,3]=-12;vp=p@v;j=np.eye(4);j[0,3]=2*.375/2560;j[1,3]=-2*(-.25)/1440
        return dict(id=1,frame=1200,fence=2400,address='1234',bindings=42,jitter=[.375,-.25],gpu_completion_observed=True,vs_only=True,source_snapshot_unchanged_by_copy=True,full_scene_jitter_contract=False,presented_via_ngx=False,
            source_projection=p.ravel().tolist(),bound_projection=(j@p).ravel().tolist(),source_vp=vp.ravel().tolist(),bound_vp=(j@vp).ravel().tolist())
    def test_expected_projection_and_vp_shift(self):
        r=validate_records([self.record()],1200);self.assertEqual(r['descriptor_bindings_overridden'],42)
    def test_float32_schedule_report_is_json_serializable(self):
        json.dumps(validate_records([self.record()],1200,expected_jitter=np.asarray([.375,-.25],dtype=np.float32)))
    def test_wrong_frame_or_missing_gpu_completion_rejected(self):
        for field,value in [('frame',1199),('gpu_completion_observed',False),('source_snapshot_unchanged_by_copy',False),('full_scene_jitter_contract',True)]:
            r=self.record();r[field]=value
            with self.assertRaises(ValueError):validate_records([r],1200)
    def test_wrong_sign_or_changed_depth_rows_rejected(self):
        for index in (2,10):
            r=self.record();r['bound_projection'][index]+=.01
            with self.assertRaises(ValueError):validate_records([r],1200)
    def test_empty_or_duplicate_clone_reports_rejected(self):
        with self.assertRaises(ValueError):validate_records([],1200)
        r=self.record()
        with self.assertRaises(ValueError):validate_records([r,r],1200)
