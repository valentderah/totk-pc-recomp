import unittest
import numpy as np
from motion import camera_flow, validated_camera

class CameraFlowTests(unittest.TestCase):
    def test_static_camera_zero_motion_at_pixel_centers(self):
        d=np.full((4,6),0.5)
        flow,valid=camera_flow(d,np.eye(4),np.eye(4))
        self.assertTrue(valid.all())
        np.testing.assert_allclose(flow,0,atol=1e-12)

    def test_previous_camera_translation_is_known_pixel_shift(self):
        previous=np.eye(4);previous[0,3]=0.5
        flow,valid=camera_flow(np.full((4,8),0.5),np.eye(4),previous)
        np.testing.assert_allclose(flow[valid,0],2)
        np.testing.assert_allclose(flow[valid,1],0)
        self.assertFalse(valid[:,-2:].any())

    def test_nan_infinite_and_out_of_range_depth_are_invalid(self):
        depth=np.array([[np.nan,np.inf,-0.1,1.1,0.5]])
        flow,valid=camera_flow(depth,np.eye(4),np.eye(4))
        np.testing.assert_array_equal(valid,[[False,False,False,False,True]])
        self.assertTrue(np.isnan(flow[~valid]).all())

    def test_behind_previous_camera_is_invalid(self):
        previous=np.eye(4);previous[3,3]=-1
        _,valid=camera_flow(np.full((3,3),0.5),np.eye(4),previous)
        self.assertFalse(valid.any())

    def test_singular_camera_is_rejected(self):
        with self.assertRaises(ValueError):camera_flow(np.ones((3,3)),np.zeros((4,4)),np.eye(4))

    def test_previously_occluded_points_are_rejected(self):
        current=np.full((4,6),0.8)
        _,valid=camera_flow(current,np.eye(4),np.eye(4),previous_depth=np.full((4,6),0.2))
        self.assertFalse(valid.any())

    def test_consistent_previous_depth_stays_valid(self):
        depth=np.full((4,6),0.5)
        _,valid=camera_flow(depth,np.eye(4),np.eye(4),previous_depth=depth)
        self.assertTrue(valid.all())

    def test_perspective_raw_depth_is_unprojected_with_opengl_convention(self):
        near,far=0.1,100
        projection=np.array([[1,0,0,0],[0,1,0,0],[0,0,-(far+near)/(far-near),-2*far*near/(far-near)],[0,0,-1,0]],float)
        view=np.eye(4);view[0,3]=0.2
        point=projection@np.array([0,0,-2,1])
        depth=(point[2]/point[3]+1)/2
        flow,valid=camera_flow(np.full((6,8),depth),projection,projection@view)
        np.testing.assert_allclose(flow[valid,0],0.4,atol=1e-10)
        np.testing.assert_allclose(flow[valid,1],0,atol=1e-10)

    def test_validated_camera_rejects_wrong_matrix_product(self):
        rows=np.zeros((16,4));rows[:3]=np.eye(4)[:3];rows[7:11]=np.eye(4);rows[3:7]=np.eye(4)
        view,projection,vp=validated_camera(rows)
        np.testing.assert_allclose(vp,np.eye(4))
        rows[4,1]=2
        with self.assertRaises(ValueError):validated_camera(rows)

if __name__=='__main__':unittest.main(verbosity=2)
