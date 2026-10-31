"""Compare the exported runtime GPU motion image with the offline CPU reference."""
import argparse,json
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw,ImageFont
from optical import cv2

def load_gpu_motion(path,w,h):
    data=np.fromfile(path,dtype='<f4')
    if data.size!=w*h*4:raise ValueError('Incomplete GPU export')
    data=data.reshape(h,w,4)
    if not np.isfinite(data).all():raise ValueError('GPU export contains non-finite values')
    if not (np.isclose(data[...,3],-1,atol=1e-6)|np.isclose(data[...,3],0,atol=1e-6)|np.isclose(data[...,3],1,atol=1e-6)).all():
        raise ValueError('Unexpected GPU validity channel')
    mask=data[...,3]>.5
    if np.any((data[...,2][mask]<0)|(data[...,2][mask]>=1)):raise ValueError('Invalid GPU depth')
    return data,mask

def compare(root,out,bounds_test=False):
    m=json.loads((out/'metrics.json').read_text());w,h=m['source_resolution'];frame=m['frames'][1]
    gpu,mask=load_gpu_motion(root/f'gpu-motion-{frame}-{w}x{h}.rgba32f',w,h)
    invalid_test_pixels=None
    if bounds_test:
        _,test_mask=load_gpu_motion(root/f'gpu-motion-{frame-1}-{w}x{h}.rgba32f',w,h)
        invalid_test_pixels=int(test_mask.sum())
        if invalid_test_pixels:raise ValueError('Invalid palette test produced valid GPU motion pixels')
    cpu=np.load(out/'character-pixel-motion.npz');common=mask&cpu['valid']
    if common.sum()<100:raise ValueError('Insufficient GPU/CPU overlap for comparison')
    residual=np.linalg.norm(gpu[...,:2]-cpu['current_to_previous_pixels'],axis=-1)
    interior=cv2.erode(common.astype(np.uint8),np.ones((3,3),np.uint8)).astype(bool)
    if not interior.any():raise ValueError('No common surface interior')
    errors=residual[interior]
    summary=dict(frame=frame,resolution=[w,h],gpu_valid_pixels=int(mask.sum()),cpu_valid_pixels=int(cpu['valid'].sum()),
        common_pixels=int(common.sum()),common_interior_pixels=int(interior.sum()),
        residual_median_pixels=float(np.median(errors)),residual_p90_pixels=float(np.percentile(errors,90)),
        residual_p99_pixels=float(np.percentile(errors,99)),residual_max_pixels=float(errors.max()),
        invalid_palette_test_valid_pixels=invalid_test_pixels,
        comparison_status='agrees_on_common_interiors' if np.median(errors)<.1 and np.percentile(errors,90)<.5 else 'needs_investigation',
        suitable_for_dlss_input=False,
        limitations=['GPU can cover additional shader-family meshes beyond the offline selected set.',
            'Only common surface interiors are compared numerically; this is not an all-pixel validation.',
            'Candidate geometry continuity is not persistent game-object identity.'])
    (out/'gpu-comparison.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))
    rgb=np.array(Image.open(root/(m['selected_color'][1]+'_tm.png')).convert('RGB'))
    rgb=np.rint((rgb/255)**(1/2.2)*255).astype(np.uint8);overlay=rgb.copy()
    magnitude=np.linalg.norm(gpu[...,:2],axis=-1);scale=max(float(np.percentile(magnitude[mask],95)),1)
    colors=cv2.cvtColor(cv2.applyColorMap(np.clip(magnitude/scale*255,0,255).astype(np.uint8),cv2.COLORMAP_TURBO),cv2.COLOR_BGR2RGB)
    overlay[mask]=colors[mask];yy,xx=np.where(mask)
    box=(max(0,int(xx.min())-60),max(0,int(yy.min())-60),min(w,int(xx.max())+61),min(h,int(yy.max())+61))
    a=Image.fromarray(rgb).crop(box);b=Image.fromarray(overlay).crop(box);pw=600;ph=round(a.height*pw/a.width)
    canvas=Image.new('RGB',(1200,ph+90),'#121922');draw=ImageDraw.Draw(canvas);font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',21)
    draw.text((12,12),'Image du jeu',font=font,fill='white');draw.text((612,12),'Mouvement calcule par le passage GPU',font=font,fill='white')
    canvas.paste(a.resize((pw,ph)),(0,48));canvas.paste(b.resize((pw,ph)),(600,48))
    draw.text((12,ph+56),'Couleur : amplitude. Sans couleur : aucune donnee de mouvement revendiquee.',font=font,fill='#cbd4df')
    canvas.save(out/'gpu-motion-comparison.png')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('output',type=Path);p.add_argument('--bounds-test',action='store_true')
    args=p.parse_args();compare(args.capture,args.output,args.bounds_test)
