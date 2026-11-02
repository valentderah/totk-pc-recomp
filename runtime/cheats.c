// Cheats of the mods' Atmosphere cheat files (cheats.h). Each enabled mod the port can use (mods.h) may hold
// <build id>.txt in its cheats folder, the build id the first 8 bytes of main's NSO build id, as Atmosphere and the
// emulators name it; the file name's case does not matter. Each cheat's switch is the setting TOTK_CHEAT_<hash>
// (cheat_<hash> under [cheats] in totk.ini), the hash FNV-1a 32 of "<mod folder name>/<cheat name>", so two mods'
// cheats of one name keep their own switches.
//
// The game's code is lifted C, so what a cheat writes into main's .text cannot take effect here: tools/cheats.py turns
// those writes into patch sets cheat_<hash> when the game is built, and the code a cheat puts in the padding after
// .text (a code cave) into code the build lifts. It writes build-cache\cheats.status next to totk.exe, one line per
// cheat: name hash, content hash, kind, buttons, flags, registers, note (tools/cheats.py has the format). A cheat whose
// status line is missing or has another content hash was added or edited after the build: a data cheat runs anyway, a
// code cheat waits for the rebuild.
//
// What else a cheat writes (the game's memory, data in a code cave) the cheat VM writes, every presented frame while
// the cheat is on: a VM after Atmosphere's dmnt cheat VM (opcodes 0-A, C0-C3, FF0, FF1, FFF; the last three do
// nothing). Registers start at 0 each frame. dmnt carries them from one enabled cheat to the next; here the setup
// cheats (flag s: a {master} code, or a file's first cheat with opcodes, which loads pointers for the rest) run first
// and carry theirs into every other cheat, which then gets the registers its status line names (the values the setup
// cheats leave that tools/cheats.py could tell, so a cheat works with its master code off too). Writes into .text are
// skipped: a cheat with a patch set has them built in, and a data cheat that makes one (at an address only its run
// knows) is switched off. Memory is the guest's, mapped at its own addresses: main's image is written with its pages
// made writable for the write (read-only data included, as dmnt writes through the debugger), everything else after
// mem_query says the guest may read or write it. Buttons are input_poll's HidNpadButton bits, the same as the cheat
// format's key masks; they read 0 while the menu is open.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cfg.h"
#include "cheats.h"
#include "host.h"
#include "input.h"
#include "mods.h"
#include "platform/platform.h"
#include "ui_text.h"

#ifdef _WIN32
#include <windows.h>   // code page 936, for gbk_to_utf8
#define SEP "\\"
#else
#define SEP "/"
#endif

enum { STATUS_MAX = 160, OPS_PER_RUN = 1 << 20 };

typedef struct {
    CheatInfo info;
    uint32_t *words;
    int n;
    int dup;              // 1 for the first cheat of its name in its file, 2 for the next ...
    uint32_t content;     // FNV-1a 32 of its words as tools/cheats.py writes them
    int set;              // patch set, -1 none
    int vm;               // runs in the VM while on
    int setup;            // a setup cheat: runs first and hands its registers to the others
    int seen;             // the build saw this version of it (it has a status line)
    uint16_t pre_mask, pre_main;   // registers to load before it runs; which of them are offsets into main
    uint64_t pre[16];
    volatile int32_t on;
    volatile int32_t text_hit;   // it wrote into .text (skipped)
    char note[STATUS_MAX];
} Cheat;

typedef struct {
    uint64_t r[16], saved[16];
} Vm;

static Cheat *g_cheats;
static int g_n, g_cap, g_vm;   // g_vm: some cheat needs cheats_frame
static char g_file[64];        // "<build id>.txt"
static char g_dir[PLAT_PATH_MAX];   // the folder of the executable, with a trailing separator
static int g_files;            // mods whose file was read
static uint64_t g_main, g_main_size, g_text_end, g_code_end;   // main's image; .text; .text with its padding
static uint64_t g_static[256];   // the 0xC3 static registers, kept across frames

static uint32_t fnv(const char *s, size_t n) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 0x01000193u;
    return h;
}

// The folder of the executable, with a trailing separator.
static int exe_dir(char *out, size_t n) {
    if (!plat_exe_path(out, n)) return 0;
    char *cut = plat_last_sep(out);
    if (!cut) return 0;
    cut[1] = 0;
    return 1;
}

static int build_id(char out[17]) {
    char path[PLAT_PATH_MAX];
    snprintf(path, sizeof path, "%s/main", rt_config()->exefs);
    PlatFile *f = plat_file_open(path);
    if (!f) return 0;
    uint8_t head[0x48];
    int ok = plat_file_pread(f, head, sizeof head, 0) == sizeof head && !memcmp(head, "NSO0", 4);
    plat_file_close(f);
    for (int i = 0; ok && i < 8; i++) snprintf(out + 2 * i, 3, "%02X", head[0x40 + i]);
    return ok;
}

// ---- opcodes

// Words of the opcode at w (left words remain), 0 when it is cut short (tools/cheats.py ops).
static int op_len(const uint32_t *w, int left) {
    uint32_t f = w[0];
    int t = f >> 28, width = (f >> 24) & 0xF, x = (f >> 8) & 0xF, n;
    switch (t) {
    case 0: case 1: n = width == 8 ? 4 : 3; break;
    case 2: case 8: n = 1; break;
    case 3: n = width == 0 ? 2 : 1; break;
    case 4: case 6: n = 3; break;
    case 5: case 7: n = 2; break;
    case 9: n = 1 + (x ? (width == 8 ? 2 : 1) : 0); break;
    case 0xA: n = x == 2 || x == 4 || x == 5 ? 2 : 1; break;
    case 0xC: n = f >> 24 != 0xC0 ? 1 : x == 0 || x == 2 ? 2 : x == 4 ? (((f >> 20) & 0xF) == 8 ? 3 : 2) : 1; break;
    case 0xF: n = f >> 20 == 0xFFF && (x == 0 || x == 2) ? 2 : 1; break;
    default: n = 1; break;
    }
    return n <= left ? n : 0;
}

// NULL when the VM runs the opcode, else why not (tools/cheats.py vm_ok).
static const char *vm_unsupported(const uint32_t *w, char *buf, int n) {
    uint32_t f = w[0], sub = f >> 24, top = f >> 20;
    int t = f >> 28, width = sub == 0xC0 ? (f >> 20) & 0xF : (f >> 24) & 0xF, x = (f >> 8) & 0xF;
    if (t == 0xB || t == 0xD || t == 0xE || (t == 0xC && sub > 0xC3) ||
        (t == 0xF && top != 0xFF0 && top != 0xFF1 && top != 0xFFF) || (t == 5 && ((f >> 12) & 0xF) > 3) ||
        ((t == 0xA || sub == 0xC0) && x > 5) || (t == 9 && ((f >> 20) & 0xF) > 13) ||
        (sub == 0xC1 && ((f >> 4) & 0xF) > 3) || (sub == 0xC2 && ((f >> 20) & 0xF) > 3)) {
        snprintf(buf, n, "opcode %08X is not supported", f);
        return buf;
    }
    if ((t <= 1 || t == 5 || t == 6 || t == 7 || t == 9 || t == 0xA || sub == 0xC0) && width != 1 && width != 2 &&
        width != 4 && width != 8) {
        snprintf(buf, n, "opcode %08X has a bad width", f);
        return buf;
    }
    if (t == 9 && ((f >> 20) & 0xF) >= 10 && width != 4 && width != 8) {
        snprintf(buf, n, "opcode %08X is float arithmetic on %d bytes", f, width);
        return buf;
    }
    return NULL;
}

// The kind of a cheat the build did not see: heading, data, rebuild (it writes main's code) or unsupported. A write
// into the code through a register shows when it runs (cheats_frame).
static CheatKind classify(Cheat *c) {
    int ops = 0, ends = 0;
    for (int i = 0; i < c->n;) {
        int len = op_len(c->words + i, c->n - i);
        if (!len) {
            snprintf(c->note, sizeof c->note, "opcode %08X is cut short", c->words[i]);
            return CHEAT_UNSUPPORTED;
        }
        uint32_t f = c->words[i];
        ops++;
        ends += f >> 28 == 2;
        if (f >> 28 == 0 && ((f >> 20) & 0xF) == 0) {
            uint64_t off = ((uint64_t)(f & 0xFF) << 32) | c->words[i + 1];
            if (off < g_code_end - g_main) return CHEAT_REBUILD;
        }
        if (vm_unsupported(c->words + i, c->note, sizeof c->note)) return CHEAT_UNSUPPORTED;
        i += len;
    }
    return ops == ends && ops <= 1 ? CHEAT_HEADING : CHEAT_DATA;
}

// ---- guest memory

static uint64_t region(int m) {
    const RtGuestLayout *l = mem_layout();
    switch (m) {
    case 0: return g_main;
    case 1: return l->heap.base;
    case 2: return l->alias.base;
    case 3: return l->aslr.base;
    default: return 0;
    }
}

static int in_main(uint64_t a, unsigned n) { return a >= g_main && a + n <= g_main + g_main_size; }

static int guest_ok(uint64_t a, unsigned n, uint32_t perm) {
    if (in_main(a, n)) return 1;
    RtMemoryInfo mi;
    if (mem_query(a, &mi) || !mi.state || (mi.perm & perm) != perm) return 0;
    return a + n <= mi.base + mi.size;
}

static int rd(uint64_t a, int width, uint64_t *v) {
    if (!a || !guest_ok(a, (unsigned)width, 1)) return 0;
    uint64_t x = 0;
    memcpy(&x, (const void *)(uintptr_t)a, (size_t)width);
    *v = x;
    return 1;
}

// Writes n bytes into main's image a page at a time: a read-only page is writable for its part of the copy only.
static void wr_main(uint64_t a, const uint8_t *from, int n) {
    while (n > 0) {
        uint64_t page = a & ~0xFFFull, end;
        int k = (int)(page + 0x1000 - a);
        if (k > n) k = n;
        int prot = plat_mem_state(page, page + 0x1000, &end);
        if (prot != PLAT_PROT_READ && prot != PLAT_PROT_RW) return;
        if (prot == PLAT_PROT_READ && !plat_mem_protect(page, 0x1000, PLAT_PROT_RW)) return;
        memcpy((void *)(uintptr_t)a, from, (size_t)k);
        if (prot == PLAT_PROT_READ) plat_mem_protect(page, 0x1000, PLAT_PROT_READ);
        a += (uint64_t)k;
        from += k;
        n -= k;
    }
}

static void wr(Cheat *c, uint64_t a, int width, uint64_t v) {
    if (a < g_text_end && a + (unsigned)width > g_main) {   // .text: the build has it, or cannot have it
        plat_xchg32(&c->text_hit, 1);
        return;
    }
    uint64_t now;
    if (!rd(a, width, &now) || !memcmp(&now, &v, (size_t)width)) return;   // unchanged: no write, no protection change
    void *p = (void *)(uintptr_t)a;
    if (in_main(a, (unsigned)width)) {
        wr_main(a, (const uint8_t *)&v, width);
    } else if (guest_ok(a, (unsigned)width, 2)) {
        memcpy(p, &v, (size_t)width);   // a frozen region thaws on the write (memory.c)
    }
}

// ---- the VM

static uint64_t masked(int width, uint64_t v) { return width >= 8 ? v : v & ((1ull << (8 * width)) - 1); }

static uint64_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static uint64_t dbits(double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}

static float bfloat(uint64_t v) {
    uint32_t u = (uint32_t)v;
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static double bdouble(uint64_t v) {
    double d;
    memcpy(&d, &v, 8);
    return d;
}

// dmnt's register arithmetic: 0 add, 1 sub, 2 mul, 3 shl, 4 shr, 5 and, 6 or, 7 not, 8 xor, 9 move, 10-13 float add,
// sub, mul, div (single on 4 bytes, double on 8), the result cut to the width.
static uint64_t arith(int op, int width, uint64_t a, uint64_t b) {
    uint64_t r;
    switch (op) {
    case 0: r = a + b; break;
    case 1: r = a - b; break;
    case 2: r = a * b; break;
    case 3: r = b < 64 ? a << b : 0; break;
    case 4: r = b < 64 ? a >> b : 0; break;
    case 5: r = a & b; break;
    case 6: r = a | b; break;
    case 7: r = ~a; break;
    case 8: r = a ^ b; break;
    case 9: r = a; break;
    default:
        if (width == 4) {
            float x = bfloat(a), y = bfloat(b);
            r = fbits(op == 10 ? x + y : op == 11 ? x - y : op == 12 ? x * y : x / y);
        } else {
            double x = bdouble(a), y = bdouble(b);
            r = dbits(op == 10 ? x + y : op == 11 ? x - y : op == 12 ? x * y : x / y);
        }
        break;
    }
    return masked(width, r);
}

static int compare(int cond, uint64_t a, uint64_t b) {
    switch (cond) {
    case 1: return a > b;
    case 2: return a >= b;
    case 3: return a < b;
    case 4: return a <= b;
    case 5: return a == b;
    case 6: return a != b;
    default: return 0;
    }
}

static int opens_block(uint32_t f) { return f >> 28 == 1 || f >> 28 == 8 || f >> 24 == 0xC0; }

// From i (after a failed condition, or an else reached while running), the opcode to continue at: after the
// matching else (else_ok) or end.
static int skip(const uint32_t *w, int n, int i, int else_ok) {
    int depth = 0;
    while (i < n) {
        int len = op_len(w + i, n - i);
        if (!len) return n;
        uint32_t f = w[i];
        i += len;
        if (opens_block(f)) depth++;
        else if (f >> 28 == 2 && ((f >> 24) & 0xF) == 0 && depth-- == 0) return i;
        else if (f >> 28 == 2 && depth == 0 && else_ok) return i;
    }
    return n;
}

static uint64_t imm64(const uint32_t *w) { return (uint64_t)w[0] << 32 | w[1]; }

// 0xC1 / 0xC2: 0 restore register d from saved s, 1 save register s as d, 2 clear saved d, 3 clear register d.
static void save_restore(Vm *v, int op, int d, int s) {
    switch (op) {
    case 0: v->r[d] = v->saved[s]; break;
    case 1: v->saved[d] = v->r[s]; break;
    case 2: v->saved[d] = 0; break;
    default: v->r[d] = 0; break;
    }
}

// One run of c on the registers in v.
static void run(Cheat *c, Vm *v, uint64_t keys) {
    uint64_t *r = v->r;
    int loop[16] = {0};
    const uint32_t *w = c->words;
    int n = c->n, budget = OPS_PER_RUN;
    for (int i = 0; i < n && budget-- > 0;) {
        int len = op_len(w + i, n - i);
        if (!len) return;
        uint32_t f = w[i];
        int t = f >> 28, width = (f >> 24) & 0xF, m = (f >> 20) & 0xF, y = (f >> 16) & 0xF, z = (f >> 12) & 0xF,
            x = (f >> 8) & 0xF, lo = (f >> 4) & 0xF;
        uint64_t rel = ((uint64_t)(f & 0xFF) << 32) | (len > 1 ? w[i + 1] : 0), val, a;
        switch (t) {
        case 0:   // 0TMR00AA AAAAAAAA V..: store a value at region M + A + register R
            wr(c, region(m) + rel + r[y], width, masked(width, width == 8 ? imm64(w + i + 2) : w[i + 2]));
            break;
        case 1:   // 1TMCXrAA AAAAAAAA V..: if [region M + A (+ register r)] <cond C> V
            a = region(m) + rel + (z ? r[x] : 0);
            if (!rd(a, width, &val) || !compare(y, val, masked(width, width == 8 ? imm64(w + i + 2) : w[i + 2]))) {
                i = skip(w, n, i + len, 1);
                continue;
            }
            break;
        case 2:   // 20000000 end, 21000000 else: an else reached while running skips to the end
            if (width == 1) {
                i = skip(w, n, i + len, 0);
                continue;
            }
            break;
        case 3:   // 300R0000 VVVVVVVV loop start, 310R0000 loop end
            if (width == 0) {
                r[y] = w[i + 1];
                loop[y] = i + len;
            } else if (r[y] && --r[y]) {
                i = loop[y];
                continue;
            }
            break;
        case 4:   // 400R0000 V V: register R = V
            r[y] = imm64(w + i + 1);
            break;
        case 5:   // 5TMR00AA / 5T0R10AA / 5T0R2SAA / 5TMR3SAA AAAAAAAA: R = [M + A / R + A / S + A / M + A + S]
            a = z == 0 ? region(m) + rel : z == 1 ? r[y] + rel : z == 2 ? r[x] + rel : region(m) + rel + r[x];
            if (!rd(a, width, &val)) return;   // a broken pointer chain: nothing more of this cheat
            r[y] = val;
            break;
        case 6:   // 6T0RIor0 V V: store V at R (+ register r); I: R += width
            wr(c, r[y] + (x ? r[lo] : 0), width, masked(width, imm64(w + i + 1)));
            if (z) r[y] += (uint64_t)width;
            break;
        case 7:   // 7T0RC000 VVVVVVVV: R = R <op C> V (add, sub, mul, shl, shr)
            r[y] = z <= 4 ? arith(z, width, r[y], w[i + 1]) : masked(width, r[y]);
            break;
        case 9:   // 9TCRS0s0 / 9TCRS100 V..: R = S <op C> (s or V)
            r[y] = arith(m, width, r[z], x ? (width == 8 ? imm64(w + i + 1) : w[i + 1]) : r[lo]);
            break;
        case 0xA: {   // ATSRIOxa (aaaaaaaa): store register S at an address of type O; I: R += width
            uint64_t imm = ((uint64_t)(f & 0xF) << 32) | (len > 1 ? w[i + 1] : 0);
            switch (x) {
            case 0: a = r[y]; break;
            case 1: a = r[y] + r[lo]; break;
            case 2: a = r[y] + imm; break;
            case 3: a = region(lo) + r[y]; break;
            case 4: a = region(lo) + imm; break;
            default: a = region(lo) + r[y] + imm; break;
            }
            wr(c, a, width, masked(width, r[m]));
            if (z) r[y] += (uint64_t)width;
            break;
        }
        case 8:   // 8kkkkkkk: if the buttons are held
            if ((keys & (f & 0x0FFFFFFF)) != (f & 0x0FFFFFFF)) {
                i = skip(w, n, i + len, 1);
                continue;
            }
            break;
        case 0xC:
            if (f >> 24 == 0xC0) {   // C0TcSX..: if register S <cond c> an operand of type X
                int ok;
                uint64_t b, imm = ((uint64_t)(f & 0xF) << 32) | (len > 1 ? w[i + 1] : 0);
                switch (x) {
                case 0: ok = rd(region(lo) + imm, m, &b); break;
                case 1: ok = rd(region(lo) + r[f & 0xF], m, &b); break;
                case 2: ok = rd(r[lo] + imm, m, &b); break;
                case 3: ok = rd(r[lo] + r[f & 0xF], m, &b); break;
                case 4: ok = 1, b = m == 8 ? imm64(w + i + 1) : w[i + 1]; break;
                default: ok = 1, b = r[lo]; break;
                }
                if (!ok || !compare(y, masked(m, r[z]), masked(m, b))) {
                    i = skip(w, n, i + len, 1);
                    continue;
                }
            } else if (f >> 24 == 0xC1) {   // C10D0Sx0: save, restore or clear one register
                save_restore(v, lo, y, x);
            } else if (f >> 24 == 0xC2) {   // C2x0XXXX: the same for the registers of a mask
                for (int k = 0; k < 16; k++)
                    if (f >> k & 1) save_restore(v, m, k, k);
            } else if (f >> 24 == 0xC3) {   // C3000XXx: static register XX <-> register x
                int k = (f >> 4) & 0xFF;
                if (k < 0x80) r[f & 0xF] = g_static[k];
                else g_static[k] = r[f & 0xF];
            }
            break;
        default:   // FF0 pause, FF1 resume, FFF log: nothing here
            break;
        }
        i += len;
    }
}

// ---- the file

#ifdef _WIN32
// Strict UTF-8, as Python's decoder: no overlong forms, no surrogates, nothing above U+10FFFF.
static int is_utf8(const unsigned char *s) {
    while (*s) {
        unsigned c = *s;
        unsigned n = c < 0x80                  ? 0
                     : c >= 0xC2 && c <= 0xDF ? 1
                     : c >= 0xE0 && c <= 0xEF ? 2
                     : c >= 0xF0 && c <= 0xF4 ? 3
                                              : 9;
        if (n == 9) return 0;
        unsigned lo = 0x80, hi = 0xBF;   // the second byte's range
        if (c == 0xE0) lo = 0xA0;
        else if (c == 0xED) hi = 0x9F;
        else if (c == 0xF0) lo = 0x90;
        else if (c == 0xF4) hi = 0x8F;
        for (unsigned i = 1; i <= n; i++, lo = 0x80, hi = 0xBF)
            if (s[i] < lo || s[i] > hi) return 0;
        s += n + 1;
    }
    return 1;
}

// A file that is not UTF-8 is read as GBK (code page 936; older Chinese cheat files) and converted, as tools/cheats.py
// reads it, so the names and their hashes agree with the setup's.
static char *gbk_to_utf8(char *s) {
    int wn = MultiByteToWideChar(936, 0, s, -1, NULL, 0);
    wchar_t *w = wn > 0 ? (wchar_t *)malloc(sizeof *w * (size_t)wn) : NULL;
    if (!w || !MultiByteToWideChar(936, 0, s, -1, w, wn)) {
        free(w);
        return s;
    }
    int un = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *u = un > 0 ? (char *)malloc((size_t)un) : NULL;
    if (!u || !WideCharToMultiByte(CP_UTF8, 0, w, -1, u, un, NULL, NULL)) {
        free(w);
        free(u);
        return s;
    }
    free(w);
    free(s);
    return u;
}
#endif

static char *read_text(const char *path) {
    uint64_t n = 0;
    char *s = (char *)plat_file_read(path, &n);
    if (s && n >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3)) memmove(s, s + 3, (size_t)n - 2);   // a UTF-8 BOM
#ifdef _WIN32
    if (s && !is_utf8((const unsigned char *)s)) {
        rt_log("cheats: %s is not UTF-8; read as GBK (code page 936)", path);
        s = gbk_to_utf8(s);
    }
#endif
    return s;
}

static void add_word(Cheat *c, uint32_t v, int *cap) {
    if (c->n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        c->words = (uint32_t *)realloc(c->words, sizeof *c->words * (size_t)*cap);
        if (!c->words) rt_fatal("cheats: out of memory");
    }
    c->words[c->n++] = v;
}

// Appends the cheats of one mod's file; mod: the mod's folder name (kept).
static void parse(char *s, const char *mod) {
    int first = g_n, wcap = 0;
    for (char *line = s; line && *line;) {
        char *end = strchr(line, '\n'), *next = end ? end + 1 : NULL;
        if (end) *end = 0;
        while (*line == ' ' || *line == '\t') line++;
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) line[--len] = 0;
        if (len >= 2 && (line[0] == '[' || line[0] == '{') && (line[len - 1] == ']' || line[len - 1] == '}')) {
            if (g_n == g_cap) {
                g_cap = g_cap ? g_cap * 2 : 32;
                g_cheats = (Cheat *)realloc(g_cheats, sizeof *g_cheats * (size_t)g_cap);
                if (!g_cheats) rt_fatal("cheats: out of memory");
            }
            Cheat *c = &g_cheats[g_n++];
            memset(c, 0, sizeof *c);
            line[len - 1] = 0;
            char *name = line + 1;
            while (*name == ' ' || *name == '\t') name++;
            size_t k = strlen(name);
            while (k && (name[k - 1] == ' ' || name[k - 1] == '\t')) name[--k] = 0;
            c->info.name = strdup(name);
            c->info.mod = mod;
            c->info.kind = line[0] == '{' ? CHEAT_MASTER : CHEAT_HEADING;
            c->set = -1;
            c->dup = 1;   // a repeated name gets "#2", "#3" ... (tools/cheats.py parse)
            for (int j = first; j < g_n - 1; j++) c->dup += !strcmp(g_cheats[j].info.name, c->info.name);
            wcap = 0;
        } else if (g_n > first) {
            for (char *tok = strtok(line, " \t"); tok; tok = strtok(NULL, " \t")) {
                char *e;
                unsigned long v = strtoul(tok, &e, 16);
                if (e - tok == 8 && !*e) add_word(&g_cheats[g_n - 1], (uint32_t)v, &wcap);
            }
        }
        line = next;
    }
}

// The file g_file in folder, its name in any case, into out; 0 when there is none.
typedef struct {
    const char *dir;
    char *out;
    size_t cap;
    int found;
} FindFile;
static int find_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    FindFile *f = (FindFile *)ctx;
    if (kind == PLAT_FS_FILE && !strcasecmp(name, g_file) &&
        snprintf(f->out, f->cap, "%s/%s", f->dir, name) < (int)f->cap)
        f->found = 1;
    return !f->found;
}
static int find_file(const char *folder, char *out, size_t cap) {
    FindFile f = {folder, out, cap, 0};
    plat_fs_list(folder, find_cb, &f);
    return f.found;
}

enum { STATUS_FIELDS = 7 };   // name hash, content hash, kind, buttons, flags, registers, note

// The status line of key, its fields split in place; 0 when none.
static int status_of(char *status, const char *key, char **fields) {
    for (char *line = status; line && *line;) {
        char *end = strchr(line, '\n'), *next = end ? end + 1 : NULL;
        if (!strncmp(line, key, 8) && line[8] == '\t') {
            static char copy[1024];
            size_t n = end ? (size_t)(end - line) : strlen(line);
            if (n >= sizeof copy) n = sizeof copy - 1;
            memcpy(copy, line, n);
            copy[n] = 0;
            if (n && copy[n - 1] == '\r') copy[n - 1] = 0;
            int k = 0;
            fields[k++] = copy;
            for (char *p = copy; *p && k < STATUS_FIELDS; p++)
                if (*p == '\t') *p = 0, fields[k++] = p + 1;
            while (k < STATUS_FIELDS) fields[k++] = (char *)"";
            return 1;
        }
        line = next;
    }
    return 0;
}

// The registers field: "<register>=<hex>" (an `m` before the number: an offset into main), comma separated.
static void registers(Cheat *c, const char *s) {
    while (*s) {
        char *e;
        int k = (int)strtol(s, &e, 16);
        if (e == s || *e != '=' || k < 0 || k > 15) break;
        s = e + 1;
        int in_main = *s == 'm';
        uint64_t v = strtoull(s + in_main, &e, 16);
        c->pre[k] = v;
        c->pre_mask |= (uint16_t)(1u << k);
        if (in_main) c->pre_main |= (uint16_t)(1u << k);
        s = *e == ',' ? e + 1 : e;
        if (e == s && *s) break;
    }
}

static CheatKind kind_of(const char *k) {
    static const struct {
        const char *name;
        CheatKind kind;
    } kinds[] = {{"code", CHEAT_CODE}, {"key", CHEAT_KEY}, {"master", CHEAT_MASTER}, {"builtin", CHEAT_BUILTIN},
                 {"restore", CHEAT_RESTORE}, {"heading", CHEAT_HEADING}, {"data", CHEAT_DATA}};
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++)
        if (!strcmp(k, kinds[i].name)) return kinds[i].kind;
    return CHEAT_UNSUPPORTED;
}

// The menu's font has no numero sign (U+2116), which cheat names often start with: they show "No" instead.
static void plain_name(char *s) {
    for (char *p; (p = strstr(s, "\xE2\x84\x96")) != NULL;) {
        p[0] = 'N', p[1] = 'o';
        memmove(p + 2, p + 3, strlen(p + 3) + 1);
    }
}

// ---- rebuild: code cheats and ExeFS patches the build lacks
// The setup writes build-cache\rebuild.txt (installer/steps.py write_rebuild_command), the command that starts its
// window in rebuild mode on this install folder, one argument per line. When an enabled mod has code cheats this build
// lacks (CHEAT_REBUILD) or ExeFS patch files it lacks (mods_exefs_pending), the game runs it and exits; the setup lifts
// and compiles them in and starts the game again.
// build-cache\rebuild.skip holds the key of the cheats a rebuild was started for: a rebuild that failed or was
// cancelled leaves them unbuilt, and the same set does not start it again. TOTK_MODS_REBUILD=off never starts it.
// Scripted runs (--seconds, --boot) never start it.
static void rebuild_if_needed(const char *dir) {
    uint32_t key = 0x811C9DC5u;
    int need = 0;
    for (int i = 0; i < g_n; i++) {
        const Cheat *c = &g_cheats[i];
        if (c->info.kind != CHEAT_REBUILD) continue;
        need++;
        key = (key ^ fnv(c->info.key, strlen(c->info.key))) * 0x01000193u;
        key = (key ^ c->content) * 0x01000193u;
    }
    int patches = mods_exefs_pending(&key);
    if (!need && !patches) return;
    char cmd_path[PLAT_PATH_MAX + 64], skip_path[PLAT_PATH_MAX + 64], skip[16] = "", want[16];
    snprintf(cmd_path, sizeof cmd_path, "%sbuild-cache" SEP "rebuild.txt", dir);
    snprintf(skip_path, sizeof skip_path, "%sbuild-cache" SEP "rebuild.skip", dir);
    snprintf(want, sizeof want, "%08X", key);
    char *old = read_text(skip_path);
    if (old) snprintf(skip, sizeof skip, "%.8s", old);
    free(old);
    const char *why = !strcmp(cfg_value("TOTK_MODS_REBUILD"), "off") ? "mods_rebuild = off"
                    : rt_config()->seconds || rt_config()->boot   ? "a scripted run"
                    : !strcmp(skip, want)                           ? "the last rebuild for them failed or was cancelled"
                                                                    : NULL;
    char *cmd = why ? NULL : read_text(cmd_path);
    if (!why && !cmd) why = "no build-cache/rebuild.txt (a build without the setup)";
    if (why) {
        rt_log("cheats: %d code cheats and %d ExeFS patch files are not built into the game; no rebuild: %s", need,
               patches, why);
        free(cmd);
        return;
    }
    const char *argv[32];
    int argc = 0;
    for (char *line = cmd; line && *line && argc < 31;) {
        char *end = strchr(line, '\n'), *next = end ? end + 1 : NULL;
        if (end) *end = 0;
        size_t n = strlen(line);
        if (n && line[n - 1] == '\r') line[n - 1] = 0;
        if (*line) argv[argc++] = line;
        line = next;
    }
    argv[argc] = NULL;
    if (argc < 2 || !plat_file_write(skip_path, want, strlen(want))) {
        rt_log("cheats: %d code cheats and %d ExeFS patch files are not built into the game; cannot start the "
               "rebuild (%s)", need, patches, cmd_path);
        free(cmd);
        return;
    }
    rt_log("cheats: %d code cheats and %d ExeFS patch files are not built into the game; starting the rebuild: %s",
           need, patches, argv[0]);
    int ok = plat_spawn(argv, dir);
    free(cmd);
    if (!ok) {
        rt_log("cheats: the rebuild did not start; the game starts with the code it has");
        return;
    }
    prof_write();   // plat_exit runs no atexit handlers
    plat_exit(0);
}

void cheats_init(void) {
    const RtLoaded *m = ldr_count() ? ldr_module(0) : NULL;
    char dir[PLAT_PATH_MAX], bid[17], path[2400];   // path: a mod's cheats folder (ModInfo.cheats) + the file
    if (!m || !exe_dir(dir, sizeof dir) || !build_id(bid)) return;
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    g_main = m->base;
    g_main_size = m->size;
    g_text_end = m->base + m->text_end;
    g_code_end = m->base + ((m->text_end + 0xFFF) & ~0xFFFull);
    snprintf(g_file, sizeof g_file, "%s.txt", bid);
    mods_scan();
    for (int k = 0; k < mods_count(); k++) {   // the Mods tab's order, top first
        const ModInfo *mod = mods_at(k);
        if (!mod->enabled || mod->problem != MOD_OK || !mod->cheats[0] || !find_file(mod->cheats, path, sizeof path))
            continue;
        char *text = read_text(path);
        if (!text) {
            rt_log("cheats: %s: cannot read %s", mod->name, path);
            continue;
        }
        parse(text, strdup(mod->name));
        free(text);
        g_files++;
    }
    if (!g_files) {
        rt_log("cheats: no enabled mod holds cheats/%s", g_file);
        rebuild_if_needed(dir);   // an enabled mod's ExeFS patches may still wait for it
        return;
    }
    snprintf(path, sizeof path, "%sbuild-cache" SEP "cheats.status", dir);
    char *status = read_text(path);
    int counts[CHEAT_UNSUPPORTED + 1] = {0}, seen_dups = 0, first = 1;
    const char *file = NULL;   // the mod whose cheats the loop is in
    for (int i = 0; i < g_n; i++) {
        Cheat *c = &g_cheats[i];
        if (c->info.mod != file) file = c->info.mod, first = 1;
        char named[900];
        snprintf(named, sizeof named, c->dup > 1 ? "%s/%s#%d" : "%s/%s", c->info.mod, c->info.name, c->dup);
        seen_dups += c->dup > 1;
        char key[9], content[9];
        snprintf(key, sizeof key, "%08X", fnv(named, strlen(named)));
        size_t tn = (size_t)c->n * 9 + 1, tl = 0;
        char *t = (char *)malloc(tn);
        if (!t) rt_fatal("cheats: out of memory");
        t[0] = 0;
        for (int k = 0; k < c->n; k++) tl += (size_t)snprintf(t + tl, tn - tl, k ? " %08X" : "%08X", c->words[k]);
        c->content = fnv(t, tl);
        snprintf(content, sizeof content, "%08X", c->content);
        free(t);
        snprintf(c->info.key, sizeof c->info.key, "TOTK_CHEAT_%s", key);
        int master = c->info.kind == CHEAT_MASTER;
        char *f[STATUS_FIELDS];
        CheatKind kind;
        if (status && status_of(status, key, f) && !strcmp(f[1], content)) {
            kind = kind_of(f[2]);
            c->seen = 1;
            c->info.buttons = (uint32_t)strtoul(f[3], NULL, 16);
            c->vm = strchr(f[4], 'v') != NULL;
            c->setup = strchr(f[4], 's') != NULL;
            registers(c, f[5]);
            snprintf(c->note, sizeof c->note, "%s", f[6]);
            if (kind == CHEAT_CODE || kind == CHEAT_KEY || kind == CHEAT_MASTER) {
                char set[32];
                snprintf(set, sizeof set, "cheat_%08x", fnv(named, strlen(named)));
                c->set = rt_patch_set(set);
                if (c->set < 0 && kind != CHEAT_MASTER) kind = CHEAT_REBUILD;
            }
        } else {
            kind = classify(c);
            c->vm = kind == CHEAT_DATA;
            c->setup = c->vm && (master || first);   // as tools/cheats.py: a {master}, a file's first cheat with opcodes
        }
        first &= kind == CHEAT_HEADING;
        if (master && kind == CHEAT_DATA) kind = CHEAT_MASTER;   // a data master code: the VM, always on
        c->info.kind = kind;
        c->info.note = c->note;
        const char *v = cfg_get(c->info.key);
        c->on = kind == CHEAT_MASTER || kind == CHEAT_BUILTIN || (v && *v && strcmp(v, "0") != 0);
        if (kind != CHEAT_CODE && kind != CHEAT_KEY && kind != CHEAT_MASTER && kind != CHEAT_BUILTIN &&
            kind != CHEAT_DATA)
            c->vm = 0;
        // A set whose cheat also has data on until the VM has written it (cheats_frame); a key cheat's while its
        // buttons are held.
        if (c->set >= 0 && (kind == CHEAT_KEY || c->vm)) rt_patch_switch(c->set, 0);
        if (c->vm || kind == CHEAT_KEY) g_vm = 1;
        counts[kind]++;
    }
    free(status);
    rebuild_if_needed(dir);
    for (int i = 0; i < g_n; i++) plain_name((char *)g_cheats[i].info.name);   // the hashes above read the file's bytes
    int on = 0;
    for (int i = 0; i < g_n; i++) on += g_cheats[i].on && g_cheats[i].info.kind != CHEAT_HEADING;
    rt_log("cheats: %s in %d mods: %d cheats, %d on; %d code, %d while buttons are held, %d data, %d master, "
           "%d built in, %d restore, %d headings, %d wait for a rebuild, %d not supported%s",
           g_file, g_files, g_n, on, counts[CHEAT_CODE], counts[CHEAT_KEY], counts[CHEAT_DATA], counts[CHEAT_MASTER],
           counts[CHEAT_BUILTIN], counts[CHEAT_RESTORE], counts[CHEAT_HEADING], counts[CHEAT_REBUILD], counts[CHEAT_UNSUPPORTED],
           seen_dups ? " (repeated names numbered)" : "");
}

int cheats_rebuild_pending(void) {
    char path[PLAT_PATH_MAX + 64];
    snprintf(path, sizeof path, "%sbuild-cache" SEP "rebuild.txt", g_dir);
    uint32_t key = 0;
    int need = mods_exefs_pending(&key);
    for (int i = 0; i < g_n && !need; i++) need = g_cheats[i].info.kind == CHEAT_REBUILD;
    return need && g_dir[0] && plat_file_exists(path);
}

void cheats_rebuild_allow(void) {
    char path[PLAT_PATH_MAX + 64];
    snprintf(path, sizeof path, "%sbuild-cache" SEP "rebuild.skip", g_dir);
    plat_fs_unlink(path);
}

int cheats_count(void) { return g_n; }

const CheatInfo *cheats_info(int i) { return i >= 0 && i < g_n ? &g_cheats[i].info : NULL; }

int cheats_on(int i) { return i >= 0 && i < g_n && g_cheats[i].on; }

const char *cheats_file(void) { return g_file; }

void cheats_set(int i, int on) {
    if (i < 0 || i >= g_n) return;
    Cheat *c = &g_cheats[i];
    if (c->info.kind != CHEAT_CODE && c->info.kind != CHEAT_KEY && c->info.kind != CHEAT_DATA) return;
    plat_xchg32(&c->on, on != 0);
    // A code cheat's set at once, but on with data only after the VM wrote it; CHEAT_KEY: cheats_frame follows the
    // buttons.
    if (c->info.kind == CHEAT_CODE && (!c->vm || !on)) rt_patch_switch(c->set, on);
    cfg_set(c->info.key, on ? "1" : "0");
    rt_log("cheats: %s/%s %s", c->info.mod, c->info.name, on ? "on" : "off");
}

// A data cheat that wrote into .text: the build has no patch for it, so it is switched off.
static void text_written(Cheat *c) {
    if (c->info.kind != CHEAT_DATA) return;   // the others' writes there are built in
    plat_xchg32(&c->on, 0);
    snprintf(c->note, sizeof c->note, "it writes the game's code at an address only its run knows");
    c->info.kind = c->seen ? CHEAT_UNSUPPORTED : CHEAT_REBUILD;
    rt_log("cheats: %s/%s writes into main's code; %s", c->info.mod, c->info.name,
           c->seen ? "switched off (not supported)" : "switched off until the rebuild");
}

static void load_registers(const Cheat *c, Vm *v) {
    for (int k = 0; k < 16; k++)
        if (c->pre_mask >> k & 1) v->r[k] = c->pre[k] + (c->pre_main >> k & 1 ? g_main : 0);
}

void cheats_frame(uint64_t frame) {
    if (!g_vm || frame < 60) return;   // the game polls its input (and starts SDL) before this
    uint64_t keys = 0;
    int32_t lx, ly, rx, ry;
    input_poll(&keys, &lx, &ly, &rx, &ry);   // 0 while the menu is open
    // The setup cheats first, in list order, on one set of registers: the other cheats start from what they leave.
    Vm setup;
    memset(&setup, 0, sizeof setup);
    for (int i = 0; i < g_n; i++) {
        Cheat *c = &g_cheats[i];
        if (c->setup && c->vm && c->on) {
            load_registers(c, &setup);
            run(c, &setup, keys);
        }
    }
    for (int i = 0; i < g_n; i++) {
        Cheat *c = &g_cheats[i];
        if (!c->setup && c->vm && c->on) {
            Vm v = setup;
            load_registers(c, &v);
            run(c, &v, keys);
        }
        if (c->text_hit) {
            plat_xchg32(&c->text_hit, 0);
            text_written(c);
        }
        int want = c->on;
        switch (c->info.kind) {
        case CHEAT_KEY: want = want && (keys & c->info.buttons) == c->info.buttons; break;
        case CHEAT_CODE: case CHEAT_MASTER: if (!c->vm) continue; break;   // cheats_set switches the plain ones
        default: continue;
        }
        if (c->set >= 0 && rt_patch_enabled(c->set) != want) rt_patch_switch(c->set, want);
    }
}

// In the menu language: the Switch buttons as the Controls tab names them (UI_BIND_A .. UI_BIND_DOWN are in
// HidNpadButton bit order), then the stick directions.
const char *cheats_buttons(uint32_t mask, char *buf, int n) {
    static const UiTextId sticks[8] = {UI_MENU_CHEAT_LSTICK_LEFT, UI_MENU_CHEAT_LSTICK_UP, UI_MENU_CHEAT_LSTICK_RIGHT,
                                       UI_MENU_CHEAT_LSTICK_DOWN, UI_MENU_CHEAT_RSTICK_LEFT, UI_MENU_CHEAT_RSTICK_UP,
                                       UI_MENU_CHEAT_RSTICK_RIGHT, UI_MENU_CHEAT_RSTICK_DOWN};
    int k = 0;
    buf[0] = 0;
    for (int b = 0; b < 24; b++)
        if (mask & (1u << b)) {
            const char *name = menu_text(b < 16 ? (UiTextId)(UI_BIND_A + b) : sticks[b - 16]);
            k += snprintf(buf + k, (size_t)(n > k ? n - k : 0), "%s%s", k ? " + " : "", name);
            if (k >= n) break;
        }
    return buf;
}
