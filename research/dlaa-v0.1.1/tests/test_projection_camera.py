import os
import ctypes,subprocess,unittest
from pathlib import Path
import numpy as np

class ProjectionCameraTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        p=Path(__file__).resolve().parents[1];source=p/'build/projection_camera_test.c';dll=p/'build/projection_camera_test.dll'
        source.write_text('#include "../native/projection_camera.h"\n__declspec(dllexport) int accepts(const float *p,unsigned size,unsigned w,unsigned h){return pj_camera(p,size,w,h);}\n')
        subprocess.run([str(Path(os.environ['TOTK_DLAA_PACKAGE'])/'llvm/bin/clang.exe'),'-shared','-O2',str(source),'-o',str(dll)],check=True,capture_output=True)
        cls.dll=ctypes.CDLL(str(dll));cls.fn=cls.dll.accepts;cls.fn.argtypes=[ctypes.POINTER(ctypes.c_float),ctypes.c_uint,ctypes.c_uint,ctypes.c_uint];cls.fn.restype=ctypes.c_int
    def camera(self):
        a=np.zeros((148,4),np.float32);v=np.eye(4);v[2,3]=-12;p=np.array([[1.5,0,0,0],[0,8/3,0,0],[0,0,-1.000008,-.2],[0,0,-1,0]])
        a[:3]=v[:3];a[7:11]=p;a[3:7]=p@v;a[14,:2]=[.1,25000];a[18,:2]=[2560,1440];return a
    def accepts(self,a,size=2368,w=2560,h=1440):return self.fn(a.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),size,w,h)
    def test_main_view_is_accepted(self):self.assertEqual(self.accepts(self.camera()),1)
    def test_other_viewports_and_camera_sizes_rejected(self):
        a=self.camera()
        for size,w,h in [(2368,1280,720),(128,2560,1440),(2368,2048,2048)]:self.assertEqual(self.accepts(a,size,w,h),0)
        a[18,:2]=[1280,720];self.assertEqual(self.accepts(a),0)
    def test_inconsistent_noncanonical_or_nonfinite_projection_rejected(self):
        for row,col,value in [(3,0,9),(7,1,.1),(10,2,0),(14,1,10000),(0,0,float('nan'))]:
            a=self.camera();a[row,col]=value;self.assertEqual(self.accepts(a),0)
    def test_singular_or_scaled_view_is_rejected(self):
        a=self.camera();a[:3]=0;a[3:7]=0;a[5,3]=a[9,3];self.assertEqual(self.accepts(a),0)
        a=self.camera();a[0]*=2;a[3]*=2;self.assertEqual(self.accepts(a),0)
