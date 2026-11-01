import os
import ctypes,subprocess,unittest
from pathlib import Path
import numpy as np
from test_ngx_eval_abi import Frame
import test_ngx_eval_abi as abi

class RasterJitterSequenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root=Path(__file__).resolve().parents[1];src=root/'build/raster_schedule_test.c';dll=root/'build/raster_schedule_test.dll'
        src.write_text('#include "../native/raster_jitter_schedule.h"\n__declspec(dllexport) int schedule(unsigned long long frame,unsigned long long end,float *j){return rj_schedule(frame,end,j);}\n__declspec(dllexport) int abort_needed(unsigned attempted,int scheduled,unsigned long long frame,unsigned long long end){return rj_abort_needed(attempted,scheduled,frame,end);}\n')
        subprocess.run([str(Path(os.environ['TOTK_DLAA_PACKAGE'])/'llvm/bin/clang.exe'),'-shared','-O2',str(src),'-o',str(dll)],check=True,capture_output=True)
        cls.dll=ctypes.CDLL(str(dll));cls.fn=cls.dll.schedule;cls.fn.argtypes=[ctypes.c_uint64,ctypes.c_uint64,ctypes.POINTER(ctypes.c_float)];cls.fn.restype=ctypes.c_int
        cls.abort=cls.dll.abort_needed;cls.abort.argtypes=[ctypes.c_uint,ctypes.c_int,ctypes.c_uint64,ctypes.c_uint64];cls.abort.restype=ctypes.c_int
    def test_partial_session_cleanup_only_after_window(self):
        for n in range(1,8):
            self.assertEqual(self.abort(n,0,1600,1600),0)
            self.assertEqual(self.abort(n,0,1601,1600),1)
            self.assertEqual(self.abort(n,1,1601,1600),0)
        self.assertEqual(self.abort(0,0,1601,1600),0)
        self.assertEqual(self.abort(8,0,1601,1600),0)
    def test_exact_bounded_schedule_and_restoration(self):
        expected=[(0,-1/6),(-.25,1/6),(.25,-7/18),(-.375,-1/18),(.125,5/18),(-.125,-5/18),(.375,1/18),(-.4375,7/18)]
        for i,pair in enumerate(expected):
            j=(ctypes.c_float*2)();self.assertEqual(self.fn(1593+i,1600,j),1);np.testing.assert_allclose(list(j),pair,atol=2e-8)
        for frame,end in [(1592,1600),(1601,1600),(0,0),(1,7)]:
            j=(ctypes.c_float*2)(99,99);self.assertEqual(self.fn(frame,end,j),0);self.assertEqual(list(j),[0,0])

class NgxRasterJitterAbiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        abi.NgxEvalAbiTests.setUpClass();cls.validate=abi.NgxEvalAbiTests.dll.totk_ngx_validate_jitter_sequence_frame;cls.validate.argtypes=[ctypes.POINTER(Frame)];cls.validate.restype=ctypes.c_int
    def test_new_entry_accepts_bounded_jitter_only(self):
        for x,y in [(0,-1/6),(-.4375,7/18),(.5,-.5)]:
            f=abi.NgxEvalAbiTests().frame();f.reset=0;f.jx=x;f.jy=y;self.assertEqual(self.validate(ctypes.byref(f)),0)
        for key,value in [('jx',.501),('jy',float('nan')),('reset',2),('reserved',1)]:
            f=abi.NgxEvalAbiTests().frame();setattr(f,key,value);self.assertNotEqual(self.validate(ctypes.byref(f)),0)
