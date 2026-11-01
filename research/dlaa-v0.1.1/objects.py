"""Inventory captured VS uniforms. Geometry addresses are candidates, not identities."""
import argparse
from collections import Counter,defaultdict
import json
from pathlib import Path
import re

def parse_objects(text,binary):
    records=[];current=None
    for line in text.splitlines():
        if line.startswith('draw '):
            m=re.fullmatch(r'draw (\d+) frame (\d+) fence (\d+) pass (\d+) vs ([0-9a-f]+) fs ([0-9a-f]+) area (\d+)x(\d+) depth (0x[0-9a-f]+|0)',line)
            if not m:raise ValueError('Malformed draw record')
            draw,frame,fence,stage,vs,fs,w,h,depth=m.groups()
            current=dict(draw=int(draw),frame=int(frame),fence=int(fence),stage=int(stage),vs=vs,fs=fs,
                size=(int(w),int(h)),depth=int(depth,0),vertices=[],uniforms={},storage=[])
            records.append(current)
        elif line.startswith(' vbuf '):
            m=re.fullmatch(r' vbuf (\d+) addr (0x[0-9a-f]+) size (0x[0-9a-f]+)',line)
            if current is None or not m:raise ValueError('Malformed vertex binding')
            current['vertices'].append(tuple(int(v,0) for v in m.groups()))
        elif line.startswith(' ubo '):
            m=re.fullmatch(r' ubo c(\d+) addr (0x[0-9a-f]+|0) size (0x[0-9a-f]+|0) offset (\d+) copied (\d+) source (snapshot|snapshot_truncated|unavailable)',line)
            if current is None or not m:raise ValueError('Malformed uniform record')
            slot,addr,size,offset,copied,source=m.groups()
            slot=int(slot);size=int(size,0);offset=int(offset);copied=int(copied)
            if offset>len(binary) or copied>len(binary)-offset or copied>size:
                raise ValueError('Uniform range exceeds captured bytes or binding size')
            if (source=='unavailable' and copied!=0) or (source=='snapshot' and copied!=size):
                raise ValueError('Inconsistent uniform availability')
            current['uniforms'][slot]=dict(addr=int(addr,0),size=size,copied=copied,source=source,
                data=bytes(binary[offset:offset+copied]) if source!='unavailable' else None)
        elif line.startswith(' ssbo '):
            if current is None:raise ValueError('Storage binding without draw')
            current['storage'].append(line.strip())
        elif line.strip():raise ValueError('Unknown object diagnostic record')
    return records

def unique_pairs(records):
    frames=sorted(set(r['frame'] for r in records))
    if len(frames)!=2 or frames[1]!=frames[0]+1:raise ValueError('Expected two consecutive object frames')
    groups=defaultdict(lambda:defaultdict(list))
    for r in records:
        if not r['vertices']:continue
        key=(r['vs'],r['fs'],tuple(r['vertices']),r['size'],r['depth'])
        groups[key][r['frame']].append(r)
    pairs=[];ambiguous=0
    for group in groups.values():
        if all(f in group for f in frames):
            if any(len(group[f])!=1 for f in frames):ambiguous+=1
            else:pairs.append((group[frames[0]][0],group[frames[1]][0]))
    return pairs,ambiguous

def inventory(root,out):
    records=parse_objects((root/'objects.txt').read_text(),(root/'object-uniforms.bin').read_bytes())
    pairs,ambiguous=unique_pairs(records)
    states=Counter(u['source'] for r in records for u in r['uniforms'].values())
    sizes=Counter((slot,u['size']) for r in records for slot,u in r['uniforms'].items())
    changed=Counter();same=Counter();examples=[]
    for a,b in pairs:
        for slot in a['uniforms'].keys()&b['uniforms'].keys():
            first=a['uniforms'][slot];second=b['uniforms'][slot]
            if first['source']!='snapshot' or second['source']!='snapshot' or first['size']!=second['size']:continue
            if first['data']==second['data']:same[slot]+=1
            else:
                changed[slot]+=1
                if slot!=4 and len(examples)<24:
                    examples.append(dict(draws=[a['draw'],b['draw']],vs=a['vs'],fs=a['fs'],slot=slot,size=first['size'],
                        changed_bytes=sum(x!=y for x,y in zip(first['data'],second['data']))))
    result=dict(draws_per_frame=dict(Counter(r['frame'] for r in records)),uniform_availability=dict(states),
        uniform_sizes=[dict(slot=s,size=n,count=c) for (s,n),c in sorted(sizes.items())],
        unique_geometry_candidates=len(pairs),ambiguous_shared_geometry_groups=ambiguous,
        complete_uniform_pairs_changed=dict(changed),complete_uniform_pairs_identical=dict(same),examples=examples,
        limitations=['Geometry bindings do not prove stable object identity.',
            'Duplicate geometry groups are excluded, including potential multiple instances.',
            'Only current submission VS uniform snapshots are captured; GPU-written SSBO contents are not.',
            'Uniforms exceeding the probe cap (4096 bytes in older captures, 8192 in later captures) are explicitly truncated and excluded from full-payload comparisons.',
            'No Link mesh identification, skeleton interpretation or engine motion reconstruction is claimed.'])
    out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
    return result

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('capture',type=Path);parser.add_argument('output',type=Path)
    args=parser.parse_args();inventory(args.capture,args.output)
