"""One-frame VS descriptor-override evidence; not a complete scene/NGX jitter integration."""
import argparse,json
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw,ImageFont
from temporal_guard import decode_r11g11b10
from projection_metadata import camera_matrices

def validate_records(records,expected_frame,require_vs_only=True,expected_jitter=None):
    if not records or len({r['id'] for r in records})!=len(records):raise ValueError('Missing or duplicate completed clone reports')
    expected_jitter=[.375,-.25] if expected_jitter is None else expected_jitter
    j=np.eye(4);j[0,3]=2*expected_jitter[0]/2560;j[1,3]=-2*expected_jitter[1]/1440;maximum=0
    for r in records:
        if r['frame']!=expected_frame or r['bindings']<1 or not np.allclose(r['jitter'],expected_jitter,atol=2e-8,rtol=0) or r['gpu_completion_observed'] is not True or r['source_snapshot_unchanged_by_copy'] is not True or (require_vs_only and r['vs_only'] is not True) or r['full_scene_jitter_contract'] is not False or r['presented_via_ngx'] is not False:
            raise ValueError('Unexpected frame/lifetime/probe scope')
        for name in ('projection','vp'):
            a=np.array(r['source_'+name]).reshape(4,4);b=np.array(r['bound_'+name]).reshape(4,4)
            if not np.isfinite(a).all() or not np.isfinite(b).all() or abs(np.linalg.det(a))<1e-12:raise ValueError('Invalid source or bound matrix')
            expected=j@a;error=float(np.max(np.abs(b-expected)));maximum=max(maximum,error)
            if not np.allclose(b,expected,atol=2e-6,rtol=2e-6) or not np.array_equal(a[2:],b[2:]):raise ValueError('Projection shift/depth rows differ')
    return dict(frame=expected_frame,completed_constant_buffers=len(records),descriptor_bindings_overridden=sum(r['bindings'] for r in records),max_matrix_component_error=maximum,jitter=[float(x) for x in expected_jitter],game_projection_modified=True,vs_only=all(r['vs_only'] for r in records),full_scene_jitter_contract=False,ngx_evaluated=False,temporal_quality_validated=False)

def analyze(capture,out,frame):
    records=[json.loads(line) for line in (capture/'projection-jitter.jsonl').read_text().splitlines()]
    result=validate_records(records,frame);code=json.loads((capture/'native-exit.json').read_text())['exit_code'];log=(capture/'run.log').read_text(errors='replace')
    if code!=0 or 'vk validation error:' in log or list(capture.glob('ngx-*')) or 'NGX probe:' in log or 'NGX offscreen:' in log:raise ValueError('Native/validation failure or NGX unexpectedly active')
    camera=json.loads((capture/f'gpu-camera-{frame}.json').read_text())
    if camera['active']!=1 or camera.get('projection_probe_mode') is not True:raise ValueError('Missing raw-camera probe annotation')
    raw_current,_,_,_=camera_matrices(camera,legacy_jitter=[.375,-.25])
    for r in records:
        if not np.allclose(r['source_vp'],raw_current.ravel(),atol=5e-4,rtol=2e-5):raise ValueError('Source copy does not match captured main camera')
    result.update(native_exit=code,vulkan_errors=0,raw_camera_anchor_matches=True)
    out.mkdir(parents=True,exist_ok=True);(out/'projection-jitter-analysis.json').write_text(json.dumps(result,indent=2))
    images=[]
    for n in (frame-1,frame):
        data=np.fromfile(capture/f'gpu-hdr-current-{n}-2560x1440.r11g11b10',dtype='<u4')
        if data.size!=2560*1440:raise ValueError('Incomplete HDR capture')
        a=decode_r11g11b10(data.reshape(1440,2560));a=np.maximum(a,0);a=(a/(1+a))**(1/2.2)
        images.append(Image.fromarray(np.rint(a*255).clip(0,255).astype(np.uint8)).resize((640,360)))
    canvas=Image.new('RGB',(1280,410),'#121922');draw=ImageDraw.Draw(canvas);font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',17)
    for i,img in enumerate(images):canvas.paste(img,(640*i,34))
    draw.text((12,8),f'Frame {frame-1}: projection normale',font=font,fill='white');draw.text((652,8),f'Frame {frame}: decalage VS (+.375,-.25) pixels',font=font,fill='white')
    draw.text((12,392),'Instants differents. Reconstruction FS/mouvements non coordonnes; NGX desactive.',fill='#cbd4df')
    canvas.save(out/'projection-jitter-preview.png');print(json.dumps(result,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('output',type=Path);p.add_argument('--frame',type=int,required=True);a=p.parse_args();analyze(a.capture,a.output,a.frame)
