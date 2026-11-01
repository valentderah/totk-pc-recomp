import unittest
import numpy as np
from skinning import decode_attribute, rigid_clip_positions, weighted_clip_positions

class SkinningTests(unittest.TestCase):
    def test_half_float_fetch_keeps_position_components(self):
        raw=np.array([1.5,-2.25,3.125,0.0],dtype='<f2').tobytes()
        np.testing.assert_equal(decode_attribute(raw,97,0,8,1),[[1.5,-2.25,3.125,0]])
    def test_unused_fourth_channel_can_overlap_next_vertex_record(self):
        raw=bytes(range(14))
        np.testing.assert_equal(decode_attribute(raw,41,3,6,2),[[3,4,5,6],[9,10,11,12]])

    def test_weighted_pose_combines_multiple_bones(self):
        palette=np.zeros((2,3,4));palette[:,:,:3]=np.eye(3);palette[1,0,3]=4
        clip=weighted_clip_positions(np.array([[1,2,3]]),np.array([[0,1]]),np.array([[.25,.75]]),palette,[0,0,0],np.eye(4),np.eye(4))
        np.testing.assert_allclose(clip,[[4,2,3,1]])
    def test_unsigned_weights_normalize_but_indices_remain_integers(self):
        raw=bytes([0,64,128,255])
        np.testing.assert_allclose(decode_attribute(raw,37,0,4,1),[[0,64/255,128/255,1]],rtol=1e-7)
        np.testing.assert_equal(decode_attribute(raw,41,0,4,1),[[0,64,128,255]])
    def test_interleaved_position_and_unsigned_bone_index(self):
        raw=bytearray(32)
        np.ndarray((2,3),dtype='<f4',buffer=raw,strides=(16,4))[:]=[[1,2,3],[4,5,6]]
        raw[12]=7;raw[28]=12
        np.testing.assert_equal(decode_attribute(raw,106,0,16,2),[[1,2,3],[4,5,6]])
        np.testing.assert_equal(decode_attribute(raw,13,12,16,2)[:,0],[7,12])

    def test_read_past_end_is_rejected(self):
        with self.assertRaises(ValueError):decode_attribute(bytes(31),106,0,16,3)

    def test_pose_and_world_origin_then_camera_projection(self):
        palette=np.array([[[1,0,0,2],[0,1,0,3],[0,0,1,4]]],np.float32)
        view=np.eye(4);view[0,3]=-10
        clip=rigid_clip_positions(np.array([[1,1,1]]),np.array([0]),palette,[10,20,30],view,np.eye(4))
        np.testing.assert_allclose(clip,[[3,24,35,1]])

    def test_bone_index_outside_palette_is_rejected(self):
        with self.assertRaises(ValueError):
            rigid_clip_positions(np.zeros((1,3)),np.array([1]),np.zeros((1,3,4)),[0,0,0],np.eye(4),np.eye(4))

    def test_changing_pose_moves_vertex_with_static_camera(self):
        palette=np.zeros((1,3,4));palette[0,:,:3]=np.eye(3)
        args=(np.array([[.1,.2,.3]]),np.array([0]))
        old=rigid_clip_positions(*args,palette,[0,0,0],np.eye(4),np.eye(4))
        palette[0,0,3]=.25
        current=rigid_clip_positions(*args,palette,[0,0,0],np.eye(4),np.eye(4))
        np.testing.assert_allclose(current-old,[[.25,0,0,0]],atol=1e-7)
