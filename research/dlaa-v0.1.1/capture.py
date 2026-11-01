"""Read native probe exports without treating transient addresses as identifiers."""
import re
from collections import Counter
import numpy as np
from motion import validated_camera

def parse_images(text):
    result=[]
    for line in text.splitlines():
        match=re.match(r'(\S+) (\d+)x(\d+)x(\d+) .* frame (\d+)$',line)
        if match:
            name,w,h,d,frame=match.groups()
            result.append(dict(name=name,width=int(w),height=int(h),layers=int(d),frame=int(frame)))
    return result

def parse_cameras(text,width,height):
    lines=text.splitlines();candidates={};frame=None;render=False;pass_id=None;area=None;depth_resource=None
    for i,line in enumerate(lines):
        if line.startswith('pass '):
            depth_resource=None
            header=re.match(r'pass (\d+): render pass,.*area (\d+)x(\d+).*frame (\d+)',line)
            render=bool(header)
            if header:
                p,w,h,f=map(int,header.groups());pass_id=p;area=(w,h);frame=f
        depth=re.match(r'  td: .* addr (0x[0-9a-f]+)',line)
        if depth:depth_resource=depth.group(1)
        if not render or area!=(width,height) or not re.search(r'\bubo c4 addr \S+ size 0x940\b',line):
            continue
        rows={}
        for following in lines[i+1:i+5]:
            for index,values in re.findall(r'\[(\d+)\]\s+([^\[]+)',following):
                try: values=[float(v) for v in values.split()]
                except ValueError: continue
                if len(values)==4:rows[int(index)]=values
        if set(rows)!=set(range(16)):continue
        a=np.array([rows[k] for k in range(16)])
        try:view,projection,vp=validated_camera(a)
        except ValueError:continue
        candidate=dict(frame=frame,pass_=pass_id,view=view,projection=projection,vp=vp,depth_resource=depth_resource)
        candidate['pass']=candidate.pop('pass_')
        candidates.setdefault(frame,[]).append(candidate)
    result={}
    for frame,items in candidates.items():
        keys=[tuple(np.round(item['vp'].ravel(),6)) for item in items]
        key,count=Counter(keys).most_common(1)[0]
        selected=items[keys.index(key)].copy();selected['supporting_blocks']=count
        selected['candidate_blocks']=len(items)
        selected['observations']=items
        result[frame]=selected
    return result

def select_camera(frame_cameras,scene_pass,depth_resource):
    items=[o for o in frame_cameras['observations'] if o['pass']<=scene_pass and o['depth_resource']==depth_resource]
    if not depth_resource or not items:raise ValueError('No camera associated with this scene depth resource')
    reference=items[-1]['vp']
    if not all(np.allclose(o['vp'],reference,atol=0.0001,rtol=0.00001) for o in items):
        raise ValueError('Multiple conflicting cameras wrote the same depth resource')
    selected=items[-1].copy();selected['supporting_blocks']=len(items)
    return selected
