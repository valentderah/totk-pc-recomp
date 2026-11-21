"""The data cheat VM of runtime/cheats.c and the opcode tables of tools/cheats.py, without game files.

tools/cheats.py: opcode lengths and which opcodes the VM runs (vm_ok), and how a whole cheat is classified.
runtime/cheats.c, compiled into a harness with stubs for the rest of the runtime: the same cheats are classified and
measured opcode by opcode (op_len must give the lengths tools/cheats.py gives), then run on a fake main image and heap:
opcode C0 with every operand type, width and condition, its else and the skip over a failed block (nested, and with
operand words that look like 20000000), and C1-C3.

Run with .venv: python tests/cheats_vm_check.py [--cc <clang.exe>]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import cheats  # noqa: E402

# Issue #973: two cheats for 1.1.0 whose only opcode the VM lacked was C046B400 FFFFFFFF (if r[B] as u32 != -1).
REPORTED = '''
[Zonai Devices 98]
580F0000 04646C60
580F1000 00000138
580F1000 000011A8
300C0000 0000003C
989BF000
540B1000 00000000
C046B400 FFFFFFFF
640F1000 00000000 00000062
21000000
780F0000 00000008
20000000
310C0000
[All Materials Collected 98]
580F0000 04646C60
580F1000 00000138
580F1000 00000F28
300C0000 000001FE
989BF000
540B1000 00000000
C046B400 FFFFFFFF
640F1000 00000000 00000062
21000000
780F0000 00000008
20000000
310C0000
'''

# (words, kind, note substring): every C0 operand type and width, C1-C3, and what stays unsupported.
CLASSIFY = [
    ('C0150010 00000010', 'data', ''),            # X=0 memory + offset, 1 byte
    ('C025011B', 'data', ''),                     # X=1 memory + offset register, 2 bytes
    ('C04512B0 00000010', 'data', ''),            # X=2 register + offset
    ('C08513BC', 'data', ''),                     # X=3 register + offset register
    ('C0151400 00000005', 'data', ''),            # X=4 static, 1 byte: one value word
    ('C0851400 00000001 00000002', 'data', ''),   # X=4 static, 8 bytes: two value words
    ('C04515B0', 'data', ''),                     # X=5 other register
    ('C1030A10', 'data', ''),
    ('C2100C00', 'data', ''),
    ('C3000801', 'data', ''),
    ('C0451600', 'unsupported', 'not supported'),   # X=6 is no operand type
    ('C0351400 00000005', 'unsupported', 'bad width'),
    ('C4000000', 'unsupported', 'not supported'),
    ('FF000000', 'unsupported', 'not supported'),
    ('FF100000', 'unsupported', 'not supported'),
]


class StubMain:
    """No code in main: tools/cheats.py code_writes sees no instruction writes."""

    def in_code(self, off):
        return False


def py_classify(words):
    c = cheats.Cheat('t', False, 1)
    c.words = words
    cheats.classify(StubMain(), c)
    return c.kind, c.note


def words_of(text):
    return [int(w, 16) for w in text.split()]


def check_python():
    for c in cheats.parse(REPORTED, 'mod'):
        kind, note = py_classify(c.words)
        assert (kind, note) == ('data', ''), f'tools/cheats.py: {c.name}: {kind} {note}'
    for text, kind, note in CLASSIFY:
        got = py_classify(words_of(text))
        assert got[0] == kind and note in got[1], f'tools/cheats.py: {text}: {got}, want {kind} {note}'
        if kind == 'data':
            assert [len(w) for _, w in cheats.ops(words_of(text))] == [len(words_of(text))], text
    print('cheats_vm_check: tools/cheats.py ok', flush=True)


HARNESS = r'''
#include <assert.h>
#include "cheats.c"

// ---- the rest of the runtime, as far as cheats.c reaches it
static uint8_t heap[0x1000] __attribute__((aligned(4096)));
static uint8_t image[0x1000] __attribute__((aligned(4096)));
const char *cfg_get(const char *name) { (void)name; return NULL; }
const char *cfg_value(const char *name) { (void)name; return ""; }
void cfg_set(const char *name, const char *value) { (void)name; (void)value; }
const RtConfig *rt_config(void) { static RtConfig c; return &c; }
const RtGuestLayout *mem_layout(void) { static RtGuestLayout l; l.heap.base = (uintptr_t)heap; return &l; }
uint32_t mem_query(uint64_t a, RtMemoryInfo *out) {
    memset(out, 0, sizeof *out);
    if (a < (uintptr_t)heap || a >= (uintptr_t)heap + sizeof heap) return 1;
    out->base = (uintptr_t)heap, out->size = sizeof heap, out->state = 1, out->perm = 3;
    return 0;
}
void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }
void rt_log(const char *fmt, ...) { (void)fmt; }
void prof_write(void) {}
int ldr_count(void) { return 0; }
const RtLoaded *ldr_module(int i) { (void)i; return NULL; }
int rt_patch_set(const char *name) { (void)name; return -1; }
int rt_patch_enabled(int set) { (void)set; return 0; }
void rt_patch_switch(int set, int on) { (void)set; (void)on; }
void input_poll(uint64_t *b, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) { *b = 0, *lx = *ly = *rx = *ry = 0; }
void mods_scan(void) {}
int mods_count(void) { return 0; }
ModInfo *mods_at(int i) { (void)i; return NULL; }
int mods_exefs_pending(uint32_t *key) { (void)key; return 0; }
int plat_exe_path(char *out, size_t cap) { (void)out; (void)cap; return 0; }
PlatFile *plat_file_open(const char *path) { (void)path; return NULL; }
void plat_file_close(PlatFile *f) { (void)f; }
int64_t plat_file_pread(PlatFile *f, void *buf, uint32_t n, uint64_t off) { (void)f; (void)buf; (void)n; (void)off; return -1; }
uint8_t *plat_file_read(const char *path, uint64_t *size) { (void)path; (void)size; return NULL; }
int plat_file_write(const char *path, const void *data, size_t n) { (void)path; (void)data; (void)n; return 0; }
int plat_file_exists(const char *path) { (void)path; return 0; }
int plat_fs_unlink(const char *path) { (void)path; return 0; }
int plat_fs_list(const char *dir, int (*cb)(const char *, int, uint64_t, void *), void *ctx) { (void)dir; (void)cb; (void)ctx; return 0; }
int plat_spawn(const char *const *argv, const char *cwd) { (void)argv; (void)cwd; return 0; }
_Noreturn void plat_exit(int code) { exit(code); }
int plat_mem_state(uint64_t addr, uint64_t limit, uint64_t *end) { (void)addr; *end = limit; return PLAT_PROT_RW; }
int plat_mem_protect(uint64_t addr, uint64_t n, int prot) { (void)addr; (void)n; (void)prot; return 1; }

#include "cases.h"

// The last cheat of text, parsed as cheats_init parses a file.
static Cheat *load(const char *text) {
    char *s = strdup(text);
    parse(s, "t");
    free(s);
    return &g_cheats[g_n - 1];
}

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

// Marks after the condition: 1 at heap+0x100 when it held, else 2 at heap+0x104; 3 at heap+0x108 after the end.
#define MARKS " 04100000 00000100 00000001 21000000 04100000 00000104 00000002 20000000 04100000 00000108 00000003"

// Runs setup + cond + MARKS; want: 1 the condition holds, 0 it fails.
static void cond(const char *setup, const char *op, int want) {
    char text[1024];
    snprintf(text, sizeof text, "[c]\n%s %s%s\n", setup, op, MARKS);
    memset(heap + 0x100, 0, 0x10);
    run(load(text), 0);
    uint32_t held = u32(heap + 0x100), other = u32(heap + 0x104), end = u32(heap + 0x108);
    if (held != (want ? 1u : 0u) || other != (want ? 0u : 2u) || end != 3) {
        fprintf(stderr, "%s %s: marks %u %u %u, want %d\n", setup, op, held, other, end, want);
        abort();
    }
}

int main(void) {
    g_main = (uintptr_t)image, g_main_size = sizeof image, g_code_end = g_main;   // no code: nothing is a code cheat
    for (int k = 0; k < (int)(sizeof cases / sizeof *cases); k++) {
        Cheat *c = load(cases[k].text);
        CheatKind kind = classify(c);
        if (kind != cases[k].kind || !strstr(c->note, cases[k].note)) {
            fprintf(stderr, "classify %s: %d (%s), want %d (%s)\n", cases[k].text, kind, c->note, cases[k].kind, cases[k].note);
            abort();
        }
        for (int i = 0, j = 0; kind == CHEAT_DATA && i < c->n; j++) {
            int len = op_len(c->words + i, c->n - i);
            if (len != cases[k].lens[j]) {
                fprintf(stderr, "op_len %s, opcode %d: %d, tools/cheats.py %d\n", cases[k].text, j, len, cases[k].lens[j]);
                abort();
            }
            i += len;
        }
    }

    char hb[64], hb10[64];   // r[B] = the heap's address, r[B] = the heap + 0x10
    snprintf(hb, sizeof hb, "400B0000 %08X %08X", (unsigned)((uintptr_t)heap >> 32), (unsigned)(uintptr_t)heap);
    snprintf(hb10, sizeof hb10, "400B0000 %08X %08X", (unsigned)((uintptr_t)heap >> 32), (unsigned)((uintptr_t)heap + 0x10));
    uint64_t v = 0x1122334455667705ull;
    memcpy(heap + 0x10, &v, 8);   // heap+0x10: 05 77 66 55 44 33 22 11
    memcpy(image + 0x20, &v, 8);

    // X=0 memory + offset: heap (M=1) and main (M=0), each width
    cond("400A0000 00000000 00000005", "C015A010 00000010", 1);
    cond("400A0000 00000000 00000005", "C016A010 00000010", 0);
    cond("400A0000 00000000 00007705", "C025A010 00000010", 1);
    cond("400A0000 00000000 55667705", "C045A000 00000020", 1);
    cond("400A0000 11223344 55667705", "C085A000 00000020", 1);
    cond("400A0000 11223344 55667706", "C085A000 00000020", 0);
    // X=1 memory + offset register
    cond("400A0000 00000000 55667705 400C0000 00000000 00000010", "C045A11C", 1);
    cond("400A0000 00000000 55667705 400C0000 00000000 00000014", "C045A11C", 0);
    // X=2 register + offset, X=3 register + offset register
    char setup[256];
    snprintf(setup, sizeof setup, "400A0000 00000000 55667705 %s", hb);
    cond(setup, "C045A2B0 00000010", 1);
    cond(setup, "C045A2B0 00000011", 0);
    snprintf(setup, sizeof setup, "400A0000 00000000 55667705 %s 400C0000 00000000 00000010", hb);
    cond(setup, "C045A3BC", 1);
    snprintf(setup, sizeof setup, "400A0000 00000000 00000005 %s 400C0000 00000000 00000010", hb10);
    cond(setup, "C015A3BC", 0);   // heap+0x20 holds 0
    // an address the guest cannot read compares as 0, as dmnt's read leaves its zeroed value
    cond("400A0000 00000000 00000000 400B0000 00000000 00000000", "C045A2B0 00000000", 1);
    cond("400A0000 00000000 00000001 400B0000 00000000 00000000", "C045A2B0 00000000", 0);
    // X=4 static: the register and the value cut to the width
    cond("400A0000 FFFFFFFF FFFFFF05", "C015A400 00000005", 1);
    cond("400A0000 FFFFFFFF FFFFFF05", "C015A400 12345605", 1);
    cond("400A0000 FFFFFFFF FFFF7705", "C025A400 00007705", 1);
    cond("400A0000 FFFFFFFF 55667705", "C045A400 55667705", 1);
    cond("400A0000 00000001 00000002", "C085A400 00000001 00000002", 1);
    cond("400A0000 00000001 00000002", "C085A400 00000000 00000002", 0);
    // X=5 other register, cut to the width
    cond("400A0000 00000000 00000005 400B0000 00000001 00000005", "C045A5B0", 1);
    cond("400A0000 00000000 00000005 400B0000 00000001 00000005", "C085A5B0", 0);
    // the six conditions, register 5 against 4, 5 and 6; condition 0 or 7 never holds
    static const int want[8][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}, {1, 0, 1}, {0, 0, 0}};
    for (int c = 0; c < 8; c++)
        for (int k = 0; k < 3; k++) {
            char op[32];
            snprintf(op, sizeof op, "C04%XA400 %08X", c, 4 + k);
            cond("400A0000 00000000 00000005", op, want[c][k]);
        }
    // a failed block skips a nested C0 with its else and end, and C0 operand words that read 20000000 / 21000000
    cond("400A0000 00000000 00000005",
         "C046A400 00000005 C045A400 00000005 04100000 00000100 00000009 21000000 20000000"
         " C085A400 21000000 20000000 20000000 C045A010 20000000 20000000 C045A2B0 20000000 20000000", 0);
    cond("400A0000 00000000 00000005", "C045A400 00000005 C046A400 00000005 21000000 20000000", 1);

    // C1: save r[A] in slot 3, clear r[A], restore slot 3 into r[B]; clear slot 3 and restore it into r[C]
    load("[s]\n400A0000 00000000 0000002A C1030A10 C10A0030 C10B0300 C1030020 C10C0300"
         " A4A00410 00000110 A4B00410 00000114 A4C00410 00000118\n");
    memcpy(heap + 0x110, "\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 12);
    run(&g_cheats[g_n - 1], 0);
    assert(u32(heap + 0x110) == 0 && u32(heap + 0x114) == 0x2A && u32(heap + 0x118) == 0);
    // C2: save A and B, clear A, restore A only; clear the saved B, restore B
    load("[m]\n400A0000 00000000 00000007 400B0000 00000000 00000008 C2100C00 C2300C00 C2000400 C2200800 C2000800"
         " A4A00410 00000110 A4B00410 00000114\n");
    memset(heap + 0x110, 0xFF, 8);
    run(&g_cheats[g_n - 1], 0);
    assert(u32(heap + 0x110) == 7 && u32(heap + 0x114) == 0);
    // C3: r[1] into static register 0x80, which stays across runs; static register 0x05 (none sets it) into r[2]
    load("[st]\n40010000 00000000 00000033 40020000 00000000 00000044 C3000801 C3000052 A8200410 00000120\n");
    memset(heap + 0x120, 0xFF, 8);
    run(&g_cheats[g_n - 1], 0);
    assert(g_static[0x80] == 0x33 && u64(heap + 0x120) == 0);
    // saved registers start at 0 in each run, like the registers
    load("[z]\nC10A0300 A8A00410 00000128\n");
    memset(heap + 0x128, 0xFF, 8);
    run(&g_cheats[g_n - 1], 0);
    assert(u64(heap + 0x128) == 0);

    puts("cheats_vm_check: runtime/cheats.c ok");
    return 0;
}
'''

KIND = {'data': 'CHEAT_DATA', 'unsupported': 'CHEAT_UNSUPPORTED'}


def cases_h():
    """The C table: every classify case and each reported cheat, with tools/cheats.py's kind and opcode lengths."""
    rows = [(' '.join(f'{w:08X}' for w in c.words), 'data', '') for c in cheats.parse(REPORTED, 'mod')] + CLASSIFY
    out = ['static const struct { const char *text; CheatKind kind; const char *note; int lens[64]; } cases[] = {']
    for text, kind, note in rows:
        try:
            lens = [len(w) for _, w in cheats.ops(words_of(text))]
        except ValueError:
            lens = []
        out.append(f'    {{"[t]\\n{text}\\n", {KIND[kind]}, "{note}", {{{", ".join(map(str, lens)) or "0"}}}}},')
    return '\n'.join(out + ['};', ''])


def check_c(cc):
    with tempfile.TemporaryDirectory(prefix='totk-cheats-') as tmp:
        folder = Path(tmp)
        (folder / 'check.c').write_text(HARNESS)
        (folder / 'cases.h').write_text(cases_h())
        exe = folder / ('check.exe' if os.name == 'nt' else 'check')
        subprocess.run([cc, '-O1', '-g', '-Wall', '-Wextra', '-Wno-unused-function', '-I', str(folder),
                        '-I', str(ROOT / 'runtime'), str(folder / 'check.c'), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--cc', default='clang')
    args = ap.parse_args()
    check_python()
    check_c(args.cc)


if __name__ == '__main__':
    main()
