"""Require a normal live parallel probe, not merely empty loading-screen exports."""
import argparse
import json
from pathlib import Path
import numpy as np

def verdict(*,exit_code,log,current,valid_pixels,finite,bounds_pixels=None):
    errors=[]
    if exit_code!=0:errors.append('Native exit is not zero')
    if 'vk validation error:' in log:errors.append('Vulkan errors')
    if 'vk_rec: the replay\'s Vulkan commands are made by a recorder thread' not in log:errors.append('Parallel recorder not confirmed')
    if not current or current.get('eligible',0)<1 or current.get('drawn',0)<1 or not current.get('history'):errors.append('No live motion draws')
    if valid_pixels<1:errors.append('Empty motion output')
    if not finite:errors.append('Nonfinite accepted motion')
    if bounds_pixels is not None and bounds_pixels!=0:errors.append('Invalid palette accepted pixels')
    return errors

def verify(root,frame,bounds_test):
    log=(root/'run.log').read_text(errors='replace')
    exit_code=json.loads((root/'native-exit.json').read_text())['exit_code']
    rows=[json.loads(s) for s in (root/'gpu-motion-log.jsonl').read_text().splitlines()]
    current=next((r for r in rows if r['frame']==frame),None)
    if not current:raise ValueError('Export frame not present in live journal')
    w,h=current['width'],current['height']
    def load(f):
        data=np.fromfile(root/f'gpu-motion-{f}-{w}x{h}.rgba32f',dtype='<f4')
        if data.size!=w*h*4:raise ValueError('Motion export has wrong size')
        return data.reshape(h,w,4)
    data=load(frame);mask=data[...,3]>.5
    bounds=None
    if bounds_test:
        prior=next((r for r in rows if r['frame']==frame-1),None)
        if not prior or not prior.get('invalid_pose_test') or prior.get('drawn',0)<1:raise ValueError('Empty-palette test did not run on live geometry')
        bounds=int((load(frame-1)[...,3]>.5).sum())
    args=dict(exit_code=exit_code,log=log,current=current,valid_pixels=int(mask.sum()),finite=bool(np.isfinite(data[mask]).all()),bounds_pixels=bounds)
    errors=verdict(**args)
    result=dict(frame=frame,native_exit=exit_code,parallel_recorder='vk_rec: the replay\'s Vulkan commands are made by a recorder thread' in log,validation_errors=log.count('vk validation error:'),
                validation_warnings=log.count('vk validation warning:'),current=current,valid_pixels=args['valid_pixels'],bounds_pixels=bounds,errors=errors,passed=not errors)
    (root/'probe-verdict.json').write_text(json.dumps(result,indent=2))
    print(json.dumps(result,indent=2))
    if errors:raise ValueError('; '.join(errors))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('--frame',type=int,required=True);p.add_argument('--bounds-test',action='store_true')
    a=p.parse_args();verify(a.capture,a.frame,a.bounds_test)
