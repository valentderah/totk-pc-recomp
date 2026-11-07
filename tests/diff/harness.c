// Runs lifted tests and writes the resulting CPU state per case.
// Shared by tests/diff/lift.py, simd.py, atomic.py and func.py; the expected state comes from Unicorn on the Python
// side.
//
// argv: buf.bin cases.bin out.bin image base [image base ...]
// Each image is mapped at its guest address (guest == host), the scratch buffer at BUF.
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "cpu.h"

extern GuestFn tests[];
extern int ntests;

#define BUF 0x7200000000ull
#define BUF_SIZE 0x40000

#define PTR_LO (BUF + 0x8000)
#define PTR_SPAN 0x20000

static int g_stubs;            // In.stubs of the running case
static uint64_t g_stub_h;      // digest of every call's state so far

static uint64_t splitmix(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static uint64_t mix(uint64_t h, uint64_t w) { return (h ^ w) * 0x100000001B3ull; }

// The callee of every call in a function under tests/diff/func.py; Runner.stub there is the same function on the
// Unicorn side. Folds the target, X0-X30, SP and V0-V7 into a digest, so a register the caller left unwritten at the
// call changes it, then sets X0-X18, NZCV and V0-V7 from the digest, so a caller reading a register it held across
// the call sees a value the callee could have left. X0-X7 mostly get pointers into the scratch buffer. The digest
// and the call count go to the top of the scratch buffer, which the memory check covers.
static void rt_stub(Cpu *c, uint64_t target) {
    uint64_t h = mix(g_stub_h, target);
    for (int i = 0; i < 31; i++) h = mix(h, c->x[i]);
    h = mix(h, c->sp);
    for (int i = 0; i < 8; i++) h = mix(mix(h, (uint64_t)c->v[i]), (uint64_t)(c->v[i] >> 64));
    g_stub_h = h;
    for (int i = 0; i < 19; i++) {
        uint64_t s = splitmix(h + i);
        c->x[i] = i < 8 && (s & 3) ? PTR_LO + ((s >> 4) % (PTR_SPAN / 16)) * 16 : s;
    }
    c->nzcv = splitmix(h + 19) >> 60 << 28;
    for (int i = 0; i < 8; i++)
        c->v[i] = (__uint128_t)splitmix(h + 33 + 2 * i) << 64 | splitmix(h + 32 + 2 * i);
    uint64_t *slot = (uint64_t *)(uintptr_t)(BUF + BUF_SIZE - 16);
    slot[0]++;
    slot[1] = h;
}

void rt_dispatch(Cpu *c) {
    if (g_stubs) rt_stub(c, c->pc);
}
void rt_stub_call(Cpu *c, uint64_t target) { rt_stub(c, target); }
void rt_spin_gate(Cpu *c, uint64_t head) { (void)c; (void)head; }
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; }
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; abort(); }
void rt_udf(Cpu *c, uint64_t pc) { (void)c; (void)pc; abort(); }
uint64_t rt_ticks(void) { return 0; }
uint64_t rt_excl_gen;   // the exclusive monitor's store generation (runtime/cpu.h, main.c)
volatile int g_fnprof;
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) { (void)addr; (void)lr; (void)x0; }

static void *map_at(uint64_t addr, size_t len) {
    len = (len + 0xffff) & ~(size_t)0xffff;
#ifdef _WIN32
    void *p = VirtualAlloc((void *)addr, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *p = mmap((void *)addr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
#endif
    if (p != (void *)addr) {
        fprintf(stderr, "map %llx (+%zx) failed\n", (unsigned long long)addr, len);
        exit(1);
    }
    return p;
}

static void *load_at(const char *path, uint64_t addr) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    size_t size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    void *p = map_at(addr, size);
    if (fread(p, 1, size, f) != size) { perror(path); exit(1); }
    fclose(f);
    return p;
}

static uint32_t crc_tab[256];
static uint32_t crc32_buf(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

// Layouts match IN_FMT / OUT_FMT in tests/diff/common.py.
struct In { uint64_t k, fpcr, x[32], sp, nzcv; __uint128_t v[32]; uint64_t memchk, stubs; };
struct Out { uint64_t x[32], sp, nzcv, pc, mem; __uint128_t v[32]; };

int main(int argc, char **argv) {
    if (argc < 6 || (argc - 4) % 2) {
        fprintf(stderr, "usage: %s buf.bin cases.bin out.bin image base [image base ...]\n", argv[0]);
        return 2;
    }
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        crc_tab[i] = c;
    }
    for (int i = 4; i < argc; i += 2) load_at(argv[i], strtoull(argv[i + 1], 0, 16));
    uint8_t *buf = map_at(BUF, BUF_SIZE);
    FILE *bf = fopen(argv[1], "rb");
    if (!bf || fread(buf, 1, BUF_SIZE, bf) != BUF_SIZE) { perror(argv[1]); return 1; }
    fclose(bf);
    uint8_t *pristine = malloc(BUF_SIZE);
    memcpy(pristine, buf, BUF_SIZE);

    FILE *f = fopen(argv[2], "rb"), *o = fopen(argv[3], "wb");
    if (!f || !o) return 1;
    static struct In in;
    static struct Out out;
    static Cpu c;
    while (fread(&in, sizeof in, 1, f) == 1) {
        if (in.k >= (uint64_t)ntests) { fprintf(stderr, "bad test index %llu\n", (unsigned long long)in.k); return 1; }
        memset(&c, 0, sizeof c);
        memcpy(c.x, in.x, sizeof c.x);
        c.sp = in.sp;
        c.nzcv = in.nzcv;
        c.fpcr = in.fpcr;
        rt_fpcr_sync(&c);
        memcpy(c.v, in.v, sizeof c.v);
        c.excl_addr = ~0ull;
        g_stubs = (int)in.stubs;
        g_stub_h = 0;
        tests[in.k](&c);
        memcpy(out.x, c.x, sizeof out.x);
        out.sp = c.sp;
        out.nzcv = c.nzcv & 0xf0000000ull;
        out.pc = c.pc;
        out.mem = 0;
        if (in.memchk) {
            out.mem = crc32_buf(buf, BUF_SIZE);
            memcpy(buf, pristine, BUF_SIZE);
        }
        memcpy(out.v, c.v, sizeof out.v);
        fwrite(&out, sizeof out, 1, o);
        // Flushed per case so a crash leaves the completed cases readable.
        fflush(o);
    }
    fclose(o);
    return 0;
}
