"""Validate synthetic GPU raster jitter. This does not validate jitter applied to TotK."""
import argparse,json
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw

def validate_case(case,a):
    h,w,components=a.shape
    if components!=4 or not np.isfinite(a).all():raise ValueError('Nonfinite/invalid calibration image')
    vertices=np.array([[-.625,-.5,-.5],[.75,-.5,0],[-.25,.75,.5]])
    xy=np.column_stack(((vertices[:,0]+1)*w/2,(1-vertices[:,1])*h/2))+case['current']
    yy,xx=np.mgrid[:h,:w]
    bary=np.linalg.solve(np.vstack((xy.T,np.ones(3))),np.stack((xx+.5,yy+.5,np.ones_like(xx))).reshape(3,-1)).reshape(3,h,w)
    core=np.min(bary,axis=0)>1e-4;outside=np.min(bary,axis=0)<-1e-4;mask=a[:,:,3]>.5
    if np.count_nonzero(mask)<100 or not mask[core].all() or mask[outside].any():raise ValueError('Raster coverage differs from jittered geometry')
    expected=np.array(case['previous'])-case['current'];motion_error=float(np.max(np.abs(a[mask,:2]-expected)))
    depth=.5*(np.sum(bary*vertices[:,2,None,None],axis=0)+1);depth_error=float(np.max(np.abs(a[mask,2]-depth[mask])))
    if motion_error>1e-4 or depth_error>2e-5:raise ValueError(f'Calibration mismatch: motion {motion_error}, depth {depth_error}')
    return dict(current=case['current'],previous=case['previous'],expected_motion=expected.tolist(),covered_pixels=int(mask.sum()),max_motion_error=motion_error,max_depth_error=depth_error)

def analyze(capture,out):
    meta=json.loads((capture/'jitter-calibration.json').read_text());exit_code=json.loads((capture/'native-exit.json').read_text())['exit_code'];log=(capture/'run.log').read_text(errors='replace')
    if exit_code!=0 or meta['gpu_completion_observed'] is not True or meta['game_projection_modified'] is not False or meta['presented'] is not False or 'vk validation error:' in log:raise ValueError('Incomplete/invalid synthetic probe')
    expected=[([0,0],[0,0]),([.375,0],[-.125,0]),([0,-.25],[0,.25]),([.375,-.25],[-.125,.25])]
    if meta.get('geometry_version')!=2:raise ValueError('Expected pixel-aligned calibration geometry version 2')
    if meta['width']!=128 or meta['height']!=96 or len(meta['cases'])!=4:raise ValueError('Unexpected calibration contract')
    results=[];images=[]
    for i,(case,(now,old)) in enumerate(zip(meta['cases'],expected)):
        if case!={'index':i,'current':now,'previous':old}:raise ValueError('Case differs from prescribed offsets')
        data=np.fromfile(capture/f'jitter-calibration-{i}.rgba32f',dtype='<f4')
        if data.size!=128*96*4:raise ValueError('Incomplete GPU readback')
        a=data.reshape(96,128,4);results.append(validate_case(case,a))
        rgb=np.zeros((96,128,3),np.uint8);rgb[a[:,:,3]>.5]=[80,190,220];images.append(Image.fromarray(rgb).resize((384,288),Image.Resampling.NEAREST))
    result=dict(native_exit=exit_code,frame=meta['frame'],gpu_completion_observed=True,cases=results,vulkan_errors=0,vulkan_validation_enabled=meta['vulkan_validation_enabled'],game_projection_modified=False,ngx_jitter_modified=False,presented=False,temporal_quality_validated=False)
    out.mkdir(parents=True,exist_ok=True);(out/'jitter-gpu-analysis.json').write_text(json.dumps(result,indent=2))
    canvas=Image.new('RGB',(768,640),'#121922');draw=ImageDraw.Draw(canvas)
    for i,img in enumerate(images):
        x=i%2*384;y=i//2*320;canvas.paste(img,(x,y+30));draw.text((x+8,y+8),f"Case {i}: current offset {meta['cases'][i]['current']}",fill='white')
    canvas.save(out/'jitter-gpu-preview.png');print(json.dumps(result,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('output',type=Path);a=p.parse_args();analyze(a.capture,a.output)
