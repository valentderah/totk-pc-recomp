import os
import ctypes
import subprocess
import unittest
from pathlib import Path
import numpy as np

class CameraMathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root=Path(__file__).resolve().parents[1]
        bridge=root/'build/camera_math_bridge.c'
        bridge.write_text('#include "../native/camera_math.h"\n__declspec(dllexport) int calculate(float *out,const float *v,const float *p,const float *ov,const float *op){return cm_reprojection(out,v,p,ov,op);}\n')
        dll=root/'build/camera_math_test.dll'
        compiler=str(Path(os.environ['TOTK_DLAA_PACKAGE'])/'llvm/bin/clang.exe')
        subprocess.run([compiler,'-shared','-O2',str(bridge),'-o',str(dll)],check=True,capture_output=True)
        cls.dll=ctypes.CDLL(str(dll));cls.fn=cls.dll.calculate
        cls.fn.argtypes=[ctypes.POINTER(ctypes.c_float)]*5;cls.fn.restype=ctypes.c_int

    def calculate(self,v,p,ov,op):
        values=[np.zeros(16,np.float32)]+[np.asarray(a,np.float32).ravel() for a in (v[:3],p,ov[:3],op)]
        ok=self.fn(*(a.ctypes.data_as(ctypes.POINTER(ctypes.c_float)) for a in values))
        return ok,values[0].reshape(4,4)

    def test_identity_and_translation(self):
        v=np.eye(4);p=np.eye(4);old=v.copy();old[0,3]=.125
        ok,out=self.calculate(v,p,old,p)
        self.assertEqual(ok,1);np.testing.assert_allclose(out,old,atol=1e-7)

    def test_perspective_rotation_and_translation(self):
        p=np.array([[1.1,0,0,0],[0,1.8,0,0],[0,0,-1.00002,-.200002],[0,0,-1,0]])
        a=.04;v=np.array([[np.cos(a),0,np.sin(a),2],[0,1,0,3],[-np.sin(a),0,np.cos(a),-12],[0,0,0,1]])
        old=np.eye(4);old[:3,3]=[2.1,3.2,-12.3]
        ok,out=self.calculate(v,p,old,p)
        v=v.astype(np.float32).astype(float);p=p.astype(np.float32).astype(float);old=old.astype(np.float32).astype(float)
        self.assertEqual(ok,1);np.testing.assert_allclose(out,p@old@np.linalg.inv(p@v),atol=1e-6,rtol=1e-6)

    def test_singular_rejected(self):
        self.assertEqual(self.calculate(np.eye(4),np.zeros((4,4)),np.eye(4),np.eye(4))[0],0)

    def test_nonfinite_current_and_previous_rejected(self):
        p=np.eye(4);p[1,1]=np.nan
        self.assertEqual(self.calculate(np.eye(4),p,np.eye(4),np.eye(4))[0],0)
        self.assertEqual(self.calculate(np.eye(4),np.eye(4),np.eye(4),p)[0],0)
