import os
import ctypes,subprocess,unittest
from pathlib import Path
class DlaaPresetSelectionTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  p=Path(__file__).resolve().parents[1];c=p/'build/dlaa_preset_test.c';d=p/'build/dlaa_preset_test.dll'
  c.write_text('#include "../native/dlaa_preset.h"\n__declspec(dllexport) int preset(const char *v,unsigned *out){return dp_select(v,out); }\n')
  subprocess.run([str(Path(os.environ['TOTK_DLAA_PACKAGE'])/'llvm/bin/clang.exe'),'-shared','-O2',str(c),'-o',str(d)],check=True,capture_output=True)
  cls.dll=ctypes.CDLL(str(d));cls.dll.preset.argtypes=[ctypes.c_char_p,ctypes.POINTER(ctypes.c_uint)]
 def test_legacy_default_and_only_explicit_supported_presets(self):
  for text,want in ((None,12),(b'',12),(b'L',12),(b'12',12),(b'K',11),(b'11',11)):
   out=ctypes.c_uint(99);self.assertEqual(self.dll.preset(text,ctypes.byref(out)),1);self.assertEqual(out.value,want)
 def test_invalid_value_never_silently_downgrades(self):
  for text in (b'M',b'13',b'0',b'Kgarbage',b'11x',b' L'):
   out=ctypes.c_uint(99);self.assertEqual(self.dll.preset(text,ctypes.byref(out)),0);self.assertEqual(out.value,99)
