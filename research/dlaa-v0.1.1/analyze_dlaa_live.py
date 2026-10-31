"""Compact continuous-session lifecycle evidence, independent of a launcher exit code."""
import argparse,json
from pathlib import Path

def analyze(capture,out,preset="L"):
    if preset not in ("K","L"):raise ValueError("Unsupported expected preset")
    expected_preset={"K":11,"L":12}[preset]
    load=lambda name:json.loads((capture/name).read_text())
    log=(capture/'run.log').read_text(errors='replace');done=load('ngx-live-shutdown.json');host=load('ngx-live-frame.json');sdk=load('ngx-evaluate.json')
    if load('native-exit.json')['exit_code']!=0 or 'vk validation error:' in log or (capture/'ngx-live-failure.json').exists():raise ValueError('Live process, Vulkan or SDK recording failed')
    count=done['evaluations']
    if count<32 or done['sdk_cleanup']!=1 or done['gpu_completion_observed'] is not True or done['continuous'] is not True:raise ValueError('Continuous shutdown incomplete')
    if host['index']!=count or host['record_result']!=1 or host['presented'] is not True or host['gpu_completion_observed'] is not True or (host['width'],host['height'])!=(2560,1440):raise ValueError('Final display completion differs')
    if sdk['frames']!=count or sdk['creates']!=1 or sdk['resets']<1 or sdk['finished'] is not True or sdk['preset_requested']!=expected_preset or sdk['preset_echo']!=expected_preset or any(sdk[x]!=1 for x in ('init','caps','query','caps_destroy','allocate','create','evaluate','release','destroy','shutdown','preset_query')):raise ValueError('Live SDK lifecycle/preset failed')
    events=[json.loads(x) for x in (capture/'ngx-probe/ngx-sequence-sdk.jsonl').read_text().splitlines()]
    offsets=[json.loads(x) for x in (capture/'ngx-probe/ngx-raster-jitter-sdk.jsonl').read_text().splitlines()]
    if len(events)!=count or len(offsets)!=count or sum(x['reset'] for x in events)!=sdk['resets']:raise ValueError('Continuous event counts differ')
    for i,(event,offset) in enumerate(zip(events,offsets),1):
        if event['index']!=i or event['evaluate']!=1 or event['creates']!=1 or offset['index']!=i or offset['motion_vectors_jittered'] is not True:raise ValueError('Incomplete continuous evaluation evidence')
    result=dict(evaluations=count,last_native_frame=host['frame'],preset=preset,native_exit=0,vulkan_errors=0,vulkan_warnings=log.count('vk validation warning:'),sdk_cleanup=1,feature_creations=1,resets=sdk['resets'],continuous=True,final_hdr_replacement_completed=True,bulk_readbacks=False,quality_validated=False,performance_qualified=False)
    out.mkdir(parents=True,exist_ok=True);(out/'dlaa-live-analysis.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('out',type=Path);p.add_argument('--preset',choices=('K','L'),default='L');a=p.parse_args();analyze(a.capture,a.out,a.preset)
