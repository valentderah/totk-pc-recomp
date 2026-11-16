"""Compile the actual optional provider-fact logger against the pinned SDK and a controlled pre-query header surface."""
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1]
out=root/'build/checks/fsr4-info-worker';out.mkdir(parents=True,exist_ok=True)
source=(root/'runtime/rsf_sr.c').read_text(encoding="utf-8")
helper=source[source.index('static void split_info_log('):source.index('int rsf_sr_open_split(')]
code=r'''
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "rsf_sr.h"
unsigned rt_trace=16;
enum {RT_TRACE_GFX=16};
static char message[512];
static int calls,missing,result;
void rt_log(const char *format,...) {
    va_list ap;va_start(ap,format);vsnprintf(message,sizeof message,format,ap);va_end(ap);
}
#if defined(RSF_SRVK_FSR4_INFO_ABI_VERSION)
static int32_t query(const rsf_fsr4vk *owner,rsf_srvk_fsr4_provider_info *info) {
    assert(owner==(void *)7 && info->struct_size==sizeof *info && info->abi_version==1);calls++;
    info->provider_version=0x123456789abcdef0ull;info->adapter_luid=0x23456789abcdef01ull;
    info->vendor_id=0x10de;info->int8_compatibility=1;return result;
}
#endif
void *rsf_runtime_symbol(const char *name) {
    assert(!strcmp(name,"rsf_srvk_fsr4_info"));
#if defined(RSF_SRVK_FSR4_INFO_ABI_VERSION)
    return missing?NULL:query;
#else
    return NULL;
#endif
}
'''+helper+r'''
int main(void) {
    split_info_log((void *)7);
#if defined(RSF_SRVK_FSR4_INFO_ABI_VERSION)
    assert(calls==1 && strstr(message,"123456789abcdef0") && strstr(message,"23456789abcdef01"));
    assert(strstr(message,"vendor 0x10de INT8 compatibility 1"));
    missing=1;split_info_log((void *)7);
    assert(calls==1 && strstr(message,"unknown") && !strstr(message,"123456789abcdef0"));
    missing=0;result=-71;split_info_log((void *)7);
    assert(calls==2 && strstr(message,"unknown (info query -71)") && !strstr(message,"123456789abcdef0"));
#else
    assert(!calls && strstr(message,"unknown (info query absent from SDK headers)"));
    missing=result=0;
#endif
    rt_trace=0;message[0]=0;split_info_log((void *)7);assert(!message[0]);
    return 0;
}
'''
fixture=out/'provider-info.c';fixture.write_text('// '+'TOTK_RECOMP_'+'GENERATED_FROM_GAME_CODE\n'+code)
sdk=root/'external/ReScaleFrame'
legacy=out/'without-info-header'
(legacy/'rescaleframe').mkdir(parents=True,exist_ok=True)
header=(sdk/'runtime/orchestrator/include/rescaleframe/native_sr_split_vulkan.h').read_text(encoding='utf-8')
# The optional query is an appended declaration block. Preserve the prior API
# surface to test callers built without it, independently of a sibling checkout.
assert '/* Optional additive query.' in header
(legacy/'rescaleframe/native_sr_split_vulkan.h').write_text(header.split('/* Optional additive query.')[0],encoding='utf-8')
for variant,overlay in [('pinned',[]),('without-info',['-I',str(legacy)])]:
    flags=['-std=gnu11','-O2','-Wall','-Werror','-I',str(root/'runtime'),*overlay]
    for directory in ('runtime/orchestrator/include','runtime/contract/include','sdk/game/include'):
        flags+=['-I',str(sdk/directory)]
    compiler='C:/msys64/ucrt64/bin/clang.exe'
    subprocess.run([compiler,*flags,str(fixture),'-o',str(out/(variant+'.exe'))],check=True)
    subprocess.run([str(out/(variant+'.exe'))],check=True)
    subprocess.run([compiler,*flags,'-c',str(root/'runtime/rsf_sr.c'),'-o',str(out/('rsf-sr-'+variant+'.o'))],check=True)
    subprocess.run([compiler,*flags,str(root/'tests/rsf_sr_lifetime_check.c'),str(root/'runtime/rsf_sr.c'),'-o',str(out/('lifetime-'+variant+'.exe'))],check=True)
    subprocess.run([str(out/('lifetime-'+variant+'.exe'))],check=True)
print('Optional FSR4 provider facts: actual logger, whole adapter and lifetime with pinned and pre-query SDK surfaces passed (CPU only)')
