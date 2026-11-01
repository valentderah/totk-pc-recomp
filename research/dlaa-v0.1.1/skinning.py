"""Bounded rigid skinning hypothesis for captured shader 68e66bf9643995b0.

Does not cover weighted skinning, other shaders, instances or GPU-generated meshes.
"""
import numpy as np

def decode_attribute(raw,vk_format,offset,stride,count):
    formats={106:('<f4',3),109:('<f4',4),97:('<f2',4),13:('u1',1),37:('u1',4),41:('u1',4)}
    if vk_format not in formats:raise ValueError('Unsupported diagnostic vertex format')
    dtype,components=formats[vk_format];item=np.dtype(dtype).itemsize
    if count<0 or offset<0 or stride<1:
        raise ValueError('Invalid attribute layout')
    needed=offset+(count-1)*stride+item*components if count else 0
    if needed>len(raw):raise ValueError('Attribute read exceeds captured bytes')
    result=np.ndarray((count,components),dtype=dtype,buffer=raw,offset=offset,strides=(stride,item)).copy()
    if vk_format==37:return result.astype(np.float32)/np.float32(255)
    return result.astype(np.float32) if vk_format==97 else result

def rigid_clip_positions(position,bones,palette,origin,view,projection):
    position=np.asarray(position,dtype=np.float32);bones=np.asarray(bones)
    palette=np.asarray(palette,dtype=np.float32);origin=np.asarray(origin,dtype=np.float32)
    view=np.asarray(view,dtype=np.float32);projection=np.asarray(projection,dtype=np.float32)
    if position.ndim!=2 or position.shape[1]!=3 or bones.shape!=(len(position),) or bones.dtype.kind not in 'iu':
        raise ValueError('Expected xyz positions and integer bone indices')
    if palette.ndim!=3 or palette.shape[1:]!=(3,4) or origin.shape!=(3,) or view.shape!=(4,4) or projection.shape!=(4,4):
        raise ValueError('Invalid pose or camera dimensions')
    if any(not np.isfinite(a).all() for a in (position,palette,origin,view,projection)):
        raise ValueError('Non-finite transform data')
    if np.any(bones<0) or np.any(bones>=len(palette)):raise ValueError('Bone index exceeds captured palette')
    bind=np.column_stack([position,np.ones(len(position),np.float32)])
    posed=np.einsum('nij,nj->ni',palette[bones],bind)+origin
    world=np.column_stack([posed,np.ones(len(position),np.float32)])
    return (world@view.T)@projection.T

def weighted_clip_positions(position,bones,weights,palette,origin,view,projection):
    bones=np.asarray(bones);weights=np.asarray(weights,dtype=np.float32)
    palette=np.asarray(palette,dtype=np.float32);position=np.asarray(position,dtype=np.float32)
    if bones.ndim!=2 or weights.shape!=bones.shape or len(position)!=len(bones) or bones.dtype.kind not in 'iu':
        raise ValueError('Invalid weighted skinning dimensions')
    if palette.ndim!=3 or palette.shape[1:]!=(3,4) or np.any(bones<0) or np.any(bones>=len(palette)):
        raise ValueError('Weighted bone index exceeds palette')
    if not np.isfinite(weights).all() or np.any(weights<0) or np.any(weights>1):raise ValueError('Invalid blend weights')
    blended=np.einsum('nk,nkij->nij',weights,palette[bones])
    return rigid_clip_positions(position,np.arange(len(position)),blended,origin,view,projection)
