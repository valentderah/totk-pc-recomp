import ctypes
import unittest
from test_ngx_eval_abi import Frame
import test_ngx_eval_abi as abi

class NgxSequenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        abi.NgxEvalAbiTests.setUpClass()
        cls.validate=abi.NgxEvalAbiTests.dll.totk_ngx_validate_sequence_frame
        cls.validate.argtypes=[ctypes.POINTER(Frame)];cls.validate.restype=ctypes.c_int

    def test_sequence_accepts_first_reset_and_history(self):
        for reset in (0,1):
            f=abi.NgxEvalAbiTests().frame();f.reset=reset
            self.assertEqual(self.validate(ctypes.byref(f)),0)

    def test_sequence_retains_packet_guards(self):
        for key,value in [('reset',2),('jx',.25),('jy',float('inf')),('width',0),('ms',-1)]:
            f=abi.NgxEvalAbiTests().frame();setattr(f,key,value)
            self.assertNotEqual(self.validate(ctypes.byref(f)),0)
        f=abi.NgxEvalAbiTests().frame();f.images[4]=f.images[1]
        self.assertNotEqual(self.validate(ctypes.byref(f)),0)
