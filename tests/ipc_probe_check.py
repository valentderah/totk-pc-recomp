"""The IPC probe must not use stale or indeterminate replies after a kernel error.

    python tests/ipc_probe_check.py --cc path/to/clang
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ipc.h"
// The probe's expected failure diagnostics are kept off the test's stdout.
static int probe_printf(const char *format, ...) { (void)format; return 0; }
static int probe_putchar(int c) { return c; }
#define printf probe_printf
#define putchar probe_putchar
PRODUCTION
#undef printf
#undef putchar

static uint32_t kernel_result, service_result;
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    (void)pc;
    assert(imm == 0x21);
    c->x[0] = kernel_result;
    if (kernel_result) return;
    uint32_t *m = (uint32_t *)(uintptr_t)c->tpidrro;
    int tipc = (m[0] & 0xffff) >= T_TIPC_BASE;
    memset(m, 0, IPC_MSG_BYTES);
    m[1] = 1u << 31;
    m[2] = service_result ? 0 : 1u << 5;
    uint32_t *p = m + 3;
    if (!service_result) *p++ = 0x1234;
    if (tipc) *p = service_result;
    else {
        p = m + (((p - m) + 3) & ~3);
        p[0] = CMIF_OUT_MAGIC;
        p[2] = service_result;
    }
}

int main(void) {
    Cpu c = {0};
    uint32_t tlr[IPC_MSG_BYTES / 4] = {0};
    c.tpidrro = (uintptr_t)tlr;
    for (int tipc = 0; tipc <= 1; tipc++) {
        failures = 0;
        kernel_result = service_result = 0;
        assert(get_service(&c, 1, "test:u", tipc) == 0x1234 && failures == 0);
        kernel_result = IPC_R_BAD_HANDLE;
        uint32_t h = get_service(&c, 1, "test:u", tipc);
        if (h) fprintf(stderr, "%s kernel error returned stale service handle %#x\n", tipc ? "TIPC" : "CMIF", h);
        assert(h == 0 && failures == 1);
    }
    Reply r;
    kernel_result = service_result = 0;
    assert(cmd(&c, 1, 0, NULL, 0, &r) == 0 && r.nmove == 1 && r.move[0] == 0x1234);
    memset(&r, 0xa5, sizeof r);
    kernel_result = IPC_R_BAD_HANDLE;
    assert(cmd(&c, 1, 0, NULL, 0, &r) == IPC_R_BAD_HANDLE);
    assert(r.result == 0 && r.ncopy == 0 && r.nmove == 0 && r.data == NULL);
    for (unsigned i = 0; i < 8; i++) assert(r.copy[i] == 0 && r.move[i] == 0);
    kernel_result = 0;
    service_result = IPC_R_UNKNOWN_COMMAND;
    assert(cmd(&c, 1, 0, NULL, 0, &r) == IPC_R_UNKNOWN_COMMAND && r.result == service_result);
    assert(r.nmove == 0 && r.data != NULL);
    puts("IPC probe: CMIF/TIPC success and kernel-error replies, reply reuse and service errors passed");
    return 0;
}
'''


def production():
    text = (ROOT / 'runtime/ipc.c').read_text(encoding='utf-8')
    magic = '\n'.join(line for line in text.splitlines() if line.startswith(('#define CMIF_IN_MAGIC', '#define CMIF_OUT_MAGIC')))
    start = text.index('enum { T_CLOSE')
    types = text[start:text.index('};', start) + 2]
    helpers = text[text.index('#define PROBE_FRAMES'):text.index('// A TotK-like renderer:')]
    return magic + '\n' + types + '\n' + helpers


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--cc', default=shutil.which('clang') or r'C:\msys64\ucrt64\bin\clang.exe')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='ipc-probe-') as temp:
        work = Path(temp)
        source = work / 'probe.c'
        source.write_text(HARNESS.replace('PRODUCTION', production()), encoding='utf-8')
        exe = work / 'probe.exe'
        subprocess.run([args.cc, '-O2', '-std=gnu11', '-ftrivial-auto-var-init=pattern',
                        '-I', str(ROOT / 'runtime'), str(source), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
