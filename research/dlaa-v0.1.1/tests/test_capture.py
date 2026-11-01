import unittest
from pathlib import Path
from capture import parse_cameras, parse_images,select_camera
import numpy as np

SAMPLE='''pass 0024: render pass, draws 3, area 8x4 layers 1, fence 10, frame 100
    vs ubo c4 addr 0x123 size 0x940
      [0] 1 0 0 0 [1] 0 1 0 0 [2] 0 0 1 0 [3] 1 0 0 0
      [4] 0 1 0 0 [5] 0 0 1 0 [6] 0 0 0 1 [7] 1 0 0 0
      [8] 0 1 0 0 [9] 0 0 1 0 [10] 0 0 0 1 [11] 0 0 0 0
      [12] 0 0 0 0 [13] 0 0 0 0 [14] 0 0 0 0 [15] 0 0 0 0
'''

class CaptureTests(unittest.TestCase):
    def test_scene_camera_is_selected_by_depth_resource(self):
        a={'pass':2,'depth_resource':'0xaa','vp':np.eye(4)}
        b={'pass':3,'depth_resource':'0xbb','vp':2*np.eye(4)}
        picked=select_camera({'observations':[a,b]},4,'0xaa')
        self.assertEqual(picked['pass'],2)
        with self.assertRaises(ValueError):select_camera({'observations':[a,b]},4,'0xcc')

    def test_conflicting_same_depth_cameras_are_rejected(self):
        a={'pass':2,'depth_resource':'0xaa','vp':np.eye(4)}
        b={'pass':3,'depth_resource':'0xaa','vp':2*np.eye(4)}
        with self.assertRaises(ValueError):select_camera({'observations':[a,b]},4,'0xaa')
    def test_reads_frame_and_validated_camera_without_address_assumption(self):
        cameras=parse_cameras(SAMPLE,8,4)
        self.assertEqual(list(cameras),[100])
        self.assertEqual(cameras[100]['pass'],24)

    def test_rejects_incomplete_camera_block(self):
        self.assertEqual(parse_cameras(SAMPLE.split('[12]')[0],8,4),{})

    def test_rejects_wrong_resolution_and_bad_matrix(self):
        self.assertEqual(parse_cameras(SAMPLE,16,8),{})
        self.assertEqual(parse_cameras(SAMPLE.replace('[4] 0 1 0 0','[4] 0 3 0 0'),8,4),{})

    def test_image_metadata_requires_frame_and_keeps_true_dimensions(self):
        lines='pass_0024_td_8x4_126_d32f 8x4x1 min 0 0 0 1 max 1 0 0 1 frame 100\nlegacy 8x4x1 min 0\n'
        items=parse_images(lines)
        self.assertEqual(len(items),1)
        self.assertEqual((items[0]['frame'],items[0]['width'],items[0]['height']),(100,8,4))

if __name__=='__main__':unittest.main(verbosity=2)
