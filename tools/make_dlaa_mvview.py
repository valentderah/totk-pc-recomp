"""Writes runtime/dlaa_mvview_spv.h: the SPIR-V of the motion-vector view (vk_color.c, dlaa_mvdebug = 2), assembled
here like tools/make_mvobj_fs.py (no GLSL compiler on the machine this was written on).

The shader, as GLSL:

    #version 450
    layout(local_size_x = 8, local_size_y = 8) in;
    layout(binding = 0) uniform sampler2D mv;                   // DLSS's motion image: last minus this window position,
                                                                // render pixels (vk_dlaa.c)
    layout(binding = 1, rgba16f) uniform writeonly image2D dst;   // the picture the present shows
    layout(push_constant) uniform P { vec4 a; vec4 b; };         // a: dst size xy, mv size zw; b: gain x, grey y
    void main() {
        vec2 g = vec2(gl_GlobalInvocationID.xy);
        if (g.x < a.x && g.y < a.y) {
            ivec2 s = ivec2((g + 0.5) * a.zw / a.xy);
            vec2 v = -texelFetch(mv, s, 0).xy;                   // the content's motion: this minus last position
            float h = atan(v.y, v.x) * (0.5 / 3.14159265) + 0.5;  // direction as hue
            vec3 k = clamp(abs(fract(vec3(h) + vec3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
            float sat = clamp(sqrt(length(v) * b.x), 0.0, 1.0);  // speed as strength; sqrt shows small motion
            imageStore(dst, ivec2(gl_GlobalInvocationID.xy), vec4(mix(vec3(b.y), k, vec3(sat)), 1.0));
        }
    }

Grey where nothing moves, a hue per direction, stronger colour for faster motion: the usual optical-flow colour coding.

    python tools/make_dlaa_mvview.py
"""
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'runtime' / 'dlaa_mvview_spv.h'

SECTIONS = ('cap', 'ext', 'mem', 'entry', 'exec', 'dec', 'types', 'func')


class Asm:
    def __init__(self):
        self.next = 1
        self.sections = {k: [] for k in SECTIONS}

    def id(self):
        self.next += 1
        return self.next - 1

    def op(self, sec, code, *words):
        self.sections[sec].append([(len(words) + 1) << 16 | code, *words])

    def words(self):
        out = [0x07230203, 0x00010000, 0, self.next, 0]
        for sec in SECTIONS:
            for ins in self.sections[sec]:
                out.extend(ins)
        return out


def string(s):
    b = s.encode() + b'\0'
    b += b'\0' * (-len(b) % 4)
    return list(struct.unpack('<%dI' % (len(b) // 4), b))


def f32(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


OpCapability, OpExtInstImport, OpExtInst, OpMemoryModel, OpEntryPoint, OpExecutionMode = 17, 11, 12, 14, 15, 16
OpDecorate, OpMemberDecorate = 71, 72
OpTypeVoid, OpTypeBool, OpTypeInt, OpTypeFloat, OpTypeVector, OpTypeImage, OpTypeSampledImage = 19, 20, 21, 22, 23, 25, 27
OpTypeStruct, OpTypePointer, OpTypeFunction = 30, 32, 33
OpConstant, OpConstantComposite, OpVariable, OpFunction, OpFunctionEnd, OpLabel, OpReturn = 43, 44, 59, 54, 56, 248, 253
OpLoad, OpAccessChain, OpVectorShuffle, OpCompositeExtract, OpCompositeConstruct = 61, 65, 79, 81, 80
OpImage, OpImageFetch, OpImageWrite, OpConvertFToS, OpConvertUToF, OpBitcast = 100, 95, 99, 110, 112, 124
OpFNegate, OpFAdd, OpFSub, OpFMul, OpFDiv, OpVectorTimesScalar, OpFOrdLessThan, OpLogicalAnd = 127, 129, 131, 133, 136, 142, 184, 167
OpSelectionMerge, OpBranch, OpBranchConditional = 247, 249, 250
FAbs, Fract, Sqrt, Atan2, FClamp, FMix, Length = 4, 10, 31, 25, 43, 46, 66   # GLSL.std.450
UniformConstant, Input, PushConstant = 0, 1, 9
Binding, DescriptorSet, Block, Offset, BuiltIn, NonReadable = 33, 34, 2, 35, 11, 25
GlobalInvocationId = 28

a = Asm()
a.op('cap', OpCapability, 1)    # Shader
glsl = a.id()
a.op('ext', OpExtInstImport, glsl, *string('GLSL.std.450'))
a.op('mem', OpMemoryModel, 0, 1)   # Logical GLSL450
main = a.id()
t_void, t_fn, t_bool, t_f, t_i, t_u = a.id(), a.id(), a.id(), a.id(), a.id(), a.id()
t_v2, t_v3, t_v4, t_v2i, t_v2u, t_v3u = a.id(), a.id(), a.id(), a.id(), a.id(), a.id()
t_img, t_simg, t_wimg = a.id(), a.id(), a.id()
t_psimg, t_pwimg, t_pgid, t_pc, t_ppc, t_ppv4 = a.id(), a.id(), a.id(), a.id(), a.id(), a.id()
v_mv, v_dst, v_gid, v_p = a.id(), a.id(), a.id(), a.id()
c_i0, c_i1, c_zero, c_half, c_one, c_three, c_six, c_inv2pi = (a.id() for _ in range(8))
c_third, c_2third, c_off3 = a.id(), a.id(), a.id()

a.op('entry', OpEntryPoint, 5, main, *string('main'), v_gid)   # GLCompute
a.op('exec', OpExecutionMode, main, 17, 8, 8, 1)   # LocalSize
a.op('dec', OpDecorate, v_gid, BuiltIn, GlobalInvocationId)
for var, binding in ((v_mv, 0), (v_dst, 1)):
    a.op('dec', OpDecorate, var, DescriptorSet, 0)
    a.op('dec', OpDecorate, var, Binding, binding)
a.op('dec', OpDecorate, v_dst, NonReadable)
a.op('dec', OpDecorate, t_pc, Block)
a.op('dec', OpMemberDecorate, t_pc, 0, Offset, 0)
a.op('dec', OpMemberDecorate, t_pc, 1, Offset, 16)

T = 'types'
a.op(T, OpTypeVoid, t_void)
a.op(T, OpTypeFunction, t_fn, t_void)
a.op(T, OpTypeBool, t_bool)
a.op(T, OpTypeFloat, t_f, 32)
a.op(T, OpTypeInt, t_i, 32, 1)
a.op(T, OpTypeInt, t_u, 32, 0)
a.op(T, OpTypeVector, t_v2, t_f, 2)
a.op(T, OpTypeVector, t_v3, t_f, 3)
a.op(T, OpTypeVector, t_v4, t_f, 4)
a.op(T, OpTypeVector, t_v2i, t_i, 2)
a.op(T, OpTypeVector, t_v2u, t_u, 2)
a.op(T, OpTypeVector, t_v3u, t_u, 3)
a.op(T, OpTypeImage, t_img, t_f, 1, 0, 0, 0, 1, 0)     # 2D, sampled, format unknown
a.op(T, OpTypeSampledImage, t_simg, t_img)
a.op(T, OpTypeImage, t_wimg, t_f, 1, 0, 0, 0, 2, 2)    # 2D, storage, Rgba16f
a.op(T, OpTypePointer, t_psimg, UniformConstant, t_simg)
a.op(T, OpTypePointer, t_pwimg, UniformConstant, t_wimg)
a.op(T, OpTypePointer, t_pgid, Input, t_v3u)
a.op(T, OpTypeStruct, t_pc, t_v4, t_v4)
a.op(T, OpTypePointer, t_ppc, PushConstant, t_pc)
a.op(T, OpTypePointer, t_ppv4, PushConstant, t_v4)
a.op(T, OpConstant, t_i, c_i0, 0)
a.op(T, OpConstant, t_i, c_i1, 1)
a.op(T, OpConstant, t_f, c_zero, f32(0.0))
a.op(T, OpConstant, t_f, c_half, f32(0.5))
a.op(T, OpConstant, t_f, c_one, f32(1.0))
a.op(T, OpConstant, t_f, c_three, f32(3.0))
a.op(T, OpConstant, t_f, c_six, f32(6.0))
a.op(T, OpConstant, t_f, c_inv2pi, f32(0.5 / 3.14159265))
a.op(T, OpConstant, t_f, c_third, f32(1.0 / 3.0))
a.op(T, OpConstant, t_f, c_2third, f32(2.0 / 3.0))
a.op(T, OpConstantComposite, t_v3, c_off3, c_one, c_2third, c_third)
a.op(T, OpVariable, t_psimg, v_mv, UniformConstant)
a.op(T, OpVariable, t_pwimg, v_dst, UniformConstant)
a.op(T, OpVariable, t_pgid, v_gid, Input)
a.op(T, OpVariable, t_ppc, v_p, PushConstant)

F = 'func'


def ins(code, rtype, *args):
    r = a.id()
    a.op(F, code, rtype, r, *args)
    return r


def ext(rtype, inst, *args):
    return ins(OpExtInst, rtype, glsl, inst, *args)


def splat3(x):
    return ins(OpCompositeConstruct, t_v3, x, x, x)


def splat2(x):
    return ins(OpCompositeConstruct, t_v2, x, x)


l_entry, l_body, l_merge = a.id(), a.id(), a.id()
a.op(F, OpFunction, t_void, main, 0, t_fn)
a.op(F, OpLabel, l_entry)
pa = ins(OpLoad, t_v4, ins(OpAccessChain, t_ppv4, v_p, c_i0))
pb = ins(OpLoad, t_v4, ins(OpAccessChain, t_ppv4, v_p, c_i1))
gid = ins(OpLoad, t_v3u, v_gid)
gu = ins(OpVectorShuffle, t_v2u, gid, gid, 0, 1)
gx = ins(OpConvertUToF, t_f, ins(OpCompositeExtract, t_u, gid, 0))
gy = ins(OpConvertUToF, t_f, ins(OpCompositeExtract, t_u, gid, 1))
ax = ins(OpCompositeExtract, t_f, pa, 0)
ay = ins(OpCompositeExtract, t_f, pa, 1)
inside = ins(OpLogicalAnd, t_bool, ins(OpFOrdLessThan, t_bool, gx, ax), ins(OpFOrdLessThan, t_bool, gy, ay))
a.op(F, OpSelectionMerge, l_merge, 0)
a.op(F, OpBranchConditional, inside, l_body, l_merge)
a.op(F, OpLabel, l_body)
# the motion image's texel under this pixel
g = ins(OpCompositeConstruct, t_v2, gx, gy)
gh = ins(OpFAdd, t_v2, g, splat2(c_half))
axy = ins(OpVectorShuffle, t_v2, pa, pa, 0, 1)
azw = ins(OpVectorShuffle, t_v2, pa, pa, 2, 3)
s = ins(OpConvertFToS, t_v2i, ins(OpFDiv, t_v2, ins(OpFMul, t_v2, gh, azw), axy))
img = ins(OpImage, t_img, ins(OpLoad, t_simg, v_mv))
texel = ins(OpImageFetch, t_v4, img, s, 2, c_i0)   # Lod 0
v = ins(OpFNegate, t_v2, ins(OpVectorShuffle, t_v2, texel, texel, 0, 1))
vx = ins(OpCompositeExtract, t_f, v, 0)
vy = ins(OpCompositeExtract, t_f, v, 1)
# direction as hue
h = ins(OpFAdd, t_f, ins(OpFMul, t_f, ext(t_f, Atan2, vy, vx), c_inv2pi), c_half)
fr = ext(t_v3, Fract, ins(OpFAdd, t_v3, splat3(h), c_off3))
k = ext(t_v3, FClamp,
        ins(OpFSub, t_v3, ext(t_v3, FAbs, ins(OpFSub, t_v3, ins(OpVectorTimesScalar, t_v3, fr, c_six), splat3(c_three))),
            splat3(c_one)),
        splat3(c_zero), splat3(c_one))
# speed as strength
gain = ins(OpCompositeExtract, t_f, pb, 0)
grey = ins(OpCompositeExtract, t_f, pb, 1)
sat = ext(t_f, FClamp, ext(t_f, Sqrt, ins(OpFMul, t_f, ext(t_f, Length, v), gain)), c_zero, c_one)
col = ext(t_v3, FMix, splat3(grey), k, splat3(sat))
cx, cy, cz = (ins(OpCompositeExtract, t_f, col, i) for i in range(3))
out = ins(OpCompositeConstruct, t_v4, cx, cy, cz, c_one)
a.op(F, OpImageWrite, ins(OpLoad, t_wimg, v_dst), ins(OpBitcast, t_v2i, gu), out)
a.op(F, OpBranch, l_merge)
a.op(F, OpLabel, l_merge)
a.op(F, OpReturn)
a.op(F, OpFunctionEnd)

w = a.words()
lines = [', '.join(f'0x{x:08x}' for x in w[i:i + 8]) for i in range(0, len(w), 8)]
OUT.write_text(
    '// Generated by tools/make_dlaa_mvview.py: the motion-vector view (vk_color.c, dlaa_mvdebug = 2). Do not edit.\n'
    '#pragma once\n#include <stdint.h>\n'
    'static const uint32_t dlaa_mvview_spv[] = {\n    ' + ',\n    '.join(lines) + '\n};\n', newline='\n')
print(f'{OUT}: {len(w)} words')
