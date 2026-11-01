"""Build an offline, masked character motion field; never a DLSS-ready buffer."""
import argparse,json
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw,ImageFont
from raster_motion import rasterize_motion
from sparse_motion import track_features
from optical import cv2

def compare_sparse_motion(flow,mask,points,observed,scale):
    if not len(points):return np.empty(0,np.float32)
    full=(points+.5)*scale-.5
    mapx=full[:,0].astype(np.float32).reshape(-1,1);mapy=full[:,1].astype(np.float32).reshape(-1,1)
    eroded=cv2.erode(mask.astype(np.uint8),np.ones((5,5),np.uint8)).astype(np.float32)
    inside=cv2.remap(eroded,mapx,mapy,cv2.INTER_LINEAR).ravel()>=.999
    sampled=cv2.remap(np.nan_to_num(flow).astype(np.float32),mapx,mapy,cv2.INTER_LINEAR).reshape(-1,2)
    return np.linalg.norm(sampled-observed*scale,axis=1)[inside]

def build(root,out):
    m=json.loads((out/'metrics.json').read_text());batch=json.loads((out/'reconstruction-batch.json').read_text())
    w,h=m['source_resolution'];depths=[];images=[]
    for color in m['selected_color']:
        prefix=color.split('_t0_')[0];file=next(root.glob(prefix+'_td_*_d32f.f32'))
        raw=np.fromfile(file,dtype='<f4')
        if raw.size!=w*h:raise ValueError('Incomplete exact depth image')
        depths.append(raw.reshape(h,w));images.append(np.array(Image.open(root/(color+'_tm.png')).convert('RGB')))
    flow=np.full((h,w,2),np.nan,np.float32);z=np.full((h,w),np.inf,np.float32);mask=np.zeros((h,w),bool)
    triangle_count=0;used_vertices=0;meshes=0
    for record in batch:
        if 'error' in record:raise ValueError('Incomplete mesh reconstruction')
        suffix=record['shader']+'-d'+str(record['draws'][0]);data=np.load(out/('mesh-motion-'+suffix+'.npz'))
        if 'triangles' not in data:raise ValueError('Mesh has no captured triangle topology')
        f,d,valid=rasterize_motion(data['current_clip'],data['previous_clip'],data['triangles'],w,h,
            scene_depth=depths[1],previous_scene_depth=depths[0])
        take=valid&(d<z);flow[take]=f[take];z[take]=d[take];mask|=take
        triangle_count+=len(data['triangles']);used_vertices+=len(np.unique(data['triangles']));meshes+=1
    if not mask.any():raise ValueError('No depth-consistent mesh pixels; motion remains unvalidated')
    np.savez_compressed(out/'character-pixel-motion.npz',current_to_previous_pixels=flow,valid=mask,mesh_depth=z)
    # Independent sparse correspondences, restricted to the interior of our mask.
    yy,xx=np.where(mask)
    roi=(max(0,int(xx.min())-64),max(0,int(yy.min())-64),min(w,int(xx.max())+65),min(h,int(yy.max())+65))
    left,top,right,bottom=roi
    gray=[cv2.cvtColor(a[top:bottom,left:right],cv2.COLOR_RGB2GRAY) for a in images]
    points,observed=track_features(*gray)
    points+=np.array([left,top],np.float32)
    errors=compare_sparse_motion(flow,mask,points,observed,1)
    magnitudes=np.linalg.norm(flow[mask],axis=-1)
    summary=dict(meshes=meshes,triangles=triangle_count,referenced_vertices_across_draws=used_vertices,
        visible_depth_consistent_pixels=int(mask.sum()),image_fraction=float(mask.mean()),
        motion_median_pixels=float(np.median(magnitudes)),motion_p90_pixels=float(np.percentile(magnitudes,90)),
        sparse_comparison_points=len(errors),sparse_residual_median_pixels=float(np.median(errors)) if len(errors) else None,
        sparse_residual_p90_pixels=float(np.percentile(errors,90)) if len(errors) else None,
        sparse_comparison_scope='Native-resolution crop around character mask; independent image tracking',sparse_crop=roi,
        direction='current pixel to its previous position; units native output pixels',depth_tolerance_raw=2e-6,
        suitable_for_dlss_input=False,
        limitations=['Offline CPU rasterization, not a runtime pass.','Only captured character meshes are covered; background is invalid.',
            'Depth tolerance and bilinear previous-depth filtering can reject valid edge pixels.',
            'No full GPU rasterization equivalence, persistent identity, jitter or exposure contract.'])
    (out/'pixel-motion-metrics.json').write_text(json.dumps(summary,indent=2))
    rgb=np.rint((images[1]/255)**(1/2.2)*255).astype(np.uint8)
    color=rgb.copy();norm=max(float(np.percentile(magnitudes,95)),1)
    heat=np.clip(np.nan_to_num(np.linalg.norm(flow,axis=-1))/norm*255,0,255).astype(np.uint8)
    mapped=cv2.cvtColor(cv2.applyColorMap(heat,cv2.COLORMAP_TURBO),cv2.COLOR_BGR2RGB)
    color[mask]=mapped[mask]
    yy,xx=np.where(mask);box=(max(0,int(xx.min())-70),max(0,int(yy.min())-70),min(w,int(xx.max())+70),min(h,int(yy.max())+70))
    a=Image.fromarray(rgb).crop(box);b=Image.fromarray(color).crop(box);pw=600;ph=round(a.height*pw/a.width)
    canvas=Image.new('RGB',(1200,ph+90),'#121922');draw=ImageDraw.Draw(canvas);font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',21)
    draw.text((12,12),'Image du jeu',fill='white',font=font);draw.text((612,12),'Mouvement par pixel — surfaces retenues',fill='white',font=font)
    canvas.paste(a.resize((pw,ph)),(0,48));canvas.paste(b.resize((pw,ph)),(600,48))
    draw.text((12,ph+56),'Couleur : amplitude. Sans couleur : aucune donnee de mouvement revendiquee.',fill='#cbd4df',font=font)
    canvas.save(out/'pixel-motion-comparison.png');print(json.dumps(summary,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('output',type=Path)
    args=p.parse_args();build(args.capture,args.output)
