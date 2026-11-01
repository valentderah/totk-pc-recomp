import unittest
import numpy as np
from raster_motion import triangles_from_indices,rasterize_motion

class RasterMotionTests(unittest.TestCase):
    def test_previous_occlusion_rejects_temporal_correspondence(self):
        clip=np.array([[-1,-1,0,1],[1,-1,0,1],[-1,1,0,1]],float)
        _,_,valid=rasterize_motion(clip,clip,[[0,1,2]],8,8,scene_depth=np.full((8,8),.5),previous_scene_depth=np.full((8,8),.2))
        self.assertFalse(valid.any())
    def test_index_base_and_out_of_range_are_checked(self):
        np.testing.assert_equal(triangles_from_indices(np.array([0,1,2]),4,2,5),[[2,3,4]])
        with self.assertRaises(ValueError):triangles_from_indices(np.array([0,1,9]),4,0,5)

    def test_stationary_triangle_has_zero_motion_at_covered_pixel_centers(self):
        clip=np.array([[-1,-1,0,1],[1,-1,0,1],[-1,1,0,1]],float)
        flow,depth,valid=rasterize_motion(clip,clip,[[0,1,2]],8,8,scene_depth=np.full((8,8),.5))
        self.assertGreater(valid.sum(),10)
        np.testing.assert_allclose(flow[valid],0,atol=1e-6)

    def test_previous_translation_produces_current_to_previous_pixels(self):
        now=np.array([[-1,-1,0,1],[1,-1,0,1],[-1,1,0,1]],float)
        old=now.copy();old[:,0]+=.25
        flow,_,valid=rasterize_motion(now,old,[[0,1,2]],8,8,scene_depth=np.full((8,8),.5))
        np.testing.assert_allclose(flow[valid],np.tile([1,0],(valid.sum(),1)),atol=1e-6)

    def test_hidden_geometry_does_not_claim_visible_motion(self):
        clip=np.array([[-1,-1,.5,1],[1,-1,.5,1],[-1,1,.5,1]],float)
        _,_,valid=rasterize_motion(clip,clip,[[0,1,2]],8,8,scene_depth=np.full((8,8),.2))
        self.assertFalse(valid.any())

    def test_previous_projection_is_perspective_correct(self):
        # Same current NDC triangle; old clip.w differs by vertex.
        now=np.array([[-1,-1,0,1],[2,-2,0,2],[-1,1,0,1]],float)
        old=now.copy();old[:,3]*=[1,2,1]
        flow,_,valid=rasterize_motion(now,old,[[0,1,2]],8,8,scene_depth=np.full((8,8),.5))
        y,x=5,2;self.assertTrue(valid[y,x])
        lam=np.array([.375,.3125,.3125]) # for this pixel: screen vertices (0,8),(8,8),(0,0)
        # Correct barycentrics are derived directly from its pixel center.
        lam[1]=(x+.5)/8;lam[2]=1-(y+.5)/8;lam[0]=1-lam[1]-lam[2]
        interpolated=(lam[:,None]*old/now[:,3:4]).sum(axis=0)
        expected=np.array([(interpolated[0]/interpolated[3]+1)*4,(1-interpolated[1]/interpolated[3])*4])-[x+.5,y+.5]
        np.testing.assert_allclose(flow[y,x],expected,atol=1e-6)
