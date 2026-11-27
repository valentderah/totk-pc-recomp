"""Compile the real input map and check Controls key edits.

INPUT_MAP_TEST_SOURCE can name an unchanged input_map.c for a before/after repro.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "input_map.h"
void rt_log(const char *fmt, ...) { (void)fmt; }
const char *cfg_get(const char *name) { (void)name; return NULL; }
double cfg_double(const char *name) { (void)name; return 15; }
int main(int argc, char **argv) {
    input_map_reset();
    if (argc > 1) {
        if (!strcmp(argv[1], "same_key")) {
            InputMap m = input_map;
            input_map_set_key(&m, B_Y, 0, KEY_MOUSE_L);
            uint8_t keys[INPUT_BINDING_MAX];
            int n = input_map_keys_of(&m, B_Y, keys, INPUT_BINDING_MAX);
            printf("Y after choosing its first key again: %d keys (expected 2)\n", n);
            return n != 2 || keys[0] != KEY_MOUSE_L || keys[1] != KEY_T;
        }
        input_map_bind("A", "key:u,key:e,key:f,pad:default");
        InputMap m = input_map;
        input_map_set_key(&m, B_A, 1, KEY_U);
        uint8_t keys[INPUT_BINDING_MAX];
        int n = input_map_keys_of(&m, B_A, keys, INPUT_BINDING_MAX);
        printf("A after moving its own U into slot 2: %d keys (%d,%d), expected U,F\n", n, keys[0], keys[1]);
        return n != 2 || keys[0] != KEY_U || keys[1] != KEY_F;
    }
    input_map_bind("A", "key:u,key:e,pad:default");
    input_map_bind("B", "key:u,key:lshift,pad:default");
    input_map_bind("X", "key:u,key:space,pad:default");
    uint8_t down[KEY_COUNT] = {[KEY_U] = 1};
    if (input_map_buttons(down) != (1ull << B_A | 1ull << B_B | 1ull << B_X)) return 3;
    InputMap original = input_map;
    InputMap edited = input_map;
    int lost = input_map_set_key(&edited, B_Y, 0, KEY_U);
    input_map = edited;
    uint64_t buttons = input_map_buttons(down);
    printf("U after Controls move: 0x%llx (expected 0x%llx), previous action %d\n",
           (unsigned long long)buttons, 1ull << B_Y, lost);
    if (buttons != (1ull << B_Y)) return 1;
    uint8_t keys[INPUT_BINDING_MAX];
    const int actions[] = {B_A, B_B, B_X};
    const int retained[] = {KEY_E, KEY_LSHIFT, KEY_SPACE};
    for (int i = 0; i < 3; i++) {
        int n = input_map_keys_of(&edited, actions[i], keys, INPUT_BINDING_MAX);
        if (n != 1 || keys[0] != retained[i]) return 2;
    }
    if (memcmp(edited.pads, original.pads, sizeof edited.pads) ||
        memcmp(edited.npads, original.npads, sizeof edited.npads)) return 4;
    input_map_set_key(&edited, B_Y, 0, KEY_U);
    if (input_map_keys_of(&edited, B_Y, keys, INPUT_BINDING_MAX) != 2 || keys[0] != KEY_U) return 5;
    input_map = original;
    edited = original;
    input_map_set_key(&edited, B_A, 0, KEY_NONE);
    input_map = edited;
    if (input_map_buttons(down) != (1ull << B_B | 1ull << B_X)) return 6;
    input_map_bind("LSTICK_UP", "key:u,key:w");
    edited = input_map;
    input_map_set_key(&edited, B_Y, 1, KEY_U);
    input_map = edited;
    int32_t stick[2];
    input_map_stick(down, 0, stick);
    if (input_map_buttons(down) != (1ull << B_Y) || stick[0] || stick[1]) return 7;
    if (input_map_keys_of(&edited, B_SL_UP, keys, INPUT_BINDING_MAX) != 1 || keys[0] != KEY_W) return 8;
    input_map_reset();
    edited = input_map;
    if (input_map_set_key(&edited, B_A, 0, KEY_F) != B_ZR) return 9;
    input_map = edited;
    memset(down, 0, sizeof down); down[KEY_F] = 1;
    if (input_map_buttons(down) != (1ull << B_A)) return 10;
    if (input_map_keys_of(&edited, B_ZR, keys, INPUT_BINDING_MAX) != 1 || keys[0] != KEY_X) return 11;
    return 0;
}
'''


class MoveKeyTest(unittest.TestCase):
    def test_key_leaves_every_previous_action(self):
        cc = shutil.which(os.environ.get('CC', r'C:\msys64\ucrt64\bin\clang.exe' if os.name == 'nt' else 'clang'))
        if not cc:
            self.skipTest('C compiler required')
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / 'move.c', Path(tmp) / 'move.exe'
            src.write_text(SOURCE, encoding='utf-8')
            mapping = os.environ.get('INPUT_MAP_TEST_SOURCE', str(ROOT / 'runtime/input_map.c'))
            built = subprocess.run([cc, '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'runtime'),
                                    str(src), mapping, '-o', str(exe),
                                    *([] if os.name == 'nt' else ['-lm'])],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            for args in ([], ['same_key'], ['own_key']):
                with self.subTest(case=args or 'shared'):
                    run = subprocess.run([str(exe), *args], capture_output=True, text=True)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
