import os
import ctypes,subprocess,unittest
from pathlib import Path

class LiveScheduleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        p=Path(__file__).resolve().parents[1];c=p/'build/live_schedule_test.c';d=p/'build/live_schedule_test.dll'
        c.write_text('#include "../native/raster_jitter_schedule.h"\n__declspec(dllexport) int live(unsigned long long f,unsigned long long ready,float *j){return rj_live_schedule(f,ready,j);}\n')
        subprocess.run([str(Path(os.environ['TOTK_DLAA_PACKAGE'])/'llvm/bin/clang.exe'),'-shared','-O2',str(c),'-o',str(d)],check=True,capture_output=True)
        cls.dll=ctypes.CDLL(str(d));cls.live=cls.dll.live;cls.live.argtypes=[ctypes.c_uint64,ctypes.c_uint64,ctypes.POINTER(ctypes.c_float)]
    def test_starts_only_after_recent_eligible_scene_and_repeats(self):
        j=(ctypes.c_float*2)()
        for f,ready in ((999,998),(1000,0),(1002,1000)):
            self.assertEqual(self.live(f,ready,j),0);self.assertEqual(list(j),[0,0])
        self.assertEqual(self.live(1000,999,j),1);first=list(j)
        self.assertEqual(self.live(1008,1007,j),1);self.assertEqual(list(j),first)
