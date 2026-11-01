import hashlib,struct,subprocess,os,shutil
from pathlib import Path
root=Path(__file__).parent;native=root/'native';source=native/'shaders'
(root/'build').mkdir(exist_ok=True)
compiler=os.environ.get('TOTK_GLSLANG') or shutil.which('glslang')
validator=os.environ.get('TOTK_SPIRV_VAL') or shutil.which('spirv-val')
if not compiler or not validator:raise SystemExit('Set TOTK_GLSLANG and TOTK_SPIRV_VAL, or put glslang and spirv-val on PATH')
items=[('motion_vs'+str(n),'motion.vert',['-DINFLUENCES='+str(n)]) for n in (1,3,4,7)]
items.append(('motion_fs','motion.frag',[]))
items.append(('motion_gs','motion.geom',[]))
items.extend([('camera_vs','camera.vert',[]),('camera_fs','camera.frag',[]),('jitter_vs','jitter.vert',[]),('jitter_fs','jitter.frag',[])])
for name,file,options in items:
    out=root/'build'/(name+'.spv')
    subprocess.run([compiler,'-V','--target-env','vulkan1.3','-Os','-I'+str(source),*options,'-o',str(out),str(source/file)],check=True)
    subprocess.run([validator,'--target-env','vulkan1.3',str(out)],check=True)
    data=out.read_bytes();words=struct.unpack('<%dI'%(len(data)//4),data)
    text='// Generated private motion shader. SHA256 '+hashlib.sha256(data).hexdigest()+'\n#pragma once\n#include <stdint.h>\n'
    text+='static const uint32_t '+name+'[] = {\n'
    text+='\n'.join(','.join('0x%08xu'%v for v in words[i:i+8])+',' for i in range(0,len(words),8))+'\n};\n'
    (root/'build'/(name+'_spv.h')).write_text(text)
