"""Reproduce the captured-mesh batch and a visual comparison; no dense motion."""
import argparse
import contextlib
import io
import json
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw,ImageFont
from reconstruct_geometry import reconstruct

def run(root,out,reuse=False):
    out.mkdir(parents=True,exist_ok=True)
    if not reuse:
        entries=[];current=None
        for line in (root/'geometry.txt').read_text().splitlines():
            if line.startswith('draw '):
                fields=line.split();current=dict(draw=int(fields[1]),frame=int(fields[3]));entries.append(current)
            elif line.startswith(' shader '):current['shader']=line.split()[1]
        first=min(e['frame'] for e in entries);batch=[]
        for entry in entries:
            if entry['frame']!=first:continue
            try:
                with contextlib.redirect_stdout(io.StringIO()):
                    result=reconstruct(root,out,entry['shader'],entry['draw'])
                batch.append(result)
            except (ValueError,KeyError,FileNotFoundError) as error:
                batch.append(dict(shader=entry['shader'],draw=entry['draw'],error=str(error)))
        (out/'reconstruction-batch.json').write_text(json.dumps(batch,indent=2))
    batch=json.loads((out/'reconstruction-batch.json').read_text())
    metrics=json.loads((out/'metrics.json').read_text())
    rgb=np.array(Image.open(root/(metrics['selected_color'][1]+'_tm.png')).convert('RGB'))
    rgb=np.rint((rgb/255)**(1/2.2)*255).astype(np.uint8)
    original=Image.fromarray(rgb);overlay=original.copy();draw=ImageDraw.Draw(overlay)
    points=[];motion=[];maxerror=0
    for result in batch:
        if 'error' in result:continue
        suffix=result['shader']+'-d'+str(result['draws'][0])
        data=np.load(out/('mesh-motion-'+suffix+'.npz'))
        for old,now in zip(data['previous_uv'][::6],data['current_uv'][::6]):
            x,y=now;px,py=old
            draw.line((px,py,x,y),fill='#ff5050',width=1)
            draw.ellipse((x-1,y-1,x+1,y+1),fill='#50ffff')
        points.append(data['current_uv'])
        motion.extend(np.linalg.norm(data['current_uv']-data['previous_uv'],axis=-1).tolist())
        maxerror=max(maxerror,result['max_formula_vs_shader_position_error_pixels'])
    if not points:raise ValueError('No reconstructed geometry; no comparison image produced')
    allpoints=np.vstack(points);lo=allpoints.min(axis=0)-[75,70];hi=allpoints.max(axis=0)+[75,70]
    box=tuple(map(int,[max(0,lo[0]),max(0,lo[1]),min(original.width,hi[0]),min(original.height,hi[1])]))
    first=original.crop(box);second=overlay.crop(box)
    width=600;height=round(first.height*width/first.width)
    canvas=Image.new('RGB',(1200,height+90),'#121922');labels=ImageDraw.Draw(canvas)
    font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',21)
    labels.text((15,12),'Image du jeu',font=font,fill='white')
    labels.text((615,12),'Sommets reconstruits et deplacement',font=font,fill='white')
    canvas.paste(first.resize((width,height)),(0,48));canvas.paste(second.resize((width,height)),(600,48))
    labels.text((15,height+56),'Cyan : position actuelle. Rouge : trajet depuis la position precedente.',font=font,fill='#d0d6df')
    canvas.save(out/'character-reconstruction.png')
    summary=dict(meshes=len(points),vertices=sum(r.get('vertex_count',0) for r in batch),
        failed_meshes=sum('error' in r for r in batch),max_formula_vs_cpu_shader_error_pixels=maxerror,
        median_vertex_motion_pixels=float(np.median(motion)),p90_vertex_motion_pixels=float(np.percentile(motion,90)),
        crop_bounds=box,visualized_vertex_step=6,
        scope='Offline mesh positions, CPU shader evaluation and visual superposition; no GPU-output equality proof or dense velocity.')
    (out/'character-summary.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))
    if summary['failed_meshes']:raise ValueError('Some captured meshes could not be reconstructed; see batch report')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('capture',type=Path);parser.add_argument('output',type=Path)
    parser.add_argument('--reuse',action='store_true',help='Only render the existing batch outputs')
    args=parser.parse_args();run(args.capture,args.output,args.reuse)
