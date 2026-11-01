import copy,unittest
import test_projection_motion_output as fixture
from analyze_dlaa_raster_jitter import schedule,validate_metadata_window

class RasterMetadataTests(unittest.TestCase):
    def window(self):
        f=fixture.ProjectionMotionOutputTests()
        return [f.meta(n,schedule(n,1200).tolist(),schedule(n-1,1200).tolist()) for n in range(1192,1202)]
    def test_complete_window_and_restoration(self):
        validate_metadata_window(self.window(),1200)
    def test_wrong_offset_or_missing_frame_rejected(self):
        rows=self.window();rows[-1]['previous_jitter']=[0,0]
        with self.assertRaises(ValueError):validate_metadata_window(rows,1200)
        with self.assertRaises(ValueError):validate_metadata_window(self.window()[1:],1200)
    def test_broken_physical_history_rejected(self):
        rows=self.window();rows[4]['raw_previous_vp'][3]=1
        with self.assertRaises(ValueError):validate_metadata_window(rows,1200)
