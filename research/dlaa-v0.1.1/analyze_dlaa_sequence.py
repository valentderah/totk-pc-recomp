"""Bounded history execution evidence. No temporal quality or presentation claim."""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw
from analyze_dlaa_output import validate_results

def validate_image(a):
    rgb=a[:,:,:3]
    if not np.isfinite(a).all() or np.std(rgb)<1e-5 or np.percentile(rgb,99)<=0:
        raise ValueError('Invalid or empty GPU output')

def validate_sequence(sdk,host,dimensions=None,count=8):
    host=sorted(host,key=lambda h:h['index'])
    if count not in (8,32) or len(sdk)!=count or len(host)!=count:raise ValueError('Expected exact bounded SDK and completed host frames')
    frames=[h['frame'] for h in host]
    if frames!=list(range(frames[0],frames[0]+count)):raise ValueError('Sequence has a frame gap')
    for i,(s,h) in enumerate(zip(sdk,host),1):
        if dimensions is not None and (h.get('width'),h.get('height'))!=dimensions:
            raise ValueError('Host dimensions differ from SDK')
        reset=int(i==1)
        if s!={'index':i,'reset':reset,'evaluate':1,'creates':1}:raise ValueError('SDK history sequence failed')
        if h['index']!=i or h['reset']!=reset or h['last']!=(i==count) or h['record_result']!=1 or h['gpu_completion_observed'] is not True or h['sdk_cleanup']!=int(i==count) or h['presented'] is not False:
            raise ValueError('Host completion/lifecycle failed')
    return frames

def analyze(capture,out):
    out.mkdir(parents=True,exist_ok=True)
    load=lambda p:json.loads(p.read_text())
    final=load(capture/'ngx-evaluate.json');host=load(capture/'ngx-evaluate-host.json')
    problems=validate_results(final,host,load(capture/'native-exit.json')['exit_code'],one_shot=False)
    if problems:raise ValueError('; '.join(problems))
    sdk=[json.loads(s) for s in (capture/'ngx-probe/ngx-sequence-sdk.jsonl').read_text().splitlines()]
    hosts=[load(p) for p in capture.glob('ngx-sequence-host-*.json')]+[host]
    frames=validate_sequence(sdk,hosts,(final['width'],final['height']))
    if final['frames']!=8 or final['resets']!=1 or final['creates']!=1 or final['reset'] is not False:raise ValueError('Final SDK counters differ')
    log=(capture/'run.log').read_text(errors='replace')
    # Same validation log spelling as the one-shot analyzer.
    errors=log.count('vk validation error:')
    if errors:raise ValueError('Vulkan errors')
    w,h=final['width'],final['height'];stats=[];previews=[]
    for f in frames:
        path=capture/f'ngx-output-{f}-{w}x{h}.rgba16f'
        if path.stat().st_size!=w*h*8:raise ValueError('Output size mismatch')
        a=np.fromfile(path,dtype='<f2').reshape(h,w,4).astype(np.float32);rgb=a[:,:,:3]
        validate_image(a)
        stats.append(dict(frame=f,bytes=path.stat().st_size,rgb_min=float(rgb.min()),rgb_max=float(rgb.max()),rgb_mean=float(rgb.mean()),finite_fraction=1.0))
        if f in (frames[0],frames[-1]):
            x=np.maximum(rgb,0);x=(x/(1+x))**(1/2.2)
            previews.append(Image.fromarray(np.rint(x*255).clip(0,255).astype('uint8')).resize((640,360)))
    result=dict(frames=frames,feature_creations=1,evaluations=8,resets=1,native_exit=0,complete_gpu_outputs=stats,sdk_results=final,vulkan_errors=errors,presented=False,jitter=[0,0],temporal_quality_validated=False)
    (out/'dlaa-sequence-analysis.json').write_text(json.dumps(result,indent=2))
    canvas=Image.new('RGB',(1280,405),'#121922');canvas.paste(previews[0],(0,35));canvas.paste(previews[1],(640,35))
    draw=ImageDraw.Draw(canvas);draw.text((12,10),f'DLAA frame {frames[0]} - reset=1',fill='white');draw.text((652,10),f'DLAA frame {frames[-1]} - reset=0, jitter=0 (quality unvalidated)',fill='white')
    canvas.save(out/'dlaa-sequence-preview.png');print(json.dumps(result,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('output',type=Path);a=p.parse_args();analyze(a.capture,a.output)
