"""Read-only audit of captured camera matrices and static shader access sites.

Static SPIR-V access sites are possible reads, not executed branches or pixel coverage.
"""
import argparse,json,re,subprocess
from pathlib import Path
from collections import Counter
import numpy as np
from objects import parse_objects

def camera_accesses(asm,slot=4):
    roots=set(re.findall(rf'OpName\s+(%\S+)\s+"[vfc]p_c{slot}"',asm))
    constants={m[0]:int(m[1]) for m in re.findall(r'(%\S+)\s*=\s*OpConstant\s+%\S+\s+(-?\d+)\s*$',asm,re.M)}
    chains={};intermediate=set();unsupported=[]
    for line in asm.splitlines():
        m=re.search(r'(%\S+)\s*=\s*(Op(?:InBounds)?AccessChain|OpPtrAccessChain)\s+%\S+\s+(%\S+)(.*)',line)
        if not m:continue
        result,op,base,tail=m.groups();indices=tail.split()
        if base in roots:root=base;prior=[]
        elif base in chains:root,prior,_=chains[base];intermediate.add(base)
        else:continue
        chains[result]=(root,prior+indices,op)
    offsets=set();unresolved=[]
    loaded=set(re.findall(r'OpLoad\s+%\S+\s+(%\S+)',asm))
    for ptr,(root,indices,op) in chains.items():
        if ptr in intermediate and ptr not in loaded:continue
        values=[constants.get(x) for x in indices]
        if op=='OpPtrAccessChain' or len(values) not in (2,3) or None in values or values[0]!=0 or values[1]<0 or (len(values)==3 and not 0<=values[2]<4):
            unresolved.append(dict(pointer=ptr,operation=op,indices=indices));continue
        components=range(4) if len(values)==2 else [values[2]]
        offsets.update(16*values[1]+4*c for c in components)
    for root in roots&loaded:unresolved.append(dict(pointer=root,operation='whole-block load',indices=[]))
    return dict(camera_declared=bool(roots),byte_offsets=sorted(offsets),vec4_rows=sorted({n//16 for n in offsets}),
                unresolved=unresolved,out_of_range=[n for n in sorted(offsets) if n>=0x940])

def matrix_contract(data):
    if len(data)!=0x940:return dict(valid=False,reason='Expected complete 0x940-byte block')
    a=np.frombuffer(data,dtype='<f4').astype(np.float64).reshape(-1,4)
    if not np.isfinite(a[:35]).all():return dict(valid=False,reason='Nonfinite matrix fields')
    errors=[];result={}
    for label,vo,po,vpo in [('current',0,7,3),('previous',21,28,24)]:
        v=np.eye(4);v[:3]=a[vo:vo+3];p=a[po:po+4];vp=a[vpo:vpo+4]
        result[label+'_vp_error']=float(np.max(np.abs(p@v-vp)))
        if abs(np.linalg.det(p))<1e-12 or np.linalg.cond(p)>1e12:errors.append(label+' projection is degenerate')
        if not np.allclose(p@v,vp,atol=5e-4,rtol=2e-5):errors.append(label+' VP differs from P*V')
    v=np.eye(4);v[:3]=a[:3];inv=np.eye(4);inv[:3]=a[11:14]
    result['inverse_view_error']=float(np.max(np.abs(inv@v-np.eye(4))))
    if not np.allclose(inv@v,np.eye(4),atol=5e-4,rtol=2e-5):errors.append('Inverse view differs')
    result.update(valid=not errors,reasons=errors,render_dimensions=a[18,:2].tolist())
    return result

def main_camera_match(data,contract,anchor):
    if not contract['valid'] or anchor is None or anchor.get('active')!=1:return False
    a=np.frombuffer(data,dtype='<f4').reshape(-1,4)
    return bool(np.allclose(a[3:7].ravel(),anchor['current_vp'],rtol=2e-5,atol=5e-4))

def parse_bindings(text,binary):
    records=[]
    for line in text.splitlines():
        r=json.loads(line);offset=r['offset'];copied=r['copied'];size=r['size']
        if not all(type(n) is int and n>=0 for n in (offset,copied,size)) or copied>size or offset>len(binary) or copied>len(binary)-offset or copied>0x940 or r['stage'] not in ('vs','fs','cs'):
            raise ValueError('Invalid camera binding snapshot range/stage')
        r['data']=binary[offset:offset+copied] if copied else None;records.append(r)
    return records

def binding_audit(capture,out,disassembler):
    out.mkdir(parents=True,exist_ok=True);cache=out/'spirv';cache.mkdir(exist_ok=True)
    records=parse_bindings((capture/'camera-bindings.jsonl').read_text(),(capture/'camera-bindings.bin').read_bytes())
    anchors={};shaders={};counts=Counter();stage_counts=Counter();details=[];paired={}
    for r in records:
        frame=r['frame']
        if frame not in anchors:
            path=capture/f'gpu-camera-{frame}.json';anchors[frame]=json.loads(path.read_text()) if path.exists() else None
        contract=matrix_contract(r['data']) if r['data'] is not None else dict(valid=False,reason='Unavailable snapshot')
        main=False
        if contract['valid'] and anchors[frame] is not None:
            main=main_camera_match(r['data'],contract,anchors[frame])
        counts['bindings']+=1;counts['unavailable']+=int(r['data'] is None);counts['truncated']+=int(r['copied']!=r['size']);counts['valid_camera_blocks']+=int(contract['valid']);counts['main_camera_bindings']+=int(main)
        if main:stage_counts[r['stage']]+=1
        sha=r['shader'];slot=r.get('slot',4);key=sha if slot==4 else sha+':c'+str(slot)
        if key not in shaders:
            source=capture/f'sh_{sha}.spv';assembly=cache/(sha+'.spvasm')
            if not assembly.exists():subprocess.run([str(disassembler),str(source),'-o',str(assembly)],check=True,capture_output=True)
            shaders[key]=camera_accesses(assembly.read_text(),slot=slot)
        r['access_key']=key
        if r['stage'] in ('vs','fs') and slot==4:
            paired.setdefault((frame,r['event']),{})[r['stage']]=r
        details.append(dict(**{k:v for k,v in r.items() if k!='data'},matrix_contract=contract,main_camera_match=main))
    groups=[v for v in paired.values() if 'vs' in v and 'fs' in v]
    summary=dict(counts=dict(counts),main_camera_bindings_by_stage=dict(stage_counts),main_camera_bindings_by_resource=dict(Counter(r['stage']+':c'+str(r.get('slot',4)) for r in details if r['main_camera_match'])),frames=sorted(anchors),
        paired_metric_scope='C4 only; other camera-like slots are counted separately by resource',graphics_events_with_both_camera_bindings=len(groups),
        paired_vs_fs_same_address=sum(g['vs']['address']==g['fs']['address'] for g in groups),
        paired_vs_fs_identical_snapshots=sum(g['vs']['data'] is not None and g['vs']['data']==g['fs']['data'] for g in groups),
        main_camera_shader_accesses={stage:dict(shader_count=len({r['shader'] for r in details if r['stage']==stage and r['main_camera_match']}),
            rows=sorted({row for r in details if r['stage']==stage and r['main_camera_match'] for row in shaders[r['access_key']]['vec4_rows']}),
            shaders_with_unresolved_accesses=sorted({r['access_key'] for r in details if r['stage']==stage and r['main_camera_match'] and shaders[r['access_key']]['unresolved']})) for stage in ('vs','fs','cs')},
        limitations=['One scene, two frames. C4 plus explicitly enabled complete 0x940-byte uniform slots; indirect or differently sized camera data remain outside coverage.',
            'Main-camera classification compares captured VP with the GPU camera diagnostic; it is not a persistent object identity.',
            'Static SPIR-V access sites are possible reads, not executed branches or pixel coverage.',
            'No matrix, game projection, NGX jitter or presentation modified by this audit.'])
    (out/'jitter-binding-audit.json').write_text(json.dumps(dict(summary=summary,bindings=details,shaders=shaders),indent=2));print(json.dumps(summary,indent=2))

def audit(capture,out,disassembler):
    out.mkdir(parents=True,exist_ok=True);cache=out/'spirv';cache.mkdir(exist_ok=True)
    records=parse_objects((capture/'objects.txt').read_text(),(capture/'object-uniforms.bin').read_bytes())
    shaders={};draws=[];counts=Counter()
    for r in records:
        u=r['uniforms'].get(4);contract=matrix_contract(u['data']) if u and u['data'] is not None else dict(valid=False,reason='No snapshot c4')
        anchor=capture/f"gpu-camera-{r['frame']}.json";matches=False
        if contract['valid'] and anchor.exists():
            meta=json.loads(anchor.read_text());matches=main_camera_match(u['data'],contract,meta)
        counts['draws']+=1;counts['valid_matrix_blocks']+=int(contract['valid']);counts['main_camera_matches']+=int(matches)
        for stage in ('vs','fs'):
            sha=r[stage]
            if sha not in shaders:
                source=capture/f'sh_{sha}.spv';assembly=cache/(sha+'.spvasm')
                if not assembly.exists():subprocess.run([str(disassembler),str(source),'-o',str(assembly)],check=True,capture_output=True)
                shaders[sha]=camera_accesses(assembly.read_text())
        draws.append(dict(draw=r['draw'],frame=r['frame'],pass_index=r['stage'],dimensions=r['size'],depth=hex(r['depth']),vs=r['vs'],fs=r['fs'],camera_size=u['size'] if u else None,
            matrix_contract=contract,main_camera_match=bool(matches)))
    main=[r for r in draws if r['main_camera_match']]
    summary=dict(counts=dict(counts),frames=sorted({r['frame'] for r in records}),
        main_camera_vertex_shaders=len({r['vs'] for r in main}),main_camera_fragment_shaders=len({r['fs'] for r in main}),
        main_vertex_camera_rows=sorted({row for r in main for row in shaders[r['vs']]['vec4_rows']}),
        main_fragment_camera_rows=sorted({row for r in main for row in shaders[r['fs']]['vec4_rows']}),
        limitations=['Only recorded draws with depth at least 640x360; HUD/no-depth/compute consumers not covered by objects capture.',
            'FS c4 declaration/access alone does not establish its bound address equals the VS c4 snapshot.',
            'Static access sites include possible branches; no dynamic execution or pixel-coverage proof.',
            'No game matrix or NGX jitter modified by this audit.'])
    (out/'jitter-camera-audit.json').write_text(json.dumps(dict(summary=summary,draws=draws,shaders=shaders),indent=2))
    print(json.dumps(summary,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('output',type=Path);p.add_argument('--bindings',action='store_true');p.add_argument('--disassembler',type=Path,default=Path(__file__).parent/'dependencies/shader-tools/ucrt64/bin/spirv-dis.exe');a=p.parse_args();(binding_audit if a.bindings else audit)(a.capture,a.output,a.disassembler)
