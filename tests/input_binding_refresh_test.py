"""Compile the runtime's binding refresh function with configuration stubs.

Run on the test machine with --cc <clang> --out build/<new-directory>.
--source <checkout> can select an unchanged checkout for the before comparison.
The output directory must be new and below that checkout's build/ or generated/.
No SDL, game files or generated game code are needed.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys


STUBS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { ACTION_COUNT = 25, INPUT_BINDING_TEXT_MAX = TEXT_LIMIT };
static unsigned version;
static unsigned applications;
static char values[ACTION_COUNT][9000];
static const char *const actions[ACTION_COUNT] = {
    "A", "B", "X", "Y", "STICK_L", "STICK_R", "L", "R", "ZL", "ZR", "PLUS", "MINUS",
    "LEFT", "UP", "RIGHT", "DOWN", "SL_LEFT", "SL_UP", "SL_RIGHT", "SL_DOWN",
    "SR_LEFT", "SR_UP", "SR_RIGHT", "SR_DOWN", "SHAKE"
};
static unsigned cfg_version(void) { return version; }
static const char *input_action_name(int a) { return actions[a]; }
static const char *cfg_get(const char *name) {
    if (strncmp(name, "TOTK_BIND_", 10)) abort();
    for (int a = 0; a < ACTION_COUNT; a++)
        if (!strcmp(name + 10, actions[a])) return values[a];
    abort();
}
static void input_map_configure(void) { applications++; }
'''

CASES = r'''
static void expect(unsigned want, const char *what) {
    binds_refresh();
    if (applications != want) {
        fprintf(stderr, "FAIL %s: applications=%u expected=%u\n", what, applications, want);
        exit(1);
    }
    printf("PASS %s\n", what);
}
int main(void) {
    strcpy(values[24], "key:t");
    expect(1, "initial configuration");
    expect(1, "unchanged version");
    version++;
    expect(1, "unrelated setting version change");

    /* Each valid value is 181 bytes, longer in aggregate than the old snapshot. */
    for (int a = 0; a < 24; a++) {
        memcpy(values[a], "key:", 4);
        memset(values[a] + 4, ' ', 170);
        strcpy(values[a] + 174, "default");
    }
    version++;
    expect(2, "apply many valid bindings");
    strcpy(values[24], "key:y");
    version++;
    expect(3, "late action changes beyond 4096 aggregate bytes");
    strcpy(values[0], "key:z");
    version++;
    expect(4, "first action changes");
    memset(values, 0, sizeof values);
    version++;
    expect(5, "reset all bindings to empty");

    /* The first parser prefix is valid; unused raw suffixes do not hide SHAKE. */
    strcpy(values[0], "key:default,");
    memset(values[0] + 12, ' ', 8180);
    strcpy(values[0] + 8192, "none");
    strcpy(values[24], "key:t");
    version++;
    expect(6, "raw first value exceeds 4096 bytes");
    strcpy(values[24], "key:y");
    version++;
    expect(7, "late change after oversized raw value");
    if (CHECK_PARSER_TAIL) {
        values[0][8193] = 'N';
        version++;
        expect(7, "ignored tail beyond parser prefix does not reconfigure");
    }
    puts("Binding refresh checks passed");
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
    source = (root / 'runtime/input.c').read_text(encoding='utf-8')
    start = source.index('static unsigned binds_seen')
    end = source.index('// TOTK_TEST_PAD=', start)
    function = source[start:end]
    header = (root / 'runtime/input_map.h').read_text(encoding='utf-8')
    limit = re.search(r'\bINPUT_BINDING_TEXT_MAX\s*=\s*(\d+)', header)
    text_limit = int(limit.group(1)) if limit else 256
    out.mkdir(parents=True)
    driver = out / 'input_binding_refresh.c'
    driver.write_text(STUBS.replace('TEXT_LIMIT', str(text_limit)) + function +
                      CASES.replace('CHECK_PARSER_TAIL', '1' if limit else '0'), encoding='utf-8')
    executable = out / 'input_binding_refresh.exe'
    compile_result = subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', str(driver), '-o', str(executable)])
    if compile_result.returncode:
        return compile_result.returncode
    return subprocess.run([str(executable)]).returncode


if __name__ == '__main__':
    sys.exit(main())
