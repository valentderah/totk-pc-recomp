import ctypes
import unittest
from pathlib import Path

class NgxBridgeTests(unittest.TestCase):
    def test_missing_handles_rejected_before_sdk_initialization(self):
        path=Path(__file__).resolve().parents[1]/'build/ngx_bridge.dll'
        if not path.exists():self.skipTest('Optional MSVC NGX bridge not built')
        dll=ctypes.CDLL(str(path));probe=dll.totk_ngx_capability_probe
        probe.argtypes=[ctypes.c_void_p]*5+[ctypes.c_char_p]*3;probe.restype=ctypes.c_int
        self.assertEqual(probe(None,None,None,None,None,None,None,None),-3)

    def test_extension_query_missing_handles_rejected(self):
        path=Path(__file__).resolve().parents[1]/'build/ngx_bridge.dll'
        if not path.exists():self.skipTest('Optional MSVC NGX bridge not built')
        dll=ctypes.CDLL(str(path));query=dll.totk_ngx_device_extensions
        query.argtypes=[ctypes.c_void_p]*2+[ctypes.c_char_p]*3+[ctypes.c_uint,ctypes.c_char_p];query.restype=ctypes.c_int
        self.assertEqual(query(None,None,None,None,None,0,None),-3)
