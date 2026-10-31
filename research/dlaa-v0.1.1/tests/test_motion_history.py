import os
import subprocess,tempfile,unittest
from pathlib import Path

class NativeHistoryTests(unittest.TestCase):
    def test_native_history_state_machine(self):
        root=Path(__file__).resolve().parents[1]
        compiler=Path(str(Path(os.environ['TOTK_DLAA_PACKAGE'])/'llvm/bin/clang.exe'))
        with tempfile.TemporaryDirectory(prefix='totk-history-test-') as temporary:
            binary=Path(temporary)/'history.exe'
            subprocess.run([str(compiler),'-O2','-I',str(root/'native'),str(root/'tests/motion_history_test.c'),'-o',str(binary)],check=True,capture_output=True)
            result=subprocess.run([str(binary)],check=True,capture_output=True,text=True)
            self.assertIn('motion history: passed',result.stdout)
