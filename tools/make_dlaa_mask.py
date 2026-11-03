"""Writes runtime/dlaa_mask_spv.h: the SPIR-V of DLSS's reactive mask pass (vk_dlaa.c, mask_pass), assembled here like
tools/make_mvobj_fs.py (no GLSL compiler on the machine this was written on).

The shader, as GLSL:

    #version 450
    layout(local_size_x = 8, local_size_y = 8) in;
    layout(binding = 0) uniform sampler2D before;   // the light target when the transparents start
    layout(binding = 1) uniform sampler2D after;    // the light target when DLSS runs
    layout(binding = 2, r8) uniform writeonly image2D mask;
    layout(push_constant) uniform P { vec4 p; };    // render size xy, threshold z, gain w
    void main() {
        vec2 g = vec2(gl_GlobalInvocationID.xy);
        if (g.x < p.x && g.y < p.y) {
            ivec2 c = ivec2(gl_GlobalInvocationID.xy);
            vec3 b = texelFetch(before, c, 0).rgb, a = texelFetch(after, c, 0).rgb;
            vec3 d = abs(a - b);
            float dm = max(max(d.r, d.g), d.b);
            float m = max(max(max(a.r, a.g), a.b), max(max(b.r, b.g), b.b));
            float r = dm / max(m, 1e-4);
            imageStore(mask, c, vec4(clamp((r - p.z) * p.w, 0.0, 1.0)));
        }
    }

The mask is the share of a pixel's colour the transparents changed (particles, smoke, sparks: what has no motion
vectors of its own), 0 where nothing was drawn over the solid scene.

    python tools/make_dlaa_mask.py
"""
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'runtime' / 'dlaa_mask_spv.h'

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
OpImage, OpImageFetch, OpImageWrite, OpConvertUToF, OpBitcast = 100, 95, 99, 112, 124
OpFSub, OpFMul, OpFDiv, OpFOrdLessThan, OpLogicalAnd = 131, 133, 136, 184, 167
OpSelectionMerge, OpBranch, OpBranchConditional = 247, 249, 250
FAbs, FMax, FClamp = 4, 40, 43   # GLSL.std.450
UniformConstant, Input, PushConstant = 0, 1, 9
Binding, DescriptorSet, Block, Offset, BuiltIn, NonReadable = 33, 34, 2, 35, 11, 25
GlobalInvocationId = 28

a = Asm()
a.op('cap', OpCapability, 1)    # Shader
a.op('cap', OpCapability, 49)   # StorageImageExtendedFormats (r8)
glsl = a.id()
a.op('ext', OpExtInstImport, glsl, *string('GLSL.std.450'))
a.op('mem', OpMemoryModel, 0, 1)   # Logical GLSL450
main = a.id()
t_void, t_fn, t_bool, t_f, t_i, t_u = a.id(), a.id(), a.id(), a.id(), a.id(), a.id()
t_v2, t_v3, t_v4, t_v2i, t_v2u, t_v3u = a.id(), a.id(), a.id(), a.id(), a.id(), a.id()
t_img, t_simg, t_wimg = a.id(), a.id(), a.id()
t_psimg, t_pwimg, t_pgid, t_pc, t_ppc, t_ppv4 = a.id(), a.id(), a.id(), a.id(), a.id(), a.id()
v_before, v_after, v_mask, v_gid, v_p = a.id(), a.id(), a.id(), a.id(), a.id()
c_i0, c_zero, c_one, c_eps = a.id(), a.id(), a.id(), a.id()

a.op('entry', OpEntryPoint, 5, main, *string('main'), v_gid)   # GLCompute
a.op('exec', OpExecutionMode, main, 17, 8, 8, 1)   # LocalSize
a.op('dec', OpDecorate, v_gid, BuiltIn, GlobalInvocationId)
for var, binding in ((v_before, 0), (v_after, 1), (v_mask, 2)):
    a.op('dec', OpDecorate, var, DescriptorSet, 0)
    a.op('dec', OpDecorate, var, Binding, binding)
a.op('dec', OpDecorate, v_mask, NonReadable)
a.op('dec', OpDecorate, t_pc, Block)
a.op('dec', OpMemberDecorate, t_pc, 0, Offset, 0)

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
a.op(T, OpTypeImage, t_wimg, t_f, 1, 0, 0, 0, 2, 15)   # 2D, storage, R8
a.op(T, OpTypePointer, t_psimg, UniformConstant, t_simg)
a.op(T, OpTypePointer, t_pwimg, UniformConstant, t_wimg)
a.op(T, OpTypePointer, t_pgid, Input, t_v3u)
a.op(T, OpTypeStruct, t_pc, t_v4)
a.op(T, OpTypePointer, t_ppc, PushConstant, t_pc)
a.op(T, OpTypePointer, t_ppv4, PushConstant, t_v4)
a.op(T, OpConstant, t_i, c_i0, 0)
a.op(T, OpConstant, t_f, c_zero, f32(0.0))
a.op(T, OpConstant, t_f, c_one, f32(1.0))
a.op(T, OpConstant, t_f, c_eps, f32(1e-4))
a.op(T, OpVariable, t_psimg, v_before, UniformConstant)
a.op(T, OpVariable, t_psimg, v_after, UniformConstant)
a.op(T, OpVariable, t_pwimg, v_mask, UniformConstant)
a.op(T, OpVariable, t_pgid, v_gid, Input)
a.op(T, OpVariable, t_ppc, v_p, PushConstant)

F = 'func'


def ins(code, rtype, *args):
    r = a.id()
    a.op(F, code, rtype, r, *args)
    return r


def ext(rtype, inst, *args):
    return ins(OpExtInst, rtype, glsl, inst, *args)


l_entry, l_body, l_merge = a.id(), a.id(), a.id()
a.op(F, OpFunction, t_void, main, 0, t_fn)
a.op(F, OpLabel, l_entry)
pp = ins(OpAccessChain, t_ppv4, v_p, c_i0)
p = ins(OpLoad, t_v4, pp)
gid = ins(OpLoad, t_v3u, v_gid)
gu = ins(OpVectorShuffle, t_v2u, gid, gid, 0, 1)
gx = ins(OpConvertUToF, t_f, ins(OpCompositeExtract, t_u, gid, 0))
gy = ins(OpConvertUToF, t_f, ins(OpCompositeExtract, t_u, gid, 1))
px = ins(OpCompositeExtract, t_f, p, 0)
py = ins(OpCompositeExtract, t_f, p, 1)
inside = ins(OpLogicalAnd, t_bool, ins(OpFOrdLessThan, t_bool, gx, px), ins(OpFOrdLessThan, t_bool, gy, py))
a.op(F, OpSelectionMerge, l_merge, 0)
a.op(F, OpBranchConditional, inside, l_body, l_merge)
a.op(F, OpLabel, l_body)
c = ins(OpBitcast, t_v2i, gu)


def fetch(var):
    img = ins(OpImage, t_img, ins(OpLoad, t_simg, var))
    texel = ins(OpImageFetch, t_v4, img, c, 2, c_i0)   # Lod 0
    return ins(OpVectorShuffle, t_v3, texel, texel, 0, 1, 2)


def maxc(v):
    x, y, z = (ins(OpCompositeExtract, t_f, v, k) for k in range(3))
    return ext(t_f, FMax, ext(t_f, FMax, x, y), z)


b = fetch(v_before)
af = fetch(v_after)
d = ext(t_v3, FAbs, ins(OpFSub, t_v3, af, b))
dm = maxc(d)
m = ext(t_f, FMax, ext(t_f, FMax, maxc(af), maxc(b)), c_eps)
r = ins(OpFDiv, t_f, dm, m)
thr = ins(OpCompositeExtract, t_f, p, 2)
gain = ins(OpCompositeExtract, t_f, p, 3)
k = ext(t_f, FClamp, ins(OpFMul, t_f, ins(OpFSub, t_f, r, thr), gain), c_zero, c_one)
out = ins(OpCompositeConstruct, t_v4, k, k, k, k)
a.op(F, OpImageWrite, ins(OpLoad, t_wimg, v_mask), c, out)
a.op(F, OpBranch, l_merge)
a.op(F, OpLabel, l_merge)
a.op(F, OpReturn)
a.op(F, OpFunctionEnd)

w = a.words()
lines = [', '.join(f'0x{x:08x}' for x in w[i:i + 8]) for i in range(0, len(w), 8)]
OUT.write_text(
    '// Generated by tools/make_dlaa_mask.py: DLSS\'s reactive mask pass (vk_dlaa.c mask_pass). Do not edit.\n'
    '#pragma once\n#include <stdint.h>\n'
    'static const uint32_t dlaa_mask_spv[] = {\n    ' + ',\n    '.join(lines) + '\n};\n', newline='\n')
print(f'{OUT}: {len(w)} words')
