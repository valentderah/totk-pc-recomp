"""Sparse image correspondences independent of dense Farneback estimates."""
import json
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw
from optical import cv2
from sparse_motion import track_features, summarize_tracks
from motion import camera_flow

def main():
    root=Path(__file__).parent
    import sys
    for name in (sys.argv[1:] or ('static-r4','rotation-snapshot','link')):
        out=root/'results'/name;m=json.loads((out/'metrics.json').read_text())
        fields=np.load(out/'motion-diagnostic.npz');size=tuple(m['analysis_resolution'])
        scale=m['source_resolution'][0]/size[0];frames=[]
        for image in m['selected_color']:
            rgb=np.array(Image.open(root/'captures'/name/(image+'_tm.png')).convert('RGB'))
            rgb=cv2.resize(rgb,size,interpolation=cv2.INTER_AREA)
            gray=cv2.cvtColor(rgb,cv2.COLOR_RGB2GRAY)
            frames.append(gray)
        prev,cur=frames;flow=np.nan_to_num(fields['camera_flow'])/scale
        result={}
        cameras=json.loads((out/'cameras.json').read_text())
        depths=[]
        for color in m['selected_color']:
            prefix=color.split('_t0_')[0]
            depth_file=next((root/'captures'/name).glob(prefix+'_td_*_d32f.f32'))
            raw=np.fromfile(depth_file,dtype='<f4').reshape(m['source_resolution'][1],m['source_resolution'][0])
            depths.append(cv2.resize(raw,size,interpolation=cv2.INTER_NEAREST_EXACT))
        alternate,alternate_valid=camera_flow(depths[1],np.array(cameras[str(m['frames'][1])]['vp']),np.array(cameras[str(m['frames'][0])]['vp']),y_down=not m['y_down_selected'],previous_depth=depths[0])
        for mode,initial in [('independent',None)]:
            points,measured=track_features(prev,cur)
            ix=np.rint(points).astype(int);valid=fields['valid'][ix[:,1],ix[:,0]]
            points=points[valid];measured=measured[valid];ix=ix[valid]
            predicted=flow[ix[:,1],ix[:,0]]
            residual=np.linalg.norm(measured-predicted,axis=-1)*scale
            alt=alternate[ix[:,1],ix[:,0]]
            alt_good=alternate_valid[ix[:,1],ix[:,0]]
            wrong_y=np.linalg.norm(measured[alt_good]-alt[alt_good],axis=-1)*scale
            result[mode]=summarize_tracks(points,measured,predicted,scale,size)
            result[mode]['alternate_y_median_pixels']=float(np.median(wrong_y)) if len(wrong_y) else None
            result[mode]['selected_y_common_support_median_pixels']=float(np.median(residual[alt_good])) if alt_good.any() else None
            result[mode]['alternate_orientation_points']=int(alt_good.sum())
            result[mode]['selected_y_down']=m['y_down_selected']
            result[mode]['scope']='sparse textured points only; not DLSS-ready motion'
            if mode=='independent':
                canvas=Image.fromarray(cv2.cvtColor(cur,cv2.COLOR_GRAY2RGB));draw=ImageDraw.Draw(canvas)
                for p,d,r in zip(points,measured,residual):
                    x,y=p;dx,dy=d
                    color='#50ff80' if r<1.5 else '#ff5050'
                    draw.ellipse((x-2,y-2,x+2,y+2),fill=color)
                    draw.line((x,y,x+dx*4,y+dy*4),fill=color,width=1)
                canvas.save(out/'feature-tracks.png')
                np.savez_compressed(out/'feature-tracks.npz',points=points,observed=measured*scale,camera=predicted*scale,residual=residual)
        (out/'feature-check.json').write_text(json.dumps(result,indent=2))
        print(name,json.dumps(result))

if __name__=='__main__':main()
