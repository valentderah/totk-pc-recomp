import os
import ctypes,subprocess,unittest
from pathlib import Path

class GpuTimestampMathTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  p=Path(__file__).resolve().parents[1];c=p/'build/gpu_timestamp_math_test.c';d=p/'build/gpu_timestamp_math_test.dll'
  c.write_text('#include "../native/gpu_timestamp_math.h"\n__declspec(dllexport) int times(const unsigned long long *v,unsigned mask,unsigned bits,double period,double *out){return gt_intervals(v,mask,bits,period,out); }\n')
  subprocess.run([str(Path(os.environ['TOTK_DLAA_PACKAGE'])/'llvm/bin/clang.exe'),'-shared','-O2',str(c),'-o',str(d)],check=True,capture_output=True)
  cls.dll=ctypes.CDLL(str(d));cls.dll.times.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_uint,ctypes.c_double,ctypes.c_void_p]
 def calc(self,values=None,mask=255,bits=64,period=1):
  v=(ctypes.c_ulonglong*8)(*(values or [i*1000000 for i in range(8)]));o=(ctypes.c_double*7)();r=self.dll.times(v,mask,bits,period,o);return r,list(o)
 def test_exact_millisecond_conversion(self):
  ok,out=self.calc(period=2);self.assertEqual(ok,1);self.assertEqual(out,[2]*7)
 def test_hardware_counter_wrap(self):
  ok,out=self.calc([250,255,3,5,8,11,14,16],bits=8,period=1000000);self.assertEqual(ok,1);self.assertEqual(out,[5,4,2,3,3,3,2])
 def test_full_width_wrap(self):
  ok,out=self.calc([(1<<64)-2,1,2,3,4,5,6,7],period=1000000);self.assertEqual(ok,1);self.assertEqual(out[0],3)
 def test_missing_queries_and_invalid_capability_rejected(self):
  for kw in ({'mask':127},{'mask':0},{'bits':0},{'bits':65},{'period':0},{'period':-1},{'period':float('nan')},{'period':float('inf')}):
   with self.subTest(kw=kw):self.assertEqual(self.calc(**kw)[0],0)
