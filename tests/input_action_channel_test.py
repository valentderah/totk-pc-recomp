"""Validate independent action edges, context entry and opt-in injection using synthetic providers."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Channel(unittest.TestCase):
    def test_edges_provider_and_masks(self):
        workroot = ROOT / 'build/input-logger/tests'
        workroot.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=workroot) as td:
            folder = Path(td)
            source = folder / 'test.c'
            source.write_text(r'''
#include "input_action_channel.h"
#include "input_map.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static int64_t clock_us;
static int foreground=1, down, menu, mode;
static uint64_t activation;
uint64_t input_context_activation(void) { return activation; }
int64_t frame_step_world_us(void) { return clock_us; }
int ikb_foreground(void) { return foreground; }
int ikb_down(int k) { return down; }
int menu_is_open(void) { return menu; }
const char *input_key_name(int k) { return k == KEY_F2 ? "f2" : "other"; }
void input_action_observe(uint64_t a,uint64_t b,int p,int h,int r) {}
static void frame(int64_t t) {
    clock_us=t;
    if (mode==2) down=(t>=3500000 && t<3680000) || (t>=4500000 && t<6500000);
    if (mode==3 && t>=3600000) activation=(1ull<<63)|(3ull<<2)|1;
    input_action_channel_begin(4096,8192);
}
int main(int argc,char **argv) {
    mode=atoi(argv[1]);
    if(mode==3) activation=(1ull<<63)|(1ull<<2)|1;
    ResearchActionState s={0};
    research_action_update(&s,0,1,1,1); assert(!s.held && !s.pressed);
    research_action_update(&s,10000,1,0,1); assert(!s.held);
    research_action_update(&s,20000,1,1,1); assert(s.pressed && s.held);
    research_action_update(&s,20000,1,1,1); assert(s.pressed && s.held);
    research_action_update(&s,30000,1,1,1); assert(!s.pressed && s.held);
    research_action_update(&s,40000,1,0,1); assert(s.released && !s.held);
    research_action_update(&s,500000,1,1,1); assert(!s.pressed && !s.held);
    research_action_update(&s,510000,1,0,1);
    research_action_update(&s,520000,1,1,1); assert(s.pressed);
    research_action_update(&s,530000,2,1,1); assert(!s.pressed && !s.held && !s.released);
    research_action_update(&s,540000,2,0,1);
    research_action_update(&s,550000,2,1,0); assert(!s.held);
    research_action_update(&s,560000,2,1,1); assert(!s.pressed && !s.held);
    research_action_update(&s,570000,2,0,1);
    research_action_update(&s,580000,2,1,1); assert(s.pressed);
    frame(3000000);
    if (mode == 0) {
        assert(input_action_channel_apply(4096,40,0x100000003ull)==0x100000003ull);
        return 0;
    }
    assert(input_action_channel_apply(4096,40,0x100000003ull)==0x100000002ull);
    for (int64_t t=3050000;t<=3500000;t+=50000) frame(t);
    assert(input_action_channel_apply(4096,40,0x100000002ull)==0x100000003ull);
    if(mode==4) {
        assert(input_action_channel_apply(4096,56,0)==0);
        assert(input_action_channel_apply(4096,56,1)==1);
        assert(input_action_channel_apply(4096,72,0)==0);
        return 0;
    }
    assert(input_action_channel_apply(4096,56,0)==1);
    assert(input_action_channel_apply(4096,72,1)==0);
    assert(input_action_channel_apply(8192,40,9)==9);
    assert(input_action_channel_apply(4096,80,9)==9);
    frame(3550000); assert(input_action_channel_apply(4096,40,1)==0);
    frame(3600000);
    if(mode==3) { assert(input_action_channel_apply(4096,40,1)==0); assert(input_action_channel_apply(4096,56,1)==0); return 0; }
    frame(3650000); frame(3700000);
    assert(input_action_channel_apply(4096,72,0)==1);
    // A pulse already held when a consumer resumes after inactivity must not create a fresh jump.
    frame(5000000); assert(input_action_channel_apply(4096,40,1)==0);
    assert(input_action_channel_apply(4096,56,1)==0);
    return 0;
}
''')
            executable = folder / 'test'
            subprocess.run(['clang', '-Wall', '-Wextra', '-Wno-unused-parameter', '-I', str(ROOT / 'tools'),
                            '-I', str(ROOT / 'runtime'), str(source), str(ROOT / 'tools/input_action_channel.c'),
                            '-lpthread', '-o', str(executable)], check=True)
            script = folder / 'jump.txt'; script.write_text('3500 180\n4500 2000\n')
            invalid = folder / 'invalid.txt'; invalid.write_text('3500 malformed\n')
            base = {k: v for k, v in os.environ.items() if not k.startswith('TOTK_JUMP_') and k != 'TOTK_ACTION_PHASES'}
            cases = [({}, 0), ({'TOTK_JUMP_CHANNEL': '4096:40:56:72:1', 'TOTK_JUMP_SCRIPT': str(script)}, 1),
                     ({'TOTK_JUMP_CHANNEL': '4096:40:56:72:1', 'TOTK_JUMP_KEY': 'f2'}, 2),
                     ({'TOTK_JUMP_CHANNEL': '4096:40:56:72:1', 'TOTK_JUMP_SCRIPT': str(script)}, 3),
                     ({'TOTK_JUMP_CHANNEL': '4096:40:56:72:1', 'TOTK_JUMP_SCRIPT': str(script), 'TOTK_ACTION_PHASES': '1'}, 4),
                     ({'TOTK_JUMP_CHANNEL': '4096:40:56:72:1', 'TOTK_JUMP_SCRIPT': str(script), 'TOTK_ACTION_PHASES': '8'}, 0),
                     ({'TOTK_JUMP_CHANNEL': '4096:40:56:72:1', 'TOTK_JUMP_SCRIPT': str(script), 'TOTK_ACTION_PHASES': '1garbage'}, 0),
                     ({'TOTK_JUMP_CHANNEL': '4096:40:56:72:3', 'TOTK_JUMP_SCRIPT': str(script)}, 0),
                     ({'TOTK_JUMP_CHANNEL': '4096:40:40:72:1', 'TOTK_JUMP_SCRIPT': str(script)}, 0),
                     ({'TOTK_JUMP_CHANNEL': '4096:40:56:72:1', 'TOTK_JUMP_SCRIPT': str(invalid)}, 0),
                     ({'TOTK_JUMP_CHANNEL': '4096:40:56:72:1', 'TOTK_JUMP_KEY': 'bad-key'}, 0)]
            for env, enabled in cases:
                with self.subTest(env=env):
                    subprocess.run([str(executable), str(enabled)], env=dict(base, **env), check=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)


if __name__ == '__main__':
    unittest.main()
