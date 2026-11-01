import unittest
import numpy as np
import analyze_dlaa_sequence as sequence
from analyze_dlaa_sequence import validate_sequence
from analyze_dlaa_output import validate_results

class SequenceOutputTests(unittest.TestCase):
    def fixture(self):
        sdk=[dict(index=i,reset=int(i==1),evaluate=1,creates=1) for i in range(1,9)]
        host=[dict(index=i,frame=1300+i,width=2560,height=1440,reset=int(i==1),last=i==8,record_result=1,gpu_completion_observed=True,sdk_cleanup=int(i==8),presented=False) for i in range(1,9)]
        return sdk,host
    def test_complete_consecutive_sequence(self):
        self.assertEqual(validate_sequence(*self.fixture()),list(range(1301,1309)))
    def test_gap_and_repeated_reset_rejected(self):
        s,h=self.fixture();h[3]['frame']+=1
        with self.assertRaises(ValueError):validate_sequence(s,h)
        s,h=self.fixture();s[3]['reset']=1
        with self.assertRaises(ValueError):validate_sequence(s,h)
    def test_incomplete_cleanup_or_feature_recreation_rejected(self):
        s,h=self.fixture();h[-1]['sdk_cleanup']=0
        with self.assertRaises(ValueError):validate_sequence(s,h)

        s,h=self.fixture();s[-1]['creates']=2
        with self.assertRaises(ValueError):validate_sequence(s,h)

    def test_sequence_common_results_still_reject_sdk_failure(self):
        sdk={k:1 for k in ('init','caps','query','caps_destroy','allocate','create','evaluate','release','destroy','shutdown','available')}
        sdk.update(finished=True,presented=False,temporal_quality_validated=False,reset=False,jitter=[0,0])
        host=dict(record_result=1,sdk_cleanup=1,gpu_completion_observed=True,presented=False)
        self.assertEqual(validate_results(sdk,host,0,one_shot=False),[])
        self.assertTrue(validate_results(sdk,host,0))
        sdk['release']=0
        self.assertTrue(validate_results(sdk,host,0,one_shot=False))

    def test_host_dimensions_match_sdk(self):
        s,h=self.fixture();h[2]['width']=1280
        with self.assertRaises(ValueError):validate_sequence(s,h,(2560,1440))

    def test_output_requires_positive_visible_scene(self):
        a=-np.arange(1,17,dtype=np.float32).reshape(2,2,4)
        with self.assertRaises(ValueError):sequence.validate_image(a)
        a=np.arange(1,17,dtype=np.float32).reshape(2,2,4)
        sequence.validate_image(a)
