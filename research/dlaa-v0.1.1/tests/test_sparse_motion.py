import unittest
import numpy as np
from optical import cv2
from sparse_motion import track_features, summarize_tracks

class SparseMotionTests(unittest.TestCase):
    def test_empty_support_is_explicit_and_has_no_nan_metrics(self):
        result=summarize_tracks(np.empty((0,2)),np.empty((0,2)),np.empty((0,2)),2.0,(960,540))
        self.assertEqual(result['status'],'insufficient_support')
        self.assertEqual(result['points'],0)
        self.assertIsNone(result['residual_p90_pixels'])
        self.assertEqual(result['occupied_grid_cells_of_16'],0)

    def test_dark_textured_translation_is_measured_without_camera_hint(self):
        rng=np.random.default_rng(416)
        previous=cv2.GaussianBlur(rng.integers(0,35,(180,240),dtype=np.uint8),(3,3),0)
        current=cv2.warpAffine(previous,np.array([[1,0,4],[0,1,-2]],np.float32),(240,180))
        points,flow=track_features(previous,current)
        self.assertGreater(len(points),50)
        self.assertLess(float(np.median(np.linalg.norm(flow-[-4,2],axis=-1))),0.2)

    def test_flat_images_provide_no_correspondences(self):
        image=np.full((100,140),10,np.uint8)
        points,flow=track_features(image,image)
        self.assertEqual(points.shape,(0,2))
        self.assertEqual(flow.shape,(0,2))

    def test_unrelated_images_are_not_a_valid_stationary_match(self):
        rng=np.random.default_rng(52)
        previous=rng.integers(0,256,(180,240),dtype=np.uint8)
        current=rng.integers(0,256,(180,240),dtype=np.uint8)
        points,_=track_features(previous,current)
        self.assertLess(len(points),5)

    def test_invalid_dimensions_are_rejected(self):
        with self.assertRaises(ValueError):
            track_features(np.zeros((60,60),np.uint8),np.zeros((40,60),np.uint8))
