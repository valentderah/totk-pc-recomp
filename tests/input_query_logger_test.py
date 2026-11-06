"""Probe CPU-state preservation and log behavior using synthetic code, without game data."""
import importlib.util
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_input_logger', ROOT / 'tools/build_input_logger.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class Probes(unittest.TestCase):
    def test_returned_contract_bounds(self):
        source = ['void f_1000(Cpu *c) {', '}']
        for contract in ({}, {'path': [1], 'field': 4, 'size': 4},
                         {'path': [0] * 9, 'field': 4, 'size': 4},
                         {'path': [], 'field': -1, 'size': 4},
                         {'path': [], 'field': True, 'size': 4},
                         {'path': [], 'field': 4, 'size': 3}):
            with self.subTest(contract=contract), self.assertRaises(ValueError):
                builder.observed_state(source, 0x1000, 0, 4, returned=contract)

    def test_signature_and_edges(self):
        import numpy as np
        pattern = [0xf9402408, 0xf9400108, 0x9ac12508, 0x12000100, 0xd65f03c0]
        mod = SimpleNamespace(lo=0x1000, words=np.array([0, *pattern, 0, *pattern], dtype='<u4'))
        self.assertEqual(builder.query_candidates(mod), {0x1004: 0x48, 0x101c: 0x48})
        mod.words[4] ^= 1
        self.assertEqual(builder.query_candidates(mod), {0x101c: 0x48})

    def test_result_state_and_logging(self):
        # Only the generated wrapper is under test: the original synthetic function modifies several CPU fields.
        original = ['void f_1000(Cpu *c) {', '    c->x[0] = c->x[1] & 1;',
                    '    c->x[8] ^= c->x[1]; c->nzcv ^= 0xf0000000; c->v[0] ^= c->v[1];', '}']
        wrapped = '\n'.join(builder.observed_function(original, 0x1000))
        baseline = '\n'.join(original).replace('f_1000', 'baseline')
        getter = ['void f_2000(Cpu *c) {', '    c->x[0] = c->x[3]; c->x[2] = 0; c->x[8] ^= 0x123;', '}']
        getter_base = '\n'.join(getter).replace('f_2000', 'getter_baseline')
        getter_wrap = '\n'.join(builder.observed_controller(getter, 0x2000))
        publisher = ['void f_3000(Cpu *c) {',
                     '    ST64(LD64(c->x[0] + 0x28), 0x100000001ull); c->x[0] = 99; c->x[8] ^= 55;', '}']
        publisher_wrap = '\n'.join(builder.observed_outputs(publisher, 0x3000, [0x28, 0x38], experimental=True,
                                                          action_channel=True, position={'path': [0x20], 'offset': 0x10},
                                                          player_state=True, gameplay_bindings=True))
        callback = ['void f_4000(Cpu *c) {', '    if (c->x[0]) ST8(c->x[0] + 24, 2);',
                    '    c->x[0] = 99; c->x[8] ^= 77;', '}']
        state_wrap = '\n'.join(builder.observed_state(callback, 0x4000, 24, 1))
        scalar = [line.replace('f_1000', 'f_5000') for line in original]
        scalar_wrap = '\n'.join(builder.observed_function(scalar, 0x5000, scalar=True))
        returned = ['void f_6000(Cpu *c) {', '    c->x[0] = c->x[1]; c->x[8] ^= 22;', '}']
        return_wrap = '\n'.join(builder.observed_state(returned, 0x6000, 0, 4,
                                                     returned={'path': [0], 'field': 4, 'size': 4}, interaction=True))
        source = ('#include "cpu.h"\n#include "input_player_state.h"\n#include <assert.h>\n' + baseline + '\n' + wrapped + '\n' +
                  getter_base + '\n' + getter_wrap + '\n' + publisher_wrap + '\n' + state_wrap + '\n' + scalar_wrap + '\n' + return_wrap + r'''
void __wrap_input_poll(uint64_t *, int32_t *, int32_t *, int32_t *, int32_t *);
void input_return_field_observe(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, int);
static unsigned player_samples, binding_calls, prompt_calls;
void input_actions_interaction_observe(uint64_t object,int64_t selected,int valid) {
    assert(object && selected==0);assert(valid==(prompt_calls==2));prompt_calls++;
}
void input_actions_bridge_begin(uint64_t site,uint64_t object) { assert(site==0x3000 && object);binding_calls=0; }
uint64_t input_actions_bridge_apply(unsigned phase,uint64_t original) { assert(phase==binding_calls++);return original; }
void input_player_state_frame(uint64_t site,uint64_t hint) { assert(site==0x3000&&hint);player_samples++; }
int64_t frame_step_world_us(void) { return 123456; }
int ikb_foreground(void) { return 1; }
int ikb_down(int key) { (void)key; return 0; }
int menu_is_open(void) { return 0; }
const char *input_key_name(int key) { (void)key; return 0; }
int plat_mem_read(uint64_t address, void *out, size_t n) {
    if (address < 4096) return 0;
    memcpy(out, (void *)(uintptr_t)address, n); return 1;
}
void __real_input_poll(uint64_t *b, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    *b = 8; *lx = 1; *ly = 2; *rx = 3; *ry = 4;
}
int main(void) {
    uint64_t b = 0; int32_t x = 0, y = 0, z = 0, w = 0;
    __wrap_input_poll(&b, &x, &y, &z, &w);
    assert(b == 8 && x == 1 && y == 2 && z == 3 && w == 4);
    for (unsigned i = 0; i < 1000; i++) {
        Cpu a, b; memset(&a, 0xa5, sizeof a); a.x[1] = i & 1; b = a;
        baseline(&a); f_1000(&b); assert(!memcmp(&a, &b, sizeof a));
    }
    // A repeated true query is deduplicated, and changing the caller creates a distinct record.
    Cpu c = {0}; c.x[0] = 123; c.x[1] = 1; f_1000(&c);
    for (unsigned i = 0; i < 20; i++) { c.x[0] = 123; f_1000(&c); }
    c.x[0] = 123; c.x[30] = 44; f_1000(&c);
    unsigned char controller[0x118] = {0};
    ST32((uintptr_t)controller + 0x114, 128); ST32((uintptr_t)controller + 8, 8);
    ST32((uintptr_t)controller + 12, 4); ST32((uintptr_t)controller + 16, 2);
    const char *name = "Player";
    for (unsigned i = 0; i < 2; i++) {
        Cpu a = {0}; a.x[2] = (uintptr_t)&name; a.x[3] = i ? 0 : (uintptr_t)controller;
        Cpu b = a; getter_baseline(&a); f_2000(&b); assert(!memcmp(&a, &b, sizeof a));
    }
    // JSON escaping must preserve unusual names without producing invalid recordings.
    name = "UI\"\\\n";
    c.x[2] = (uintptr_t)&name; c.x[3] = (uintptr_t)controller; f_2000(&c);
    uint64_t mask = 0, held_mask = 0;
    unsigned char outputs[0x40] = {0}; ST64((uintptr_t)outputs + 0x28, (uintptr_t)&mask);
    ST64((uintptr_t)outputs + 0x38, (uintptr_t)&held_mask);
    float actor[8] = {0}; actor[4] = 1.25f; actor[5] = 2.5f; actor[6] = 3.75f;
    ST64((uintptr_t)outputs + 0x20, (uintptr_t)actor);
    Cpu a = {0}; a.x[0] = (uintptr_t)outputs; a.x[30] = 0x44; Cpu d = a;
    outputs_original_3000(&a); mask = 0; f_3000(&d);
    assert(mask == 0x100000001ull && !memcmp(&a, &d, sizeof a));
    assert(player_samples==1 && binding_calls==2);
    for(unsigned i=0;i<2;i++) {
        Cpu before={0}; before.x[0]=i ? 0 : (uintptr_t)outputs; Cpu after=before;
        state_original_4000(&before); f_4000(&after); assert(!memcmp(&before,&after,sizeof before));
    }
    Cpu scalar_a={0}; scalar_a.x[0]=123; scalar_a.x[1]=1; scalar_a.x[30]=55; Cpu scalar_b=scalar_a;
    query_original_5000(&scalar_a); f_5000(&scalar_b); assert(!memcmp(&scalar_a,&scalar_b,sizeof scalar_a));
    uint32_t data[2]={0,37}; uint64_t entry=(uintptr_t)data, empty=0;
    uint64_t results[]={0,8,(uintptr_t)&entry,(uintptr_t)&empty};
    for(unsigned i=0;i<4;i++) {
        Cpu before={0}; before.x[0]=(uintptr_t)outputs; before.x[1]=results[i]; Cpu after=before;
        state_original_6000(&before); f_6000(&after); assert(!memcmp(&before,&after,sizeof before));
    }
    assert(prompt_calls==4);
    // A failed read must not be deduplicated against a valid zero or vice versa.
    input_return_field_observe(0x7000,123,4,0,0,1);
    struct InputPlayerState state={0};state.status=IPS_ABSENT;state.known=IPS_VALID;
    input_player_state_observe(0x8000,0,&state);
    state.status=IPS_READY;state.flags=IPS_VALID;input_player_state_observe(0x8000,1,&state);
    input_player_state_observe(0x8000,1,&state);
    state.status=IPS_UNKNOWN;state.flags=state.known=0;input_player_state_observe(0x8000,0,&state);
    input_return_field_observe(0x7000,123,4,0,0,0);
    input_return_field_observe(0x7000,123,4,0,0,1);
    return 0;
}
''')
        workroot = ROOT / 'build/input-logger/tests'
        workroot.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=workroot) as td:
            work = Path(td)
            cfile = work / 'test.c'
            cfile.write_text(source)
            executable = work / 'test'
            subprocess.run(['clang', '-O2', '-Wall', '-Wextra', '-I', str(ROOT / 'tools'), '-I', str(ROOT / 'runtime'), str(cfile),
                            str(ROOT / 'tools/input_query_logger.c'), str(ROOT / 'tools/input_action_channel.c'),
                            '-lpthread', '-o', str(executable)], check=True)
            log = work / 'queries.jsonl'
            env = dict(os.environ, TOTK_QUERY_LOG=str(log))
            env.pop('TOTK_MASK_CLEAR', None)
            env.pop('TOTK_QUERY_CONTROLLERS', None)
            for key in ('TOTK_JUMP_CHANNEL', 'TOTK_JUMP_SCRIPT', 'TOTK_JUMP_KEY'):
                env.pop(key, None)
            subprocess.run([str(executable)], check=True, env=env)
            rows = [json.loads(line) for line in log.read_text().splitlines()]
            self.assertEqual(rows[0]['type'], 'session')
            states = [r for r in rows if r['type']=='player_state']
            self.assertEqual([r['status'] for r in states], [1,2,0])
            self.assertEqual((states[-1]['samples'],states[-1]['publisher_samples'],states[-1]['poll_samples']), (4,2,2))
            self.assertTrue(any(r['type']=='return_field' and r['id']==4 and r['result']==37 for r in rows))
            self.assertTrue(any(r['type']=='return_field_unavailable' for r in rows))
            self.assertEqual([r['type'] for r in rows if r.get('site')=='0x7000'],
                             ['return_field','return_field_unavailable','return_field'])
            self.assertTrue(any(r['type'] == 'input' and r['buttons'] == 8 for r in rows))
            self.assertTrue(all(r.get('world_us') == 123456 for r in rows if r['type'] != 'session'))
            self.assertEqual({r['id']: r['result'] for r in rows if r['type'] == 'output_mask'},
                             {0x28: 0x100000001, 0x38: 0})
            self.assertEqual([r['position'] for r in rows if r['type'] == 'actor_position'], [[1.25, 2.5, 3.75]])
            self.assertEqual([(r['id'], r['result']) for r in rows if r['type'] == 'object_state' and r['site']=='0x4000'], [(24, 2)])
            self.assertEqual([(r['object'],r['caller'],r['result']) for r in rows if r['type']=='scalar_result'],
                             [('0x7b','0x37',1)])
            q = [r for r in rows if r['type'] == 'query']
            self.assertEqual({r['result'] for r in q}, {0, 1})
            self.assertTrue(any(r['caller'] == '0x2c' for r in q))
            self.assertLess(len(q), 1010)
            filtered_log=work/'filtered-queries.jsonl'
            filtered_env=dict(env,TOTK_QUERY_CONTROLLERS='0',TOTK_QUERY_LOG=str(filtered_log))
            subprocess.run([str(executable)],check=True,env=filtered_env)
            filtered=[json.loads(line) for line in filtered_log.read_text().splitlines()]
            self.assertFalse(any(r['type']=='controller' for r in filtered))
            self.assertTrue(any(r['type']=='player_state' for r in filtered))
            self.assertTrue(any(r['type']=='query' for r in filtered))
            controllers = [r for r in rows if r['type'] == 'controller']
            self.assertTrue(any(r['name'] == 'Player' and r['held'] == 128 and r['pressed'] == 8 and
                                r['released'] == 4 and r['repeat'] == 2 for r in controllers))
            self.assertTrue(any(r['object'] == '0x0' and r['held'] == 0 for r in controllers))
            self.assertTrue(any(r['name'] == 'UI"\\\n' for r in controllers))
            # Existing recordings must never be overwritten. Disabled logging preserves behavior as well.
            saved = log.read_bytes()
            subprocess.run([str(executable)], check=True, env=env)
            self.assertEqual(log.read_bytes(), saved)
            env.pop('TOTK_QUERY_LOG')
            subprocess.run([str(executable)], check=True, env=env)


if __name__ == '__main__':
    unittest.main()
