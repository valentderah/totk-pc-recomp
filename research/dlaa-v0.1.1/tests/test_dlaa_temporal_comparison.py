import unittest
import numpy as np
from compare_dlaa_temporal import unjitter_motion, temporal_residual

class TemporalComparisonTests(unittest.TestCase):
    def fields(self):
        a=np.zeros((40,48,4),np.float32);a[:,:,3]=2
        return a,np.zeros((40,48),np.float32)
    def test_jitter_only_motion_cancels(self):
        a,b=self.fields();now=np.array([.25,-.3]);old=np.array([-.25,.2]);a[:,:,:2]=old-now
        flow,masks=unjitter_motion(a,b,now,old)
        np.testing.assert_allclose(flow[8:-8,8:-8],0,atol=1e-7)
        self.assertGreater(masks['camera'].sum(),500)
        self.assertEqual(masks['mesh'].sum(),0)
    def test_pan_is_preserved_and_warp_direction_is_previous(self):
        a,b=self.fields();a[:,:,0]=1-.5
        flow,masks=unjitter_motion(a,b,[.25,0],[-.25,0])
        np.testing.assert_allclose(flow[8:-8,8:-8,0],1)
        previous=np.tile(np.arange(48,dtype=np.float32)[None,:,None],(40,1,3))/48
        current=np.roll(previous,-1,axis=1)
        stats=temporal_residual(current,previous,flow,masks['camera'])
        self.assertLess(stats['mean'],1e-7)
    def test_invalid_or_uncertain_fields_are_excluded(self):
        a,b=self.fields();b[:]=1
        _,masks=unjitter_motion(a,b,[0,0],[0,0])
        self.assertFalse(masks['camera'].any())
        a[0,0,0]=np.nan
        with self.assertRaises(ValueError):unjitter_motion(a,b,[0,0],[0,0])
        with self.assertRaises(ValueError):temporal_residual(np.zeros((40,48,3)),np.zeros((40,48,3)),a[:,:,:2],np.zeros((40,48),bool))
