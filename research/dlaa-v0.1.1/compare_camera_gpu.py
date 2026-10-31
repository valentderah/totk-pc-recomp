"""Camera-only GPU candidates versus CPU math and sparse image tracks; no static-object proof."""
import argparse,json
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw,ImageFont
from optical import cv2
from motion import camera_flow
from sparse_motion import track_features
from compare_gpu_motion import load_gpu_motion

def sample_background_tracks(gpu,track_mask,points,observed,scale):
    if not len(points):return np.empty(0,np.float32)
    maps=(points+.5)*scale-.5;mx=maps[:,0].astype(np.float32).reshape(-1,1);my=maps[:,1].astype(np.float32).reshape(-1,1)
    keep=cv2.remap(track_mask.astype(np.float32),mx,my,cv2.INTER_LINEAR).ravel()>.999
    sampled=cv2.remap(gpu[...,:2],mx,my,cv2.INTER_LINEAR).reshape(-1,2)
    return np.linalg.norm(sampled-observed*scale,axis=-1)[keep]

def compare(root,out,reset_test=False):
    metrics=json.loads((out/'metrics.json').read_text());frame=metrics['frames'][1];w,h=metrics['source_resolution']
    meta=json.loads((root/f'gpu-camera-{frame}.json').read_text())
    if not meta['active'] or meta['support']<1:raise ValueError('No supported live camera input')
    if json.loads((root/'native-exit.json').read_text())['exit_code']!=0:raise ValueError('Native run failed')
    log=(root/'run.log').read_text(errors='replace')
    if 'vk validation error:' in log:raise ValueError('Vulkan validation errors')
    gpu,mask=load_gpu_motion(root/f'gpu-camera-{frame}-{w}x{h}.rgba32f',w,h)
    reset_pixels=None
    if reset_test:
        prior=json.loads((root/f'gpu-camera-{frame-1}.json').read_text())
        if not prior.get('forced_invalid_test') or prior['active'] or prior['support']<1:raise ValueError('Camera reset was not tested on live geometry')
        _,prior_mask=load_gpu_motion(root/f'gpu-camera-{frame-1}-{w}x{h}.rgba32f',w,h)
        reset_pixels=int(prior_mask.sum())
        if reset_pixels:raise ValueError('Invalid camera retained stale valid pixels')
    if mask.sum()<10000:raise ValueError('Camera output lacks useful coverage')
    depths=[];images=[]
    for color in metrics['selected_color']:
        prefix=color.split('_t0_')[0];file=next(root.glob(prefix+'_td_*_d32f.f32'))
        data=np.fromfile(file,dtype='<f4')
        if data.size!=w*h:raise ValueError('Wrong depth dimensions')
        depths.append(data.reshape(h,w));images.append(np.array(Image.open(root/(color+'_tm.png')).convert('RGB')))
    if np.max(np.abs(gpu[...,2][mask]-depths[1][mask]))>1e-7:raise ValueError('Camera field depth does not match selected scene depth')
    now=np.array(meta['current_vp']).reshape(4,4);old=np.array(meta['previous_vp']).reshape(4,4)
    captured=json.loads((out/'cameras.json').read_text())
    for f,vp in ((frame,now),(frame-1,old)):
        c=captured[str(f)];reference=np.array(c['projection'])@np.array(c['view'])
        if not np.allclose(vp,reference,atol=1e-4,rtol=1e-5):raise ValueError('GPU camera does not match scene-depth camera')
    flow,valid=camera_flow(depths[1],now,old,y_down=False,previous_depth=depths[0],depth_tolerance=2e-6)
    common=mask&valid;interior=cv2.erode(common.astype(np.uint8),np.ones((3,3),np.uint8)).astype(bool)
    if interior.sum()<10000:raise ValueError('Insufficient common camera interior')
    error=np.linalg.norm(gpu[...,:2]-flow,axis=-1)[interior]
    # GPU-backed character field remains a different provenance from static-world candidates.
    skin,skin_mask=load_gpu_motion(root/f'gpu-motion-{frame}-{w}x{h}.rgba32f',w,h)
    composite=np.full((h,w,2),np.nan,np.float32);provenance=np.zeros((h,w),np.uint8)
    camera_candidate=mask&(skin[...,3]>=0)
    composite[camera_candidate]=gpu[...,:2][camera_candidate];provenance[camera_candidate]=2
    composite[skin_mask]=skin[...,:2][skin_mask];provenance[skin_mask]=1
    np.savez_compressed(out/'camera-and-character-candidates.npz',current_to_previous_pixels=composite,provenance=provenance)
    # Independent image tracking, conservatively excluding the rectangle around accepted character pixels.
    small=[cv2.resize(a,(960,540),interpolation=cv2.INTER_AREA) for a in images];scale=w/960
    points,observed=track_features(*(cv2.cvtColor(a,cv2.COLOR_RGB2GRAY) for a in small))
    track_mask=cv2.erode(mask.astype(np.uint8),np.ones((5,5),np.uint8))
    yy,xx=np.where(skin_mask)
    if len(xx):track_mask[max(0,yy.min()-80):min(h,yy.max()+81),max(0,xx.min()-80):min(w,xx.max()+81)]=0
    sparse=sample_background_tracks(gpu,track_mask,points,observed,scale)
    summary=dict(frame=frame,resolution=[w,h],camera_support=meta['support'],gpu_camera_candidate_pixels=int(mask.sum()),
        cpu_camera_candidate_pixels=int(valid.sum()),common_interior_pixels=int(interior.sum()),
        residual_median_pixels=float(np.median(error)),residual_p90_pixels=float(np.percentile(error,90)),residual_p99_pixels=float(np.percentile(error,99)),residual_max_pixels=float(error.max()),
        gpu_cpu_mask_disagreement_pixels=int(np.count_nonzero(mask^valid)),camera_reset_accepted_pixels=reset_pixels,character_motion_pixels=int(skin_mask.sum()),
        composite_camera_candidate_pixels=int((provenance==2).sum()),composite_invalid_pixels=int((provenance==0).sum()),
        sparse_background_points=len(sparse),sparse_background_median_pixels=float(np.median(sparse)) if len(sparse) else None,
        sparse_background_p90_pixels=float(np.percentile(sparse,90)) if len(sparse) else None,
        suitable_for_dlss_input=False,provenance_codes={'0':'invalid','1':'known-family skinned motion','2':'camera-only static-world candidate'},
        limitations=['Camera candidates include unrecognized animated surfaces; this does not classify static scene geometry.',
            'Shared CPU math validates numerical implementation, not all model assumptions.',
            'Sparse background tracking covers selected textured points outside a character rectangle, not every object.',
            'No persistent identity, complete jitter/exposure/reset contract or NGX integration.'])
    if summary['residual_median_pixels']>.05 or summary['residual_p99_pixels']>.25:raise ValueError('Camera GPU/CPU disagreement: '+json.dumps(summary))
    (out/'camera-gpu-comparison.json').write_text(json.dumps(summary,indent=2))
    rgb=np.rint((images[1]/255)**(1/2.2)*255).astype(np.uint8);overlay=rgb.copy()
    overlay[provenance==2]=(overlay[provenance==2]*.45+np.array([45,115,220])*.55).astype(np.uint8)
    overlay[provenance==1]=(overlay[provenance==1]*.35+np.array([50,230,115])*.65).astype(np.uint8)
    canvas=Image.new('RGB',(1280,435),'#121922');draw=ImageDraw.Draw(canvas);font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',19)
    draw.text((12,10),'Image du jeu',font=font,fill='white');draw.text((652,10),'Origine des mouvements — diagnostic',font=font,fill='white')
    canvas.paste(Image.fromarray(rgb).resize((640,360)),(0,42));canvas.paste(Image.fromarray(overlay).resize((640,360)),(640,42))
    draw.text((12,407),'Bleu : hypothese camera seule. Vert : maillages suivis. Sans couleur : invalide. Pas de DLAA.',font=font,fill='#d0d6df')
    canvas.save(out/'camera-motion-provenance.png');print(json.dumps(summary,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('output',type=Path);p.add_argument('--reset-test',action='store_true');a=p.parse_args();compare(a.capture,a.output,a.reset_test)
