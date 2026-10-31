import unittest
import numpy as np
from build_pixel_motion import compare_sparse_motion

class PixelComparisonTests(unittest.TestCase):
    def test_empty_sparse_support_does_not_discard_geometric_result(self):
        errors=compare_sparse_motion(np.zeros((20,20,2)),np.ones((20,20),bool),np.empty((0,2)),np.empty((0,2)),1)
        self.assertEqual(errors.shape,(0,))

    def test_matching_pixel_flow_has_zero_sparse_error(self):
        flow=np.zeros((20,20,2),np.float32);flow[:]=[1,-2]
        errors=compare_sparse_motion(flow,np.ones((20,20),bool),np.array([[10,10]],np.float32),np.array([[1,-2]],np.float32),1)
        np.testing.assert_allclose(errors,[0])
