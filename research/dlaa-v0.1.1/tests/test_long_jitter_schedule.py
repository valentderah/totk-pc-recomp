import os
import ctypes,subprocess,unittest
from pathlib import Path

class LongJitterScheduleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root=Path(__file__).resolve().parents[1];c=root/'build/long_jitter_test.c';dll=root/'build/long_jitter_test.dll'
        c.write_text('#include "../native/raster_jitter_schedule.h"\n__declspec(dllexport) int schedule(unsigned long long f,unsigned long long end,unsigned n,float *j){return rj_schedule_count(f,end,n,j);}\n__declspec(dllexport) int abort_needed(unsigned a,int s,unsigned long long f,unsigned long long end,unsigned n){return rj_abort_needed_count(a,s,f,end,n);}\n')
        subprocess.run([str(Path(os.environ['TOTK_DLAA_PACKAGE'])/'llvm/bin/clang.exe'),'-shared','-O2',str(c),'-o',str(dll)],check=True,capture_output=True)
        cls.dll=ctypes.CDLL(str(dll));cls.schedule=cls.dll.schedule;cls.schedule.argtypes=[ctypes.c_uint64,ctypes.c_uint64,ctypes.c_uint,ctypes.POINTER(ctypes.c_float)];cls.schedule.restype=ctypes.c_int
        cls.abort=cls.dll.abort_needed;cls.abort.argtypes=[ctypes.c_uint,ctypes.c_int,ctypes.c_uint64,ctypes.c_uint64,ctypes.c_uint];cls.abort.restype=ctypes.c_int
    def test_real_window_repeats_phase_and_restores(self):
        pairs=[]
        for f in range(1769,1801):
            j=(ctypes.c_float*2)();self.assertEqual(self.schedule(f,1800,32,j),1);pairs.append(list(j))
        self.assertEqual(pairs[:8],pairs[8:16]);self.assertEqual(pairs[:8],pairs[24:])
        for f,n in [(1768,32),(1801,32),(1770,31),(1770,64)]:
            j=(ctypes.c_float*2)(1,1);self.assertEqual(self.schedule(f,1800,n,j),0);self.assertEqual(list(j),[0,0])
    def test_incomplete_long_session_cleanup(self):
        self.assertEqual(self.abort(31,0,1801,1800,32),1)
        self.assertEqual(self.abort(31,0,1800,1800,32),0)
        self.assertEqual(self.abort(32,0,1801,1800,32),0)
        self.assertEqual(self.abort(31,1,1801,1800,32),0)
