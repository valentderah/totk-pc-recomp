import unittest
import numpy as np
from compare_camera_gpu import sample_background_tracks

class CameraTrackTests(unittest.TestCase):
    def test_empty_tracks_are_a_supported_result(self):
        errors=sample_background_tracks(np.zeros((4,4,2),np.float32),np.ones((4,4),np.uint8),np.empty((0,2),np.float32),np.empty((0,2),np.float32),1)
        self.assertEqual(len(errors),0)

    def test_scale_and_mask(self):
        flow=np.ones((8,8,2),np.float32)*2;mask=np.zeros((8,8),np.uint8);mask[2:6,2:6]=1
        errors=sample_background_tracks(flow,mask,np.array([[1.5,1.5],[0,0]],np.float32),np.ones((2,2),np.float32),2)
        self.assertEqual(len(errors),1);self.assertAlmostEqual(float(errors[0]),0)
