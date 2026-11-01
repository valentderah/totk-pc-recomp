"""Prepare exact captured SDK inputs; diagnostic-camera Z is never used as substitute depth."""
import argparse,hashlib,json
from pathlib import Path
import numpy as np
from analyze_dlaa_raster_jitter import validate_metadata_window

def tsv_text(w,h,rows):
    lines=[f'{w} {h} {len(rows)}']
    for r in rows:
        lines.append(f"{r['frame']} {r['jitter'][0]:.9g} {r['jitter'][1]:.9g} {r['frame_time_ms']:.9g} "+' '.join(json.dumps(x['path']) for x in r['inputs']))
    return '\n'.join(lines)+'\n'

def verify_manifest(root):
    data=json.loads((root/'manifest.json').read_text())
    if (data['width'],data['height'],data.get('exact_depth'))!=(2560,1440,True) or len(data['frames']) not in (8,32):raise ValueError('Unsupported replay manifest')
    if (root/'replay.tsv').read_text(encoding='utf-8')!=tsv_text(data['width'],data['height'],data['frames']):raise ValueError('Replay command manifest differs')
    checked={}
    for row in data['frames']:
        for item in row['inputs']+[row['original_output']]:
            fingerprint=(item['bytes'],item['sha256'])
            if item['path'] in checked:
                if checked[item['path']]!=fingerprint:raise ValueError('Conflicting replay fingerprints for one path')
                continue
            p=Path(item['path'])
            if p.stat().st_size!=item['bytes']:raise ValueError('Replay data size changed')
            with p.open('rb') as stream:digest=hashlib.file_digest(stream,'sha256').hexdigest()
            if digest!=item['sha256']:raise ValueError('Replay data changed')
            checked[item['path']]=fingerprint
    return data

def validate_depth(depth,camera,min_valid=10000):
    if depth.ndim!=2 or camera.shape!=(*depth.shape,4) or not np.isfinite(depth).all() or np.min(depth)<0 or np.max(depth)>1:raise ValueError('Invalid exact D32 capture')
    valid=camera[:,:,3]>.5
    if int(valid.sum())<min_valid or not np.array_equal(depth[valid],camera[:,:,2][valid]):raise ValueError('Depth does not match valid shader samples')
    if (~valid).any() and not ((~valid)&(depth!=0)).any():raise ValueError('Complete native D32 cannot be replaced by zeroed diagnostic-camera Z')
    return dict(camera_valid_pixels=int(valid.sum()),depth_outside_valid_camera_pixels=int((~valid).sum()),nonzero_depth_outside_valid_camera_pixels=int(((~valid)&(depth!=0)).sum()))

def prepare(capture,out,end,count=8):
    if out.exists():raise ValueError('Refusing to replace a prepared replay')
    if json.loads((capture/'native-exit.json').read_text())['exit_code']!=0:raise ValueError('Native capture did not finish')
    metas=[json.loads((capture/f'gpu-camera-{f}.json').read_text()) for f in range(end-count,end+2)];validate_metadata_window(metas,end,count)
    w,h=2560,1440;rows=[]
    def file(path,size):
        if path.stat().st_size!=size:raise ValueError('Unexpected input size: '+path.name)
        with path.open('rb') as stream:digest=hashlib.file_digest(stream,'sha256').hexdigest()
        return dict(path=path.resolve().as_posix(),bytes=size,sha256=digest)
    for m in metas[1:-1]:
        f=m['frame'];paths=[capture/f'gpu-hdr-current-{f}-{w}x{h}.r11g11b10',capture/f'gpu-depth-current-{f}-{w}x{h}.d32f',capture/f'gpu-ngx-motion-{f}-{w}x{h}.rg32f',capture/f'gpu-uncertainty-{f}-{w}x{h}.r32f']
        inputs=[file(p,w*h*b) for p,b in zip(paths,(4,4,8,4))]
        depth=np.fromfile(paths[1],dtype='<f4').reshape(h,w);camera=np.fromfile(capture/f'gpu-camera-{f}-{w}x{h}.rgba32f',dtype='<f4').reshape(h,w,4)
        stats=validate_depth(depth,camera);del depth,camera
        row=dict(frame=f,jitter=m['current_jitter'],frame_time_ms=16.666667,inputs=inputs,depth_check=stats,original_output=file(capture/f'ngx-output-{f}-{w}x{h}.rgba16f',w*h*8));rows.append(row)
    out.mkdir(parents=True);(out/'replay.tsv').write_text(tsv_text(w,h,rows),encoding='utf-8');(out/'manifest.json').write_text(json.dumps(dict(width=w,height=h,frames=rows,exact_depth=True,source=capture.resolve().as_posix()),indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('output',type=Path,nargs='?');p.add_argument('--frame',type=int);p.add_argument('--frames',type=int,choices=(8,32),default=8);p.add_argument('--verify',action='store_true');a=p.parse_args()
    if a.verify:verify_manifest(a.capture)
    else:
        if a.output is None or a.frame is None:p.error('Preparation requires output and frame')
        prepare(a.capture,a.output,a.frame,a.frames)
