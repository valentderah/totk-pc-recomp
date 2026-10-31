import unittest
import numpy as np
from optical import observed_flow

class OpticalTests(unittest.TestCase):
    def test_detects_known_image_translation(self):
        rng=np.random.default_rng(123)
        previous=rng.integers(0,256,(100,140),dtype=np.uint8)
        current=np.roll(previous,3,axis=1)
        flow,confidence=observed_flow(previous,current)
        good=confidence[20:-20,20:-20]
        vectors=flow[20:-20,20:-20][good]
        self.assertGreater(len(vectors),100)
        self.assertAlmostEqual(float(np.median(vectors[:,0])),-3,delta=0.3)
        self.assertAlmostEqual(float(np.median(vectors[:,1])),0,delta=0.3)

    def test_flat_image_not_claimed_as_confident_motion(self):
        _,confidence=observed_flow(np.full((64,64),128,np.uint8),np.full((64,64),128,np.uint8))
        self.assertFalse(confidence.any())

if __name__=='__main__':unittest.main(verbosity=2)
