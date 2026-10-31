"""Independent photometric comparison; not a velocity validation."""
import json
from pathlib import Path
import numpy as np
from PIL import Image
from optical import cv2

root=Path(__file__).parent
for name in ('static-r4','rotation-snapshot','link'):
    out=root/'results'/name
    m=json.loads((out/'metrics.json').read_text())
    fields=np.load(out/'motion-diagnostic.npz')
    size=tuple(m['analysis_resolution']);scale=m['source_resolution'][0]/size[0]
    frames=[]
    for image in m['selected_color']:
        rgb=np.array(Image.open(root/'captures'/name/(image+'_tm.png')).convert('RGB'))
        frames.append(cv2.cvtColor(cv2.resize(rgb,size,interpolation=cv2.INTER_AREA),cv2.COLOR_RGB2GRAY).astype(np.float32))
    prev,cur=frames;h,w=cur.shape;yy,xx=np.mgrid[:h,:w].astype(np.float32)
    flow=np.nan_to_num(fields['camera_flow'])/scale
    mask=fields['valid']&fields['confidence']
    # Compare identical pixels, excluding the border for both hypotheses.
    mask[:12]=False;mask[-12:]=False;mask[:,:12]=False;mask[:,-12:]=False
    scores={}
    for label,dx,dy in [('identity',0,0),('camera',flow[...,0],flow[...,1]),('camera_y_flipped',flow[...,0],-flow[...,1])]:
        warped=cv2.remap(prev,xx+dx,yy+dy,cv2.INTER_LINEAR)
        error=np.abs(warped-cur)[mask]
        scores[label]={'mean_abs_8bit':float(error.mean()),'p90_abs_8bit':float(np.percentile(error,90))}
    (out/'photometric-check.json').write_text(json.dumps(scores,indent=2))
    print(name,json.dumps(scores))
