"""Compare live composite/bias outputs with the HDR CPU reference. Not a static-world certificate."""
import argparse,json
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw,ImageFont
from temporal_guard import compose_guard,decode_r11g11b10
from compare_gpu_motion import load_gpu_motion

def provenance_matches(gpu,reference,bias,reference_bias):
    camera_boundary=((gpu==0)&(reference==2))|((gpu==2)&(reference==0))
    rounding=camera_boundary&(np.abs(reference_bias-.5)<=.001)&(np.abs(bias-reference_bias)<=.001)
    return (gpu==reference)|rounding

def compare(root,out,frame):
    out.mkdir(parents=True,exist_ok=True)
    meta=json.loads((root/f'gpu-camera-{frame}.json').read_text());w,h=meta['width'],meta['height']
    code=json.loads((root/'native-exit.json').read_text())['exit_code'];log=(root/'run.log').read_text(errors='replace')
    if code or 'vk validation error:' in log:raise ValueError('Native probe did not complete cleanly')
    camera,cam_mask=load_gpu_motion(root/f'gpu-camera-{frame}-{w}x{h}.rgba32f',w,h)
    skin,skin_mask=load_gpu_motion(root/f'gpu-motion-{frame}-{w}x{h}.rgba32f',w,h)
    def read(stem,suffix,dtype,components):
        a=np.fromfile(root/f'{stem}-{frame}-{w}x{h}.{suffix}',dtype=dtype)
        if a.size!=w*h*components:raise ValueError('Incomplete '+stem)
        return a.reshape((h,w,components) if components>1 else (h,w))
    gpu=read('gpu-composite','rgba32f','<f4',4);bias=read('gpu-uncertainty','r32f','<f4',1)
    if not np.isfinite(gpu).all() or not np.isfinite(bias).all() or (bias<0).any() or (bias>1).any():raise ValueError('Invalid guard output')
    if not np.isin(gpu[...,3],[0,1,2]).all():raise ValueError('Unexpected provenance')
    current=decode_r11g11b10(read('gpu-hdr-current','r11g11b10','<u4',1))
    history=bool(meta['active'] and meta['color_history_valid'])
    previous=decode_r11g11b10(read('gpu-hdr-previous','r11g11b10','<u4',1)) if meta['color_history_valid'] else np.zeros_like(current)
    test_box=np.zeros((h,w),bool);base_count=None
    if meta['color_discontinuity_test']:
        yy,xx=np.mgrid[:h,:w];test_box=((xx+.5)/w>.1)&((xx+.5)/w<.2)&((yy+.5)/h>.1)&((yy+.5)/h<.2)
        baseline,_=compose_guard(camera,skin,current,previous,history=history)
        base_count=int((baseline[...,3]>0).sum());test_eligible=test_box&(baseline[...,3]==2)
        current=current.copy();current[test_box]+=1
    else:test_eligible=test_box
    expected,reference_bias=compose_guard(camera,skin,current,previous,history=history)
    mismatch=gpu[...,3]!=expected[...,3];common=(gpu[...,3]>0)&(expected[...,3]>0)
    residual=np.abs(bias-reference_bias);velocity_error=np.max(np.abs(gpu[...,:3][common]-expected[...,:3][common])) if common.any() else 0
    if not provenance_matches(gpu[...,3],expected[...,3],bias,reference_bias).all():raise ValueError('Composite provenance differs beyond camera-threshold rounding')
    if np.percentile(residual,99.9)>.001 or velocity_error>.001:raise ValueError('Guard GPU/CPU numerical mismatch')
    invalid_surface=skin[...,3]<0;fallback=int(((gpu[...,3]>0)&invalid_surface).sum())
    if fallback:raise ValueError('Invalid known surface fell back to camera')
    test_rejected=int((test_eligible&(gpu[...,3]==0)).sum())
    if meta['color_discontinuity_test'] and history and test_rejected<100:raise ValueError('Color-discontinuity test had insufficient new rejections')
    camera_only=cam_mask&~skin_mask&~invalid_surface
    summary=dict(frame=frame,native_exit=code,history_valid=history,camera_candidate_pixels=int(cam_mask.sum()),known_valid_motion_pixels=int(skin_mask.sum()),
        known_invalid_surface_pixels=int(invalid_surface.sum()),invalid_surface_fallback_pixels=fallback,
        composite_known_pixels=int((gpu[...,3]==1).sum()),composite_camera_candidate_pixels=int((gpu[...,3]==2).sum()),composite_invalid_pixels=int((gpu[...,3]==0).sum()),
        camera_candidates_after_surface_exclusion=int(camera_only.sum()),camera_candidates_rejected_by_guard=int((camera_only&(gpu[...,3]==0)).sum()),
        bias_over_half_pixels=int((bias>=.5).sum()),bias_error_median=float(np.median(residual)),bias_error_p99=float(np.percentile(residual,99)),bias_error_max=float(residual.max()),
        provenance_disagreement_pixels=int(mismatch.sum()),provenance_disagreement_max_distance_to_half=float(np.max(np.abs(reference_bias[mismatch]-.5))) if mismatch.any() else 0,
        motion_component_error_max=float(velocity_error),
        color_test_enabled=bool(meta['color_discontinuity_test']),color_test_baseline_eligible_pixels=int(test_eligible.sum()),color_test_new_rejected_pixels=test_rejected,
        baseline_retained_pixels=base_count,suitable_for_dlss_input=False,
        limitations=['Low color residual is not proof of static geometry: uniform moving surfaces can escape detection.',
            'Lighting, animated materials and exposure changes can cause conservative rejection.',
            'Known-family coverage does not include every dynamic object; unsupported current geometry remains unknown.',
            'No NGX evaluation, jitter integration or complete identity/reset contract.'])
    (out/f'temporal-guard-{frame}.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary,indent=2))
    display=np.clip(np.nan_to_num(current)/(1+np.nan_to_num(current)),0,1)**(1/2.2);rgb=np.rint(display*255).astype(np.uint8)
    overlay=rgb.copy();risk=bias[...,None]*.8;overlay=(overlay*(1-risk)+np.array([235,35,35])*risk).astype(np.uint8)
    canvas=Image.new('RGB',(1280,440),'#121922');draw=ImageDraw.Draw(canvas);font=ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',18)
    draw.text((12,10),'HDR du diagnostic (compression pour affichage)',font=font,fill='white');draw.text((652,10),'Rouge : historique incertain ou invalide',font=font,fill='white')
    canvas.paste(Image.fromarray(rgb).resize((640,360)),(0,42));canvas.paste(Image.fromarray(overlay).resize((640,360)),(640,42))
    draw.text((12,409),'Masque heuristique. Une zone non rouge ne prouve pas que son mouvement est correct. DLAA absent.',font=font,fill='#cbd4df')
    canvas.save(out/f'temporal-guard-{frame}.png')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('output',type=Path);p.add_argument('--frame',type=int,required=True);a=p.parse_args();compare(a.capture,a.output,a.frame)
