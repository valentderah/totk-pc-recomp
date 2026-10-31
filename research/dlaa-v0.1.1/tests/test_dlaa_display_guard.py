import os
import ctypes,subprocess,unittest
from pathlib import Path

class DisplayGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        p=Path(__file__).resolve().parents[1];c=p/'build/display_guard_test.c';d=p/'build/display_guard_test.dll'
        c.write_text('#include "../native/dlaa_display_guard.h"\n__declspec(dllexport) int guard(unsigned w,unsigned h,unsigned a,unsigned b,unsigned s,unsigned t){return dd_supported(w,h,a,b,s,t);}\n__declspec(dllexport) int eligible(unsigned w,unsigned h,int failed){return dd_live_eligible(w,h,failed);}\n')
        subprocess.run([str(Path(os.environ['TOTK_DLAA_PACKAGE'])/'llvm/bin/clang.exe'),'-shared','-O2',str(c),'-o',str(d)],check=True,capture_output=True)
        cls.dll=ctypes.CDLL(str(d));cls.guard=cls.dll.guard;cls.guard.argtypes=[ctypes.c_uint]*6
        cls.eligible=cls.dll.eligible
    def test_live_reduced_scene_and_poisoned_session_fall_back(self):
        self.assertEqual(self.eligible(2560,1440,0),1)
        self.assertEqual(self.eligible(2048,1152,0),0)
        self.assertEqual(self.eligible(2560,1440,1),0)
    def test_requires_native_supported_format_conversion(self):
        self.assertEqual(self.guard(2560,1440,97,122,0x400,0x800),1)
        for args in ((2048,1152,97,122,0x400,0x800),(2560,1440,126,122,0x400,0x800),(2560,1440,97,43,0x400,0x800),(2560,1440,97,122,0,0x800),(2560,1440,97,122,0x400,0)):
            self.assertEqual(self.guard(*args),0)
