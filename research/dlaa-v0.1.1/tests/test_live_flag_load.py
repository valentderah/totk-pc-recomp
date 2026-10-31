import os
import ctypes,subprocess,unittest
from pathlib import Path
class LiveFlagLoadTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  p=Path(__file__).resolve().parents[1];c=p/'build/live_flag_load_test.c';d=p/'build/live_flag_load_test.dll'
  c.write_text('#include "../native/live_flag_load.h"\nstatic volatile LONG flag;static int payload;\nstatic DWORD WINAPI publish(void *p){(void)p;payload=42;InterlockedExchange(&flag,1);return 0;}\n__declspec(dllexport) int value(int v,int fast){InterlockedExchange(&flag,v);return lf_load(&flag,fast);}\n__declspec(dllexport) int publication(unsigned n){int failures=0;for(unsigned i=0;i<n;i++){InterlockedExchange(&flag,0);payload=0;HANDLE t=CreateThread(NULL,0,publish,NULL,0,NULL);if(!t)return -1;ULONGLONG limit=GetTickCount64()+2000;while(!lf_load(&flag,1)&&GetTickCount64()<limit)SwitchToThread();if(!lf_load(&flag,1)||payload!=42)failures++;WaitForSingleObject(t,INFINITE);CloseHandle(t);}return failures;}\n')
  subprocess.run([str(Path(os.environ['TOTK_DLAA_PACKAGE'])/'llvm/bin/clang.exe'),'-shared','-O2',str(c),'-o',str(d)],check=True,capture_output=True)
  cls.dll=ctypes.CDLL(str(d));cls.dll.value.argtypes=[ctypes.c_int,ctypes.c_int];cls.dll.publication.argtypes=[ctypes.c_uint]
 def test_all_lifecycle_flag_values_preserved(self):
  for mode in (0,1):
   for value in (0,1,2):self.assertEqual(self.dll.value(value,mode),value)
 def test_acquire_observes_interlocked_publication(self):self.assertEqual(self.dll.publication(64),0)
