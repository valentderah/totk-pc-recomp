"""Analyze a two-frame native dump. Outputs are diagnostics, never DLSS inputs."""
import argparse
import json
import re
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw,ImageFont
from capture import parse_cameras,parse_images,select_camera
from motion import camera_flow
from optical import observed_flow,cv2

def pass_id(name):return int(re.search(r'pass_(\d+)',name).group(1))

def registration_status(median,p90,support_fraction,valid_fraction,camera_cut):
    if camera_cut or not np.isfinite([median,p90]).all() or median>1.5 or p90>4 or support_fraction<0.1 or valid_fraction<0.5:
        return 'rejected'
    return 'accepted_for_diagnostic'

def scene_pair(root,items,frame,w,h,required_pass=None):
    records=[r for r in items if r['frame']==frame and (r['width'],r['height'])==(w,h)]
    depths=[r for r in records if r['name'].endswith('_d32f') and (root/(r['name']+'.f32')).is_file()]
    for depth in sorted(depths,key=lambda r:pass_id(r['name']),reverse=True):
        if required_pass is not None and pass_id(depth['name'])!=required_pass:continue
        colors=[r for r in records if pass_id(r['name'])==pass_id(depth['name']) and '_t0_' in r['name'] and 'r11g11b10f' in r['name']]
        if colors:
            color=colors[0];path=root/(color['name']+'_tm.png')
            if not path.is_file():path=root/(color['name']+'.png')
            data=np.fromfile(root/(depth['name']+'.f32'),dtype='<f4')
            if data.size!=w*h:raise ValueError('Incomplete float depth export')
            if np.mean(np.isfinite(data)&(data>=0)&(data<1))<0.01:continue
            return np.array(Image.open(path).convert('RGB')),data.reshape(h,w),depth['name'],color['name']
    raise ValueError(f'No matching scene/HDR/depth pair in frame {frame}')

def flow_picture(flow,valid):
    clean=np.nan_to_num(flow);mag=np.linalg.norm(clean,axis=-1)
    hue=(np.arctan2(clean[...,1],clean[...,0])+np.pi)*(179/(2*np.pi))
    scale=max(float(np.percentile(mag[valid],95)) if valid.any() else 0,0.5)
    hsv=np.zeros((*mag.shape,3),np.uint8);hsv[...,0]=hue.astype(np.uint8)
    hsv[...,1]=220;hsv[...,2]=np.clip(mag/scale*255,0,255).astype(np.uint8)
    rgb=cv2.cvtColor(hsv,cv2.COLOR_HSV2RGB);rgb[~valid]=[65,65,65]
    return rgb

def analyze(root,out):
    out.mkdir(parents=True,exist_ok=True)
    items=parse_images((root/'images.txt').read_text())
    depth_items=[r for r in items if r['name'].endswith('_d32f') and (root/(r['name']+'.f32')).is_file()]
    if not depth_items:raise ValueError('This dump has no exact float depth; stock PNG depth is insufficient')
    w,h=max(((r['width'],r['height']) for r in depth_items),key=lambda p:p[0]*p[1])
    cameras=parse_cameras((root/'passes.txt').read_text(),w,h)
    frames=sorted(cameras)
    if len(frames)!=2 or frames[1]!=frames[0]+1:raise ValueError(f'Expected exactly two consecutive camera frames: {frames}')
    # Match the same render stage using its shader set, not its changing pass number.
    signatures={};depth_resources={};p=None
    for line in (root/'passes.txt').read_text().splitlines():
        match=re.match(r'pass (\d+):',line)
        if match:p=int(match.group(1));signatures[p]=[]
        target=re.match(r'  td: .* addr (0x[0-9a-f]+)',line)
        if target and p is not None:depth_resources[p]=target.group(1)
        shader=re.search(r'pipe \d+ .*vs ([0-9a-f]+) fs ([0-9a-f]+)',line)
        if shader and p is not None:signatures[p].append(shader.groups())
    available={}
    for f in frames:
        available[f]={}
        for r in items:
            if r['frame']==f and (r['width'],r['height'])==(w,h) and '_t0_' in r['name'] and 'r11g11b10f' in r['name']:
                p=pass_id(r['name']);key=tuple(sorted(signatures.get(p,[])))
                if key:available[f][key]=p
    shared=set(available[frames[0]])&set(available[frames[1]])
    selected=None
    for key in sorted(shared,key=lambda k:available[frames[0]][k],reverse=True):
        try:
            first=scene_pair(root,items,frames[0],w,h,available[frames[0]][key])
            second=scene_pair(root,items,frames[1],w,h,available[frames[1]][key])
        except ValueError:continue
        selected=(first,second,key);break
    if selected is None:raise ValueError('No common render stage with valid exact depth in both frames')
    (previous,previous_depth,_,previous_name),(current,depth,depth_name,current_name),stage_signature=selected
    for frame,name in zip(frames,[previous_name,current_name]):
        selected_pass=pass_id(name)
        cameras[frame]=select_camera(cameras[frame],selected_pass,depth_resources.get(selected_pass))
    scale=w/960;size=(960,round(h/scale))
    prev=cv2.resize(previous,size,interpolation=cv2.INTER_AREA)
    cur=cv2.resize(current,size,interpolation=cv2.INTER_AREA)
    d=cv2.resize(depth,size,interpolation=cv2.INTER_NEAREST_EXACT)
    previous_d=cv2.resize(previous_depth,size,interpolation=cv2.INTER_NEAREST_EXACT)
    observed,confidence=observed_flow(cv2.cvtColor(prev,cv2.COLOR_RGB2GRAY),cv2.cvtColor(cur,cv2.COLOR_RGB2GRAY))
    solutions=[]
    for down in (False,True):
        predicted,valid=camera_flow(d,cameras[frames[1]]['vp'],cameras[frames[0]]['vp'],y_down=down,previous_depth=previous_d)
        good=valid&confidence
        residual=np.linalg.norm(observed-predicted,axis=-1)
        score=float(np.median(residual[good])) if good.any() else float('inf')
        solutions.append((score,down,predicted,valid))
    score,down,predicted,valid=min(solutions,key=lambda s:s[0])
    good=valid&confidence
    if not good.any():raise ValueError('No valid, textured correspondences; this capture cannot validate motion')
    residual=np.linalg.norm(observed-predicted,axis=-1)*scale
    residual[~good]=np.nan
    rotation=cameras[frames[0]]['view'][:3,:3]@cameras[frames[1]]['view'][:3,:3].T
    angle=float(np.degrees(np.arccos(np.clip((np.trace(rotation)-1)/2,-1,1))))
    position0=np.linalg.inv(cameras[frames[0]]['view'])[:3,3]
    position1=np.linalg.inv(cameras[frames[1]]['view'])[:3,3]
    translation=float(np.linalg.norm(position1-position0))
    summary={'frames':frames,'source_resolution':[w,h],'analysis_resolution':list(size),
        'selected_color':[previous_name,current_name],'selected_depth':depth_name,
        'matched_stage_shader_signature':stage_signature,
        'camera_support':{str(f):cameras[f]['supporting_blocks'] for f in frames},
        'y_down_selected':down,'orientation_scores_pixels':{str(s[1]):s[0]*scale for s in solutions},
        'camera_rotation_degrees':angle,'camera_translation_world_units':translation,
        'valid_projection_fraction':float(valid.mean()),'optical_confidence_fraction':float(confidence.mean()),
        'camera_median_pixels':float(np.median(np.linalg.norm(predicted[valid],axis=-1))*scale),
        'observed_median_pixels':float(np.median(np.linalg.norm(observed[confidence],axis=-1))*scale) if confidence.any() else None,
        'residual_median_pixels':float(np.nanmedian(residual)) if good.any() else None,
        'residual_p90_pixels':float(np.nanpercentile(residual,90)) if good.any() else None,
        'camera_cut_suspected':angle>20 or translation>5,
        'orientation_ambiguous':abs(solutions[0][0]-solutions[1][0])*scale<0.1,
        'limitations':['Optical flow is an image estimate, not ground truth.','Camera reprojection omits animated objects.','Depth consistency rejects occluded samples with a fixed raw-depth tolerance; thin edges remain sensitive.','Y orientation is selected diagnostically against optical flow; it is not a validated renderer contract.']}
    summary['registration_status']=registration_status(summary['residual_median_pixels'],summary['residual_p90_pixels'],float(good.mean()),summary['valid_projection_fraction'],summary['camera_cut_suspected'])
    summary['suitable_for_dlss_velocity_input']=False
    serial={str(f):{k:(v.tolist() if isinstance(v,np.ndarray) else v) for k,v in cameras[f].items()} for f in frames}
    (out/'cameras.json').write_text(json.dumps(serial,indent=2))
    (out/'metrics.json').write_text(json.dumps(summary,indent=2))
    np.savez_compressed(out/'motion-diagnostic.npz',camera_flow=predicted.astype(np.float32)*scale,
        observed_flow=observed*scale,valid=valid,confidence=confidence,residual=residual)
    panels=[cur,flow_picture(predicted,valid),flow_picture(observed,confidence)]
    heat=np.clip(np.nan_to_num(residual)/max(float(np.nanpercentile(residual,95)) if good.any() else 1,1)*255,0,255).astype(np.uint8)
    heat=cv2.cvtColor(cv2.applyColorMap(heat,cv2.COLORMAP_INFERNO),cv2.COLOR_BGR2RGB);heat[~good]=[65,65,65];panels.append(heat)
    labels=['Scene sans HUD — deux images successives','Mouvement predit par la camera','Mouvement estime depuis les images','Ecart entre les deux — pas une segmentation de Link']
    pw,ph=640,360;canvas=Image.new('RGB',(pw*2,(ph+42)*2+48),'#121922');draw=ImageDraw.Draw(canvas)
    font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',19)
    for i,(panel,label) in enumerate(zip(panels,labels)):
        x=(i%2)*pw;y=(i//2)*(ph+42)
        draw.text((x+12,y+9),label,font=font,fill='white')
        canvas.paste(Image.fromarray(panel).resize((pw,ph)),(x,y+42))
    draw.text((12,(ph+42)*2+12),'Diagnostic uniquement. Gris : donnees rejetees. Intensites normalisees par panneau.',font=font,fill='#c4cbd2')
    canvas.save(out/'diagnostic.png')
    print(json.dumps(summary,indent=2))
    return summary

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('capture',type=Path);parser.add_argument('output',type=Path)
    args=parser.parse_args();analyze(args.capture,args.output)
