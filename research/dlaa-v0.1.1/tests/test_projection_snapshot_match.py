import os
import ctypes,subprocess,unittest
from pathlib import Path
class ProjectionSnapshotMatchTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  p=Path(__file__).resolve().parents[1];c=p/'build/projection_snapshot_match_test.c';d=p/'build/projection_snapshot_match_test.dll'
  c.write_text('#include "../native/projection_snapshot_match.h"\n__declspec(dllexport) int matches(const void *copy,const void *now,unsigned n,unsigned long long owner,unsigned long long oldgen,unsigned long long gen,int fast){return pj_snapshot_matches(copy,now,n,(uintptr_t)owner,oldgen,gen,fast);}\n')
  subprocess.run([str(Path(os.environ['TOTK_DLAA_PACKAGE'])/'llvm/bin/clang.exe'),'-shared','-O2',str(c),'-o',str(d)],check=True,capture_output=True)
  cls.dll=ctypes.CDLL(str(d));cls.dll.matches.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_uint,ctypes.c_ulonglong,ctypes.c_ulonglong,ctypes.c_ulonglong,ctypes.c_int]
 def arrays(self):return (ctypes.c_ubyte*32)(*range(32)),(ctypes.c_ubyte*32)(*range(32))
 def test_immutable_snapshot_identity_matches(self):
  a,b=self.arrays();self.assertEqual(self.dll.matches(a,b,32,ctypes.addressof(b),7,7,1),1)
 def test_new_generation_rechecks_reused_storage(self):
  a,b=self.arrays();b[8]=99;self.assertEqual(self.dll.matches(a,b,32,ctypes.addressof(b),7,8,1),0);b[8]=8;self.assertEqual(self.dll.matches(a,b,32,ctypes.addressof(b),7,8,1),1)
 def test_other_storage_never_uses_identity_shortcut(self):
  a,b=self.arrays();self.assertEqual(self.dll.matches(a,b,32,ctypes.addressof(a),7,7,1),1);b[2]=99;self.assertEqual(self.dll.matches(a,b,32,ctypes.addressof(a),7,7,1),0)
 def test_legacy_mode_compares_all_bytes_and_null_rejects(self):
  a,b=self.arrays();b[5]=99;self.assertEqual(self.dll.matches(a,b,32,ctypes.addressof(b),7,7,0),0)
  self.assertEqual(self.dll.matches(None,b,32,ctypes.addressof(b),7,7,1),0);self.assertEqual(self.dll.matches(a,None,32,0,7,7,1),0)
