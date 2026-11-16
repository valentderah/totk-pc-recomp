"""Actual narrow texture IR transform and backwards-compatible descriptor metadata, no device/game files."""
from pathlib import Path
import subprocess

from rsf_snapshot_currentness_check import extract_function

root = Path(__file__).resolve().parents[1]
out = root / 'build/checks/rsf-texture-caller-worker'
shader = out / 'xlate/ryujinx/src/Ryujinx.Graphics.Shader'
assert (shader / 'Translation/Transforms/MotionPrevious.cs').is_file()
(shader / 'SyntheticTextureProof.cs').write_text(r'''
using System;
using System.IO;
using System.Linq;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation.Transforms;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;
namespace Ryujinx.Graphics.Shader.Translation;
public static class SyntheticTextureProof {
    class Accessor : IGpuAccessor {
        int next=1;
        public ReadOnlySpan<ulong> GetCode(ulong address,int size)=>throw new Exception();
        public SetBindingPair CreateConstantBufferBinding(int index)=>new(0,next++);
        public SetBindingPair CreateStorageBufferBinding(int index)=>new(0,next++);
        public SetBindingPair CreateTextureBinding(int n,bool b)=>new(0,next++);
        public SetBindingPair CreateImageBinding(int n,bool b)=>new(0,next++);
        public int QuerySamplerArrayLengthFromPool()=>1;
        public int QueryTextureArrayLengthFromBuffer(int slot)=>1;
        public int QueryTextureArrayLengthFromPool()=>1;
    }
    static void Check(bool b) { if(!b) throw new Exception("texture IR assertion"); }
    public static void Run(string output) {
        foreach(int test in new[]{0,1,2,3}) {
            var gpu=new Accessor(); var r=new ResourceManager(ShaderStage.Vertex,gpu);
            var type=test==3?SamplerType.Texture3D:SamplerType.Texture2D;
            var binding=r.GetTextureOrImageBinding(Instruction.TextureSample,type,TextureFormat.Unknown,TextureFlags.LodLevel,-1,8);
            var block=new BasicBlock(0);var value=Local();var position=Local();
            var sample=new TextureOperation(Instruction.TextureSample,type,TextureFormat.Unknown,TextureFlags.LodLevel,
                binding.SetIndex,binding.Binding,0,new[]{value},new[]{Cbuf(test==1?7:15,0),Const(0),Const(0)});
            block.Operations.AddLast(sample);
            block.Operations.AddLast(new Operation(Instruction.Add,position,value,Cbuf(4,0)));
            block.Operations.AddLast(new Operation(Instruction.Store,StorageKind.Output,null,Const((int)IoVariable.Position),Const(0),position));
            if(test==2) block.Operations.AddLast(new Operation(Instruction.BranchIfTrue,null,Const(1),Const(0)));
            int location=MotionPrevious.Run(new[]{block},r,new AttributeUsage(gpu),out int banks);
            if(test!=0) { Check(location<0);continue; }
            Check(location>=0 && banks==((1<<4)|(1<<15)));
            var previous=r.GetTextureDescriptors().Single(d=>d.CbufSlot==63);
            Check(previous.HandleIndex==8 && previous.Binding!=binding.Binding && !previous.Separate && previous.ArrayLength==1);
            var samples=block.Operations.OfType<TextureOperation>().ToArray();
            Check(samples.Length==2 && sample.Binding==binding.Binding && samples.Any(x=>x.Binding==previous.Binding));
            Check(samples.Single(x=>x.Binding==previous.Binding).GetSource(0).GetCbufSlot()==30);
            File.WriteAllText(output,$@"#version 450
layout(binding={binding.Binding}) uniform sampler2D current_texture;
layout(binding={previous.Binding}) uniform sampler2D previous_texture;
layout(location=0) out vec4 previous_position;
void main() {{ gl_Position=textureLod(current_texture,vec2(0.25),0); previous_position=textureLod(previous_texture,vec2(0.5),0); }}");
        }
    }
}
''')
project = out / 'texture-proof'
project.mkdir(exist_ok=True)
(project / 'proof.csproj').write_text(f'''<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>
<OutputType>Exe</OutputType><TargetFramework>net10.0</TargetFramework></PropertyGroup><ItemGroup>
<ProjectReference Include="{shader / 'Ryujinx.Graphics.Shader.csproj'}" /></ItemGroup></Project>''')
(project / 'Program.cs').write_text('Ryujinx.Graphics.Shader.Translation.SyntheticTextureProof.Run(args[0]);')
subprocess.run(['dotnet','run','--project',str(project/'proof.csproj'),'-c','Release','--',str(out/'texture.vert')],check=True)
sdk=Path('C:/VulkanSDK/1.4.357.0/Bin')
subprocess.run([str(sdk/'glslangValidator.exe'),'-V',str(out/'texture.vert'),'-o',str(out/'texture.spv')],check=True)
subprocess.run([str(sdk/'spirv-val.exe'),str(out/'texture.spv')],check=True)

source=(root/'runtime/gfx_shader.c').read_text()
fixture='''#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "gfx.h"
struct GfxShader { GfxShaderRes res[GFX_RES_KINDS][8]; int nres[GFX_RES_KINDS]; };
static int fmt_code(const char *s) { (void)s; return 0; }
'''
for name in ('static const char *json_key(', 'static long json_int(', 'static void json_str(', 'static int handle_slot(', 'static void add_res('):
    fixture+=extract_function(source, name)
fixture+=r'''int main(void) {
    GfxShader s={0};
    const char *old="{\"binding\":1,\"type\":3,\"cbuf_slot\":-1,\"handle\":8,\"flags\":16}";
    const char *prev="{\"binding\":2,\"type\":3,\"cbuf_slot\":63,\"handle\":8}";
    add_res(&s,GFX_RES_TEX,old,old+strlen(old)); add_res(&s,GFX_RES_TEX,prev,prev+strlen(prev));
    assert(s.res[GFX_RES_TEX][0].slot==0 && s.res[GFX_RES_TEX][0].flags==16);
    assert(s.res[GFX_RES_TEX][1].slot==0 && (s.res[GFX_RES_TEX][1].flags&128));
    return 0;
}'''
(out/'metadata.c').write_text(fixture)
subprocess.run(['C:/msys64/ucrt64/bin/clang.exe','-std=gnu11','-Wall','-Wextra','-Werror','-I',str(root/'runtime'),str(out/'metadata.c'),'-o',str(out/'metadata.exe')],check=True)
subprocess.run([str(out/'metadata.exe')],check=True)
print('actual one-binding IR/metadata compatibility and synthetic previous/current texture shader validation passed')
