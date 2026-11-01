"""Same-input image comparison; numerical differences alone do not certify sharpness."""
import argparse,hashlib,json
from pathlib import Path
import numpy as np
from PIL import Image,ImageDraw
from temporal_guard import decode_r11g11b10
from prepare_dlaa_replay import verify_manifest

def validate_receipts(inputs,roots):
    expected={}
    for file,key in (('manifest.json','manifest_json_sha256'),('replay.tsv','manifest_tsv_sha256')):
        expected[key]=hashlib.sha256((inputs/file).read_bytes()).hexdigest().upper()
    reference=None;receipts=[]
    for root,preset in zip(roots,(0,11,12)):
        r=json.loads((root/'input-receipt.json').read_text())
        if r.get('preset_requested')!=preset or r.get('verified_before_and_after_gpu_completion') is not True or r.get('runtime_signed_nvidia') is not True:raise ValueError('Replay provenance unconfirmed')
        if any(r.get(k,'').upper()!=v for k,v in expected.items()):raise ValueError('Replay did not use supplied command/input manifests')
        binaries=(r['bridge_sha256'].upper(),r['runtime_sha256'].upper())
        if reference is not None and reference!=binaries:raise ValueError('Different bridge/runtime used in preset comparison')
        reference=binaries;receipts.append(r)
    return receipts

def compare(inputs,default,k,l,out):
    manifest=verify_manifest(inputs);receipts=validate_receipts(inputs,(default,k,l));stats=[];count=len(manifest['frames'])
    for row in manifest['frames']:
        f=row['frame'];name=f'ngx-output-{f}-2560x1440.rgba16f';arrays=[]
        for root,preset in ((default,0),(k,11),(l,12)):
            summary=json.loads((root/'replay-summary.json').read_text());sdk=json.loads((root/'ngx-evaluate.json').read_text())
            if summary['validation_errors']!=0 or summary['frames']!=count or summary['sdk_cleanup']!=1 or summary['presented'] is not False or sdk['preset_requested']!=preset or sdk['preset_echo']!=preset:raise ValueError('Replay lifecycle/preset differs')
            if any(sdk[x]!=1 for x in ('init','caps','query','caps_destroy','allocate','create','evaluate','release','destroy','shutdown','preset_query')):raise ValueError('SDK status failed')
            if sdk['frames']!=count or sdk['resets']!=1 or sdk['creates']!=1 or sdk['finished'] is not True or sdk['reset'] is not False:raise ValueError('SDK history/lifecycle differs')
            events=[json.loads(x) for x in (root/'replay-frames.jsonl').read_text().splitlines()];offsets=[json.loads(x) for x in (root/'ngx-raster-jitter-sdk.jsonl').read_text().splitlines()]
            if len(events)!=count or len(offsets)!=count:raise ValueError('Incomplete replay frame evidence')
            for index,(event,offset,m) in enumerate(zip(events,offsets,manifest['frames']),1):
                if event!={'index':index,'frame':m['frame'],'reset':int(index==1),'gpu_completion_observed':True} or offset['index']!=index or offset['motion_vectors_jittered'] is not True or not np.allclose(offset['jitter'],m['jitter'],atol=2e-8,rtol=0):raise ValueError('Different frame/jitter replayed')
            a=np.fromfile(root/name,dtype='<f2').reshape(1440,2560,4)
            if not np.isfinite(a).all():raise ValueError('Nonfinite replay image')
            arrays.append(a)
        original=np.fromfile(row['original_output']['path'],dtype='<f2').reshape(1440,2560,4)
        if not np.array_equal(original.view('<u2'),arrays[0].view('<u2')):raise ValueError('Replay does not reproduce original output exactly')
        diff=np.abs(arrays[1][:,:,:3].astype(np.float32)-arrays[2][:,:,:3].astype(np.float32))
        stats.append(dict(frame=f,default_replay_matches_original_exactly=True,default_equals_requested_k=bool(np.array_equal(arrays[0],arrays[1])),k_equals_l=bool(np.array_equal(arrays[1],arrays[2])),k_l_rgb_mean_abs_difference=float(diff.mean()),k_l_rgb_p99_abs_difference=float(np.percentile(diff,99))))
    source=decode_r11g11b10(np.fromfile(manifest['frames'][-1]['inputs'][0]['path'],dtype='<u4').reshape(1440,2560))
    rgb=[source,arrays[1][:,:,:3].astype(np.float32),arrays[2][:,:,:3].astype(np.float32)]
    def display(a):
        a=np.maximum(a,0);return Image.fromarray(np.rint((a/(1+a))**(1/2.2)*255).clip(0,255).astype('uint8'))
    canvas=Image.new('RGB',(1920,1100),'#121922');draw=ImageDraw.Draw(canvas)
    for i,(a,label) in enumerate(zip(rgb,('Avant DLAA','DLAA - profil K','DLAA - profil L'))):
        im=display(a);x=i*640;draw.text((x+12,8),label,fill='white');canvas.paste(im.resize((640,360)),(x,28))
        canvas.paste(im.crop((1024,480,1344,640)).resize((640,320),Image.Resampling.NEAREST),(x,416))
        canvas.paste(im.crop((1152,768,1472,928)).resize((640,320),Image.Resampling.NEAREST),(x,764))
    draw.text((12,394),'Detail du decor - agrandissement x2 sans filtre',fill='#cbd4df');draw.text((12,742),'Detail du personnage - agrandissement x2 sans filtre',fill='#cbd4df')
    draw.text((12,1080),'Memes entrees GPU et meme conversion d affichage. Aucun changement applique au jeu habituel.',fill='#cbd4df')
    out.mkdir(parents=True,exist_ok=True);canvas.save(out/'preset-comparison.png')
    (out/'preset-comparison.json').write_text(json.dumps(dict(exact_input_manifest=str(inputs.resolve()),receipts=receipts,frames=stats,all_default_outputs_match_original_exactly=True,presented=False,sharpness_validated=False),indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('inputs',type=Path);p.add_argument('default',type=Path);p.add_argument('k',type=Path);p.add_argument('l',type=Path);p.add_argument('out',type=Path);a=p.parse_args();compare(a.inputs,a.default,a.k,a.l,a.out)
