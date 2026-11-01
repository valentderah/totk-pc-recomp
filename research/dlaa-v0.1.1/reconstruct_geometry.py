"""Project one captured rigid-skinned mesh and compare its two poses visually."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw
from objects import parse_objects
from motion import validated_camera
from skinning import decode_attribute,rigid_clip_positions,weighted_clip_positions
from vertex_shader import PositionProgram
from raster_motion import triangles_from_indices

def reconstruct(root,out,shader_id='68e66bf9643995b0',first_draw=None):
    out.mkdir(parents=True,exist_ok=True)
    objects=parse_objects((root/'objects.txt').read_text(),(root/'object-uniforms.bin').read_bytes())
    by_draw={r['draw']:r for r in objects};geometry=[];entry=None
    for line in (root/'geometry.txt').read_text().splitlines():
        if line.startswith('draw '):
            draw,frame,stage=map(int,re.fullmatch(r'draw (\d+) frame (\d+) pass (\d+)',line).groups())
            entry=dict(draw=draw,frame=frame,stage=stage,shader='68e66bf9643995b0',attributes={},streams={});geometry.append(entry)
        elif line.startswith(' shader '):entry['shader']=line.split()[1]
        elif line.startswith(' viewport '):entry['viewport']=list(map(float,line.split()[1:]))
        elif line.startswith(' indices '):
            match=re.fullmatch(r' indices prim (-?\d+) count (\d+) itype (\d+) base (-?\d+) instances (\d+) base_instance (\d+) restart (\d+) restart_index (\d+) file (\S+)',line)
            prim,count,itype,base,instances,base_instance,restart,restart_index,file=match.groups()
            entry['indices']=dict(prim=int(prim),count=int(count),itype=int(itype),base=int(base),instances=int(instances),
                base_instance=int(base_instance),restart=int(restart),file=file)
        elif line.startswith(' arrays ') or line.startswith(' draw_parameters '):entry['indices']=None
        elif line.startswith(' attr '):
            values=list(map(int,re.fullmatch(r' attr (\d+) nvn (\d+) vk (\d+) type (\d+) offset (\d+) stream (\d+) stride (\d+) divisor (\d+)',line).groups()))
            slot,nvn,vk,kind,offset,stream,stride,divisor=values
            entry['attributes'][slot]=dict(vk=vk,offset=offset,stream=stream,stride=stride,divisor=divisor)
        elif line.startswith(' stream '):
            slot,addr,size,file=re.fullmatch(r' stream (\d+) addr (0x[0-9a-f]+) size (\d+) file (\S+)',line).groups()
            path=root/file
            if file=='unavailable' or not path.is_file() or path.stat().st_size!=int(size):raise ValueError('Incomplete GPU geometry capture')
            entry['streams'][int(slot)]=dict(addr=int(addr,0),size=int(size),path=path)
        elif line.strip():raise ValueError('Unknown geometry metadata')
    geometry=[g for g in geometry if g['shader']==shader_id]
    frames=sorted(set(g['frame'] for g in geometry))
    if len(frames)!=2 or frames[1]!=frames[0]+1:raise ValueError('Expected consecutive geometry frames')
    first=[g for g in geometry if g['frame']==frames[0] and (first_draw is None or g['draw']==first_draw)]
    if not first:raise ValueError('Selected first-frame mesh is absent')
    a=first[0]
    def key(g):return (g['attributes'],[(k,s['addr'],s['size']) for k,s in sorted(g['streams'].items())])
    second=[g for g in geometry if g['frame']==frames[1] and key(g)==key(a)]
    if len(second)!=1:raise ValueError('No unique matching second-frame geometry')
    geometry=[a,second[0]]
    clips=[];attributes=[];hashes=[];palette_sizes=[];bone_sets=[];references=[]
    shader=PositionProgram((root/('sh_'+shader_id+'.spv')).read_bytes())
    for entry in geometry:
        record=by_draw[entry['draw']]
        if record['frame']!=entry['frame'] or record['stage']!=entry['stage'] or record['vs']!=shader_id:
            raise ValueError('Geometry/uniform draw association differs')
        attrs=entry['attributes'];streams=entry['streams']
        count=streams[attrs[0]['stream']]['size']//attrs[0]['stride']
        decoded={}
        for location in sorted(shader.input_locations):
            a=attrs[location]
            if a['divisor']:raise ValueError('Instanced attributes are unsupported')
            raw=streams[a['stream']]['path'].read_bytes()
            decoded[location]=decode_attribute(raw,a['vk'],a['offset'],a['stride'],count)
        uniforms={}
        for slot in (4,5,7):
            u=record['uniforms'][slot]
            if u['source']!='snapshot' or u['size']%16:raise ValueError('Incomplete or misaligned uniform snapshot')
            uniforms[slot]=np.frombuffer(u['data'],dtype='<f4').reshape(-1,4)
        view,projection,_=validated_camera(uniforms[4][:16])
        palette=uniforms[5].reshape(-1,3,4)
        if 4 in decoded:
            components=sorted(shader.input_components[6])
            bones=decoded[6][:,components];weights=decoded[4][:,components]
            if 7 in decoded:
                components=sorted(shader.input_components[7])
                bones=np.column_stack([bones,decoded[7][:,components]])
                weights=np.column_stack([weights,decoded[5][:,components]])
            clip=weighted_clip_positions(decoded[0][:,:3],bones,weights,palette,uniforms[7][7,:3],view,projection)
        else:
            bones=decoded[6][:,0]
            clip=rigid_clip_positions(decoded[0][:,:3],bones,palette,uniforms[7][7,:3],view,projection)
        inputs={}
        for location,data in decoded.items():
            padded=np.zeros((count,4),data.dtype);padded[:,3]=1;padded[:,:data.shape[1]]=data
            inputs[location]=padded
        reference=shader.evaluate(inputs,uniforms)
        if not np.isfinite(reference).all() or np.any(reference[:,3]<=0):raise ValueError('Shader position evaluation is incomplete or behind camera')
        references.append(reference)
        clips.append(clip);attributes.append(decoded);palette_sizes.append(len(palette));bone_sets.append(sorted(set(map(int,bones.ravel()))))
        hashes.append([hashlib.sha256(streams[attrs[k]['stream']]['path'].read_bytes()).hexdigest() for k in sorted(shader.input_locations)])
    if hashes[0]!=hashes[1] or geometry[0]['attributes']!=geometry[1]['attributes']:
        raise ValueError('Bind geometry or attribute layout changes between poses')
    triangles=None
    if 'indices' in geometry[0]:
        decoded_indices=[]
        for entry in geometry:
            d=entry.get('indices')
            if not d or d['instances'] not in (0,1) or d['base_instance'] or d['restart'] or d['itype'] not in (0,1,2):
                raise ValueError('Unsupported indexed drawing contract')
            path=root/d['file'];expected=d['count']*(1<<d['itype'])
            if not path.is_file() or path.stat().st_size<expected:raise ValueError('Incomplete index capture')
            index=np.fromfile(path,dtype=('u1','<u2','<u4')[d['itype']],count=d['count'])
            decoded_indices.append(triangles_from_indices(index,d['prim'],d['base'],len(clips[0])))
        if not np.array_equal(*decoded_indices):raise ValueError('Triangle correspondence differs between poses')
        triangles=decoded_indices[0]
    # The selected pre-HUD scene is independently located by analyze.py.
    metrics=json.loads((out/'metrics.json').read_text());w,h=metrics['source_resolution']
    for entry in geometry:
        if 'viewport' not in entry or not np.allclose(entry['viewport'],[0,h,w,-h]):
            raise ValueError('Requires explicitly captured full-image negative-height Vulkan viewport')
    uv=[]
    for clip in clips:
        if not np.isfinite(clip).all() or np.any(clip[:,3]<=0):raise ValueError('Candidate vertices lie behind camera or are non-finite')
        ndc=clip[:,:3]/clip[:,3:4]
        uv.append(np.column_stack([(ndc[:,0]+1)*w/2,(1-ndc[:,1])*h/2]))
    reference_uv=[np.column_stack([(clip[:,0]/clip[:,3]+1)*w/2,(1-clip[:,1]/clip[:,3])*h/2]) for clip in references]
    formula_error=[np.linalg.norm(expected-measured,axis=-1) for expected,measured in zip(uv,reference_uv)]
    image=Image.open(root/(metrics['selected_color'][1]+'_tm.png')).convert('RGB')
    rgb=np.array(image);rgb=np.rint((rgb/255)**(1/2.2)*255).astype(np.uint8)
    image=Image.fromarray(rgb);draw=ImageDraw.Draw(image)
    for first,second in zip(*uv):
        x,y=second;px,py=first
        draw.line((px,py,x,y),fill='#ff3030',width=2)
        draw.ellipse((x-3,y-3,x+3,y+3),fill='#50ffff')
    suffix=shader_id+'-d'+str(geometry[0]['draw'])
    image.save(out/('mesh-overlay-'+suffix+'.png'))
    displacement=np.linalg.norm(uv[1]-uv[0],axis=-1)
    summary=dict(shader=shader_id,frames=[g['frame'] for g in geometry],draws=[g['draw'] for g in geometry],vertex_count=len(clips[0]),
        palette_sizes=palette_sizes,bone_indices_used=bone_sets,bind_geometry_identical=True,
        max_formula_vs_shader_position_error_pixels=float(max(np.max(e) for e in formula_error)),
        median_vertex_motion_pixels=float(np.median(displacement)),p90_vertex_motion_pixels=float(np.percentile(displacement,90)),
        screen_bounds_current=[uv[1].min(axis=0).tolist(),uv[1].max(axis=0).tolist()],
        influences_per_vertex=bones.shape[1] if bones.ndim==2 else 1,
        status='formula_agrees_with_cpu_shader' if max(np.max(e) for e in formula_error)<.01 else 'formula_differs_from_cpu_shader',
        limitations=['All bound vertices are projected; draw indices and visibility are not captured.',
            'CPU evaluation of the captured shader is not a direct GPU output measurement.',
            'Mesh identity is checked visually in the overlay, not established by uniforms alone.',
            'Not a dense engine motion buffer, and not suitable for DLSS input.'])
    payload=dict(previous_clip=clips[0],current_clip=clips[1],previous_uv=uv[0],current_uv=uv[1])
    if triangles is not None:
        payload['triangles']=triangles
        summary['triangle_count']=len(triangles)
        summary['referenced_vertex_count']=len(np.unique(triangles))
        summary['limitations'][0]='Exact triangle list captured; visibility requires a separate depth-tested rasterization.'
    np.savez_compressed(out/('mesh-motion-'+suffix+'.npz'),**payload)
    (out/('mesh-metrics-'+suffix+'.json')).write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))
    return summary

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('capture',type=Path);parser.add_argument('output',type=Path)
    parser.add_argument('--shader',default='68e66bf9643995b0')
    parser.add_argument('--first-draw',type=int)
    args=parser.parse_args();reconstruct(args.capture,args.output,args.shader,args.first_draw)
