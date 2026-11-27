"""Compile the actual typed settings readers with minimal value/metadata stubs.

Run with --cc <clang> --out build/<new-directory>. --source <checkout> selects
the unchanged source for the before comparison. No game data or runtime build.
Finite doubles outside a consumer's setting/float range remain accepted here.
"""
import argparse
from pathlib import Path
import subprocess
import sys


STUBS = r'''
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { const char *def; } CfgMeta;
static const CfgMeta chunks = {"0"}, gamma = {"1"};
static const char *selected;
static const CfgMeta *cfg_meta(const char *name) {
    return !strcmp(name, "TOTK_REPLAY_CHUNKS") ? &chunks : &gamma;
}
static const char *cfg_value(const char *name) {
    return selected && *selected ? selected : cfg_meta(name)->def;
}
'''

CASES = r'''
int main(void) {
    static const struct { const char *text; int want; } ints[] = {
        {"2147483648", 0}, {"-2147483649", 0}, {"4294967296", 0},
        {"999999999999999999999999999999999999", 0},
        {"-999999999999999999999999999999999999", 0},
        {"2147483647", 2147483647}, {"-2147483648", -2147483647 - 1},
        {"0", 0}, {"+17", 17}, {" 17", 17}, {"17 ", 0}, {"17x", 0}, {"1.5", 0}, {"+", 0}, {"", 0}
    };
    static const struct { const char *text; double want; } doubles[] = {
        {"inf", 1}, {"-inf", 1}, {"INFINITY", 1}, {"1e999", 1}, {"-1e999", 1}, {"NaN", 1},
        {"1.25e1", 12.5}, {"-2.5e-1", -.25}, {"1e-9999", 0}, {"1e100", 1e100},
        {"1.5x", 1}, {"", 1}
    };
    unsigned failures = 0;
    for (size_t i = 0; i < sizeof ints / sizeof *ints; i++) {
        selected = ints[i].text;
        int got = cfg_int("TOTK_REPLAY_CHUNKS");
        printf("%s int '%s': got=%d expected=%d\n", got == ints[i].want ? "PASS" : "FAIL", selected, got, ints[i].want);
        failures += got != ints[i].want;
    }
    for (size_t i = 0; i < sizeof doubles / sizeof *doubles; i++) {
        selected = doubles[i].text;
        double got = cfg_double("TOTK_COLOR_GAMMA");
        printf("%s double '%s': got=%.17g expected=%.17g\n",
               got == doubles[i].want ? "PASS" : "FAIL", selected, got, doubles[i].want);
        failures += got != doubles[i].want;
    }
    printf("Numeric settings: %u failures\n", failures);
    return failures ? 1 : 0;
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
    source = (root / 'runtime/cfg.c').read_text(encoding='utf-8')
    start = source.index('static double default_number(')
    end = source.index('int cfg_bool(', start)
    out.mkdir(parents=True)
    driver = out / 'numeric_settings.c'
    driver.write_text(STUBS + source[start:end] + CASES, encoding='utf-8')
    executable = out / 'numeric_settings.exe'
    result = subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', str(driver), '-o', str(executable)])
    return result.returncode if result.returncode else subprocess.run([str(executable)]).returncode


if __name__ == '__main__':
    sys.exit(main())
