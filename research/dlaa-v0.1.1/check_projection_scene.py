"""Independent sparse image check of a one-frame VS projection shift. Not full velocity/quality validation."""
import argparse,json
from pathlib import Path
import numpy as np
from temporal_guard import decode_r11g11b10
from motion import camera_flow
from projection_metadata import camera_matrices
from sparse_motion import track_features
from optical import cv2

def check(capture,out,frame):
    meta=json.loads((capture/f'gpu-camera-{frame}.json').read_text());w,h=meta['width'],meta['height']
    if meta.get('projection_probe_mode') is not True or not meta['active'] or json.loads((capture/'native-exit.json').read_text())['exit_code']!=0:raise ValueError('Expected a completed annotated projection experiment')
    cur,old,raster_cur,raster_old=camera_matrices(meta,legacy_jitter=[.375,-.25])
    gpu=np.fromfile(capture/f'gpu-camera-{frame}-{w}x{h}.rgba32f',dtype='<f4').reshape(h,w,4);images=[]
    for n in [frame-1,frame]:
        a=decode_r11g11b10(np.fromfile(capture/f'gpu-hdr-current-{n}-{w}x{h}.r11g11b10',dtype='<u4').reshape(h,w));a=np.maximum(a,0)
        rgb=np.rint((a/(1+a))**(1/2.2)*255).clip(0,255).astype(np.uint8);images.append(cv2.cvtColor(rgb,cv2.COLOR_RGB2GRAY))
    points,measured=track_features(*images)
    if not len(points):raise ValueError('No independent textured image correspondences')
    # Fixed background selection, chosen before examining residuals. Does not certify static-object identity.
    base=(gpu[:,:,3]>.5).astype(np.uint8);base[:,int(.3*w):int(.7*w)]=0;base=cv2.erode(base,np.ones((5,5),np.uint8))
    mx=points[:,0].astype(np.float32).reshape(-1,1);my=points[:,1].astype(np.float32).reshape(-1,1)
    keep=cv2.remap(base.astype(np.float32),mx,my,cv2.INTER_LINEAR).ravel()>.999
    predicted=[]
    for v,prior in [(cur,old),(raster_cur,raster_old)]:
        flow,valid=camera_flow(gpu[:,:,2],v,prior)
        keep&=cv2.remap(valid.astype(np.float32),mx,my,cv2.INTER_LINEAR).ravel()>.999
        predicted.append(cv2.remap(np.nan_to_num(flow).astype(np.float32),mx,my,cv2.INTER_LINEAR).reshape(-1,2))
    if int(keep.sum())<20:raise ValueError('Insufficient selected background tracks for a useful diagnostic')
    cells=np.unique(np.stack([np.minimum((points[keep,0]*4/w).astype(int),3),np.minimum((points[keep,1]*4/h).astype(int),3)],axis=-1),axis=0)
    result=dict(frame=frame,textured_background_tracks=int(keep.sum()),occupied_grid_cells_of_16=len(cells),central_40_percent_excluded=True)
    for name,pred in zip(['raw_camera','jitter_corrected_camera'],predicted):
        err=np.linalg.norm(measured-pred,axis=1)[keep];result[name]=dict(median_error_pixels=float(np.median(err)),p90_error_pixels=float(np.percentile(err,90)))
    sampled_gpu=cv2.remap(gpu[:,:,:2],mx,my,cv2.INTER_LINEAR).reshape(-1,2)
    e=np.linalg.norm(measured-sampled_gpu,axis=1)[keep]
    model=np.linalg.norm(sampled_gpu-predicted[1],axis=1)[keep]
    result['gpu_camera']=dict(median_error_pixels=float(np.median(e)),p90_error_pixels=float(np.percentile(e,90)),raster_model_error_p99_pixels=float(np.percentile(model,99)))
    result['motion_projection_coordinated']=bool(meta.get('motion_projection_coordinated'))
    result['observed_minus_raw_camera_median']=np.median((measured-predicted[0])[keep],axis=0).tolist()
    result['expected_jitter_correction_median']=np.median((predicted[1]-predicted[0])[keep],axis=0).tolist()
    result['scope']='Sparse image tracking checks projection shift on selected background texture, not complete motion/temporal quality or fragment reconstruction.'
    out.mkdir(parents=True,exist_ok=True);(out/'scene-tracks.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('output',type=Path);p.add_argument('--frame',type=int,required=True);a=p.parse_args();check(a.capture,a.output,a.frame)
