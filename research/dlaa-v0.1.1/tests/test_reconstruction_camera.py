import os
import ctypes,subprocess,unittest
from pathlib import Path
import numpy as np

class ReconstructionCameraTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        p=Path(__file__).resolve().parents[1];source=p/'build/reconstruction_camera_test.c';dll=p/'build/reconstruction_camera_test.dll'
        source.write_text('#include "../native/projection_camera.h"\n__declspec(dllexport) int accepts(const float *a,unsigned size,unsigned w,unsigned h,const float *ref){return pj_reconstruction_camera(a,size,w,h,ref);}\n')
        subprocess.run([str(Path(os.environ['TOTK_DLAA_PACKAGE'])/'llvm/bin/clang.exe'),'-shared','-O2',str(source),'-o',str(dll)],check=True,capture_output=True)
        cls.dll=ctypes.CDLL(str(dll));cls.fn=cls.dll.accepts;cls.fn.argtypes=[ctypes.POINTER(ctypes.c_float),ctypes.c_uint,ctypes.c_uint,ctypes.c_uint,ctypes.POINTER(ctypes.c_float)];cls.fn.restype=ctypes.c_int
    def camera(self):
        a=np.zeros((148,4),np.float32);v=np.eye(4);v[2,3]=-12;p=np.array([[1.5,0,0,0],[0,8/3,0,0],[0,0,-1.000008,-.2],[0,0,-1,0]])
        a[:3]=v[:3];a[3:7]=p@v;a[7:11]=p;a[11:14]=np.linalg.inv(v)[:3];a[14,:2]=[.1,25000];a[17,:2]=[1/p[0,0],1/p[1,1]];a[18,:2]=[2560,1440];return a
    def accepts(self,a,w=640,h=360,ref=None):
        if ref is None:ref=a[3:7].copy()
        return self.fn(a.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),2368,w,h,ref.ctypes.data_as(ctypes.POINTER(ctypes.c_float)))
    def test_native_and_reduced_camera_extents(self):
        a=self.camera();self.assertEqual(self.accepts(a),1)
        a[18,:2]=[640,360];self.assertEqual(self.accepts(a),1)
        self.assertEqual(self.accepts(a,640,640),0)
    def test_other_view_or_incoherent_ray_parameters_rejected(self):
        a=self.camera();ref=a[3:7].copy();ref[0,3]+=2;self.assertEqual(self.accepts(a,ref=ref),0)
        a=self.camera();a[17,0]=0;self.assertEqual(self.accepts(a),0)
        a=self.camera();a[11,3]+=2;self.assertEqual(self.accepts(a),0)
    def test_fullscreen_ray_uses_jittered_principal_point(self):
        a=self.camera();p=a[7:11].copy();j=np.eye(4);j[0,3]=2*.375/2560;j[1,3]=-2*(-.25)/1440;jp=j@p
        ndc=np.array([.23,-.31]);depth=8
        def ray(matrix):return np.array([depth*a[17,0]*(ndc[0]+matrix[0,2]),depth*a[17,1]*(ndc[1]+matrix[1,2]),-depth,1])
        v=ray(jp);clip=jp@v;np.testing.assert_allclose(clip[:2]/clip[3],ndc,atol=1e-7)
        bad=jp@ray(p);shift=(bad[:2]/bad[3]-ndc)*[1280,-720];np.testing.assert_allclose(shift,[.375,-.25],atol=1e-5)
