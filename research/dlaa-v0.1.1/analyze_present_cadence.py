"""CPU-side present cadence, not display latency or a universal performance benchmark."""
import argparse,json,math,re
from pathlib import Path
import numpy as np

def summarize(rows,first):
    if any(b['frame']!=a['frame']+1 or b['ns']<=a['ns'] for a,b in zip(rows,rows[1:])):raise ValueError('Missing or unordered present times')
    rows=[r for r in rows if r['frame']>=first]
    if len(rows)<32:raise ValueError('Insufficient post-warmup presents')
    ms=np.diff([r['ns'] for r in rows])/1e6
    slow=np.sort(ms)[-max(1,math.ceil(len(ms)*.01)):]
    return dict(first_frame=rows[0]['frame'],last_frame=rows[-1]['frame'],intervals=len(ms),seconds=float(ms.sum()/1000),mean_fps=float(1000/ms.mean()),one_percent_low_fps=float(1000/slow.mean()),median_frame_ms=float(np.median(ms)),p99_frame_ms=float(np.percentile(ms,99)),worst_frame_ms=float(ms.max()),over_50ms=int((ms>50).sum()),over_100ms=int((ms>100).sum()))

def analyze(capture,out):
    rows=[json.loads(x) for x in (capture/'frame-present-times.jsonl').read_text().splitlines()]
    log=(capture/'run.log').read_text(errors='replace')
    first=re.search(r'NGX offscreen: frame (\d+) recorded result 1',log)
    if not first or json.loads((capture/'native-exit.json').read_text())['exit_code']!=0:raise ValueError('Incomplete native profile')
    result=summarize(rows,int(first[1])+120)
    result.update(warmup_excluded_frames=120,source=str(capture),metric='CPU-side present callbacks',performance_universally_qualified=False)
    out.mkdir(parents=True,exist_ok=True);(out/'present-cadence.json').write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('out',type=Path);a=p.parse_args();analyze(a.capture,a.out)
