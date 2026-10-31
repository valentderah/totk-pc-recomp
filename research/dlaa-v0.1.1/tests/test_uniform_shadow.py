import os
import ctypes,subprocess,unittest
from pathlib import Path

class UniformShadowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        p=Path(__file__).resolve().parents[1];c=p/'build/uniform_shadow_test.c';d=p/'build/uniform_shadow_test.dll'
        c.write_text('#include "../native/uniform_shadow.h"\n__declspec(dllexport) void snapshot(void *g,void *s,const void *p,unsigned n){us_copy_snapshot(g,s,p,n);}\n')
        subprocess.run([str(Path(os.environ['TOTK_DLAA_PACKAGE'])/'llvm/bin/clang.exe'),'-shared','-O2',str(c),'-o',str(d)],check=True,capture_output=True)
        cls.dll=ctypes.CDLL(str(d));cls.copy=cls.dll.snapshot;cls.copy.argtypes=[ctypes.c_void_p]*3+[ctypes.c_uint]
    def test_shadow_and_gpu_share_immutable_snapshot(self):
        raw=(ctypes.c_ubyte*256)(*range(256));gpu=(ctypes.c_ubyte*256)();shadow=(ctypes.c_ubyte*256)()
        self.copy(gpu,shadow,raw,256);raw[13]=0
        self.assertEqual(bytes(gpu),bytes(shadow));self.assertEqual(shadow[13],13)
    def test_legacy_copy_without_shadow(self):
        raw=(ctypes.c_ubyte*4)(1,2,3,4);gpu=(ctypes.c_ubyte*4)()
        self.copy(gpu,None,raw,4);self.assertEqual(bytes(gpu),bytes(raw))
