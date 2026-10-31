import unittest
import numpy as np
import analyze_projection_motion as analysis
from analyze_projection_motion import validate_window,validate_camera_field
from motion import camera_flow

class ProjectionMotionOutputTests(unittest.TestCase):
    def matrix(self,jitter,w=2560,h=1440):
        j=np.eye(4);j[0,3]=2*jitter[0]/w;j[1,3]=-2*jitter[1]/h;return j
    def meta(self,frame,now,old,w=2560,h=1440):
        return dict(frame=frame,width=w,height=h,active=1,support=25,projection_probe_mode=True,motion_projection_coordinated=True,
            current_jitter=now,previous_jitter=old,raw_current_vp=np.eye(4).ravel().tolist(),raw_previous_vp=np.eye(4).ravel().tolist(),
            current_vp=self.matrix(now,w,h).ravel().tolist(),previous_vp=self.matrix(old,w,h).ravel().tolist())
    def window(self):return [self.meta(1199,[0,0],[0,0]),self.meta(1200,[.375,-.25],[0,0]),self.meta(1201,[0,0],[.375,-.25])]
    def test_current_and_previous_offset_window(self):
        self.assertEqual(validate_window(self.window(),1200),[1199,1200,1201])
    def test_previous_jitter_loss_is_rejected(self):
        rows=self.window();rows[2]=self.meta(1201,[0,0],[0,0])
        with self.assertRaises(ValueError):validate_window(rows,1200)
    def test_broken_camera_history_link_is_rejected(self):
        rows=self.window();old=np.eye(4);old[0,3]=.5;rows[2]['raw_previous_vp']=old.ravel().tolist();rows[2]['previous_vp']=(self.matrix([.375,-.25])@old).ravel().tolist()
        with self.assertRaises(ValueError):validate_window(rows,1200)
    def test_gpu_field_must_follow_raster_projection(self):
        meta=self.meta(1200,[.375,-.25],[0,0],64,48);gpu=np.zeros((48,64,4),np.float32);gpu[:,:,2]=.5;gpu[:,:,3]=1
        with self.assertRaises(ValueError):validate_camera_field(meta,gpu,min_pixels=500)
        flow,valid=camera_flow(gpu[:,:,2],self.matrix([.375,-.25],64,48),np.eye(4));gpu[:,:,:2]=np.nan_to_num(flow);gpu[:,:,3]=valid
        self.assertLess(validate_camera_field(meta,gpu,min_pixels=500)['residual_p99_pixels'],1e-5)
    def test_mesh_field_requires_useful_valid_coverage(self):
        a=np.zeros((48,64,4),np.float32)
        with self.assertRaises(ValueError):analysis.validate_mesh_field(a,min_pixels=100)
        a[10:30,10:30,3]=1
        self.assertEqual(analysis.validate_mesh_field(a,min_pixels=100)['known_valid_mesh_pixels'],400)
