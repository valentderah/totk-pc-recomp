import unittest
import numpy as np
from temporal_guard import compose_guard,decode_r11g11b10

class TemporalGuardTests(unittest.TestCase):
    def inputs(self):
        camera=np.zeros((4,4,4),np.float32);camera[...,2]=.5;camera[...,3]=1
        return camera,np.zeros_like(camera),np.ones((4,4,3),np.float32),np.ones((4,4,3),np.float32)
    def test_matching_camera_candidates_retained(self):
        output,bias=compose_guard(*self.inputs())
        self.assertTrue((output[...,3]==2).all());self.assertTrue((bias==0).all())
    def test_known_skin_priority_and_invalid_coverage(self):
        camera,skin,now,old=self.inputs();skin[1,1]=[.2,0,.5,1];skin[2,2,3]=-1
        output,bias=compose_guard(camera,skin,now,old)
        self.assertEqual(output[1,1,3],1);self.assertAlmostEqual(float(output[1,1,0]),.2)
        self.assertEqual(output[2,2,3],0);self.assertEqual(bias[2,2],1)
    def test_color_change_rejects_camera_but_preserves_known_motion(self):
        camera,skin,now,old=self.inputs();now*=4;skin[1,1]=[0,0,.5,1]
        output,bias=compose_guard(camera,skin,now,old)
        self.assertEqual(output[0,0,3],0);self.assertEqual(output[1,1,3],1);self.assertTrue((bias==1).all())
    def test_missing_history_is_not_confidence(self):
        output,bias=compose_guard(*self.inputs(),history=False)
        self.assertFalse(output[...,3].any());self.assertTrue((bias==1).all())
    def test_nonfinite_input_rejected(self):
        camera,skin,now,old=self.inputs();now[1,1]=np.nan
        output,bias=compose_guard(camera,skin,now,old)
        self.assertEqual(output[1,1,3],0);self.assertEqual(bias[1,1],1)
    def test_warp_and_out_of_bounds(self):
        camera,skin,now,old=self.inputs();old[:,:,0]=np.arange(4);now[:,:,0]=np.minimum(np.arange(4)+1,3)
        camera[...,0]=1
        output,bias=compose_guard(camera,skin,now,old)
        self.assertTrue((output[:,:3,3]==2).all());self.assertTrue((output[:,3,3]==0).all())
    def test_decode_zero_one_subnormal_inf(self):
        one=(15<<6)|((15<<6)<<11)|((15<<5)<<22)
        values=decode_r11g11b10(np.array([0,one,1,31<<6],np.uint32))
        np.testing.assert_array_equal(values[0],[0,0,0]);np.testing.assert_array_equal(values[1],[1,1,1])
        self.assertAlmostEqual(values[2,0],2**-20);self.assertTrue(np.isinf(values[3,0]))
