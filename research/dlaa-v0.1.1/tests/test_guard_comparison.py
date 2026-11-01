import unittest
import numpy as np
from compare_temporal_guard import provenance_matches

class GuardComparisonTests(unittest.TestCase):
    def test_only_camera_validity_rounding_at_half_is_tolerated(self):
        accepted=provenance_matches(np.array([0,1,0,0]),np.array([2,2,1,2]),np.array([.5000001,.5,.5,1]),np.array([.4999999,.5,.5,.4995]))
        np.testing.assert_array_equal(accepted,[True,False,False,False])
    def test_disagreement_away_from_threshold_is_not_rounding(self):
        self.assertFalse(provenance_matches(np.array([0]),np.array([2]),np.array([.51]),np.array([.49]))[0])
