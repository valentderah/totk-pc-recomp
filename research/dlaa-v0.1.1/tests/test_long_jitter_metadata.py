import unittest
import test_projection_motion_output as fixture
from analyze_dlaa_raster_jitter import schedule,validate_metadata_window
from analyze_dlaa_sequence import validate_sequence

class LongMetadataTests(unittest.TestCase):
    def test_all_cycles_and_restoration(self):
        f=fixture.ProjectionMotionOutputTests();rows=[f.meta(n,schedule(n,1800,32).tolist(),schedule(n-1,1800,32).tolist()) for n in range(1768,1802)]
        validate_metadata_window(rows,1800,32)
        rows[10]['previous_jitter']=[0,0]
        with self.assertRaises(ValueError):validate_metadata_window(rows,1800,32)
    def test_exact_long_sdk_lifecycle(self):
        sdk=[dict(index=i,reset=int(i==1),evaluate=1,creates=1) for i in range(1,33)]
        host=[dict(index=i,frame=1768+i,width=2560,height=1440,reset=int(i==1),last=i==32,record_result=1,gpu_completion_observed=True,sdk_cleanup=int(i==32),presented=False) for i in range(1,33)]
        self.assertEqual(validate_sequence(sdk,host,(2560,1440),count=32),list(range(1769,1801)))
        with self.assertRaises(ValueError):validate_sequence(sdk[:-1],host[:-1],count=32)
        with self.assertRaises(ValueError):validate_sequence(sdk,host)
