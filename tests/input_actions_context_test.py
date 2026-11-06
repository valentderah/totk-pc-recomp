"""Production pause registry: collision wrap, invalid reads, generation changes and unsupported-game fallback."""
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
class Context(unittest.TestCase):
    def setUp(self): (ROOT/'build/input-logger/tests').mkdir(parents=True,exist_ok=True)
    def test_guarded_registry(self):
        code=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "input_actions_backend.h"
#include "platform/platform.h"
static uint64_t slot;
static int supported=1;
static struct {uintptr_t at;size_t size;} ranges[8];static unsigned count,reads;
void plat_mutex_lock(PlatMutex *m){(void)m;}
void plat_mutex_unlock(PlatMutex *m){(void)m;}
int input_actions_game_supported(void){return supported;}
uint64_t input_actions_registry_slot(void){return slot;}
uint32_t input_actions_pause_hash(void){return 7;}
int plat_mem_read(uint64_t at,void *out,size_t n){
 reads++;
 for(unsigned i=0;i<count;i++)if(at>=ranges[i].at&&n<=ranges[i].size&&at-ranges[i].at<=ranges[i].size-n){
  memcpy(out,(void *)(uintptr_t)at,n);return 1;}
 return 0;
}
static void allow(void *p,size_t n){ranges[count].at=(uintptr_t)p;ranges[count++].size=n;}
static void put(void *at,uint64_t v){memcpy(at,&v,8);}
int main(void){
 unsigned char manager[80]={0},table[32]={0};uint32_t keys[8]={7,9,0,0,0,0,0,15};
 uint8_t values[8]={5,6,0,0,0,0,0,0};uint32_t capacity=8,bits=1<<5;
 uint64_t global=(uintptr_t)manager;slot=(uintptr_t)&global;
 allow(&global,8);allow(manager,sizeof manager);allow(table,sizeof table);allow(keys,sizeof keys);allow(values,sizeof values);
 put(manager+0x40,(uintptr_t)table);put(table,(uintptr_t)keys);put(table+8,(uintptr_t)values);
 memcpy(table+0x14,&capacity,4);memcpy(manager+0x30,&bits,4);
 assert(input_context_flag(7)==1&&input_context_flag(9)==0&&input_context_flag(11)==-1);
 assert((input_context_activation()&3)==2);
 bits=0;memcpy(manager+0x30,&bits,4);uint64_t clear=input_context_activation();assert((clear&3)==1);
 bits=1<<5;memcpy(manager+0x30,&bits,4);assert((input_context_activation()&3)==2);
 bits=0;memcpy(manager+0x30,&bits,4);assert(input_context_activation()!=clear);
 values[0]=128;assert(input_context_flag(7)==-1&&(input_context_activation()&3)==0);
 capacity=65537;memcpy(table+0x14,&capacity,4);assert(input_context_flag(9)==-1);
 global=1;assert(input_context_flag(7)==-1);
 supported=0;unsigned before=reads;assert(input_context_flag(7)==-1&&input_context_activation()==0&&reads==before);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(dir=ROOT/'build/input-logger/tests') as td:
            p=Path(td);(p/'test.c').write_text(code)
            subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-I',str(ROOT/'runtime'),str(p/'test.c'),
                            str(ROOT/'runtime/input_actions_context.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
if __name__=='__main__':unittest.main()
