"""Sparse current-to-previous correspondences, independent of camera matrices.

Used only to check captured camera reprojection. Not a dense velocity buffer.
"""
import numpy as np
from optical import cv2

def track_features(previous,current):
    previous=np.asarray(previous);current=np.asarray(current)
    if previous.ndim!=2 or previous.shape!=current.shape or previous.dtype!=np.uint8 or current.dtype!=np.uint8:
        raise ValueError('Expected equally sized uint8 grayscale images')
    # Same monotonic contrast transform for both images; no camera initialization.
    lut=np.rint((np.arange(256)/255)**(1/2.2)*255).astype(np.uint8)
    previous=lut[previous];current=lut[current]
    points=cv2.goodFeaturesToTrack(current,1500,0.01,9,blockSize=7)
    empty=lambda:(np.empty((0,2),np.float32),np.empty((0,2),np.float32))
    if points is None:return empty()
    options=dict(winSize=(25,25),maxLevel=4,
        criteria=(cv2.TERM_CRITERIA_EPS|cv2.TERM_CRITERIA_COUNT,40,0.001))
    old,ok,error=cv2.calcOpticalFlowPyrLK(current,previous,points,None,**options)
    if old is None:return empty()
    returned,back,_=cv2.calcOpticalFlowPyrLK(previous,current,old,None,**options)
    if returned is None:return empty()
    h,w=current.shape
    good=(ok[:,0]>0)&(back[:,0]>0)&(np.linalg.norm(returned-points,axis=(1,2))<0.35)&(error[:,0]<12)
    for xy in (points[:,0],old[:,0]):
        good&=np.isfinite(xy).all(axis=1)&(xy[:,0]>=12)&(xy[:,0]<w-12)&(xy[:,1]>=12)&(xy[:,1]<h-12)
    return points[good,0],old[good,0]-points[good,0]

def summarize_tracks(points,measured,predicted,scale,size):
    points=np.asarray(points);measured=np.asarray(measured);predicted=np.asarray(predicted)
    result=dict(points=len(points),status='insufficient_support',residual_median_pixels=None,
        residual_p90_pixels=None,observed_median_pixels=None,camera_median_pixels=None,
        within_1_5_pixels=None,occupied_grid_cells_of_16=0)
    if not len(points):return result
    residual=np.linalg.norm(measured-predicted,axis=-1)*scale
    cells=np.unique(np.stack([np.minimum((points[:,0]*4/size[0]).astype(int),3),
        np.minimum((points[:,1]*4/size[1]).astype(int),3)],axis=-1),axis=0)
    result.update(residual_median_pixels=float(np.median(residual)),residual_p90_pixels=float(np.percentile(residual,90)),
        observed_median_pixels=float(np.median(np.linalg.norm(measured,axis=-1))*scale),
        camera_median_pixels=float(np.median(np.linalg.norm(predicted,axis=-1))*scale),
        within_1_5_pixels=float(np.mean(residual<1.5)),occupied_grid_cells_of_16=len(cells))
    if len(points)>=50 and len(cells)>=8:
        result['status']='supported_on_textured_points' if (result['residual_median_pixels']<1.5 and result['residual_p90_pixels']<4 and result['within_1_5_pixels']>0.8) else 'rejected'
    return result
