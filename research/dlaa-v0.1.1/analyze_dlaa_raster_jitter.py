"""Bounded offscreen execution with actual raster offsets, not an image quality approval."""
import argparse,json
from pathlib import Path
import numpy as np
from analyze_dlaa_output import validate_results
from analyze_dlaa_sequence import validate_sequence,validate_image
from analyze_projection_jitter import validate_records
from analyze_projection_reconstruction import reconstruction_scope
from analyze_projection_motion import validate_camera_field,validate_mesh_field
from projection_metadata import camera_matrices

SAMPLES=np.asarray([(0,-1/6),(-.25,1/6),(.25,-7/18),(-.375,-1/18),(.125,5/18),(-.125,-5/18),(.375,1/18),(-.4375,7/18)],dtype=np.float32)
def schedule(frame,end,count=8):
    return SAMPLES[(frame-(end-count+1))%8] if count in (8,32) and end>=count and end-count+1<=frame<=end else np.zeros(2,dtype=np.float32)

def validate_metadata_window(metas,end,count=8):
    if [m['frame'] for m in metas]!=list(range(end-count,end+2)):raise ValueError('Missing bounded raster input/restoration window')
    prior=None
    for m in metas:
        if m.get('projection_probe_mode') is not True or m.get('motion_projection_coordinated') is not True or m['active']!=1 or m['support']<1 or (m['width'],m['height'])!=(2560,1440):raise ValueError('Unsupported raster input')
        f=m['frame']
        if not np.allclose(m['current_jitter'],schedule(f,end,count),atol=2e-8,rtol=0) or not np.allclose(m['previous_jitter'],schedule(f-1,end,count),atol=2e-8,rtol=0):raise ValueError('Raster jitter history differs from actual sequence')
        raw,old,raster,raster_old=camera_matrices(m)
        if prior is not None and (not np.allclose(old,prior[0],atol=5e-5,rtol=2e-6) or not np.allclose(raster_old,prior[1],atol=5e-5,rtol=2e-6)):raise ValueError('Raster/physical history broken')
        prior=raw,raster

def analyze(capture,out,end,count=8):
    load=lambda p:json.loads(p.read_text())
    final=load(capture/'ngx-evaluate.json');host=load(capture/'ngx-evaluate-host.json')
    problems=validate_results(final,host,load(capture/'native-exit.json')['exit_code'],one_shot=False,expected_jitter=SAMPLES[-1])
    if problems or final.get('motion_vectors_jittered') is not True:raise ValueError(str(problems or 'SDK MVJittered flag absent'))
    sdk=[json.loads(x) for x in (capture/'ngx-probe/ngx-sequence-sdk.jsonl').read_text().splitlines()]
    hosts=[load(p) for p in capture.glob('ngx-sequence-host-*.json')]+[host]
    frames=validate_sequence(sdk,hosts,(2560,1440),count=count)
    if frames!=list(range(end-count+1,end+1)) or final['frames']!=count or final['resets']!=1 or final['creates']!=1 or final['reset'] is not False:raise ValueError('SDK evaluated a different raster window')
    offsets=[json.loads(x) for x in (capture/'ngx-probe/ngx-raster-jitter-sdk.jsonl').read_text().splitlines()]
    if len(offsets)!=count:raise ValueError('Missing SDK offsets')
    for i,r in enumerate(offsets):
        if r['index']!=i+1 or r.get('motion_vectors_jittered') is not True or not np.allclose(r['jitter'],SAMPLES[i%8],atol=2e-8,rtol=0):raise ValueError('SDK offsets do not match raster')
    log=(capture/'run.log').read_text(errors='replace')
    if 'vk validation error:' in log:raise ValueError('Vulkan validation errors')
    metas=[load(capture/f'gpu-camera-{f}.json') for f in range(end-count,end+2)];validate_metadata_window(metas,end,count)
    records=[json.loads(x) for x in (capture/'projection-jitter.jsonl').read_text().splitlines()]
    if {r['frame'] for r in records}!=set(frames):raise ValueError('Projection copy frame window differs')
    rows=[]
    for m in metas:
        f=m['frame'];w,h=m['width'],m['height'];row=dict(frame=f,current_jitter=m['current_jitter'],previous_jitter=m['previous_jitter'])
        camera=np.fromfile(capture/f'gpu-camera-{f}-{w}x{h}.rgba32f',dtype='<f4').reshape(h,w,4)
        row.update(validate_camera_field(m,camera));del camera
        skin=np.fromfile(capture/f'gpu-motion-{f}-{w}x{h}.rgba32f',dtype='<f4').reshape(h,w,4);row.update(validate_mesh_field(skin));del skin
        if f in frames:
            r=[x for x in records if x['frame']==f];row['copies']=validate_records(r,f,False,expected_jitter=schedule(f,end,count));row['copies']['ngx_evaluated']=True
            passes='\n'.join(f"vs {p['vs']} fs {p['fs']} prog {p['id']}" for x in r for p in x['programs'])
            row['scope']=reconstruction_scope(r,passes);raw,_,_,_=camera_matrices(m)
            if any(not np.allclose(x['source_vp'],raw.ravel(),atol=5e-4,rtol=2e-5) for x in r):raise ValueError('Raster copy does not match motion camera')
            a=np.fromfile(capture/f'ngx-output-{f}-{w}x{h}.rgba16f',dtype='<f2').reshape(h,w,4).astype(np.float32);validate_image(a)
            row['output_rgb_mean']=float(a[:,:,:3].mean());del a
            mv=np.fromfile(capture/f'gpu-ngx-motion-{f}-{w}x{h}.rg32f',dtype='<f4').reshape(h,w,2)
            composite=np.fromfile(capture/f'gpu-composite-{f}-{w}x{h}.rgba32f',dtype='<f4').reshape(h,w,4)
            if not np.isfinite(mv).all() or not np.isfinite(composite).all() or np.max(np.abs(mv-composite[:,:,:2]))>1e-6:raise ValueError('SDK motion differs from coordinated composite')
            del mv,composite
        rows.append(row)
    result=dict(frames=frames,input_window=[m['frame'] for m in metas],feature_creations=1,resets=1,evaluations=count,raster_jitter_sent_to_ngx=True,native_exit=0,vulkan_errors=0,vulkan_warnings=log.count('vk validation warning:'),sdk_results=final,frame_results=rows,drs_feedback_probe="DRS feedback probe:" in log,performance_qualified=False,presented=False,full_scene_jitter_contract=False,temporal_quality_validated=False)
    out.mkdir(parents=True,exist_ok=True);(out/'dlaa-raster-jitter-analysis.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('output',type=Path);p.add_argument('--frame',type=int,required=True);p.add_argument('--frames',type=int,choices=(8,32),default=8);a=p.parse_args();analyze(a.capture,a.output,a.frame,a.frames)
