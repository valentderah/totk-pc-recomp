import tempfile,unittest
from pathlib import Path
import numpy as np
from compare_gpu_motion import load_gpu_motion

class GpuCompareTests(unittest.TestCase):
    def test_negative_current_surface_coverage_is_not_valid_velocity(self):
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder)/'motion.bin';a=np.zeros((2,3,4),np.float32);a[0,0]=[0,0,.5,-1];a.tofile(p)
            data,mask=load_gpu_motion(p,3,2);self.assertFalse(mask.any());self.assertEqual(data[0,0,3],-1)

    def test_unknown_validity_code_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder)/'motion.bin';a=np.zeros((2,3,4),np.float32);a[0,0,3]=2;a.tofile(p)
            with self.assertRaises(ValueError):load_gpu_motion(p,3,2)
    def test_gpu_clear_pixels_are_invalid_and_export_size_is_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder)/'motion.bin';np.zeros((2,3,4),np.float32).tofile(p)
            _,mask=load_gpu_motion(p,3,2);self.assertFalse(mask.any())
            with self.assertRaises(ValueError):load_gpu_motion(p,4,2)

    def test_nonfinite_gpu_results_are_not_silently_accepted(self):
        with tempfile.TemporaryDirectory() as folder:
            p=Path(folder)/'motion.bin';a=np.zeros((2,3,4),np.float32);a[0,0]=[np.nan,0,.5,1];a.tofile(p)
            with self.assertRaises(ValueError):load_gpu_motion(p,3,2)
