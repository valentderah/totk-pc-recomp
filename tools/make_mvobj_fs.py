"""Writes runtime/vk_mvobj_fs.h: the SPIR-V of the per-object motion fragment shader (vk_mvobj.c), assembled here
because it must be patched at run time (the Location of its input follows the vertex shader variant) and is small.

The shader, as GLSL:

    #version 450
    layout(location = 0) in vec4 prev;            // last frame's clip position (patch 0015); Location patched
    layout(set = 0, binding = 1, std140) uniform P { vec4 p; };   // render size xy, this frame's jitter zw (pixels)
    layout(location = 0) out vec4 o;
    void main() {
        vec2 win = gl_FragCoord.xy - p.zw;                         // this frame's window position, without the jitter
        vec2 pn = prev.xy / prev.w;
        vec2 pwin = vec2((pn.x * 0.5 + 0.5) * p.x, (0.5 - pn.y * 0.5) * p.y);
        o = prev.w > 1e-6 ? vec4(pwin - win, 0.0, 0.0) : vec4(0.0);
    }

The vector is last frame's window position minus this frame's, in render pixels, as runtime/shaders/dlaa_mv.comp
writes it. Its descriptor set and binding are renumbered by vk_pipeline.c like a translated shader's (the .res.json
text in vk_mvobj.c names binding 1 as slot 3).

    python tools/make_mvobj_fs.py
"""
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'runtime' / 'vk_mvobj_fs.h'


class Asm:
    def __init__(self):
        self.next = 1
        self.sections = {k: [] for k in ('cap', 'mem', 'entry', 'exec', 'dec', 'types', 'func')}
        self.loc_word = None   # index of the patched Location literal in the final words

    def id(self):
        self.next += 1
        return self.next - 1

    def op(self, sec, code, *words):
        self.sections[sec].append([(len(words) + 1) << 16 | code, *words])
        return self.sections[sec][-1]

    def words(self):
        out = [0x07230203, 0x00010000, 0, self.next, 0]
        for sec in ('cap', 'mem', 'entry', 'exec', 'dec', 'types', 'func'):
            for ins in self.sections[sec]:
                if ins is self.loc_ins:
                    self.loc_word = len(out) + 3
                out.extend(ins)
        return out


def string(s):
    b = s.encode() + b'\0'
    b += b'\0' * (-len(b) % 4)
    return list(struct.unpack('<%dI' % (len(b) // 4), b))


def f32(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


a = Asm()
OpCapability, OpMemoryModel, OpEntryPoint, OpExecutionMode, OpDecorate, OpMemberDecorate = 17, 14, 15, 16, 71, 72
OpTypeVoid, OpTypeFunction, OpTypeFloat, OpTypeVector, OpTypePointer, OpTypeStruct, OpTypeInt, OpTypeBool = 19, 33, 22, 23, 32, 30, 21, 20
OpConstant, OpConstantComposite, OpVariable, OpFunction, OpLabel, OpLoad, OpStore, OpReturn, OpFunctionEnd = 43, 44, 59, 54, 248, 61, 62, 253, 56
OpAccessChain, OpVectorShuffle, OpCompositeExtract, OpCompositeConstruct, OpFSub, OpFMul, OpFAdd, OpFDiv = 65, 79, 81, 80, 131, 133, 129, 136
OpFOrdGreaterThan, OpSelect = 186, 169
Input, Output, Uniform = 1, 3, 2
Location, Binding, DescriptorSet, Block, Offset, BuiltIn = 30, 33, 34, 2, 35, 11
FragCoord = 15

a.op('cap', OpCapability, 1)   # Shader
a.op('mem', OpMemoryModel, 0, 1)   # Logical GLSL450
main = a.id()
t_void, t_fn, t_f, t_v2, t_v4, t_u = a.id(), a.id(), a.id(), a.id(), a.id(), a.id()
t_bool = a.id()
t_pin, t_pout, t_struct, t_pu, t_pf_u = a.id(), a.id(), a.id(), a.id(), a.id()
v_prev, v_frag, v_out, v_p = a.id(), a.id(), a.id(), a.id()
c_half, c_zero, c_eps, c_u0, c_vzero = a.id(), a.id(), a.id(), a.id(), a.id()
t_pv4u = a.id()
t_bv4 = a.id()
a.op('entry', OpEntryPoint, 4, main, *string('main'), v_prev, v_frag, v_out)   # Fragment
a.op('exec', OpExecutionMode, main, 7)   # OriginUpperLeft
a.loc_ins = a.op('dec', OpDecorate, v_prev, Location, 0)
a.op('dec', OpDecorate, v_frag, BuiltIn, FragCoord)
a.op('dec', OpDecorate, v_out, Location, 0)
a.op('dec', OpDecorate, t_struct, Block)
a.op('dec', OpMemberDecorate, t_struct, 0, Offset, 0)
a.op('dec', OpDecorate, v_p, DescriptorSet, 0)
a.op('dec', OpDecorate, v_p, Binding, 1)
a.op('types', OpTypeVoid, t_void)
a.op('types', OpTypeFunction, t_fn, t_void)
a.op('types', OpTypeFloat, t_f, 32)
a.op('types', OpTypeVector, t_v2, t_f, 2)
a.op('types', OpTypeVector, t_v4, t_f, 4)
a.op('types', OpTypeInt, t_u, 32, 0)
a.op('types', OpTypeBool, t_bool)
a.op('types', OpTypeVector, t_bv4, t_bool, 4)
a.op('types', OpTypePointer, t_pin, Input, t_v4)
a.op('types', OpTypePointer, t_pout, Output, t_v4)
a.op('types', OpTypeStruct, t_struct, t_v4)
a.op('types', OpTypePointer, t_pu, Uniform, t_struct)
a.op('types', OpTypePointer, t_pv4u, Uniform, t_v4)
a.op('types', OpConstant, t_f, c_half, f32(0.5))
a.op('types', OpConstant, t_f, c_zero, f32(0.0))
a.op('types', OpConstant, t_f, c_eps, f32(1e-6))
a.op('types', OpConstant, t_u, c_u0, 0)
a.op('types', OpConstantComposite, t_v4, c_vzero, c_zero, c_zero, c_zero, c_zero)
a.op('types', OpVariable, t_pin, v_prev, Input)
a.op('types', OpVariable, t_pin, v_frag, Input)
a.op('types', OpVariable, t_pout, v_out, Output)
a.op('types', OpVariable, t_pu, v_p, Uniform)

F = 'func'
a.op(F, OpFunction, t_void, main, 0, t_fn)
a.op(F, OpLabel, a.id())
pp = a.id(); a.op(F, OpAccessChain, t_pv4u, pp, v_p, c_u0)
p = a.id(); a.op(F, OpLoad, t_v4, p, pp)
prev = a.id(); a.op(F, OpLoad, t_v4, prev, v_prev)
frag = a.id(); a.op(F, OpLoad, t_v4, frag, v_frag)
fxy = a.id(); a.op(F, OpVectorShuffle, t_v2, fxy, frag, frag, 0, 1)
jit = a.id(); a.op(F, OpVectorShuffle, t_v2, jit, p, p, 2, 3)
win = a.id(); a.op(F, OpFSub, t_v2, win, fxy, jit)
px, py, pw = a.id(), a.id(), a.id()
a.op(F, OpCompositeExtract, t_f, px, prev, 0)
a.op(F, OpCompositeExtract, t_f, py, prev, 1)
a.op(F, OpCompositeExtract, t_f, pw, prev, 3)
nx, ny = a.id(), a.id()
a.op(F, OpFDiv, t_f, nx, px, pw)
a.op(F, OpFDiv, t_f, ny, py, pw)
sx, sy = a.id(), a.id()
a.op(F, OpCompositeExtract, t_f, sx, p, 0)
a.op(F, OpCompositeExtract, t_f, sy, p, 1)
# x: (nx * 0.5 + 0.5) * sx
x1, x2, x3 = a.id(), a.id(), a.id()
a.op(F, OpFMul, t_f, x1, nx, c_half)
a.op(F, OpFAdd, t_f, x2, x1, c_half)
a.op(F, OpFMul, t_f, x3, x2, sx)
# y: (0.5 - ny * 0.5) * sy
y1, y2, y3 = a.id(), a.id(), a.id()
a.op(F, OpFMul, t_f, y1, ny, c_half)
a.op(F, OpFSub, t_f, y2, c_half, y1)
a.op(F, OpFMul, t_f, y3, y2, sy)
wx, wy = a.id(), a.id()
a.op(F, OpCompositeExtract, t_f, wx, win, 0)
a.op(F, OpCompositeExtract, t_f, wy, win, 1)
mx, my = a.id(), a.id()
a.op(F, OpFSub, t_f, mx, x3, wx)
a.op(F, OpFSub, t_f, my, y3, wy)
mv = a.id(); a.op(F, OpCompositeConstruct, t_v4, mv, mx, my, c_zero, c_zero)
ok = a.id(); a.op(F, OpFOrdGreaterThan, t_bool, ok, pw, c_eps)
# SPIR-V 1.0 (the module's version): OpSelect of a vector takes a condition vector of the same size; a scalar
# condition is allowed only from 1.4 on, and spirv-val (the validation layer) rejects the module otherwise.
okv = a.id(); a.op(F, OpCompositeConstruct, t_bv4, okv, ok, ok, ok, ok)
res = a.id(); a.op(F, OpSelect, t_v4, res, okv, mv, c_vzero)
a.op(F, OpStore, v_out, res)
a.op(F, OpReturn)
a.op(F, OpFunctionEnd)

w = a.words()
lines = [', '.join(f'0x{x:08x}' for x in w[i:i + 8]) for i in range(0, len(w), 8)]
OUT.write_text(
    '// Generated by tools/make_mvobj_fs.py: the per-object motion fragment shader (vk_mvobj.c). Do not edit.\n'
    f'#define MVOBJ_FS_LOCATION_WORD {a.loc_word}   // the input Location literal, patched to the variant\'s\n'
    'static const uint32_t mvobj_fs_spv[] = {\n    ' + ',\n    '.join(lines) + '\n};\n', newline='\n')
print(f'{OUT}: {len(w)} words, Location at word {a.loc_word}')
