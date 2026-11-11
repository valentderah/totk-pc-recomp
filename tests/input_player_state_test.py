"""Player state reads against synthetic guest memory: identity, partial failure, classification and read-only behavior."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class StateReader(unittest.TestCase):
    def test_guarded_reader(self):
        source = r'''
#include "input_player_state.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static unsigned char memory[0x10000], before[0x10000];
static uint64_t fail_at;
static unsigned root_reads, swap_identity;
static void put(uint64_t p, uint64_t value, size_t n) { memcpy(memory+p-0x10000,&value,n); }
int plat_mem_read(uint64_t p, void *out, size_t n) {
    if (p<0x10000 || p>=0x20000 || n>0x20000-p || (fail_at && p<=fail_at && fail_at-p<n)) return 0;
    if (p==0x10000 && ++root_reads==2 && swap_identity) {
        put(0x10258,8,4); put(0x10320,8,4);
    }
    memcpy(out,memory+p-0x10000,n); return 1;
}
static const uint32_t body[]={0,IPS_AIR,IPS_CLIMB|IPS_AIR,IPS_SWIM,IPS_GLIDE,0};
static const uint32_t upper[]={0,0,IPS_ABILITY,IPS_GUARD,IPS_AIM,0};
static struct InputPlayerLayout l={
    .main_start=0x100000,.main_end=0x110000,.global_slot=0x10000,.site=123,
    .root_link=64,.link_data=8,.link_id=24,.unit_proc=16,.unit_id=32,.proc_state=24,
    .actor_components=32,.as_component=8,.player_component=16,.attention_component=24,.power_component=32,
    .as_slots=64,.slot_verb=16,.slot_alternate=20,.invalid_verb=5,.verb_count=6,
    .state_bytes=64,.swim_mask=16,.climb_mask=256,.attention_state=16,.power_current=16,.power_hand=24,.hand_state=16,.holding_ability=1,
    .body_known=IPS_AIR|IPS_CLIMB|IPS_SWIM|IPS_GLIDE,.any_known=IPS_AIM|IPS_GUARD|IPS_ABILITY,
    .body_flags=body,.any_flags=upper
};
const struct InputPlayerLayout *input_player_state_config(void) { return &l; }
static void obj(uint64_t p) { put(p,0x100080,8); }
static void setup(void) {
    memset(memory,0,sizeof memory); fail_at=0;root_reads=0;swap_identity=0;
    put(0x10000,0x10100,8);put(0x10100,0x10200,8);put(0x10248,0x10300,8);put(0x10258,7,4);
    put(0x10310,0x10400,8);put(0x10320,7,4);obj(0x10400);put(0x10420,0x10800,8);
    put(0x10808,0x10c00,8);put(0x10810,0x11800,8);put(0x10818,0x11c00,8);put(0x10820,0x12000,8);
    obj(0x10c00);obj(0x11800);obj(0x11c00);obj(0x12000);obj(0x12400);put(0x12018,0x12400,8);
    for(unsigned i=0;i<5;i++){uint64_t p=0x11000+i*256;obj(p);put(0x10c40+8*i,p,8);}
}
int main(void) {
    struct InputPlayerState s;
    setup();memcpy(before,memory,sizeof memory);input_player_state_read(&l,0x10400,&s);
    assert(!memcmp(before,memory,sizeof memory));assert(s.status==IPS_READY&&s.identity==7&&s.slots_known==31);
    assert(s.flags==IPS_VALID&&s.ability==0&&(s.known&l.body_known)==l.body_known);
    put(0x11000,0,8);input_player_state_read(&l,0,&s);assert(s.slots_known==31);
    put(0x11010,1,4);put(0x11110,4,4);input_player_state_read(&l,0,&s);
    assert((s.flags&(IPS_AIR|IPS_AIM))==(IPS_AIR|IPS_AIM));assert(!(s.flags&IPS_GLIDE));
    put(0x11010,5,4);put(0x11014,2,4);input_player_state_read(&l,0,&s);
    assert(s.verbs[0]==2&&(s.flags&IPS_CLIMB));
    put(0x11010,0,4);put(0x11840,16|256,2);put(0x11c10,3,4);put(0x12010,1,4);put(0x12410,5,4);
    input_player_state_read(&l,0,&s);assert((s.flags&(IPS_CLIMB|IPS_SWIM|IPS_LOCKON|IPS_HOLDING))==
        (IPS_CLIMB|IPS_SWIM|IPS_LOCKON|IPS_HOLDING));assert(!(s.flags&IPS_ABILITY)&&s.ability==1);
    put(0x12010,2,4);input_player_state_read(&l,0,&s);assert(!(s.flags&IPS_HOLDING)&&(s.known&IPS_HOLDING));
    const uint32_t uncertain[]={0,IPS_AIR,0,0,0,0};l.body_unknown=uncertain;
    put(0x11010,1,4);input_player_state_read(&l,0,&s);assert(!(s.known&IPS_AIR));l.body_unknown=0;
    setup();fail_at=0x11840;input_player_state_read(&l,0,&s);
    assert(s.status==IPS_READY&&!(s.known&(IPS_SWIM|IPS_CLIMB)));
    setup();fail_at=0x11010;input_player_state_read(&l,0,&s);
    assert(s.verbs[0]==-1&&!(s.known&IPS_AIR));
    setup();put(0x10c48,0,8);input_player_state_read(&l,0,&s);assert(s.verbs[1]==0&&s.slots_known==31);
    setup();put(0x11010,99,4);input_player_state_read(&l,0,&s);assert(s.verbs[0]==-1&&!(s.known&IPS_AIR));
    setup();input_player_state_read(&l,0x10500,&s);assert(s.status==IPS_UNKNOWN&&!s.known);
    setup();put(0x10320,8,4);input_player_state_read(&l,0,&s);assert(s.status==IPS_UNKNOWN);
    setup();put(0x10418,7,4);input_player_state_read(&l,0,&s);assert(s.status==IPS_ABSENT&&s.known==IPS_VALID);
    setup();put(0x10100,0,8);input_player_state_read(&l,0,&s);assert(s.status==IPS_ABSENT);
    setup();fail_at=0x10100;input_player_state_read(&l,0,&s);assert(s.status==IPS_UNKNOWN&&!s.flags&&!s.known);
    setup();swap_identity=1;input_player_state_read(&l,0,&s);assert(s.status==IPS_UNKNOWN);


    l.global_slot=UINT64_MAX;input_player_state_read(&l,0,&s);assert(s.status==IPS_UNKNOWN);
    puts("player state synthetic checks passed");return 0;
}
'''
        workroot = ROOT / 'build/tests/independent-controls'
        workroot.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=workroot) as td:
            work = Path(td)
            (work / 'test.c').write_text(source)
            executable = work / 'test'
            subprocess.run(['clang', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'runtime'),
                            str(work / 'test.c'), str(ROOT / 'runtime/input_player_state.c'), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    unittest.main()
