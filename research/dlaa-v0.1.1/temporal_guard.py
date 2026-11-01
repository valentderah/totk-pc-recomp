"""HDR consistency heuristic; low bias does not prove static geometry or correct object identity."""
import numpy as np

def decode_r11g11b10(packed):
    packed=np.asarray(packed,dtype=np.uint32);channels=[]
    for shift,bits in ((0,6),(11,6),(22,5)):
        value=(packed>>shift)&((1<<(bits+5))-1);exponent=value>>bits;mantissa=value&((1<<bits)-1)
        normal=(1+mantissa.astype(float)/(1<<bits))*np.exp2(exponent.astype(float)-15)
        result=np.where(exponent==0,mantissa.astype(float)*2.0**(-14-bits),normal)
        result=np.where(exponent==31,np.where(mantissa==0,np.inf,np.nan),result);channels.append(result)
    return np.stack(channels,axis=-1).astype(np.float32)

def compose_guard(camera,skin,current,previous,*,history=True):
    camera=np.asarray(camera);skin=np.asarray(skin);current=np.asarray(current);previous=np.asarray(previous)
    h,w=camera.shape[:2]
    if camera.shape!=(h,w,4) or skin.shape!=camera.shape or current.shape!=(h,w,3) or previous.shape!=current.shape:raise ValueError('Inconsistent guard dimensions')
    output=np.zeros_like(camera,dtype=np.float32);bias=np.ones((h,w),np.float32)
    if not history:return output,bias
    known=(skin[...,3]>.5)&np.isfinite(skin).all(axis=-1)
    candidate=(camera[...,3]>.5)&np.isfinite(camera).all(axis=-1)&(skin[...,3]>=0)
    chosen=np.where(known[...,None],skin,camera);eligible=known|candidate
    yy,xx=np.mgrid[:h,:w];px=xx+.5+np.nan_to_num(chosen[...,0]);py=yy+.5+np.nan_to_num(chosen[...,1])
    eligible&=(px>=0)&(px<w)&(py>=0)&(py<h)
    sx=np.clip(px-.5,0,w-1);sy=np.clip(py-.5,0,h-1);x=np.floor(sx).astype(int);y=np.floor(sy).astype(int)
    qx=np.minimum(x+1,w-1);qy=np.minimum(y+1,h-1);fx=(sx-x)[...,None];fy=(sy-y)[...,None]
    old=(previous[y,x]*(1-fx)+previous[y,qx]*fx)*(1-fy)+(previous[qy,x]*(1-fx)+previous[qy,qx]*fx)*fy
    colors_ok=np.isfinite(current).all(axis=-1)&np.isfinite(old).all(axis=-1)&(current>=0).all(axis=-1)&(old>=0).all(axis=-1)
    delta=np.max(np.abs(np.log2(1+np.maximum(current,0))-np.log2(1+np.maximum(old,0))),axis=-1)
    weight=np.clip((delta-.04)/.16,0,1);valid=eligible&colors_ok
    bias[valid]=weight[valid]
    retain=valid&(known|(bias<.5));output[retain]=chosen[retain];output[...,3][retain]=np.where(known[retain],1,2)
    return output,bias
