"""Offline diagnostic mathematics; not a DLSS velocity implementation.

Flow points from the current pixel to its location in the previous frame.
Depth files contain raw Vulkan D32 values; the game's projection is OpenGL-style.
"""
import numpy as np

def validated_camera(rows):
    a=np.asarray(rows,dtype=np.float64)
    if a.shape!=(16,4) or not np.isfinite(a).all():
        raise ValueError('Expected sixteen finite camera vec4 rows')
    view=np.eye(4);view[:3]=a[:3]
    projection=a[7:11].copy()
    combined=a[3:7].copy()
    if not np.allclose(projection@view,combined,atol=0.004,rtol=0.0001):
        raise ValueError('Projection/view product disagrees with the captured combined matrix')
    if abs(np.linalg.det(combined))<1e-12:
        raise ValueError('Singular camera')
    return view,projection,combined

def camera_flow(depth,current_vp,previous_vp,*,y_down=False,previous_depth=None,depth_tolerance=0.0001):
    d=np.asarray(depth,dtype=np.float64)
    current=np.asarray(current_vp,dtype=np.float64)
    previous=np.asarray(previous_vp,dtype=np.float64)
    if d.ndim!=2 or current.shape!=(4,4) or previous.shape!=(4,4):
        raise ValueError('Expected a depth image and two 4x4 matrices')
    if not np.isfinite(current).all() or not np.isfinite(previous).all():
        raise ValueError('Non-finite camera')
    try: inverse=np.linalg.inv(current)
    except np.linalg.LinAlgError as e: raise ValueError('Singular camera') from e
    h,w=d.shape
    yy,xx=np.mgrid[:h,:w];u=xx+0.5;v=yy+0.5
    nx=2*u/w-1;ny=2*v/h-1 if y_down else 1-2*v/h
    valid=np.isfinite(d)&(d>=0)&(d<1)
    safe=np.where(valid,d,0.5)
    clip=np.stack([nx,ny,2*safe-1,np.ones_like(d)],axis=-1)
    world=clip@inverse.T
    valid &= np.abs(world[...,3])>1e-12
    world/=np.where(np.abs(world[...,3:4])>1e-12,world[...,3:4],1)
    old=world@previous.T
    valid &= np.isfinite(old).all(axis=-1)&(old[...,3]>1e-10)
    old_ndc=old[...,:3]/np.where(old[...,3:4]>1e-10,old[...,3:4],1)
    px=(old_ndc[...,0]+1)*w/2
    py=(old_ndc[...,1]+1)*h/2 if y_down else (1-old_ndc[...,1])*h/2
    valid &= (px>=0)&(px<w)&(py>=0)&(py<h)&(old_ndc[...,2]>=-1)&(old_ndc[...,2]<=1)
    if previous_depth is not None:
        previous_depth=np.asarray(previous_depth,dtype=np.float64)
        if previous_depth.shape!=d.shape:raise ValueError('Previous depth dimensions disagree')
        sx=np.clip(px-0.5,0,w-1);sy=np.clip(py-0.5,0,h-1)
        x0=np.floor(sx).astype(int);y0=np.floor(sy).astype(int)
        x1=np.minimum(x0+1,w-1);y1=np.minimum(y0+1,h-1)
        wx=sx-x0;wy=sy-y0
        sampled=(previous_depth[y0,x0]*(1-wx)*(1-wy)+previous_depth[y0,x1]*wx*(1-wy)+
                 previous_depth[y1,x0]*(1-wx)*wy+previous_depth[y1,x1]*wx*wy)
        predicted_depth=(old_ndc[...,2]+1)/2
        valid &= np.isfinite(sampled)&(sampled>=0)&(sampled<1)&(np.abs(sampled-predicted_depth)<=depth_tolerance)
    flow=np.stack([px-u,py-v],axis=-1)
    flow[~valid]=np.nan
    return flow,valid
