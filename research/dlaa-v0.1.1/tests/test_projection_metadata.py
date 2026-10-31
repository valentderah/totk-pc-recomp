import unittest
import numpy as np
from projection_metadata import camera_matrices

class ProjectionMetadataTests(unittest.TestCase):
    def matrix(self,jitter):
        p=np.eye(4);p[0,3]=2*jitter[0]/2560;p[1,3]=-2*jitter[1]/1440;return p
    def test_legacy_metadata_retains_raw_projection(self):
        meta=dict(width=2560,height=1440,current_vp=np.eye(4).ravel().tolist(),previous_vp=np.eye(4).ravel().tolist(),projection_probe_mode=True)
        raw,old,raster,prior=camera_matrices(meta,legacy_jitter=[.375,-.25])
        np.testing.assert_array_equal(raw,np.eye(4));np.testing.assert_allclose(raster,self.matrix([.375,-.25]))
    def meta(self,current=[.375,-.25],previous=[0,0]):
        return dict(width=2560,height=1440,projection_probe_mode=True,motion_projection_coordinated=True,
            current_jitter=current,previous_jitter=previous,raw_current_vp=np.eye(4).ravel().tolist(),raw_previous_vp=np.eye(4).ravel().tolist(),
            current_vp=self.matrix(current).ravel().tolist(),previous_vp=self.matrix(previous).ravel().tolist())
    def test_coordinated_projection_is_not_shifted_twice(self):
        raw,old,raster,prior=camera_matrices(self.meta())
        np.testing.assert_array_equal(raw,np.eye(4));np.testing.assert_allclose(raster,self.matrix([.375,-.25]))
    def test_previous_frame_jitter_is_preserved(self):
        raw,old,raster,prior=camera_matrices(self.meta([0,0],[.375,-.25]))
        np.testing.assert_array_equal(raster,np.eye(4));np.testing.assert_allclose(prior,self.matrix([.375,-.25]))
    def test_mislabeled_raster_projection_is_rejected(self):
        meta=self.meta();meta['current_vp']=np.eye(4).ravel().tolist()
        with self.assertRaises(ValueError):camera_matrices(meta)
    def test_coordinated_metadata_requires_independent_raw_matrices(self):
        meta=self.meta([0,0],[0,0]);del meta['raw_current_vp']
        with self.assertRaises(ValueError):camera_matrices(meta)
