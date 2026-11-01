"""Same-frame source/output contact sheet with identical display conversion."""
import argparse,json
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw
from temporal_guard import decode_r11g11b10

def preview(capture,out,frame):
    w,h=2560,1440
    source=decode_r11g11b10(np.fromfile(capture/f'gpu-hdr-current-{frame}-{w}x{h}.r11g11b10',dtype='<u4').reshape(h,w))
    result=np.fromfile(capture/f'ngx-output-{frame}-{w}x{h}.rgba16f',dtype='<f2').reshape(h,w,4)[:,:,:3].astype(np.float32)
    def display(a):
        a=np.maximum(a,0);return Image.fromarray(np.rint((a/(1+a))**(1/2.2)*255).clip(0,255).astype('uint8'))
    images=[display(source),display(result)];canvas=Image.new('RGB',(1280,770),'#121922');d=ImageDraw.Draw(canvas)
    for i,im in enumerate(images):
        canvas.paste(im.resize((640,360)),(i*640,28))
        # Fixed central stone/door region, nearest-neighbor enlargement preserves pixel structure.
        canvas.paste(im.crop((1024,480,1344,640)).resize((640,320),Image.Resampling.NEAREST),(i*640,412))
    count=json.loads((capture/'ngx-evaluate.json').read_text())['frames']
    d.text((12,8),f'Image avant DLAA - instant {frame}',fill='white');d.text((652,8),f'Capture DLAA - historique de {count} images',fill='white')
    d.text((12,392),'Conversion identique. Detail agrandi x2 sans filtre. Gain de nettete non valide.',fill='#cbd4df')
    d.text((12,746),'Essai en capture separee. Le jeu habituel conserve son affichage.',fill='#cbd4df')
    out.mkdir(parents=True,exist_ok=True);canvas.save(out/'dlaa-raster-jitter-preview.png')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('output',type=Path);p.add_argument('--frame',type=int,required=True);a=p.parse_args();preview(a.capture,a.output,a.frame)
