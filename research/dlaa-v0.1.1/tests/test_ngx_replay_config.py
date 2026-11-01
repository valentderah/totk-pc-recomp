import ctypes,unittest
import test_ngx_eval_abi as abi

class ReplayPresetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        abi.NgxEvalAbiTests.setUpClass();cls.configure=abi.NgxEvalAbiTests.dll.totk_ngx_configure_replay_preset;cls.configure.argtypes=[ctypes.c_uint];cls.configure.restype=ctypes.c_int
    def test_only_documented_comparison_presets(self):
        for p in (0,11,12):self.assertEqual(self.configure(p),1)
        for p in (5,7,10,13,999):self.assertEqual(self.configure(p),-4)
        self.assertEqual(self.configure(0),1)
