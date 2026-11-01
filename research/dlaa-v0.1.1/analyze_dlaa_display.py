"""Verify actual HDR replacement and raw history isolation; this is not a quality verdict."""
import argparse,json
from pathlib import Path
import numpy as np
from temporal_guard import decode_r11g11b10

def validate_conversion(resolved,presented):
    if resolved.shape!=presented.shape or resolved.ndim!=3 or resolved.shape[2]!=3 or not np.isfinite(resolved).all() or not np.isfinite(presented).all():raise ValueError('Invalid display readback')
    reference=np.maximum(resolved,0)
    exponent=np.floor(np.log2(np.maximum(reference,2**-14)))
    quantum=np.exp2(exponent-np.array([6,6,5],dtype=np.float32))
    ulps=np.abs(reference-presented)/quantum
    if float(ulps.max())>1.01:raise ValueError('Displayed HDR does not match format-converted SDK output')
    return dict(max_error_ulps=float(ulps.max()),mean_error_ulps=float(ulps.mean()))

def analyze(capture,out,end,count,preset="L"):
    if preset not in ("K","L"):raise ValueError("Unsupported expected preset")
    expected_preset={"K":11,"L":12}[preset]
    load=lambda p:json.loads(p.read_text())
    if load(capture/'native-exit.json')['exit_code']!=0:raise ValueError('Native process failed')
    log=(capture/'run.log').read_text(errors='replace')
    if 'vk validation error:' in log:raise ValueError('Vulkan validation error')
    hosts=sorted([load(p) for p in capture.glob('ngx-sequence-host-*.json')]+[load(capture/'ngx-evaluate-host.json')],key=lambda r:r['index'])
    if len(hosts)!=count:raise ValueError('Incomplete display sequence')
    for i,row in enumerate(hosts,1):
        if row['index']!=i or row['frame']!=end-count+i or row['record_result']!=1 or row['reset']!=int(i==1) or row['last']!=(i==count) or row['sdk_cleanup']!=int(i==count) or row['gpu_completion_observed'] is not True or row['presented'] is not True:raise ValueError('Display lifecycle differs')
    sdk=load(capture/'ngx-evaluate.json')
    if sdk['frames']!=count or sdk['creates']!=1 or sdk['resets']!=1 or sdk['preset_requested']!=expected_preset or sdk['preset_echo']!=expected_preset or sdk['finished'] is not True or any(sdk[x]!=1 for x in ('init','caps','query','caps_destroy','allocate','create','evaluate','release','destroy','shutdown','preset_query')):raise ValueError('SDK lifecycle/preset mismatch')
    for frame in range(end-count+2,end+1):
        raw=np.fromfile(capture/f'gpu-hdr-current-{frame-1}-2560x1440.r11g11b10',dtype='<u4')
        old=np.fromfile(capture/f'gpu-hdr-previous-{frame}-2560x1440.r11g11b10',dtype='<u4')
        if raw.size!=2560*1440 or not np.array_equal(raw,old):raise ValueError('Resolved output contaminated raw HDR history')
    output=np.fromfile(capture/f'ngx-output-{end}-2560x1440.rgba16f',dtype='<f2').reshape(1440,2560,4)[:,:,:3].astype(np.float32)
    packed=np.fromfile(capture/f'gpu-hdr-presented-{end}-2560x1440.r11g11b10',dtype='<u4').reshape(1440,2560)
    conversion=validate_conversion(output,decode_r11g11b10(packed))
    raw=np.fromfile(capture/f'gpu-hdr-current-{end}-2560x1440.r11g11b10',dtype='<u4').reshape(1440,2560)
    different=int((raw!=packed).sum())
    if different<10000:raise ValueError('Display target did not meaningfully change')
    result=dict(frames=count,preset=preset,hdr_target_replaced_before_hud=True,raw_history_verified_pairs=count-1,conversion=conversion,changed_target_pixels=different,native_exit=0,vulkan_errors=0,vulkan_warnings=log.count('vk validation warning:'),sdk_cleanup=1,continuous=False,quality_validated=False)
    out.mkdir(parents=True,exist_ok=True);(out/'dlaa-display-analysis.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('out',type=Path);p.add_argument('--frame',type=int,required=True);p.add_argument('--frames',type=int,choices=(8,32),default=32);p.add_argument('--preset',choices=('K','L'),default='L');a=p.parse_args();analyze(a.capture,a.out,a.frame,a.frames,a.preset)
