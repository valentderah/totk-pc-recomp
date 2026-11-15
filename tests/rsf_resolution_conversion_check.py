"""Run resolution owner against read-only generated 1.4.3 conversion blocks, CPU only."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
from check_content import GENERATED_MARKER
out = root / "build/checks/rsf-resolution-conversion-worker"
out.mkdir(parents=True, exist_ok=True)
renderer = (root / "generated/v143/main/main_0062.c").read_text(encoding="utf-8")
allocator = (root / "generated/v143/main/main_0008.c").read_text(encoding="utf-8")

def block(source, begin, end):
    start = source.index(begin)
    return source[start:source.index(end, start)]

# Execute the generated straight-line float instructions with the
# real runtime FP helpers; it does not claim to test all guest FP state.
scale = block(renderer, "    c->v[0] = (uint32_t)x0;\n    { uint64_t ea = x19 + 0xcc4ull;",
              "    { uint64_t ea = x8 + 0x148ull;")
quantize = block(allocator, "    FP_SETS(4, fromint_s(c, VS32(4), 32, 1, 0));",
                 "L_70000d5d2c:;")
code = r'''#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "cpu.h"
static uint32_t bits(float f) { uint32_t u;memcpy(&u,&f,4);return u; }
static float value(uint32_t u) { float f;memcpy(&f,&u,4);return f; }
static void game_scene(float page,unsigned w,unsigned h,unsigned base,unsigned actual[2]) {
    Cpu state={0},*c=&state;
    unsigned char renderer[0xd00]={0};
    uint64_t x19=(uintptr_t)renderer,x8=0,x0=bits(1.0f);
    ST32(x19+0xcc4,bits(page));ST32(x19+0xcc0,bits((float)base));
''' + scale + r'''
    (void)x8;
    float factor=value(LD32(x19+0xccc));
    FP_SETS(0,(float)w);FP_SETS(1,(float)h);
    FP_SETS(2,factor);FP_SETS(3,factor);
    fp_op_s(c,FOP_MUL,0,0,2);fp_op_s(c,FOP_MUL,1,1,3);
    c->v[4]=8;
''' + quantize + r'''
    actual[0]=(unsigned)VSF32(0);actual[1]=(unsigned)VSF32(1);
}
#define RSF_GAME_CONVERSION_CHECK
#include "rsf_resolution_check.c"
'''
path=out/"conversion.c"
path.write_text(f"// {GENERATED_MARKER}: extracted CPU fixture, do not commit.\n"+code, encoding="utf-8")
clang="C:/msys64/ucrt64/bin/clang.exe"
subprocess.run([clang,"-std=gnu11","-O1","-march=x86-64-v3","-Wall","-Werror","-I",str(root/"runtime"),"-I",str(root/"tests"),
                str(path),"C:/msys64/ucrt64/lib/libzstd.a","-o",str(out/"conversion.exe")],check=True)
subprocess.run([str(out/"conversion.exe")],check=True)
subprocess.run([clang,"-std=gnu11","-O2","-g","-Wall","-Werror","-fno-strict-aliasing","-march=x86-64-v3",
                "-D_WIN32_WINNT=0x0A00","-DRT_CPU_LEVEL=3","-I",str(root/"runtime"),
                "-c",str(root/"runtime/resolution.c"),"-o",str(out/"resolution.o")],check=True)
print("Resolution conversion: actual owner + generated conversion blocks passed; whole resolution.c compiled")
