"""Exercise the bounded context-registry observer against synthetic guest memory."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Registry(unittest.TestCase):
    def test_collision_wrap_unknown_and_bounds(self):
        with tempfile.TemporaryDirectory() as td:
            folder = Path(td)
            source = folder / 'test.c'
            source.write_text(r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
static struct { uintptr_t lo; size_t size; } ranges[8];
static unsigned nr, events;
static int results[16];
static int64_t clock_us;
static void allow(void *p,size_t n) { ranges[nr].lo=(uintptr_t)p; ranges[nr++].size=n; }
int plat_mem_read(uint64_t a,void *out,size_t n) {
    for(unsigned i=0;i<nr;i++) if(a>=ranges[i].lo && n<=ranges[i].size && a-ranges[i].lo<=ranges[i].size-n) {
        memcpy(out,(void *)(uintptr_t)a,n); return 1;
    }
    return 0;
}
int64_t frame_step_world_us(void) { return clock_us; }
void input_context_observe(uint64_t global,uint32_t hash,int status) { assert(hash<16); results[hash]=status; events++; }
void input_context_word_observe(uint64_t global,unsigned index,uint32_t word) { assert(index<4); }
void input_context_entry_observe(uint64_t global,uint32_t hash,uint8_t bit) { assert(bit<128); }
void input_context_tick(void);
uint64_t input_context_activation(void);
static void store(void *p,uint64_t v) { memcpy(p,&v,8); }
int main(void) {
    unsigned char manager[80]={0},table[32]={0};
    uint32_t keys[8]={7,9,0,0,0,0,0,15},capacity=8;
    uint8_t values[8]={5,6,0,0,0,0,0,0};
    uint64_t global=(uintptr_t)manager;
    allow(&global,8);allow(manager,sizeof manager);allow(table,sizeof table);allow(keys,sizeof keys);allow(values,sizeof values);
    store(manager+0x40,(uintptr_t)table);store(table,(uintptr_t)keys);store(table+8,(uintptr_t)values);
    memcpy(table+0x14,&capacity,4);
    uint32_t bits=1u<<5;memcpy(manager+0x30,&bits,4);
    char config[128];snprintf(config,sizeof config,"0x%llx:7:9:11",(unsigned long long)(uintptr_t)&global);
    setenv("TOTK_CONTEXT_FLAGS",config,1);
    setenv("TOTK_CONTEXT_BLOCK_HASH","7",1);
    input_context_tick(); assert(events==3 && results[7]==1 && results[9]==0 && results[11]==-1);
    assert((input_context_activation()&3)==2);
    input_context_tick(); assert(events==3); // repeated poll in the same world tick is throttled.
    bits=0;memcpy(manager+0x30,&bits,4);clock_us=10000;input_context_tick();
    uint64_t clear=input_context_activation();assert((clear&3)==1);
    bits=1u<<5;memcpy(manager+0x30,&bits,4);clock_us=20000;input_context_tick();
    bits=0;memcpy(manager+0x30,&bits,4);clock_us=30000;input_context_tick();
    uint64_t resumed=input_context_activation();assert((resumed&3)==1 && resumed!=clear);
    values[0]=128;clock_us=20000;input_context_tick();assert(results[7]==-1);
    assert((input_context_activation()&3)==0);
    capacity=65537;memcpy(table+0x14,&capacity,4);clock_us=40000;input_context_tick();
    assert(results[7]==-1 && results[9]==-1);
    global=1;clock_us=60000;input_context_tick();assert(results[11]==-1);
    return 0;
}
''')
            executable = folder / 'test'
            subprocess.run(['clang', '-Wall', '-Wextra', '-Wno-unused-parameter', str(source),
                            str(ROOT / 'tools/input_context_flags.c'), '-lpthread', '-o', str(executable)], check=True)
            env = dict(os.environ)
            env.pop('TOTK_CONTEXT_FLAGS', None)
            subprocess.run([str(executable)], env=env, check=True)


if __name__ == '__main__':
    unittest.main()
