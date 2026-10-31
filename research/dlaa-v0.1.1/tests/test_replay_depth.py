import unittest
import numpy as np
from prepare_dlaa_replay import validate_depth

class ReplayDepthTests(unittest.TestCase):
    def test_invalid_camera_regions_keep_original_depth(self):
        d=np.full((32,32),.98,np.float32);c=np.zeros((32,32,4),np.float32);c[8:24,8:24,2]=.98;c[8:24,8:24,3]=1
        r=validate_depth(d,c,min_valid=100)
        self.assertEqual(r['camera_valid_pixels'],256)
        self.assertEqual(r['depth_outside_valid_camera_pixels'],768)
        with self.assertRaises(ValueError):validate_depth(c[:,:,2].copy(),c,min_valid=100)
    def test_reconstructed_or_nonfinite_depth_rejected(self):
        d=np.full((32,32),.98,np.float32);c=np.zeros((32,32,4),np.float32);c[:,:,2]=.98;c[:,:,3]=1
        for bad in (np.zeros_like(d),np.full_like(d,np.nan),np.full_like(d,1.1)):
            with self.assertRaises(ValueError):validate_depth(bad,c,min_valid=100)
