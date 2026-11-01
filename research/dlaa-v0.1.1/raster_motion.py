"""Offline triangle motion rasterizer. Current-to-previous, pixel-center convention."""
import numpy as np

def triangles_from_indices(indices,primitive,base,vertex_count):
    indices=np.asarray(indices,dtype=np.int64)+base
    if primitive!=4 or len(indices)%3:raise ValueError('Only complete triangle lists are supported')
    if np.any(indices<0) or np.any(indices>=vertex_count):raise ValueError('Index exceeds vertex range')
    return indices.reshape(-1,3)

def rasterize_motion(current,previous,triangles,width,height,*,scene_depth,previous_scene_depth=None,depth_tolerance=2e-6):
    current=np.asarray(current,np.float64);previous=np.asarray(previous,np.float64)
    triangles=np.asarray(triangles,np.int64);scene_depth=np.asarray(scene_depth)
    if current.shape!=previous.shape or current.ndim!=2 or current.shape[1]!=4 or scene_depth.shape!=(height,width):
        raise ValueError('Invalid mesh or depth dimensions')
    if triangles.ndim!=2 or triangles.shape[1]!=3 or np.any(triangles<0) or np.any(triangles>=len(current)):
        raise ValueError('Invalid triangle indices')
    flow=np.full((height,width,2),np.nan,np.float32);depth=np.full((height,width),np.inf,np.float32)
    valid=np.zeros((height,width),bool)
    if previous_scene_depth is not None and np.asarray(previous_scene_depth).shape!=(height,width):
        raise ValueError('Previous depth dimensions disagree')
    for ids in triangles:
        c=current[ids];p=previous[ids]
        if not np.isfinite(c).all() or not np.isfinite(p).all() or np.any(c[:,3]<=0):continue
        ndc=c[:,:3]/c[:,3:4]
        xy=np.column_stack([(ndc[:,0]+1)*width/2,(1-ndc[:,1])*height/2])
        lo=np.maximum(np.floor(xy.min(axis=0)-.5).astype(int),0)
        hi=np.minimum(np.ceil(xy.max(axis=0)-.5).astype(int),[width-1,height-1])
        if np.any(lo>hi):continue
        a,b,d=xy;area=(b[1]-d[1])*(a[0]-d[0])+(d[0]-b[0])*(a[1]-d[1])
        if abs(area)<1e-10:continue
        yy,xx=np.mgrid[lo[1]:hi[1]+1,lo[0]:hi[0]+1];x=xx+.5;y=yy+.5
        l0=((b[1]-d[1])*(x-d[0])+(d[0]-b[0])*(y-d[1]))/area
        l1=((d[1]-a[1])*(x-d[0])+(a[0]-d[0])*(y-d[1]))/area
        bary=np.stack([l0,l1,1-l0-l1],axis=-1)
        z=(bary@ndc[:,2]+1)/2
        old=np.einsum('...i,ij->...j',bary,p/c[:,3:4])
        ok=(bary.min(axis=-1)>=-1e-8)&(z>=0)&(z<1)&(old[...,3]>1e-10)
        old_ndc=old[...,:3]/np.where(old[...,3:4]>1e-10,old[...,3:4],1)
        old_x=(old_ndc[...,0]+1)*width/2;old_y=(1-old_ndc[...,1])*height/2
        ok&=(old_x>=0)&(old_x<width)&(old_y>=0)&(old_y<height)&(old_ndc[...,2]>=-1)&(old_ndc[...,2]<=1)
        scene=scene_depth[yy,xx]
        ok&=np.isfinite(scene)&(scene>=0)&(scene<1)&(abs(z-scene)<=depth_tolerance)&(z<depth[yy,xx])
        if previous_scene_depth is not None:
            sx=np.clip(old_x-.5,0,width-1);sy=np.clip(old_y-.5,0,height-1)
            x0=np.floor(sx).astype(int);y0=np.floor(sy).astype(int);x1=np.minimum(x0+1,width-1);y1=np.minimum(y0+1,height-1)
            wx=sx-x0;wy=sy-y0;d=np.asarray(previous_scene_depth)
            sample=d[y0,x0]*(1-wx)*(1-wy)+d[y0,x1]*wx*(1-wy)+d[y1,x0]*(1-wx)*wy+d[y1,x1]*wx*wy
            ok&=np.isfinite(sample)&(sample>=0)&(sample<1)&(abs(sample-(old_ndc[...,2]+1)/2)<=depth_tolerance)
        iy=yy[ok];ix=xx[ok]
        flow[iy,ix]=np.stack([old_x-x,old_y-y],axis=-1)[ok]
        depth[iy,ix]=z[ok];valid[iy,ix]=True
    return flow,depth,valid
