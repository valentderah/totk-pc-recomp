import os
import ctypes,subprocess,unittest
from pathlib import Path
class ProjectionValidationCacheTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  p=Path(__file__).resolve().parents[1];c=p/'build/projection_validation_cache_test.c';d=p/'build/projection_validation_cache_test.dll'
  c.write_text('#include "../native/projection_validation_cache.h"\nstatic PjValidationCache cache;\n__declspec(dllexport) void reset(void){memset(&cache,0,sizeof cache);}\n__declspec(dllexport) void put(const uint64_t *w,int result){PjValidationKey k;memcpy(k.words,w,sizeof k.words);pj_validation_store(&cache,&k,result);}\n__declspec(dllexport) int get(const uint64_t *w,int *out){PjValidationKey k;memcpy(k.words,w,sizeof k.words);return pj_validation_find(&cache,&k,out);}\n')
  subprocess.run([str(Path(os.environ['TOTK_DLAA_PACKAGE'])/'llvm/bin/clang.exe'),'-shared','-O2',str(c),'-o',str(d)],check=True,capture_output=True)
  cls.dll=ctypes.CDLL(str(d));cls.dll.put.argtypes=[ctypes.c_void_p,ctypes.c_int];cls.dll.get.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_int)]
 def setUp(self):self.dll.reset();self.key=[0x10000000,0x20000000,45,1400,1400,0x940,2560,1440]
 def put(self,key,value):self.dll.put((ctypes.c_ulonglong*8)(*key),value)
 def get(self,key):
  out=ctypes.c_int(99);found=self.dll.get((ctypes.c_ulonglong*8)(*key),ctypes.byref(out));return found,out.value
 def test_accepted_and_rejected_results_are_distinct_from_miss(self):
  self.assertEqual(self.get(self.key),(0,99));self.put(self.key,1);self.assertEqual(self.get(self.key),(1,1));self.put(self.key,0);self.assertEqual(self.get(self.key),(1,0))
 def test_each_dependency_invalidates(self):
  self.put(self.key,1)
  for field in range(8):
   k=self.key.copy();k[field]+=1
   with self.subTest(field=field):self.assertEqual(self.get(k),(0,99))
 def test_reused_address_with_new_snapshot_does_not_reuse_verdict(self):
  self.put(self.key,1);k=self.key.copy();k[2]+=1;self.assertEqual(self.get(k),(0,99));self.put(k,0);self.assertEqual(self.get(k),(1,0))
 def test_many_collisions_never_return_another_keys_result(self):
  keys=[]
  for i in range(512):
   k=self.key.copy();k[0]+=i*0x1000;k[6]+=i%3;keys.append((k,i%2));self.put(k,i%2);self.assertEqual(self.get(k),(1,i%2))
  hits=0
  for k,want in keys:
   found,value=self.get(k)
   if found:hits+=1;self.assertEqual(value,want)
  self.assertGreater(hits,0)
