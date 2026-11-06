"""Production adapter validation, native fallback and Cpu-preserving hooks, without game files."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('production_actions',ROOT/'recomp/input_actions.py')
adapter=importlib.util.module_from_spec(spec);spec.loader.exec_module(adapter)

class FakeModule:
    def __init__(self):
        self.name='main';self.base=0x100000000;self.hi=self.base+0x2800000
        image=bytearray(0x3ae0000)
        for off in adapter.GUARD_CALLS:struct.pack_into('<I',image,off,0x52800041)
        struct.pack_into('<II',image,adapter.GUARD_BYTE,0x39400108,0x37000008|(2<<19))
        required=set(adapter.NATIVE)|{'Ride','UseSpecialPower','Manipulate'}
        for group in [*adapter.BODY_GROUPS.values(),*adapter.ANY_GROUPS.values()]:required.update(group.split())
        names=['None','Walk','WalkBackwards']+sorted(required-{'None','Walk','WalkBackwards'})
        names += ['SyntheticVerb'+str(i) for i in range(284-len(names))]
        rel=[]
        for i,name in enumerate(names):
            at=0x3200000+128*i;value=name.encode()+b'\0';image[at:at+len(value)]=value
            rel.append((0x3100000+8*i,1027,0,at))
        self.image=bytes(image);self.nso=SimpleNamespace(relocs=rel)
        self.starts=sorted(self.base+n for n in [0x100,adapter.PUBLISHER,adapter.INTERACTION,
                                               *adapter.GUARD_CALLS,adapter.GUARD_BYTE,adapter.GUARD_BYTE+4])
    def read(self,at,n):return int.from_bytes(self.image[at-self.base:at-self.base+n],'little')
    def in_text(self,at):return self.base<=at<self.hi and not at%4
    def in_image(self,at,n=1):return self.base<=at and at+n<=self.base+len(self.image)

class Codegen(unittest.TestCase):
    def setUp(self): (ROOT/'build/input-logger/tests').mkdir(parents=True,exist_ok=True)
    @classmethod
    def setUpClass(cls):cls.module=FakeModule()
    def configure(self,module,patched=()):
        with tempfile.TemporaryDirectory(dir=ROOT/'build/input-logger/tests') as td:
            p=Path(td)/'synthetic-nso';p.write_bytes(b'authored test fixture')
            expected={'exefs_sha256':{'main':hashlib.sha256(p.read_bytes()).hexdigest()}}
            with mock.patch.object(adapter.json,'loads',return_value=expected):
                return adapter.discover(module,p,module.starts,patched)
    def test_verified_profile_extracts_names_and_guard_rewrites(self):
        config=self.configure(self.module)
        self.assertTrue(config['supported'],config)
        self.assertEqual(len(config['names']),284)
        self.assertEqual(len(config['guards']),8)
        for off,before,after in config['guards']:
            self.assertEqual(self.module.read(self.module.base+off,4),before)
            self.assertNotEqual(before,after)
    def test_overlapping_patch_disables_entire_adapter(self):
        config=self.configure(self.module,[self.module.base+adapter.PUBLISHER])
        self.assertFalse(config['supported']);self.assertIn('overlaps',config['reason'])
    def test_bad_guard_instruction_disables_adapter(self):
        mod=copy.copy(self.module);image=bytearray(mod.image)
        struct.pack_into('<I',image,adapter.GUARD_CALLS[0],0xd503201f);mod.image=bytes(image)
        config=self.configure(mod)
        self.assertFalse(config['supported']);self.assertIn('MOV',config['reason'])
    def test_unrecognized_executable_has_no_hooks(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build/input-logger/tests') as td:
            p=Path(td)/'other';p.write_bytes(b'not the supported executable')
            config=adapter.discover(self.module,p,self.module.starts,{})
        self.assertFalse(config['supported'])
        lines=['void f_123(Cpu *c) {','}']
        self.assertEqual(adapter.wrap_function(lines,0x123,config),lines)
        self.assertIs(adapter.shadow_module(self.module,config),self.module)
    def test_supported_and_fallback_configuration_compile(self):
        configs=[{'supported':False},self.configure(self.module)]
        with tempfile.TemporaryDirectory(dir=ROOT/'build/input-logger/tests') as td:
            p=Path(td);src=p/'test.c'
            for config in configs:
                with self.subTest(supported=config['supported']):
                    src.write_text('#include "input_actions.h"\n#include "input_player_state.h"\n#include <assert.h>\n'+
                                   adapter.header(config,'synthetic output')+
                                   'int main(void){assert(input_actions_game_supported()=='+str(int(config['supported']))+');return 0;}\n')
                    subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'runtime'),str(src),'-o',str(p/'test')],check=True)
                    subprocess.run([str(p/'test')],check=True)
    def test_hooks_preserve_complete_cpu_result_without_telemetry(self):
        config={'supported':True,'base':0}
        pub=adapter.PUBLISHER;prompt=adapter.INTERACTION
        native=[f'void f_{pub:x}(Cpu *c) {{',
                'ST64(LD64(c->x[0]+0x28),0x100);c->x[0]=99;c->x[8]^=55;','}']
        getter=[f'void f_{prompt:x}(Cpu *c) {{','c->x[0]=c->x[1];c->x[8]^=22;','}']
        code='\n'.join(adapter.wrap_function(native,pub,config)+adapter.wrap_function(getter,prompt,config))
        self.assertNotIn('input_query_observe',code);self.assertNotIn('input_output_observe',code)
        harness=r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
typedef struct {uint64_t x[32];unsigned char rest[1024];} Cpu;
#define LD64(a) (*(uint64_t *)(uintptr_t)(a))
#define ST64(a,v) (LD64(a)=(v))
static uint64_t object;static unsigned begun,phases,observed;
void input_actions_bridge_begin(uint64_t site,uint64_t value){(void)site;assert(value==object);begun++;}
uint64_t input_actions_bridge_apply(unsigned phase,uint64_t value){assert(phase==phases++);return value|8;}
int plat_mem_read(uint64_t at,void *out,size_t n){memcpy(out,(void *)(uintptr_t)at,n);return 1;}
void input_actions_interaction_observe(uint64_t value,int64_t selected,int valid){assert(value==object&&selected==42&&valid);observed++;}
'''+code+f'''
int main(void){{
Cpu c,expected;memset(&c,0x5a,sizeof c);uint64_t values[3]={{0}};uint64_t fields[16]={{0}};
for(int i=0;i<3;i++)fields[5+2*i]=(uintptr_t)&values[i];object=(uintptr_t)fields;
c.x[0]=object;expected=c;expected.x[0]=99;expected.x[8]^=55;
f_{pub:x}(&c);assert(!memcmp(&c,&expected,sizeof c));assert(begun==1&&phases==3&&values[0]==0x108);
unsigned char target[64]={{0}};uint64_t returned=(uintptr_t)target;int32_t selected=42;memcpy((char *)fields+0x38,&selected,4);
c.x[0]=object;c.x[1]=(uintptr_t)&returned;expected=c;expected.x[0]=c.x[1];expected.x[8]^=22;
f_{prompt:x}(&c);assert(!memcmp(&c,&expected,sizeof c)&&observed==1);return 0;}}
'''
        with tempfile.TemporaryDirectory(dir=ROOT/'build/input-logger/tests') as td:
            p=Path(td);(p/'test.c').write_text(harness)
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)

if __name__=='__main__':unittest.main()
