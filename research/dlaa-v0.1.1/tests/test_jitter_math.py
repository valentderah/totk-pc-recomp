import os
import ctypes,subprocess,unittest
from pathlib import Path
import numpy as np
from motion import camera_flow

class JitterMathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        p=Path(__file__).resolve().parents[1];source=p/'build/jitter_math_test.c';dll=p/'build/jitter_math_test.dll'
        source.write_text('#include "../native/jitter_math.h"\n__declspec(dllexport) int offset(float *o,const float *p,float x,float y,unsigned w,unsigned h){return jm_offset(o,p,x,y,w,h);}\n')
        subprocess.run([str(Path(os.environ['TOTK_DLAA_PACKAGE'])/'llvm/bin/clang.exe'),'-shared','-O2',str(source),'-o',str(dll)],check=True,capture_output=True)
        cls.dll=ctypes.CDLL(str(dll));cls.fn=cls.dll.offset;cls.fn.argtypes=[ctypes.POINTER(ctypes.c_float)]*2+[ctypes.c_float]*2+[ctypes.c_uint]*2;cls.fn.restype=ctypes.c_int
    def offset(self,p,jx,jy,w=2560,h=1440):
        p=np.asarray(p,dtype=np.float32);o=np.zeros((4,4),np.float32)
        ok=self.fn(o.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),p.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),jx,jy,w,h)
        return ok,o
    def test_exact_screen_shift_with_negative_viewport(self):
        p=np.array([[1.5,0,0,0],[0,2.7,0,0],[0,0,-1.000008,-.2],[0,0,-1,0]])
        ok,j=self.offset(p,.375,-.25);self.assertEqual(ok,1)
        points=np.array([[0,0,-2,1],[.8,-.3,-8,1],[-.2,.1,-.7,1]])
        a=points@p.T;b=points@j.T
        da=(b[:,:2]/b[:,3,None]-a[:,:2]/a[:,3,None])*[1280,-720]
        np.testing.assert_allclose(da,np.tile([.375,-.25],(3,1)),atol=2e-5)
        np.testing.assert_array_equal(j[2:],np.asarray(p,np.float32)[2:])
    def test_reconstruction_requires_jittered_inverse(self):
        p=np.array([[1.5,0,0,0],[0,2.7,0,0],[0,0,-1.000008,-.2],[0,0,-1,0]])
        _,j=self.offset(p,.375,-.25)
        world=np.array([.8,-.3,-8,1]);clip=j@world
        restored=np.linalg.inv(j.astype(float))@clip
        np.testing.assert_allclose(restored/restored[3],world,atol=2e-5)
        wrong=np.linalg.inv(p)@clip;self.assertGreater(np.linalg.norm(wrong[:2]/wrong[3]-world[:2]),.001)
    def test_invalid_inputs_and_zero_offset(self):
        p=np.eye(4)
        self.assertEqual(self.offset(p,0,0)[0],1);np.testing.assert_array_equal(self.offset(p,0,0)[1],p)
        for jx,jy,w,h in [(float('nan'),0,2560,1440),(0,0,0,1440),(.6,0,2560,1440),(0,0,2560,0)]:
            self.assertEqual(self.offset(p,jx,jy,w,h)[0],0)
    def test_reprojection_removes_current_jitter_in_pixel_coordinates(self):
        ok,j=self.offset(np.eye(4),.375,-.25,w=64,h=48);self.assertEqual(ok,1)
        flow,valid=camera_flow(np.full((48,64),.5),j,np.eye(4))
        self.assertGreater(int(valid.sum()),2000)
        np.testing.assert_allclose(flow[valid],np.tile([-.375,.25],(int(valid.sum()),1)),atol=1e-6)
