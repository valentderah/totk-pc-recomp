"""Characterize changing c5 arrays without claiming a skeleton interpretation."""
import json
from pathlib import Path
import hashlib
import numpy as np
from objects import parse_objects,unique_pairs

root=Path(__file__).parent/'captures'/'link-objects'
records=parse_objects((root/'objects.txt').read_text(),(root/'object-uniforms.bin').read_bytes())
pairs,_=unique_pairs(records);candidates={}
for a,b in pairs:
    u=a['uniforms'].get(5);v=b['uniforms'].get(5)
    if not u or not v or u['source']!='snapshot' or v['source']!='snapshot':continue
    if u['size']!=v['size'] or u['size']%48 or u['size']<1000 or u['data']==v['data']:continue
    x=np.frombuffer(u['data'],dtype='<f4').reshape(-1,3,4)
    y=np.frombuffer(v['data'],dtype='<f4').reshape(-1,3,4)
    if not np.isfinite(x).all() or not np.isfinite(y).all():continue
    rotation=x[:,:,:3];other_rotation=y[:,:,:3]
    orth=np.max(abs(rotation@rotation.transpose(0,2,1)-np.eye(3)),axis=(1,2))
    orth_previous=np.max(abs(other_rotation@other_rotation.transpose(0,2,1)-np.eye(3)),axis=(1,2))
    key=hashlib.sha256(u['data']+v['data']).hexdigest()
    if key not in candidates:
        candidates[key]=dict(matrix_count=len(x),bytes=u['size'],slot=5,
            orthonormal_fraction_first=float(np.mean(orth<.01)),
            orthonormal_fraction_second=float(np.mean(orth_previous<.01)),
            positive_unit_determinant_fraction=float(np.mean(abs(np.linalg.det(rotation)-1)<.01)),
            changed_matrices=int(np.sum(np.max(abs(x-y),axis=(1,2))>.0001)),draw_pairs=[],vertex_shaders=[])
    candidates[key]['draw_pairs'].append([a['draw'],b['draw']])
    if a['vs'] not in candidates[key]['vertex_shaders']:candidates[key]['vertex_shaders'].append(a['vs'])
result=dict(candidates=list(candidates.values()),
    interpretation='Changing arrays consistent with rigid 3x4 transform palettes. Semantic role unproven.',
    limitations=['Vertex blend indices/weights and shader use have not been decoded.',
        'No definite identification of Link, bone names, bind poses or vertex trajectories.',
        'Only unique geometry candidates and complete snapshots are included.'])
out=Path(__file__).parent/'results'/'link-objects'/'pose-candidates.json'
out.write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
