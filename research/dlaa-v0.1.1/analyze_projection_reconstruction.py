"""Scoped evidence for recognized graphics camera consumers; NGX remains excluded."""
import argparse,json,re
from pathlib import Path
import numpy as np
from analyze_projection_jitter import validate_records
from projection_metadata import camera_matrices

def reconstruction_scope(records,passes):
    programs={};hashes={}
    for r in records:
        if r.get('reconstruction_probe_mode') is not True or r.get('other_camera_bytes_unchanged') is not True or r.get('program_overflow')!=0:
            raise ValueError('Incomplete reconstruction copy provenance')
        if r['bindings']!=r['vs_bindings']+r['fs_bindings']:raise ValueError('Stage accounting differs')
        for p in r['programs']:
            if 'vs' in p and 'fs' in p:
                pair=(p['vs'],p['fs'])
                if p['id'] in hashes and hashes[p['id']]!=pair:raise ValueError('Shader variants cannot share slot evidence')
                hashes[p['id']]=pair
            a=programs.setdefault(p['id'],[0,0]);a[0]|=p['vs_slot_mask'];a[1]|=p['fs_slot_mask']
    found={};pairs={'91fe9ba82ba38c82':'d88166141a033b44','f93d393cea3eea0b':'8756fd5dd052d836'}
    for vs,fs,p in re.findall(r'vs ([0-9a-f]+) fs ([0-9a-f]+) prog (\d+)',passes):
        slots=programs.get(int(p),[0,0])
        if pairs.get(vs)==fs and (int(p) not in hashes or hashes[int(p)]==(vs,fs)) and slots[0]&(1<<3) and slots[1]&(1<<4):found[vs]=dict(fs=fs,program=int(p))
    if len(found)!=2:raise ValueError('Both known ray programs require VS c3 and FS c4 copies')
    extents=0
    for r in records:extents|=r['extent_mask']
    if extents&7!=7:raise ValueError('Missing native, half or quarter camera consumption')
    return dict(ray_programs=found,extent_mask=extents,vs_bindings=sum(r['vs_bindings'] for r in records),fs_bindings=sum(r['fs_bindings'] for r in records))

def analyze(capture,out,frame):
    records=[json.loads(x) for x in (capture/'projection-jitter.jsonl').read_text().splitlines()]
    result=validate_records(records,frame,require_vs_only=False)
    result.update(reconstruction_scope(records,(capture/'passes.txt').read_text()))
    code=json.loads((capture/'native-exit.json').read_text())['exit_code'];log=(capture/'run.log').read_text(errors='replace')
    if code!=0 or 'vk validation error:' in log or list(capture.glob('ngx-*')):raise ValueError('Native/validation failure or unexpected NGX')
    camera=json.loads((capture/f'gpu-camera-{frame}.json').read_text());raw,_,_,_=camera_matrices(camera)
    for r in records:
        if not np.allclose(r['source_vp'],raw.ravel(),atol=5e-4,rtol=2e-5):raise ValueError('Different main camera copied')
    result.update(native_exit=code,vulkan_errors=0,raw_camera_anchor_matches=True,recognized_reconstruction_only=True)
    out.mkdir(parents=True,exist_ok=True);(out/'projection-reconstruction-analysis.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('output',type=Path);p.add_argument('--frame',type=int,required=True);a=p.parse_args();analyze(a.capture,a.output,a.frame)
