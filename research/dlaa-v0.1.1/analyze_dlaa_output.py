"""Validate one-shot SDK execution/readback. Image difference is not an AA quality metric."""
import argparse,json
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw,ImageFont
from temporal_guard import decode_r11g11b10

def validate_results(sdk,host,exit_code,one_shot=True,expected_jitter=None):
    errors=[]
    if exit_code!=0:errors.append('Native exit not zero')
    for field in ('init','caps','query','caps_destroy','allocate','create','evaluate','release','destroy','shutdown'):
        if sdk.get(field)!=1:errors.append('SDK '+field+' not successful')
    if sdk.get('available')!=1 or sdk.get('finished') is not True:errors.append('SDK not available or unfinished')
    if host.get('record_result')!=1 or host.get('sdk_cleanup')!=1:errors.append('Host recording or cleanup failed')
    if host.get('gpu_completion_observed') is not True:errors.append('GPU completion missing')
    if sdk.get('presented') is not False or host.get('presented') is not False:errors.append('Unexpected presentation')
    if sdk.get('temporal_quality_validated') is not False:errors.append('Unexpected quality claim')
    if not np.allclose(sdk.get('jitter',[float('nan')]*2),[0,0] if expected_jitter is None else expected_jitter,atol=2e-8,rtol=0) or (one_shot and sdk.get('reset') is not True):errors.append('Unexpected one-shot contract')
    return errors

def analyze(root,out):
    out.mkdir(parents=True,exist_ok=True)
    sdk=json.loads((root/'ngx-evaluate.json').read_text());host=json.loads((root/'ngx-evaluate-host.json').read_text());exit_code=json.loads((root/'native-exit.json').read_text())['exit_code']
    errors=validate_results(sdk,host,exit_code);log=(root/'run.log').read_text(errors='replace')
    if 'vk validation error:' in log:errors.append('Vulkan validation errors')
    if errors:raise ValueError('; '.join(errors))
    w,h=sdk['width'],sdk['height'];frame=host['frame']
    def read(stem,suffix,dtype,components):
        a=np.fromfile(root/f'{stem}-{frame}-{w}x{h}.{suffix}',dtype=dtype)
        if a.size!=w*h*components:raise ValueError('Wrong export size: '+stem)
        return a.reshape(h,w,components) if components>1 else a.reshape(h,w)
    output=read('ngx-output','rgba16f','<f2',4).astype(np.float32);rgb=output[...,:3]
    if not np.isfinite(rgb).all() or float(rgb.std())<1e-5 or float(np.percentile(rgb,99))<=0:raise ValueError('SDK output is not a finite nonempty scene')
    source=decode_r11g11b10(read('gpu-hdr-current','r11g11b10','<u4',1))
    if not np.isfinite(source).all():raise ValueError('Nonfinite input HDR')
    mv=read('gpu-ngx-motion','rg32f','<f4',2);composite=read('gpu-composite','rgba32f','<f4',4)
    motion_error=float(np.max(np.abs(mv-composite[...,:2])))
    if not np.isfinite(mv).all() or motion_error>1e-6:raise ValueError('NGX motion export disagrees with composite')
    delta=np.abs(rgb-source)
    result=dict(frame=frame,resolution=[w,h],native_exit=exit_code,sdk_results=sdk,host_results=host,
        vulkan_errors=log.count('vk validation error:'),vulkan_warnings=log.count('vk validation warning:'),
        output_rgb_finite_fraction=float(np.isfinite(rgb).mean()),output_rgb_min=float(rgb.min()),output_rgb_max=float(rgb.max()),output_rgb_mean=float(rgb.mean()),
        output_rgb_negative_fraction=float((rgb<0).mean()),output_alpha_finite_fraction=float(np.isfinite(output[...,3]).mean()),
        input_output_mean_absolute_difference=float(delta.mean()),input_output_p99_absolute_difference=float(np.percentile(delta,99)),
        ngx_motion_composite_component_error_max=motion_error,evaluation_completed=True,presented=False,temporal_quality_validated=False,
        limitations=['One evaluated frame with reset=1 and jitter=0; no temporal anti-aliasing quality validation.',
            'Input motion and uncertainty retain previously documented candidate/identity limitations.',
            'Linear HDR differences and the shared display compression do not measure sharpness improvement.',
            'No gameplay presentation, resizing, ongoing history or performance benchmark.'])
    (out/'dlaa-output-analysis.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))
    def display(a):return np.rint((np.maximum(a,0)/(1+np.maximum(a,0)))**(1/2.2)*255).clip(0,255).astype(np.uint8)
    canvas=Image.new('RGB',(1280,440),'#121922');draw=ImageDraw.Draw(canvas);font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',19)
    draw.text((12,10),"HDR d'entree",font=font,fill='white');draw.text((652,10),'Sortie NGX — essai unique DLAA',font=font,fill='white')
    canvas.paste(Image.fromarray(display(source)).resize((640,360)),(0,42));canvas.paste(Image.fromarray(display(rgb)).resize((640,360)),(640,42))
    draw.text((12,409),'Compression identique. Reset=1, jitter=0. Aucun gain de nettete temporelle valide.',font=font,fill='#cbd4df')
    canvas.save(out/'dlaa-output-comparison.png')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('output',type=Path);a=p.parse_args();analyze(a.capture,a.output)
