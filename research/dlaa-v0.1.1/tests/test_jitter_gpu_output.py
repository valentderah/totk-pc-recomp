import unittest
import numpy as np
from analyze_jitter_gpu import validate_case

class JitterGpuOutputTests(unittest.TestCase):
    def image(self,jitter):
        w,h=128,96
        vertices=np.array([[-.625,-.5,-.5],[.75,-.5,0],[-.25,.75,.5]])
        xy=np.column_stack(((vertices[:,0]+1)*w/2,(1-vertices[:,1])*h/2))+jitter
        yy,xx=np.mgrid[:h,:w];bary=np.linalg.solve(np.vstack((xy.T,np.ones(3))),np.stack((xx+.5,yy+.5,np.ones_like(xx))).reshape(3,-1)).reshape(3,h,w)
        mask=np.min(bary,axis=0)>1e-4;a=np.zeros((h,w,4),np.float32)
        a[mask,2]=(.5*(np.sum(bary*vertices[:,2,None,None],axis=0)+1))[mask];a[mask,3]=1
        return a
    def test_expected_current_to_previous_velocity_and_depth(self):
        case=dict(current=[.375,-.25],previous=[-.125,.25]);a=self.image(case['current']);a[a[:,:,3]>.5,:2]=[-.5,.5]
        r=validate_case(case,a);self.assertLess(r['max_motion_error'],1e-5)
    def test_wrong_vertical_sign_and_missing_raster_are_rejected(self):
        case=dict(current=[.375,-.25],previous=[-.125,.25]);a=self.image(case['current']);a[a[:,:,3]>.5,:2]=[-.5,-.5]
        with self.assertRaises(ValueError):validate_case(case,a)
        with self.assertRaises(ValueError):validate_case(case,np.zeros_like(a))
    def test_nonfinite_and_wrong_depth_are_rejected(self):
        case=dict(current=[0,0],previous=[0,0]);a=self.image(case['current']);a[a[:,:,3]>.5,2]+=.01
        with self.assertRaises(ValueError):validate_case(case,a)
