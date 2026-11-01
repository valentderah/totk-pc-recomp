"""Separate physical-camera evidence from raster/motion matrices; never apply jitter twice."""
import numpy as np

def camera_matrices(meta,legacy_jitter=None):
    if meta.get('motion_projection_coordinated') and any(k not in meta for k in ('raw_current_vp','raw_previous_vp','current_jitter','previous_jitter')):
        raise ValueError('Coordinated motion needs independent raw matrices and both raster offsets')
    current=np.asarray(meta['current_vp'],dtype=float).reshape(4,4);previous=np.asarray(meta['previous_vp'],dtype=float).reshape(4,4)
    raw_current=np.asarray(meta.get('raw_current_vp',meta['current_vp']),dtype=float).reshape(4,4)
    raw_previous=np.asarray(meta.get('raw_previous_vp',meta['previous_vp']),dtype=float).reshape(4,4)
    def shifted(raw,offset):
        offset=np.asarray(offset,dtype=float)
        if offset.shape!=(2,) or not np.isfinite(offset).all() or np.max(np.abs(offset))>.5:raise ValueError('Invalid raster jitter')
        j=np.eye(4);j[0,3]=2*offset[0]/meta['width'];j[1,3]=-2*offset[1]/meta['height'];return j@raw
    if meta.get('projection_probe_mode'):
        now=meta.get('current_jitter',legacy_jitter)
        if now is None:raise ValueError('Legacy projection metadata needs an explicit selected-frame jitter')
        raster_current=shifted(raw_current,now);raster_previous=shifted(raw_previous,meta.get('previous_jitter',[0,0]))
    else:raster_current=raw_current.copy();raster_previous=raw_previous.copy()
    if not all(np.isfinite(a).all() and abs(np.linalg.det(a))>1e-12 for a in (current,previous,raw_current,raw_previous,raster_current,raster_previous)):
        raise ValueError('Invalid camera matrices')
    if meta.get('motion_projection_coordinated') and (not np.allclose(current,raster_current,atol=5e-5,rtol=2e-6) or not np.allclose(previous,raster_previous,atol=5e-5,rtol=2e-6)):
        raise ValueError('Motion matrices do not match declared raster projection')
    return raw_current,raw_previous,raster_current,raster_previous
