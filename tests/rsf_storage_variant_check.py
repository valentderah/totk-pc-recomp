"""Test MotionPrevious's actual IR transform, then compile a synthetic split-storage interface.

First build tools/build_xlate.py --out build/checks/rsf-storage-variant-worker/xlate.
All shader data in this fixture is synthetic, and all generated outputs stay ignored.
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build/checks/rsf-storage-variant-worker'
shader = out / 'xlate/ryujinx/src/Ryujinx.Graphics.Shader'
assert (shader / 'Translation/Transforms/MotionPrevious.cs').is_file()
(shader / 'SyntheticStorageProof.cs').write_text(r'''
using System;
using System.IO;
using System.Linq;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation.Transforms;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;
namespace Ryujinx.Graphics.Shader.Translation;
public static class SyntheticStorageProof {
    class Accessor : IGpuAccessor {
        int next = 1;
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => throw new Exception();
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, next++);
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, next++);
        public SetBindingPair CreateImageBinding(int count, bool buffer) => new(0, next++);
        public SetBindingPair CreateTextureBinding(int count, bool buffer) => new(0, next++);
        public int QuerySamplerArrayLengthFromPool() => 1;
        public int QueryTextureArrayLengthFromBuffer(int slot) => 1;
        public int QueryTextureArrayLengthFromPool() => 1;
    }
    static void Check(bool condition) { if (!condition) throw new Exception("storage variant assertion"); }
    public static void Run(string output) {
        foreach (int bank in new[]{4, 7}) foreach (bool written in new[]{false, true}) {
            var gpu = new Accessor(); var resources = new ResourceManager(ShaderStage.Vertex, gpu);
            Check(resources.TryGetStorageBufferBinding(6, 0, written, out int current));
            var block = new BasicBlock(0); var value = Local(); var position = Local();
            var load = new Operation(Instruction.Load, StorageKind.StorageBuffer, value, Const(current), Const(0), Const(0));
            block.Operations.AddLast(load);
            block.Operations.AddLast(new Operation(Instruction.Add, position, value, Cbuf(bank, 0)));
            block.Operations.AddLast(new Operation(Instruction.Store, StorageKind.Output, null,
                Const((int)IoVariable.Position), Const(0), position));
            int location = MotionPrevious.Run(new[]{block}, resources, new AttributeUsage(gpu), out int banks);
            if (written) { Check(location < 0); continue; }
            Check(location >= 0 && (banks & (1<<bank)) != 0);
            var previous = resources.GetStorageBufferDescriptors().Single(d => d.SbCbSlot == 70);
            Check(previous.Binding != current && previous.SbCbOffset == 0 && previous.Flags == BufferUsageFlags.None);
            var loads = block.Operations.OfType<Operation>().Where(o => o.Inst == Instruction.Load &&
                o.StorageKind == StorageKind.StorageBuffer).ToArray();
            Check(loads.Length == 2 && load.GetSource(0).Value == current);
            Check(loads.Any(o => o.GetSource(0).Value == previous.Binding));
            File.WriteAllText(output, $@"#version 450
layout(std430,binding={current}) readonly buffer Current {{ float v[]; }} current;
layout(std430,binding={previous.Binding}) readonly buffer Previous {{ float v[]; }} previous;
layout(location=0) out vec4 old_position;
void main() {{ gl_Position=vec4(current.v[0],0,0,1); old_position=vec4(previous.v[0],0,0,1); }}");
        }
    }
}
''')
project = out / 'proof'
project.mkdir(exist_ok=True)
(project / 'proof.csproj').write_text(f'''<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup>
<OutputType>Exe</OutputType><TargetFramework>net10.0</TargetFramework></PropertyGroup>
<ItemGroup><ProjectReference Include="{shader / 'Ryujinx.Graphics.Shader.csproj'}" /></ItemGroup></Project>''')
(project / 'Program.cs').write_text('Ryujinx.Graphics.Shader.Translation.SyntheticStorageProof.Run(args[0]);')
synthetic = out / 'split-storage.vert'
subprocess.run(['dotnet', 'run', '--project', str(project / 'proof.csproj'), '-c', 'Release', '--', str(synthetic)], check=True)
sdk = Path('C:/VulkanSDK/1.4.357.0/Bin')
subprocess.run([str(sdk / 'glslangValidator.exe'), '-V', str(synthetic), '-o', str(out / 'split-storage.spv')], check=True)
subprocess.run([str(sdk / 'spirv-val.exe'), str(out / 'split-storage.spv')], check=True)
print('actual IR transform preserves current storage, adds previous binding and refuses writable storage; synthetic interface validates')
