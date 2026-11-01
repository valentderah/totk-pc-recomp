"""GPU motion/raster coordination evidence. Fragment reconstruction and NGX remain excluded."""
import argparse,json
from pathlib import Path
import numpy as np
from optical import cv2
from motion import camera_flow
from projection_metadata import camera_matrices

def validate_window(metas,frame):
    metas=sorted(metas,key=lambda m:m['frame']);frames=[m['frame'] for m in metas]
    if frames!=[frame-1,frame,frame+1]:raise ValueError('Expected before/during/after projection frames')
    offsets=[([0,0],[0,0]),([.375,-.25],[0,0]),([0,0],[.375,-.25])];prior=None
    for meta,(now,old) in zip(metas,offsets):
        if meta.get('projection_probe_mode') is not True or meta.get('motion_projection_coordinated') is not True or meta['active']!=1 or meta['support']<1 or (meta['width'],meta['height'])!=(2560,1440):raise ValueError('Unsupported/inactive coordinated camera input')
        if meta['current_jitter']!=now or meta['previous_jitter']!=old:raise ValueError('Raster offset history lost or shifted to wrong frame')
        raw,raw_old,raster,raster_old=camera_matrices(meta)
        if prior is not None and (not np.allclose(raw_old,prior[0],atol=5e-5,rtol=2e-6) or not np.allclose(raster_old,prior[1],atol=5e-5,rtol=2e-6)):
            raise ValueError('Physical/raster history does not link to the previous frame')
        prior=(raw,raster)
    return frames

def validate_camera_field(meta,gpu,min_pixels=10000):
    w,h=meta['width'],meta['height']
    if gpu.shape!=(h,w,4) or not np.isfinite(gpu).all():raise ValueError('Invalid GPU camera field')
    _,_,cur,old=camera_matrices(meta);flow,valid=camera_flow(gpu[:,:,2],cur,old)
    mask=(gpu[:,:,3]>.5)&valid;mask=cv2.erode(mask.astype(np.uint8),np.ones((5,5),np.uint8)).astype(bool)
    mask[:8]=mask[-8:]=False;mask[:,:8]=mask[:,-8:]=False
    if mask.sum()<min_pixels:raise ValueError('Insufficient interior GPU camera candidates')
    error=np.linalg.norm(gpu[:,:,:2]-np.nan_to_num(flow),axis=-1)[mask]
    p99=float(np.percentile(error,99))
    if p99>.02:raise ValueError(f'GPU camera does not follow raster projection: p99 {p99} pixels')
    return dict(compared_interior_pixels=int(mask.sum()),residual_median_pixels=float(np.median(error)),residual_p99_pixels=p99,residual_max_pixels=float(error.max()))

def validate_mesh_field(skin,min_pixels=1000):
    if skin.ndim!=3 or skin.shape[2]!=4 or not np.isfinite(skin).all():raise ValueError('Invalid/nonfinite mesh motion')
    known=int((skin[:,:,3]>.5).sum())
    if known<min_pixels:raise ValueError('Insufficient valid known-mesh coverage for a mesh+camera diagnostic')
    return dict(known_valid_mesh_pixels=known,known_invalid_surface_pixels=int((skin[:,:,3]<0).sum()))

def analyze(capture,out,frame):
    if json.loads((capture/'native-exit.json').read_text())['exit_code']!=0 or 'vk validation error:' in (capture/'run.log').read_text(errors='replace') or list(capture.glob('ngx-*')):raise ValueError('Native/Vulkan failure or unexpected NGX execution')
    metas=[json.loads((capture/f'gpu-camera-{n}.json').read_text()) for n in (frame-1,frame,frame+1)];frames=validate_window(metas,frame);results=[]
    for meta in metas:
        n=meta['frame'];w,h=meta['width'],meta['height']
        gpu=np.fromfile(capture/f'gpu-camera-{n}-{w}x{h}.rgba32f',dtype='<f4').reshape(h,w,4)
        stats=validate_camera_field(meta,gpu)
        skin=np.fromfile(capture/f'gpu-motion-{n}-{w}x{h}.rgba32f',dtype='<f4').reshape(h,w,4)
        stats.update(validate_mesh_field(skin))
        stats.update(frame=n,current_jitter=meta['current_jitter'],previous_jitter=meta['previous_jitter'],jitter_in_motion_vectors=bool(any(meta['current_jitter']) or any(meta['previous_jitter'])))
        results.append(stats)
    result=dict(frames=frames,native_exit=0,vulkan_errors=0,motion_projection_coordinated=True,frame_results=results,full_scene_jitter_contract=False,ngx_evaluated=False,temporal_quality_validated=False)
    out.mkdir(parents=True,exist_ok=True);(out/'projection-motion-analysis.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('output',type=Path);p.add_argument('--frame',type=int,required=True);a=p.parse_args();analyze(a.capture,a.output,a.frame)
