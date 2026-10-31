// Cheats of an Atmosphere cheat file (cheats.h). The file is cheats\<build id>.txt next to totk.exe, the build id the
// first 8 bytes of main's NSO build id, as Atmosphere and the emulators name it. Each cheat's switch is the setting
// TOTK_CHEAT_<name hash> (cheat_<hash> under [cheats] in totk.ini).
//
// Code cheats (opcode 0 writes into main's code) cannot be applied here: the game's code is lifted C. tools/cheats.py
// turns them into patch sets cheat_<hash> when the game is built and writes cheats\<build id>.status, one line per
// cheat: name hash, content hash, kind, buttons, note (tools/cheats.py has the format). A cheat whose status line is
// missing or has another content hash was added or edited after the build: a data cheat runs anyway, a code cheat waits
// for the setup to run again.
//
// Data cheats run in a VM after Atmosphere's dmnt cheat VM (opcodes 0-A; the 0xC.. and 0xF.. extensions are not
// supported), every presented frame while their switch is on, with registers cleared before each cheat. Memory is the
// guest's, mapped at its own addresses: main's image is written through VirtualProtect (its read-only data included,
// as dmnt writes through the debugger), everything else after mem_query says the guest may read or write it. Buttons
// are input_poll's HidNpadButton bits, the same as the cheat format's key masks; they read 0 while the menu is open.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "cheats.h"
#include "host.h"
#include "input.h"
#include "platform/platform.h"

enum { STATUS_MAX = 160, OPS_PER_RUN = 1 << 20 };

typedef struct {
    CheatInfo info;
    uint32_t *words;
    int n;
    int set;              // patch set, -1 none
    volatile LONG on;
    char note[STATUS_MAX];
} Cheat;

static Cheat *g_cheats;
static int g_n, g_vm;     // g_vm: some cheat needs cheats_frame
static char g_file[64];
static uint64_t g_main, g_main_size, g_code_end;   // main's image; its code with the padding up to .rodata

static uint32_t fnv(const char *s, size_t n) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 0x01000193u;
    return h;
}

// The folder of totk.exe, UTF-8, with a trailing backslash.
static int exe_dir(char *out, int n) {
    wchar_t w[MAX_PATH * 2];
    DWORD k = GetModuleFileNameW(NULL, w, MAX_PATH * 2);
    if (!k || k >= MAX_PATH * 2) return 0;
    wchar_t *cut = wcsrchr(w, L'\\');
    if (!cut) return 0;
    cut[1] = 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL) > 0;
}

static int build_id(char out[17]) {
    char path[1024];
    snprintf(path, sizeof path, "%s/main", rt_config()->exefs);
    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024)) return 0;
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    uint8_t head[0x48];
    DWORD got = 0;
    int ok = ReadFile(f, head, sizeof head, &got, NULL) && got == sizeof head && !memcmp(head, "NSO0", 4);
    CloseHandle(f);
    for (int i = 0; ok && i < 8; i++) snprintf(out + 2 * i, 3, "%02X", head[0x40 + i]);
    return ok;
}

// ---- opcodes

// Words of the opcode at w (left words remain), 0 when it is cut short.
static int op_len(const uint32_t *w, int left) {
    uint32_t f = w[0];
    int t = f >> 28, width = (f >> 24) & 0xF, n;
    switch (t) {
    case 0: case 1: n = width == 8 ? 4 : 3; break;
    case 2: case 8: n = 1; break;
    case 3: n = width == 0 ? 2 : 1; break;
    case 4: case 6: n = 3; break;
    case 5: case 7: n = 2; break;
    case 9: n = 1 + (((f >> 8) & 0xF) ? (width == 8 ? 2 : 1) : 0); break;
    case 0xA: n = ((f >> 8) & 0xF) == 2 || ((f >> 8) & 0xF) == 4 || ((f >> 8) & 0xF) == 5 ? 2 : 1; break;
    default: n = 1; break;
    }
    return n <= left ? n : 0;
}

// NULL when the VM runs the opcode, else why not (tools/cheats.py vm_ok).
static const char *vm_unsupported(const uint32_t *w, char *buf, int n) {
    uint32_t f = w[0];
    int t = f >> 28, width = (f >> 24) & 0xF;
    if (t > 0xA || (t == 5 && ((f >> 12) & 0xF) > 1) || (t == 0xA && ((f >> 8) & 0xF) > 5)) {
        snprintf(buf, n, "opcode %08X is not supported", f);
        return buf;
    }
    if ((t <= 1 || t == 5 || t == 6 || t == 7 || t == 9 || t == 0xA) && width != 1 && width != 2 && width != 4 &&
        width != 8) {
        snprintf(buf, n, "opcode %08X has a bad width", f);
        return buf;
    }
    return NULL;
}

// The kind of a cheat the build did not see: heading, data, rebuild (it writes main's code) or unsupported.
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

static void wr(uint64_t a, int width, uint64_t v) {
    uint64_t now;
    if (!rd(a, width, &now) || !memcmp(&now, &v, (size_t)width)) return;   // unchanged: no write, no protection change
    void *p = (void *)(uintptr_t)a;
    if (in_main(a, (unsigned)width)) {
        DWORD old, back;
        if (!VirtualProtect(p, (SIZE_T)width, PAGE_READWRITE, &old)) return;
        memcpy(p, &v, (size_t)width);
        VirtualProtect(p, (SIZE_T)width, old, &back);
    } else if (guest_ok(a, (unsigned)width, 2)) {
        memcpy(p, &v, (size_t)width);   // a frozen region thaws on the write (memory.c)
    }
}

static uint64_t masked(int width, uint64_t v) { return width >= 8 ? v : v & ((1ull << (8 * width)) - 1); }

static uint64_t arith(int op, uint64_t a, uint64_t b) {
    switch (op) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return b < 64 ? a << b : 0;
    case 4: return b < 64 ? a >> b : 0;
    case 5: return a & b;
    case 6: return a | b;
    case 7: return ~a;
    case 8: return a ^ b;
    default: return a;   // 9: move
    }
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

// From i (after a failed condition, or an else reached while running), the opcode to continue at: after the
// matching else (else_ok) or end.
static int skip(const uint32_t *w, int n, int i, int else_ok) {
    int depth = 0;
    while (i < n) {
        int len = op_len(w + i, n - i);
        if (!len) return n;
        uint32_t f = w[i];
        int t = f >> 28;
        i += len;
        if (t == 1 || t == 8) depth++;
        else if (t == 2 && ((f >> 24) & 0xF) == 0 && depth-- == 0) return i;
        else if (t == 2 && depth == 0 && else_ok) return i;
    }
    return n;
}

static uint64_t imm64(const uint32_t *w) { return (uint64_t)w[0] << 32 | w[1]; }

static void run(const Cheat *c, uint64_t keys) {
    uint64_t r[16] = {0};
    int loop[16] = {0};
    const uint32_t *w = c->words;
    int n = c->n, budget = OPS_PER_RUN;
    for (int i = 0; i < n && budget-- > 0;) {
        int len = op_len(w + i, n - i);
        if (!len) return;
        uint32_t f = w[i];
        int t = f >> 28, width = (f >> 24) & 0xF, x = (f >> 20) & 0xF, y = (f >> 16) & 0xF, z = (f >> 12) & 0xF;
        uint64_t rel = ((uint64_t)(f & 0xFF) << 32) | (len > 1 ? w[i + 1] : 0), v;
        switch (t) {
        case 0:   // 0TMR00AA AAAAAAAA V..: store a value at region M + A + register R
            wr(region(x) + rel + r[y], width, width == 8 ? imm64(w + i + 2) : w[i + 2]);
            break;
        case 1:   // 1TMC00AA AAAAAAAA V..: if (region M + A) <cond C> V
            if (!rd(region(x) + rel, width, &v) || !compare(y, v, width == 8 ? imm64(w + i + 2) : w[i + 2])) {
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
        case 5:   // 5TMRI0AA AAAAAAAA: R = (I ? R : region M) + A, read
            if (!rd(z ? r[y] + rel : region(x) + rel, width, &v)) return;   // a broken pointer chain: nothing more
            r[y] = v;
            break;
        case 6: {   // 6T0RIor0 V V: store V at R (+ register r); I: R += width
            uint64_t a = r[y] + (((f >> 8) & 0xF) ? r[(f >> 4) & 0xF] : 0);
            wr(a, width, imm64(w + i + 1));
            if (z) r[y] += (uint64_t)width;
            break;
        }
        case 7:   // 7T0RC000 VVVVVVVV: R = R <op C> V
            r[y] = masked(width, arith(z, r[y], w[i + 1]));
            break;
        case 8:   // 8kkkkkkk: if the buttons are held
            if ((keys & (f & 0x0FFFFFFF)) != (f & 0x0FFFFFFF)) {
                i = skip(w, n, i + len, 1);
                continue;
            }
            break;
        case 9: {   // 9TCRS0s0 / 9TCRS100 V..: R = S <op C> (s or V)
            uint64_t b = (f >> 8) & 0xF ? (width == 8 ? imm64(w + i + 1) : w[i + 1]) : r[(f >> 4) & 0xF];
            r[y] = masked(width, arith(x, r[z], b));
            break;
        }
        case 0xA: {   // ATSRIOxa (aaaaaaaa): store register S at an address of type O
            int o = (f >> 8) & 0xF, m = (f >> 4) & 0xF;
            uint64_t imm = ((uint64_t)(f & 0xF) << 32) | (len > 1 ? w[i + 1] : 0), a;
            switch (o) {
            case 0: a = r[y]; break;
            case 1: a = r[y] + r[m]; break;
            case 2: a = r[y] + imm; break;
            case 3: a = region(m) + r[y]; break;
            case 4: a = region(m) + imm; break;
            default: a = region(m) + r[y] + imm; break;
            }
            wr(a, width, r[x]);
            if (z) r[y] += (uint64_t)width;
            break;
        }
        default:
            return;
        }
        i += len;
    }
}

// ---- the file

static char *read_text(const char *path) {
    uint64_t n = 0;
    char *s = (char *)plat_file_read(path, &n);
    if (s && n >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3)) memmove(s, s + 3, (size_t)n - 2);   // a UTF-8 BOM
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

static void parse(char *s) {
    int cap = 0, wcap = 0;
    for (char *line = s; line && *line;) {
        char *end = strchr(line, '\n'), *next = end ? end + 1 : NULL;
        if (end) *end = 0;
        while (*line == ' ' || *line == '\t') line++;
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) line[--len] = 0;
        if (len >= 2 && (line[0] == '[' || line[0] == '{') && (line[len - 1] == ']' || line[len - 1] == '}')) {
            if (g_n == cap) {
                cap = cap ? cap * 2 : 32;
                g_cheats = (Cheat *)realloc(g_cheats, sizeof *g_cheats * (size_t)cap);
                if (!g_cheats) rt_fatal("cheats: out of memory");
            }
            Cheat *c = &g_cheats[g_n++];
            memset(c, 0, sizeof *c);
            line[len - 1] = 0;
            char *name = line + 1;
            while (*name == ' ' || *name == '\t') name++;
            size_t k = strlen(name);
            while (k && (name[k - 1] == ' ' || name[k - 1] == '\t')) name[--k] = 0;
            c->info.name = _strdup(name);
            c->info.kind = line[0] == '{' ? CHEAT_MASTER : CHEAT_HEADING;
            c->set = -1;
            wcap = 0;
        } else if (g_n) {
            for (char *tok = strtok(line, " \t"); tok; tok = strtok(NULL, " \t")) {
                char *e;
                unsigned long v = strtoul(tok, &e, 16);
                if (e - tok == 8 && !*e) add_word(&g_cheats[g_n - 1], (uint32_t)v, &wcap);
            }
        }
        line = next;
    }
}

// The status line of key, its fields split in place; 0 when none.
static int status_of(char *status, const char *key, char **fields) {
    for (char *line = status; line && *line;) {
        char *end = strchr(line, '\n'), *next = end ? end + 1 : NULL;
        if (!strncmp(line, key, 8) && line[8] == '\t') {
            static char copy[512];
            size_t n = end ? (size_t)(end - line) : strlen(line);
            if (n >= sizeof copy) n = sizeof copy - 1;
            memcpy(copy, line, n);
            copy[n] = 0;
            if (n && copy[n - 1] == '\r') copy[n - 1] = 0;
            int k = 0;
            fields[k++] = copy;
            for (char *p = copy; *p && k < 5; p++)
                if (*p == '\t') *p = 0, fields[k++] = p + 1;
            while (k < 5) fields[k++] = (char *)"";
            return 1;
        }
        line = next;
    }
    return 0;
}

void cheats_init(void) {
    const RtLoaded *m = ldr_count() ? ldr_module(0) : NULL;
    char dir[MAX_PATH * 3], bid[17], path[MAX_PATH * 3 + 64];
    if (!m || !exe_dir(dir, sizeof dir) || !build_id(bid)) return;
    g_main = m->base;
    g_main_size = m->size;
    g_code_end = m->base + ((m->text_end + 0xFFF) & ~0xFFFull);
    snprintf(g_file, sizeof g_file, "cheats\\%s.txt", bid);
    snprintf(path, sizeof path, "%s%s", dir, g_file);
    char *text = read_text(path);
    if (!text) {
        rt_log("cheats: no %s", g_file);
        return;
    }
    parse(text);
    free(text);
    snprintf(path, sizeof path, "%scheats\\%s.status", dir, bid);
    char *status = read_text(path);
    int counts[CHEAT_UNSUPPORTED + 1] = {0}, seen_dups = 0;
    for (int i = 0; i < g_n; i++) {
        Cheat *c = &g_cheats[i];
        // the name hash: a repeated name gets "#2", "#3" ... (tools/cheats.py parse)
        int dup = 1;
        for (int j = 0; j < i; j++) dup += !strcmp(g_cheats[j].info.name, c->info.name);
        char named[600];
        snprintf(named, sizeof named, dup > 1 ? "%s#%d" : "%s", c->info.name, dup);
        seen_dups += dup > 1;
        char key[9], content[9];
        snprintf(key, sizeof key, "%08X", fnv(named, strlen(named)));
        size_t tn = (size_t)c->n * 9 + 1, tl = 0;
        char *t = (char *)malloc(tn);
        if (!t) rt_fatal("cheats: out of memory");
        t[0] = 0;
        for (int k = 0; k < c->n; k++) tl += (size_t)snprintf(t + tl, tn - tl, k ? " %08X" : "%08X", c->words[k]);
        snprintf(content, sizeof content, "%08X", fnv(t, tl));
        free(t);
        snprintf(c->info.key, sizeof c->info.key, "TOTK_CHEAT_%s", key);
        int master = c->info.kind == CHEAT_MASTER;
        char *f[5];
        CheatKind kind;
        if (status && status_of(status, key, f) && !strcmp(f[1], content)) {
            const char *k = f[2];
            kind = !strcmp(k, "code") ? CHEAT_CODE : !strcmp(k, "key") ? CHEAT_KEY : !strcmp(k, "master") ? CHEAT_MASTER
                 : !strcmp(k, "restore") ? CHEAT_RESTORE : !strcmp(k, "heading") ? CHEAT_HEADING
                 : !strcmp(k, "data") ? CHEAT_DATA : CHEAT_UNSUPPORTED;
            c->info.buttons = (uint32_t)strtoul(f[3], NULL, 16);
            snprintf(c->note, sizeof c->note, "%s", f[4]);
            if (kind == CHEAT_CODE || kind == CHEAT_KEY || kind == CHEAT_MASTER) {
                char set[32];
                snprintf(set, sizeof set, "cheat_%08x", fnv(named, strlen(named)));
                c->set = rt_patch_set(set);
                if (c->set < 0 && kind != CHEAT_MASTER) kind = CHEAT_REBUILD;
            }
        } else {
            kind = classify(c);
        }
        if (master && kind == CHEAT_DATA) kind = CHEAT_MASTER;   // a data master code: the VM, always on
        c->info.kind = kind;
        c->info.note = c->note;
        const char *v = cfg_get(c->info.key);
        c->on = kind == CHEAT_MASTER || (v && *v && strcmp(v, "0") != 0);
        if (kind == CHEAT_KEY && c->set >= 0) rt_patch_switch(c->set, 0);   // on while the buttons are held
        if (kind == CHEAT_KEY || kind == CHEAT_DATA || (kind == CHEAT_MASTER && c->set < 0)) g_vm = 1;
        counts[kind]++;
    }
    free(status);
    int on = 0;
    for (int i = 0; i < g_n; i++) on += g_cheats[i].on && g_cheats[i].info.kind != CHEAT_HEADING;
    rt_log("cheats: %s: %d cheats, %d on; %d code, %d while buttons are held, %d data, %d master, %d restore, "
           "%d headings, %d need the setup again, %d not supported%s",
           g_file, g_n, on, counts[CHEAT_CODE], counts[CHEAT_KEY], counts[CHEAT_DATA], counts[CHEAT_MASTER],
           counts[CHEAT_RESTORE], counts[CHEAT_HEADING], counts[CHEAT_REBUILD], counts[CHEAT_UNSUPPORTED],
           seen_dups ? " (repeated names numbered)" : "");
}

int cheats_count(void) { return g_n; }

const CheatInfo *cheats_info(int i) { return i >= 0 && i < g_n ? &g_cheats[i].info : NULL; }

int cheats_on(int i) { return i >= 0 && i < g_n && g_cheats[i].on; }

const char *cheats_file(void) { return g_file; }

void cheats_set(int i, int on) {
    if (i < 0 || i >= g_n) return;
    Cheat *c = &g_cheats[i];
    if (c->info.kind != CHEAT_CODE && c->info.kind != CHEAT_KEY && c->info.kind != CHEAT_DATA) return;
    InterlockedExchange(&c->on, on != 0);
    if (c->info.kind == CHEAT_CODE) rt_patch_switch(c->set, on);   // CHEAT_KEY: cheats_frame follows the buttons
    cfg_set(c->info.key, on ? "1" : "0");
    rt_log("cheats: %s %s", c->info.name, on ? "on" : "off");
}

void cheats_frame(uint64_t frame) {
    if (!g_vm || frame < 60) return;   // the game polls its input (and starts SDL) before this
    uint64_t keys = 0;
    int32_t lx, ly, rx, ry;
    input_poll(&keys, &lx, &ly, &rx, &ry);   // 0 while the menu is open
    for (int i = 0; i < g_n; i++) {
        Cheat *c = &g_cheats[i];
        switch (c->info.kind) {
        case CHEAT_KEY:
            if (c->set >= 0) {
                int want = c->on && (keys & c->info.buttons) == c->info.buttons;
                if (rt_patch_enabled(c->set) != want) rt_patch_switch(c->set, want);
            }
            break;
        case CHEAT_DATA:
            if (c->on) run(c, keys);
            break;
        case CHEAT_MASTER:
            if (c->set < 0) run(c, keys);
            break;
        default:
            break;
        }
    }
}

const char *cheats_buttons(uint32_t mask, char *buf, int n) {
    static const char *const names[] = {"A", "B", "X", "Y", "Left stick press", "Right stick press", "L", "R", "ZL",
                                        "ZR", "+", "-", "D-pad left", "D-pad up", "D-pad right", "D-pad down",
                                        "Left stick left", "Left stick up", "Left stick right", "Left stick down",
                                        "Right stick left", "Right stick up", "Right stick right", "Right stick down"};
    int k = 0;
    buf[0] = 0;
    for (int b = 0; b < 24; b++)
        if (mask & (1u << b)) k += snprintf(buf + k, (size_t)(n > k ? n - k : 0), "%s%s", k ? " + " : "", names[b]);
    return buf;
}
