"""Exercise the full binding parser and the Controls page's actual writer.

Run with --cc <clang> --out build/<new-directory>. Use --source <checkout>
to demonstrate the same roundtrip on an unchanged checkout. Only the settings
store, logging and deferred-save notification are stubbed; no game files or SDL.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys


STUBS = r'''
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cfg.h"
#include "input_map.h"
enum { DRIVER_CAP = TEXT_LIMIT };
static char values[ACTION_COUNT][8192];
static unsigned char present[ACTION_COUNT];
static unsigned saves;
static int lookup(const char *name) {
    if (!strncmp(name, "TOTK_", 5)) name += 5;
    if (strncmp(name, "BIND_", 5)) return -1;
    for (int a = 0; a < ACTION_COUNT; a++)
        if (!strcmp(name + 5, input_action_name(a))) return a;
    return -1;
}
const char *cfg_get(const char *name) {
    int a = lookup(name);
    return a >= 0 && present[a] ? values[a] : NULL;
}
const char *cfg_value(const char *name) {
    const char *v = cfg_get(name);
    return v && *v ? v : "";
}
double cfg_double(const char *name) { (void)name; return 15; }
int cfg_int(const char *name) { (void)name; return 0; }
void cfg_set(const char *name, const char *value) {
    int a = lookup(name);
    if (a < 0 || strlen(value) >= sizeof values[a]) abort();
    strcpy(values[a], value);
    present[a] = 1;
}
int cfg_reset(const char *const *names, int n) {
    int changed = 0;
    for (int i = 0; i < n; i++) {
        int a = lookup(names[i]);
        if (a >= 0 && present[a]) {
            changed++;
            present[a] = 0;
            values[a][0] = 0;
        }
    }
    return changed;
}
void rt_log(const char *fmt, ...) { (void)fmt; }
static void opt_save_soon(void) { saves++; }
'''

CASES = r'''
static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        exit(1);
    }
    printf("PASS %s\n", what);
}
int main(void) {
    static const char compact[] =
        "key:a,key:b,key:c,key:d,key:e,key:f,key:g,key:h,key:i,key:j,key:k,key:l,key:m,"
        "key:n,key:o,key:p,key:q,key:r,key:s,key:t,key:u,key:v,key:w,key:x,key:y,key:z,"
        "key:space,key:tab,key:lshift,key:lctrl,key:up,key:down,key:left,key:right,key:enter,pad:default";
    uint8_t before_keys[INPUT_BINDING_MAX], after_keys[INPUT_BINDING_MAX];
    uint8_t before_pads[PAD_LAYOUT_COUNT][INPUT_PAD_BINDING_MAX];
    int before_npads[PAD_LAYOUT_COUNT];
    check(strlen(compact) == 251, "compact fixture is 251 bytes");
    cfg_set("TOTK_BIND_A", compact);
    input_map_configure();
    InputMap m = input_map;
    check(input_map_keys_of(&m, B_A, before_keys, INPUT_BINDING_MAX) == 35,
          "initial parser retains all 35 keys");
    for (int l = 0; l < PAD_LAYOUT_COUNT; l++) {
        before_npads[l] = input_map_pads_of(&m, B_A, l, before_pads[l], INPUT_PAD_BINDING_MAX);
        check(before_npads[l] == 1, "initial parser retains default gamepad button");
    }
    input_map_set_key(&m, B_B, 0, KEY_F2);
    write_lines(&m);
    check(saves == 1, "Controls writer requests deferred save");
    printf("Controls saved bind_A bytes: %zu\n", strlen(cfg_get("TOTK_BIND_A")));
    input_map_configure();
    int nk = input_map_keys_of(&input_map, B_A, after_keys, INPUT_BINDING_MAX);
    printf("A keys after unrelated B edit: %d\n", nk);
    check(nk == 35 && !memcmp(before_keys, after_keys, 35),
          "unrelated Controls edit preserves every A key");
    for (int l = 0; l < PAD_LAYOUT_COUNT; l++) {
        uint8_t after_pads[INPUT_PAD_BINDING_MAX];
        int np = input_map_pads_of(&input_map, B_A, l, after_pads, INPUT_PAD_BINDING_MAX);
        check(np == before_npads[l] && !memcmp(before_pads[l], after_pads, (size_t)np),
              "unrelated Controls edit preserves A gamepad bindings");
    }
    uint8_t down[KEY_COUNT] = {0};
    down[KEY_F2] = 1;
    check(input_map_held(down, B_B), "unrelated B edit remains applied");

    char full[DRIVER_CAP];
    input_map_line(&input_map, B_A, full, sizeof full);
    check(strlen(full) == 286, "full formatter retains spaced 286-byte representation");
    struct { unsigned char before; char text[16]; unsigned char after; } small;
    memset(&small, 0xa5, sizeof small);
    input_map_line(&input_map, B_A, NULL, 0);
    input_map_line(&input_map, B_A, small.text, 0);
    for (size_t i = 0; i < sizeof small; i++)
        check(((unsigned char *)&small)[i] == 0xa5, "zero-size output is untouched");
    for (size_t cap = 1; cap <= sizeof small.text; cap++) {
        memset(&small, 0xa5, sizeof small);
        input_map_line(&input_map, B_A, small.text, cap);
        check(small.before == 0xa5 && small.after == 0xa5 && small.text[cap - 1] == 0 &&
                  !memcmp(small.text, full, cap - 1), "tiny output is a terminated prefix");
        for (size_t i = cap; i < sizeof small.text; i++)
            check((unsigned char)small.text[i] == 0xa5, "tiny output respects declared capacity");
    }
    puts("Binding roundtrip checks passed");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True, help='C compiler executable (Clang)')
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.source.resolve()
    out = args.out.resolve()
    if not any(out.is_relative_to(root / name) for name in ('build', 'generated')):
        parser.error('--out must be below the source checkout build/ or generated/')
    if out.exists():
        parser.error('--out must name a new directory')
    header = (root / 'runtime/input_map.h').read_text(encoding='utf-8')
    limit = re.search(r'\bINPUT_BINDING_TEXT_MAX\s*=\s*(\d+)', header)
    text_limit = int(limit.group(1)) if limit else 256
    ui_source = (root / 'runtime/ui/binds_page.cpp').read_text(encoding='utf-8')
    start = ui_source.index('static void write_lines(')
    end = ui_source.index('static void set_note(', start)
    writer = ui_source[start:end]
    out.mkdir(parents=True)
    driver = out / 'input_binding_roundtrip.c'
    driver.write_text(STUBS.replace('TEXT_LIMIT', str(text_limit)) + writer + CASES, encoding='utf-8')
    executable = out / 'input_binding_roundtrip.exe'
    result = subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-I', str(root / 'runtime'),
                             str(root / 'runtime/input_map.c'), str(driver), '-lm', '-o', str(executable)])
    return result.returncode if result.returncode else subprocess.run([str(executable)]).returncode


if __name__ == '__main__':
    sys.exit(main())
