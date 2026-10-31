"""Common-mask motion-compensated residuals: low residual alone does not prove quality."""
import argparse,json
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw
from optical import cv2
from prepare_dlaa_replay import verify_manifest
from compare_dlaa_presets import validate_receipts

def unjitter_motion(composite,bias,now,old):
    h,w=composite.shape[:2]
    if composite.shape!=(h,w,4) or bias.shape!=(h,w) or not np.isfinite(composite).all() or not np.isfinite(bias).all():raise ValueError('Invalid motion/confidence capture')
    now=np.asarray(now,dtype=np.float32);old=np.asarray(old,dtype=np.float32)
    if now.shape!=(2,) or old.shape!=(2,) or not np.isfinite(now).all() or not np.isfinite(old).all():raise ValueError('Invalid jitter')
    yy,xx=np.mgrid[:h,:w].astype(np.float32);sx=xx+now[0];sy=yy+now[1]
    sample=cv2.remap(composite,sx,sy,cv2.INTER_LINEAR,borderMode=cv2.BORDER_CONSTANT)
    confidence=cv2.remap(bias,sx,sy,cv2.INTER_LINEAR,borderMode=cv2.BORDER_CONSTANT,borderValue=1)
    flow=sample[:,:,:2]+now-old
    inside=(sx>=0)&(sx<=w-1)&(sy>=0)&(sy<=h-1)&(xx+flow[:,:,0]>=0)&(xx+flow[:,:,0]<=w-1)&(yy+flow[:,:,1]>=0)&(yy+flow[:,:,1]<=h-1)&(confidence<.2)
    masks={}
    for name,category in (('mesh',1),('camera',2)):
        mask=inside&(np.abs(sample[:,:,3]-category)<.01)
        masks[name]=cv2.erode(mask.astype(np.uint8),np.ones((7,7),np.uint8)).astype(bool)
    return flow,masks

def temporal_residual(current,previous,flow,mask):
    h,w=mask.shape
    if current.shape!=(h,w,3) or previous.shape!=current.shape or flow.shape!=(h,w,2) or mask.sum()<100:raise ValueError('Insufficient common temporal coverage')
    if not np.isfinite(current).all() or not np.isfinite(previous).all() or not np.isfinite(flow).all():raise ValueError('Nonfinite temporal input')
    yy,xx=np.mgrid[:h,:w].astype(np.float32)
    warped=cv2.remap(previous,xx+flow[:,:,0],yy+flow[:,:,1],cv2.INTER_LINEAR,borderMode=cv2.BORDER_CONSTANT)
    delta=np.abs(np.log1p(np.maximum(current,0))-np.log1p(np.maximum(warped,0))).mean(axis=-1)[mask]
    return dict(pixels=int(mask.sum()),mean=float(delta.mean()),median=float(np.median(delta)),p95=float(np.percentile(delta,95)))

def analyze(inputs,default,k,l,out):
    manifest=verify_manifest(inputs);receipts=validate_receipts(inputs,(default,k,l))
    # Require the separate full comparison to have validated lifecycle and bit-exact default replay.
    comparison=json.loads((out/'preset-comparison.json').read_text())
    frames=manifest['frames'];capture=Path(manifest['source']);w,h=manifest['width'],manifest['height']
    if comparison['exact_input_manifest']!=str(inputs.resolve()) or comparison['receipts']!=receipts or not comparison['all_default_outputs_match_original_exactly'] or [x['frame'] for x in comparison['frames']]!=[x['frame'] for x in frames]:raise ValueError('Run exact-input preset comparison first')
    rows=[];prior={};sheets=[];indices={8,16,24,len(frames)-1}
    def display(a):
        a=np.maximum(a,0);return Image.fromarray(np.rint((a/(1+a))**(1/2.2)*255).clip(0,255).astype('uint8'))
    for index,m in enumerate(frames):
        f=m['frame'];current={name:np.fromfile(root/f'ngx-output-{f}-{w}x{h}.rgba16f',dtype='<f2').reshape(h,w,4)[:,:,:3].astype(np.float32) for name,root in (('K',k),('L',l))}
        if index:
            composite=np.fromfile(capture/f'gpu-composite-{f}-{w}x{h}.rgba32f',dtype='<f4').reshape(h,w,4)
            supplied=np.fromfile(m['inputs'][2]['path'],dtype='<f4').reshape(h,w,2)
            if not np.array_equal(composite[:,:,:2],supplied):raise ValueError('Temporal motion differs from exact SDK input')
            bias=np.fromfile(m['inputs'][3]['path'],dtype='<f4').reshape(h,w)
            flow,masks=unjitter_motion(composite,bias,m['jitter'],frames[index-1]['jitter'])
            row=dict(frame=f,regions={})
            for region,mask in masks.items():
                row['regions'][region]={name:temporal_residual(current[name],prior[name],flow,mask) for name in ('K','L')}
            # Camera-backed pixels provide measured scene movement, with raster jitter removed.
            row['camera_motion_median_pixels']=float(np.median(np.linalg.norm(flow[masks['camera']],axis=-1)))
            rows.append(row)
        if index in indices:
            sheet=Image.new('RGB',(1280,740),'#121922');d=ImageDraw.Draw(sheet)
            for col,name in enumerate(('K','L')):
                im=display(current[name]);x=640*col;d.text((x+12,8),f'Profil {name} - image {index+1}/{len(frames)} (trame {f})',fill='white')
                sheet.paste(im.resize((640,360)),(x,28))
                sheet.paste(im.crop((1024,640,1344,800)).resize((640,320),Image.Resampling.NEAREST),(x,412))
            d.text((12,394),'Meme conversion; detail central agrandi x2 sans filtre. Capture separee.',fill='white');sheets.append(sheet)
        prior=current
    if len(rows)!=len(frames)-1 or min(x['camera_motion_median_pixels'] for x in rows)<.5:raise ValueError('Rotation not established independently of raster jitter')
    summary={}
    for region in ('camera','mesh'):
        summary[region]={name:dict(mean_of_frame_means=float(np.mean([r['regions'][region][name]['mean'] for r in rows])),minimum_pixels=min(r['regions'][region][name]['pixels'] for r in rows)) for name in ('K','L')}
    result=dict(frame_pairs=len(rows),common_masks=True,motion_dejittered=True,camera_motion_median_range_pixels=[min(r['camera_motion_median_pixels'] for r in rows),max(r['camera_motion_median_pixels'] for r in rows)],summary=summary,frames=rows,quality_validated=False,presented=False,limitation='Lower temporal residual may indicate blur; heuristic common masks do not establish all object identities or disocclusions.')
    (out/'temporal-comparison.json').write_text(json.dumps(result,indent=2))
    contact=Image.new('RGB',(1280,len(sheets)*740),'#121922')
    for i,s in enumerate(sheets):contact.paste(s,(0,i*740))
    contact.save(out/'motion-crops.png')
    print(json.dumps({x:result[x] for x in ('frame_pairs','camera_motion_median_range_pixels','summary')},indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('inputs',type=Path);p.add_argument('default',type=Path);p.add_argument('k',type=Path);p.add_argument('l',type=Path);p.add_argument('out',type=Path);a=p.parse_args();analyze(a.inputs,a.default,a.k,a.l,a.out)
