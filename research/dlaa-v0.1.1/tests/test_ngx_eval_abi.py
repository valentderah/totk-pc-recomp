import ctypes
import unittest
from pathlib import Path

class Frame(ctypes.Structure):
    _fields_=[('version',ctypes.c_uint32),('size',ctypes.c_uint32),('width',ctypes.c_uint32),('height',ctypes.c_uint32),
        ('cb',ctypes.c_uint64),('images',ctypes.c_uint64*5),('views',ctypes.c_uint64*5),('formats',ctypes.c_uint32*5),('reset',ctypes.c_uint32),
        ('jx',ctypes.c_float),('jy',ctypes.c_float),('ms',ctypes.c_float),('reserved',ctypes.c_uint32)]

class NgxEvalAbiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dll=ctypes.CDLL(str(Path(__file__).resolve().parents[1]/'build/ngx_bridge.dll'))
        cls.validate=cls.dll.totk_ngx_validate_eval_frame;cls.validate.argtypes=[ctypes.POINTER(Frame)];cls.validate.restype=ctypes.c_int
    def frame(self):
        f=Frame();f.version=1;f.size=ctypes.sizeof(Frame);f.width=2560;f.height=1440;f.cb=1;f.images[:]=[11,12,13,14,15];f.views[:]=[21,22,23,24,25];f.formats[:]=[122,126,103,100,97];f.reset=1;f.ms=16.6667
        return f
    def test_abi_and_valid_packet_without_sdk_execution(self):
        self.assertEqual(ctypes.sizeof(Frame),144);self.assertEqual(self.validate(ctypes.byref(self.frame())),0)
    def test_null_and_version_size(self):
        self.assertNotEqual(self.validate(None),0)
        for name in ('version','size'):
            f=self.frame();setattr(f,name,0);self.assertNotEqual(self.validate(ctypes.byref(f)),0)
    def test_missing_view_or_wrong_format(self):
        f=self.frame();f.views[2]=0;self.assertNotEqual(self.validate(ctypes.byref(f)),0)
        f=self.frame();f.formats[2]=109;self.assertNotEqual(self.validate(ctypes.byref(f)),0)
    def test_dimensions_reset_and_jitter_scope(self):
        for name,value in [('width',0),('height',99999),('reset',0),('jx',.25),('jy',float('nan')),('ms',0)]:
            f=self.frame();setattr(f,name,value);self.assertNotEqual(self.validate(ctypes.byref(f)),0)
    def test_output_cannot_alias_an_input(self):
        f=self.frame();f.images[4]=f.images[0];self.assertNotEqual(self.validate(ctypes.byref(f)),0)
        f=self.frame();f.views[4]=f.views[2];self.assertNotEqual(self.validate(ctypes.byref(f)),0)
