import unittest
import numpy as np
from analyze_dlaa_display import validate_conversion

class DisplayOutputTests(unittest.TestCase):
    def test_quantized_resolve_accepted_wrong_image_rejected(self):
        a=np.full((20,20,3),.75,np.float32)
        self.assertLess(validate_conversion(a,a)['max_error_ulps'],.01)
        b=a.copy();b[2,2,0]=.8
        with self.assertRaises(ValueError):validate_conversion(a,b)
        b[2,2,0]=np.nan
        with self.assertRaises(ValueError):validate_conversion(a,b)
