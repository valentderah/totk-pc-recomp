import os
import ctypes,subprocess,unittest
from pathlib import Path
class ProjectionShaderPairTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  p=Path(__file__).resolve().parents[1];c=p/'build/projection_shader_pair_test.c';d=p/'build/projection_shader_pair_test.dll'
  c.write_text('#include "../native/projection_shader_pair.h"\nstatic PjShaderPair cache;static unsigned calls;static unsigned long long hash(const void *p){calls++;return (unsigned long long)(uintptr_t)p*101;}\n__declspec(dllexport) void reset(void){memset(&cache,0,sizeof cache);calls=0;}\n__declspec(dllexport) unsigned update(unsigned long long v,unsigned long long f,unsigned long long *o){pj_shader_pair_update(&cache,(const void*)(uintptr_t)v,(const void*)(uintptr_t)f,hash);o[0]=cache.hash[0];o[1]=cache.hash[1];return calls;}\n')
  subprocess.run([str(Path(os.environ['TOTK_DLAA_PACKAGE'])/'llvm/bin/clang.exe'),'-shared','-O2',str(c),'-o',str(d)],check=True,capture_output=True)
  cls.dll=ctypes.CDLL(str(d));cls.dll.update.argtypes=[ctypes.c_ulonglong,ctypes.c_ulonglong,ctypes.c_void_p]
 def setUp(self):self.dll.reset()
 def update(self,v,f):
  out=(ctypes.c_ulonglong*2)();n=self.dll.update(v,f,out);self.assertEqual(list(out),[v*101,f*101]);return n
 def test_repeated_pair_avoids_repeated_lookups(self):
  self.assertEqual(self.update(1,2),2)
  for _ in range(100):self.assertEqual(self.update(1,2),2)
 def test_stage_changes_invalidate_independently(self):
  self.assertEqual(self.update(1,2),2);self.assertEqual(self.update(3,2),3);self.assertEqual(self.update(3,4),4);self.assertEqual(self.update(1,2),6)
 def test_null_shader_is_a_valid_zero_identity(self):
  self.assertEqual(self.update(0,0),2);self.assertEqual(self.update(0,0),2);self.assertEqual(self.update(1,0),3);self.assertEqual(self.update(0,2),5)
