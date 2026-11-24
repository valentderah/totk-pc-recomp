// Mod plugins' inits (recomp/layout.py plugin<N>, an exlaunch plugin such as a mod's exefs/subsdk9), run natively on
// this runtime's own kernel and sdk. The design follows #845 by itaskaev2, with the init run natively instead of under
// Unicorn, and run again at every start instead of an image taken after one run at build time.
//
// The service run: totk.exe --plugin-init DIR runs the plugins' DT_INITs, records what the inits did and exits before
// any game module's constructor, nnMain, graphics or audio. TOTK_PLUGIN_ONLY names the one plugin module whose init
// runs ("-": none); the others stay loaded, as they are at play. tools/exefs_plugins.py runs each plugin on its own,
// so a plugin that is refused, crashes or hangs costs only itself; it checks each record and writes the plugin's patch
// set (its hook sites, recomp/patches.py `plugin` lines).
//
// At every start (plugins_start, from the module-init callback in boot.c): when the build has a plugin whose patch set
// its mod switched on, sdk's DT_INIT runs first, then the plugin's, in the same environment as in the service run, and
// then the game's constructors. That is the order of the Switch's rtld, which runs the modules' DT_INITs from the last
// loaded to the first (oss-rtld, librtld/source/ld.cpp __rtld_modules_init walks g_pAutoLoadList backwards; the list is
// filled in address order by __rtld_relocate_modules in librtld/source/relocation.cpp: main, subsdk*, sdk), so sdk
// runs before a subsdk plugin and the plugin before the game's main. The plugin keeps what its init made: heap blocks,
// its GOT, its alias addresses. What the init wrote into the game modules (its hook branches) and the plugin's .text
// after it (its stubs and trampolines) are compared with the record the build used (patches.h RT_PLUGIN_WRITES and
// RT_PLUGINS): equal, the plugin's set stays on and the generated hook sites call into it; different, the set goes off
// for this run, the reason is logged and the mod waits for a rebuild (mods.c mods_plugin_off). Each plugin is compared
// right after its own init, and the branches it wrote into main's .text are taken back before the next init: the next
// plugin sees the game's own bytes, as in its own service run (two plugins hooking one site each relocate the game's
// instruction; the generated site runs the one the Mods tab order picks). The lifted code is the code that runs, and
// main keeps the game's bytes.
// Without a plugin in the build none of this runs (RT_PLUGIN_COUNT 0).
//
// What exlaunch's init needs from the process, and how this run gives it:
//   - Its module walk (exl::util::FindModules) reads QueryMemory from 0 up and takes every Code RX, Code R, CodeData
//     RW run as a module, the first being rtld and the second main. Our layout has no rtld and reports the host's
//     ranges as Code RX (memory.c taken_gap), which would make it take a gap for a module. mem_query_hook shows the
//     game's own rtld NSO (exefs/rtld, read, never run) right below main, and the host's ranges as Inaccessible.
//   - It picks a random free range in the ASLR region (GetSystemTick seeds it) for its process-memory aliases. Our
//     ASLR region is almost all host ranges, so a free pool of POOL_SIZE is reserved after the alias region.
//   - GetInfo 65001 (svc.c) gives it the current process handle; MapProcessMemory maps main's .text and its own pages
//     at the picked address. Guest addresses are host addresses and the modules are not file-backed, so an alias is a
//     copy: its pages hold the source's bytes when mapped, and the words written through it are written into the
//     source when it is unmapped and when the init returns. That is how the stubs reach the plugin's image and the
//     hook branches reach main's .text.
//   - nn::oe::GetDisplayVersion is answered with this build's game version, so the plugin picks its table for the
//     installed game without the applet services.
//   - GetInfo 6, 16 and 17, the total memory and the system resource size and use, are constants (memory.c
//     mem_info); 7, the memory in use, is the same at the same point of the same init. Zonai Fieldworks asks them
//     after its hooks are in.
//   - SetHeapSize grows the process heap past the size the game's nninitStartup set before the init runs (Zonai
//     Fieldworks keeps a pool in the tail). The heap base is fixed and the game asked its size the same way at every
//     start, so the init gets the same answer each time.
// Only svcs the init needs are allowed (SetHeapSize, QueryMemory, GetSystemTick, OutputDebugString, GetInfo 2-7,
// 12-17 and 65001, Map/UnmapProcessMemory); any other one, such as CreateSession and ReplyAndReceive of exlaunch's
// process-handle fallback, ends the run with the reason in the record.
//
// The record, in DIR: record.json (modules, which plugins' inits ran, the writes into every game module as words, the
// alias calls, svc and import counts, the tick offset, a refusal), plugin<N>.img (the plugin's image after its init,
// all segments) and plugin<N>.pre.img (the same after loading and relocation, before the init).
// TOTK_PLUGIN_TICK_OFFSET adds to every GetSystemTick, so two runs pick different aliases.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "dispatch.h"
#include "host.h"
#include "kernel.h"
#include "mods.h"
#include "platform/platform.h"
#include "plugin_writes.h"

#if __has_include("patches.h")
#include "patches.h"
#endif
#ifndef RT_PLUGIN_COUNT
#define RT_PLUGIN_COUNT 0
#endif

#define PAGE 0x1000ull
#define GiB (1ull << 30)
#define POOL_SIZE (64 * GiB)
#define POOL_ALIGN (4 * GiB)
#define RTLD_ALIGN 0x200000ull
// exlaunch maps a page alias for every patch it writes and unmaps it after; Zonai Fieldworks makes over 211 in its
// init. Each slot is 48 bytes, and only the live aliases hold a copy of their pages.
#define MAX_ALIASES 2048
#define MAX_IMPORTS 64
#define CUR_PROCESS 0xFFFF8001u
// Horizon memory states and permissions of QueryMemory, and results.
enum { ST_FREE = 0, ST_CODE = 3, ST_CODEDATA = 4, ST_SHARED_CODE = 0xF, ST_INACCESSIBLE = 0x10 };
enum { PERM_R = 1, PERM_RW = 3, PERM_RX = 5 };
#define R_OK 0u
#define R_INVALID_ADDRESS 0xcc01u
#define R_INVALID_STATE 0xd401u
#define R_INVALID_HANDLE 0xe401u
#define R_LIMIT_REACHED 0x10801u
#define SYM_DISPLAY_VERSION "_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE"

typedef struct {
    uint64_t dst, src, size;
    uint8_t *snap;   // the source's bytes when mapped: the words that differ from it were written through the alias
    int live, unmapped;
} Alias;
static Alias aliases[MAX_ALIASES];
static int naliases;
static uint64_t rtld_base, rtld_size, rtld_text_end, rtld_data_off;
static uint64_t pool_base;
static int running;   // a plugin's init runs: svcs and imports are counted
static int play;      // plugins_start: a refused SVC fails the plugin instead of ending the process
static char failed[256];   // play: why the running init was refused
static uint64_t svc_count[0x80], info_count[18], info_65001;
static struct { const char *name; uint64_t calls; } imports[MAX_IMPORTS];
static int nimports;
static uint8_t **pre;   // per loaded module: its image before the plugins' inits
static uint8_t *ran;    // service run, per loaded module: its init ran

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static void clip(uint64_t a, uint64_t *lo, uint64_t *hi, uint64_t b, uint64_t n) {
    if (!n) return;
    uint64_t e = b + n;
    if (e <= a && e > *lo) *lo = e;
    if (b > a && b < *hi) *hi = b;
}

static void clip_aliases(uint64_t a, uint64_t *lo, uint64_t *hi) {
    for (int i = 0; i < naliases; i++)
        if (aliases[i].live) clip(a, lo, hi, aliases[i].dst, aliases[i].size);
}

static int in_module(uint64_t a) {
    for (int i = 0; i < ldr_count(); i++)
        if (a - ldr_module(i)->base < ldr_module(i)->size) return 1;
    return 0;
}

// ---- QueryMemory as exlaunch expects it (memory.c mem_query_hook)
static void query_hook(uint64_t a, RtMemoryInfo *r) {
    for (int i = 0; i < naliases; i++) {
        const Alias *al = &aliases[i];
        if (al->live && a - al->dst < al->size) {
            *r = (RtMemoryInfo){.base = al->dst, .size = al->size, .state = ST_SHARED_CODE, .perm = PERM_RW};
            return;
        }
    }
    if (a - rtld_base < rtld_size) {
        uint64_t off = a - rtld_base, text = align_up(rtld_text_end, PAGE);
        if (off < text) *r = (RtMemoryInfo){.base = rtld_base, .size = text, .state = ST_CODE, .perm = PERM_RX};
        else if (off < rtld_data_off)
            *r = (RtMemoryInfo){.base = rtld_base + text, .size = rtld_data_off - text, .state = ST_CODE, .perm = PERM_R};
        else
            *r = (RtMemoryInfo){.base = rtld_base + rtld_data_off, .size = rtld_size - rtld_data_off,
                                .state = ST_CODEDATA, .perm = PERM_RW};
        return;
    }
    uint64_t lo = r->base, hi = r->base + r->size;
    if (a - pool_base < POOL_SIZE) {
        lo = pool_base, hi = pool_base + POOL_SIZE;
        *r = (RtMemoryInfo){.state = ST_FREE};
    } else if (r->state == ST_CODE && !in_module(a)) {   // a host range (memory.c taken_gap)
        clip(a, &lo, &hi, rtld_base, rtld_size);
        clip(a, &lo, &hi, pool_base, POOL_SIZE);
        *r = (RtMemoryInfo){.state = ST_INACCESSIBLE};
    } else if (r->state != ST_FREE) {
        return;
    }
    clip_aliases(a, &lo, &hi);
    r->base = lo;
    r->size = hi - lo;
}

// ---- Map/UnmapProcessMemory (svc.c rt_process_memory)
static const RtLoaded *module_holding(uint64_t a, uint64_t n) {
    for (int i = 0; i < ldr_count(); i++) {
        const RtLoaded *m = ldr_module(i);
        if (a >= m->base && n <= m->size && a - m->base <= m->size - n) return m;
    }
    return NULL;
}

// Writes the words changed through an alias into its source. The source's read-only pages (sealed .text and .rodata)
// are made writable for the copy and sealed again.
static void flush(Alias *al) {
    const RtLoaded *m = module_holding(al->src, al->size);
    const uint8_t *now = (const uint8_t *)(uintptr_t)al->dst;
    for (uint64_t off = 0; off < al->size; off += PAGE) {
        if (!memcmp(now + off, al->snap + off, PAGE)) continue;
        uint64_t page = al->src + off;
        int sealed = page - m->base < m->data_off;
        if (sealed && !plat_mem_protect(page, PAGE, PLAT_PROT_RW)) rt_fatal("plugin-init: cannot unseal %#llx", (unsigned long long)page);
        for (uint64_t w = 0; w < PAGE; w += 4)
            if (memcmp(now + off + w, al->snap + off + w, 4)) memcpy((void *)(uintptr_t)(page + w), now + off + w, 4);
        if (sealed && !plat_mem_protect(page, PAGE, PLAT_PROT_READ)) rt_fatal("plugin-init: cannot seal %#llx", (unsigned long long)page);
    }
    memcpy(al->snap, now, al->size);   // flushed: a later flush writes only what changes after this one
}

static uint32_t process_memory(int unmap, uint64_t dst, uint32_t process, uint64_t src, uint64_t size) {
    if (process != CUR_PROCESS) return R_INVALID_HANDLE;
    if (!size || ((dst | src | size) & (PAGE - 1))) return R_INVALID_ADDRESS;
    if (unmap) {
        for (int i = 0; i < naliases; i++) {
            Alias *al = &aliases[i];
            if (!al->live || al->dst != dst || al->src != src || al->size != size) continue;
            flush(al);
            if (!plat_mem_decommit(dst, size)) rt_fatal("plugin-init: cannot unmap the alias at %#llx", (unsigned long long)dst);
            free(al->snap);
            al->snap = NULL;
            al->live = 0;
            al->unmapped = 1;
            return R_OK;
        }
        return R_INVALID_STATE;
    }
    if (!module_holding(src, size)) {   // only the modules' own memory
        rt_log("plugin-init: MapProcessMemory refused: the source %#llx+%#llx is no module's memory",
               (unsigned long long)src, (unsigned long long)size);
        return R_INVALID_STATE;
    }
    RtMemoryInfo q;
    mem_query(dst, &q);
    if (q.state != ST_FREE || dst + size > q.base + q.size) {
        rt_log("plugin-init: MapProcessMemory refused: the destination %#llx+%#llx is not free (state %#x)",
               (unsigned long long)dst, (unsigned long long)size, q.state);
        return R_INVALID_STATE;
    }
    if (naliases == MAX_ALIASES) {
        rt_log("plugin-init: MapProcessMemory refused: all %d alias slots used", MAX_ALIASES);
        return R_LIMIT_REACHED;
    }
    if (!plat_mem_commit(dst, size)) return R_INVALID_STATE;   // not inside one of our reservations
    Alias *al = &aliases[naliases];
    al->snap = malloc(size);
    if (!al->snap) rt_fatal("plugin-init: out of memory for a %#llx byte alias", (unsigned long long)size);
    memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, size);
    memcpy(al->snap, (const void *)(uintptr_t)src, size);
    *al = (Alias){dst, src, size, al->snap, 1, 0};
    naliases++;
    return R_OK;
}

// ---- svcs and imports
static void write_record(const char *refused);

static void refuse(const char *why) {
    rt_log("plugin-init: refused: %s", why);
    if (play) {   // the init goes on with an error result; the plugin stays off (plugins_start)
        if (!failed[0]) snprintf(failed, sizeof failed, "%s", why);
        return;
    }
    write_record(why);
    fflush(NULL);
    plat_exit(3);
}

#define R_NOT_IMPLEMENTED 0xF601u

static int plugin_svc(Cpu *c, uint32_t imm, uint64_t pc) {
    if (!running) return 0;
    svc_count[imm & 0x7F]++;
    char why[256], at[128];
    switch (imm) {
    case 0x01: case 0x06: case 0x1E: case 0x27: case 0x74: case 0x75: return 0;
    case 0x29: {
        uint32_t id = (uint32_t)c->x[1];
        if ((id >= 2 && id <= 7) || (id >= 12 && id <= 17)) {
            info_count[id]++;
            return 0;
        }
        if (id == 65001) {
            info_65001++;
            return 0;
        }
        snprintf(why, sizeof why, "GetInfo id %u at %s", id, ldr_describe(pc, at, sizeof at));
        refuse(why);
        c->x[0] = R_NOT_IMPLEMENTED;
        return 1;
    }
    }
    snprintf(why, sizeof why, "svc %#x at %s (only SetHeapSize, QueryMemory, GetSystemTick, OutputDebugString, "
             "GetInfo 2-7, 12-17 and 65001, Map/UnmapProcessMemory are served)", imm, ldr_describe(pc, at, sizeof at));
    refuse(why);
    c->x[0] = R_NOT_IMPLEMENTED;
    return 1;
}

// Every JUMP_SLOT import of a plugin binds here (loader.c resolve): counted by name, then the guest definition runs,
// except nn::oe::GetDisplayVersion, which gets this build's game version.
static void plugin_import(Cpu *c) {
    const char *name = hostfn_name(c->pc);
    int i = 0;
    while (i < nimports && strcmp(imports[i].name, name)) i++;
    if (i == nimports && nimports < MAX_IMPORTS) imports[nimports++].name = name;
    if (i < MAX_IMPORTS && running) imports[i].calls++;
    if (!strcmp(name, SYM_DISPLAY_VERSION)) {   // DisplayVersion { char value[16]; }
        char v[16] = {0};
        snprintf(v, sizeof v, "%d.%d.%d", rt_game_version / 100, rt_game_version / 10 % 10, rt_game_version % 10);
        memcpy((void *)(uintptr_t)c->x[0], v, sizeof v);
        return;
    }
    rt_original(c);
}

// ---- the record
static FILE *open_out(const char *file, const char *mode) {
    char path[PLAT_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", rt_config()->plugin_init, file);
    FILE *f = fopen(path, mode);
    if (!f) rt_fatal("plugin-init: cannot write %s", path);
    return f;
}

static void write_image(const char *file, const void *p, uint64_t n) {
    FILE *f = open_out(file, "wb");
    if (fwrite(p, 1, n, f) != n || fclose(f)) rt_fatal("plugin-init: cannot write %s", file);
}

static void sha256_hex(const void *p, uint64_t n, char out[65]) {
    uint8_t d[32];
    rt_sha256(p, n, d);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", *s);
        else fputc(*s, f);
    }
    fputc('"', f);
}

// The words of each game module that differ from its image before the plugins' inits: (module, offset, old, new).
static void write_writes(FILE *f) {
    int first = 1;
    for (int i = 0; i < ldr_count(); i++) {
        const RtLoaded *m = ldr_module(i);
        if (m->plugin || !pre || !pre[i]) continue;
        const uint8_t *now = (const uint8_t *)(uintptr_t)m->base;
        for (uint64_t off = 0; off < m->size; off += PAGE) {
            if (!memcmp(now + off, pre[i] + off, PAGE)) continue;
            for (uint64_t w = off; w < off + PAGE; w += 4) {
                uint32_t a, b;
                memcpy(&a, pre[i] + w, 4);
                memcpy(&b, now + w, 4);
                if (a == b) continue;
                // fn_start: a lifted function starts at the word (a b there replaces the function)
                fprintf(f, "%s\n  {\"module\": \"%s\", \"offset\": \"%#llx\", \"old\": \"%#010x\", \"new\": \"%#010x\", "
                        "\"fn_start\": %s}", first ? "" : ",", m->name, (unsigned long long)w, a, b,
                        rt_dispatch_lookup(m->base + w) ? "true" : "false");
                first = 0;
            }
        }
    }
}

static void write_record(const char *refused) {
    for (int i = 0; i < naliases; i++)
        if (aliases[i].live) flush(&aliases[i]);
    FILE *f = open_out("record.json", "w");
    // The marker of generated game-derived files (tools/check_content.py): the record never goes into the repository.
    fprintf(f, "{\n\"marker\": \"%s%s\",\n\"game_version\": %d,\n\"tick_offset\": \"%#llx\",\n\"refused\": ",
            "TOTK_RECOMP_", "GENERATED_FROM_GAME_CODE", rt_game_version, (unsigned long long)rt_svc_tick_offset);
    if (refused) json_str(f, refused);
    else fputs("null", f);
    fprintf(f, ",\n\"rtld\": {\"base\": \"%#llx\", \"size\": \"%#llx\"},\n\"pool\": {\"base\": \"%#llx\", \"size\": \"%#llx\"},",
            (unsigned long long)rtld_base, (unsigned long long)rtld_size, (unsigned long long)pool_base,
            (unsigned long long)POOL_SIZE);
    fputs("\n\"modules\": [", f);
    for (int i = 0; i < ldr_count(); i++) {
        const RtLoaded *m = ldr_module(i);
        fprintf(f, "%s\n  {\"name\": \"%s\", \"base\": \"%#llx\", \"size\": \"%#llx\", \"text_end\": \"%#llx\", "
                "\"data_off\": \"%#llx\", \"init\": \"%#llx\", \"plugin\": %s",
                i ? "," : "", m->name, (unsigned long long)m->base, (unsigned long long)m->size,
                (unsigned long long)m->text_end, (unsigned long long)m->data_off, (unsigned long long)m->init,
                m->plugin ? "true" : "false");
        if (m->plugin) fprintf(f, ", \"ran\": %s", ran && ran[i] ? "true" : "false");
        if (m->plugin && !refused) {
            char img[64], pre_img[64], hex[65];
            snprintf(img, sizeof img, "%s.img", m->name);
            snprintf(pre_img, sizeof pre_img, "%s.pre.img", m->name);
            write_image(img, (const void *)(uintptr_t)m->base, m->size);
            if (pre && pre[i]) write_image(pre_img, pre[i], m->size);
            sha256_hex((const void *)(uintptr_t)m->base, m->size, hex);
            fprintf(f, ", \"image\": \"%s\", \"image_sha256\": \"%s\", \"pre_image\": \"%s\"", img, hex, pre_img);
        }
        if (m->plugin) {   // the lifted function starts: a main write may branch straight to one (tools/exefs_plugins.py)
            RtModule lm[16];
            int n = rt_linked_modules(lm, 16);
            for (int k = 0; k < n; k++) {
                if (strcmp(lm[k].name, m->name)) continue;
                fputs(", \"functions\": [", f);
                for (uint64_t j = 0; j < lm[k].count; j++)
                    fprintf(f, "%s\"%#llx\"", j ? ", " : "", (unsigned long long)(lm[k].addrs[j] - m->base));
                fputc(']', f);
            }
        }
        for (int k = 0; k < rt_layout_count; k++)
            if (rt_layout[k].file && !strcmp(rt_layout[k].name, m->name)) {
                fputs(", \"file\": ", f);
                json_str(f, rt_layout[k].file);
                fprintf(f, ", \"nso_sha256\": \"%s\"", rt_layout[k].sha256);
            }
        fputc('}', f);
    }
    fputs("\n],\n\"writes\": [", f);
    write_writes(f);
    fputs("\n],\n\"aliases\": [", f);
    for (int i = 0; i < naliases; i++)
        fprintf(f, "%s\n  {\"dst\": \"%#llx\", \"src\": \"%#llx\", \"size\": \"%#llx\", \"unmapped\": %s}", i ? "," : "",
                (unsigned long long)aliases[i].dst, (unsigned long long)aliases[i].src,
                (unsigned long long)aliases[i].size, aliases[i].unmapped ? "true" : "false");
    fputs("\n],\n\"svcs\": {", f);
    int first = 1;
    for (int i = 0; i < 0x80; i++)
        if (svc_count[i]) fprintf(f, "%s\"%#x\": %llu", first ? "" : ", ", i, (unsigned long long)svc_count[i]), first = 0;
    fputs("},\n\"get_info\": {", f);
    first = 1;
    for (int i = 0; i < 18; i++)
        if (info_count[i]) fprintf(f, "%s\"%d\": %llu", first ? "" : ", ", i, (unsigned long long)info_count[i]), first = 0;
    if (info_65001) fprintf(f, "%s\"65001\": %llu", first ? "" : ", ", (unsigned long long)info_65001);
    fputs("},\n\"imports\": {", f);
    for (int i = 0; i < nimports; i++) {
        fputs(i ? ", " : "", f);
        json_str(f, imports[i].name);
        fprintf(f, ": %llu", (unsigned long long)imports[i].calls);
    }
    fputs("}\n}\n", f);
    if (fclose(f)) rt_fatal("plugin-init: cannot write record.json");
}

// ---- setup and run
static int layout_plugins(void) {
    int plugins = 0;
    for (int i = 0; i < rt_layout_count; i++) plugins += rt_layout[i].file != NULL;
    return plugins;
}

static void environment(void);

// Before ldr_load: the import observer, the svc hooks, the pool and the stand-in for rtld.
void plugin_init_setup(void) {
    if (!layout_plugins()) rt_fatal("plugin-init: this build's layout has no plugin module (recomp/gen.py --plugins)");
    plat_file_mkdir_p(rt_config()->plugin_init);
    const char *off = cfg_get("TOTK_PLUGIN_TICK_OFFSET");
    rt_svc_tick_offset = off ? strtoull(off, NULL, 0) : 0;
    environment();
}

// The environment of a plugin's init, the same in the service run and at every start.
static void environment(void) {
    ldr_plugin_import = plugin_import;
    rt_process_memory = process_memory;
    rt_plugin_svc = plugin_svc;

    const RtGuestLayout *g = mem_layout();
    uint64_t main_base = rt_layout[0].base;
    rtld_size = ldr_nso_size(rt_config()->exefs, "rtld");
    rtld_base = main_base - align_up(rtld_size, RTLD_ALIGN);
    for (uint64_t at = align_up(g->alias.base + g->alias.size, POOL_ALIGN); at + POOL_SIZE <= rtld_base && !pool_base;
         at += POOL_ALIGN)
        if (plat_mem_reserve(at, POOL_SIZE, 0)) pool_base = at;
    if (!pool_base) rt_fatal("plugin-init: no free %llu GiB below main for the alias pool", POOL_SIZE / GiB);
    if (!plat_mem_reserve(rtld_base, align_up(rtld_size, PAGE), 0))
        rt_fatal("plugin-init: %#llx below main is taken; rtld goes there", (unsigned long long)rtld_base);
    ldr_map_image(rt_config()->exefs, "rtld", rtld_base, &rtld_text_end, &rtld_data_off);
    mem_query_hook = query_hook;
    rt_log("plugin-init: rtld at %#llx+%#llx, alias pool %#llx+%llu GiB, tick offset %#llx", (unsigned long long)rtld_base,
           (unsigned long long)rtld_size, (unsigned long long)pool_base, POOL_SIZE / GiB,
           (unsigned long long)rt_svc_tick_offset);
}

// rt_boot, before ldr_load: the plugins' environment when the build has one (nothing otherwise).
void plugins_setup(void) {
    if (!RT_PLUGIN_COUNT || !layout_plugins()) return;
    play = 1;
    environment();
}

// From nn::init::Start's module-init callback (boot.c), in place of the game's constructors. rtld initialises the
// modules from the last loaded to the first, so on the Switch sdk's DT_INIT runs before a subsdk plugin's; then each
// plugin's, the last first. Exits with the record written.
void plugin_init_run(Cpu *c, const RtLoaded *sdk) {
    rt_log("plugin-init: ENTER %s DT_INIT", sdk->name);
    rt_call(c, sdk->init, 0);
    pre = calloc((size_t)ldr_count(), sizeof *pre);
    if (!pre) rt_fatal("plugin-init: out of memory");
    for (int i = 0; i < ldr_count(); i++) {   // the images before the plugins run
        const RtLoaded *m = ldr_module(i);
        pre[i] = malloc(m->size);
        if (!pre[i]) rt_fatal("plugin-init: out of memory for a copy of %s", m->name);
        memcpy(pre[i], (const void *)(uintptr_t)m->base, m->size);
    }
    const char *only = cfg_get("TOTK_PLUGIN_ONLY");
    ran = calloc((size_t)ldr_count(), 1);
    if (!ran) rt_fatal("plugin-init: out of memory");
    for (int i = ldr_count() - 1; i >= 0; i--) {
        const RtLoaded *m = ldr_module(i);
        if (!m->plugin) continue;
        if (only && *only && strcmp(only, m->name)) {
            rt_log("plugin-init: %s stays loaded, its init does not run (TOTK_PLUGIN_ONLY=%s)", m->name, only);
            continue;
        }
        ran[i] = 1;
        if (!m->init) refuse("a plugin without DT_INIT");
        rt_log("plugin-init: ENTER %s DT_INIT at %#llx (%llu init_array entries)", m->name, (unsigned long long)m->init,
               (unsigned long long)(m->init_array_size / 8));
        running = 1;
        rt_trace |= RT_TRACE_PLUGIN;
        rt_call(c, m->init, 0);
        rt_trace &= ~(unsigned)RT_TRACE_PLUGIN;
        running = 0;
        rt_log("plugin-init: %s DT_INIT returned", m->name);
    }
    write_record(NULL);
    int live = 0;
    for (int i = 0; i < naliases; i++) live += aliases[i].live;
    rt_log("plugin-init: record written to %s (%d process-memory maps, %d still mapped)", rt_config()->plugin_init,
           naliases, live);
    fflush(NULL);
    plat_exit(0);
}

// ---- at every start
#if RT_PLUGIN_COUNT
static const struct { const char *module; int set; const char *sha256; } plugin_tab[] = {
#define X(i, mod, set, sha) {mod, set, sha},
    RT_PLUGINS(X)
#undef X
};
static const PluginWrite write_tab[] = {RT_PLUGIN_WRITES(PLUGIN_WRITE)};
#define NWRITES ((int)(sizeof write_tab / sizeof *write_tab))
#ifdef RT_PLUGIN_ROOTS
static const struct { const char *module; uint64_t off; } root_tab[] = {
#define X(mod, off) {mod, off},
    RT_PLUGIN_ROOTS(X)
#undef X
};
#define NROOTS ((int)(sizeof root_tab / sizeof *root_tab))
#else
#define NROOTS 0
#endif

static const RtLoaded *loaded(const char *name) {
    for (int i = 0; i < ldr_count(); i++)
        if (!strcmp(ldr_module(i)->name, name)) return ldr_module(i);
    return NULL;
}

// TOTK_PLUGIN_TRACE = 1: the first call of each hook entry (patches.h RT_PLUGIN_ROOTS: callbacks, replace and call
// targets) is logged with its count so far; the entries run through count_entry, their dispatch slots rerouted once
// at start (dispatch.c rt_dispatch_reroute). The calls inside the plugin are direct and not counted.
static struct { uint64_t addr; GuestFn fn; uint64_t calls; } traced[64];
static int ntraced;

static void count_entry(Cpu *c) {
    for (int i = 0; i < ntraced; i++) {
        if (traced[i].addr != c->pc) continue;
        if (__atomic_add_fetch(&traced[i].calls, 1, __ATOMIC_RELAXED) == 1) {
            char at[128];
            rt_log("plugins: hook entry %s reached (first call)", ldr_describe(c->pc, at, sizeof at));
        }
        __attribute__((musttail)) return traced[i].fn(c);
    }
    rt_fatal("plugins: traced entry %#llx without its function", (unsigned long long)c->pc);
}

static void trace_entries(void) {
#ifdef RT_PLUGIN_ROOTS
    for (int i = 0; i < NROOTS && ntraced < 64; i++) {
        const RtLoaded *m = loaded(root_tab[i].module);
        if (!m) continue;
        uint64_t a = m->base + root_tab[i].off;
        GuestFn f = rt_dispatch_lookup(a);
        if (!f) continue;
        traced[ntraced] = (typeof(traced[0])){a, f, 0};
        if (rt_dispatch_reroute(a, count_entry)) ntraced++;
        else rt_log("plugins: trace: cannot reroute %#llx", (unsigned long long)a);
    }
#endif
    rt_log("plugins: trace: %d hook entries counted", ntraced);
}

// Takes back a page an init changed in the sealed part (.text, .rodata) of a game module: main keeps the game's own
// bytes, the lifted code being what runs.
static void restore_page(const RtLoaded *m, uint64_t off, const uint8_t *was) {
    if (!plat_mem_protect(m->base + off, PAGE, PLAT_PROT_RW)) rt_fatal("plugins: cannot unseal %s+%#llx", m->name,
                                                                     (unsigned long long)off);
    memcpy((void *)(uintptr_t)(m->base + off), was, PAGE);
    plat_mem_protect(m->base + off, PAGE, PLAT_PROT_READ);
}

// After plugin i's init: what it wrote into the game modules, word by word, against the record of the build (its
// entries of RT_PLUGIN_WRITES, plugin_writes.h plugin_write_find). A recorded word the init did not change counts as
// missing, unless the record takes any value there (the init may have written the value the word held). The first
// difference goes into why (unless a reason is there already). Then the game
// modules are made what they were before the init: the sealed part (.text, .rodata) gets the game's bytes back, a
// write elsewhere stays and goes into pre, so the next plugin's init is compared only with what it writes itself.
// The aliases this init mapped (from first_alias on) are flushed first; an earlier init's were flushed after it.
static void check_writes(int plugin, int first_alias, char *why, size_t cap) {
    for (int i = first_alias; i < naliases; i++)
        if (aliases[i].live) flush(&aliases[i]);
    uint8_t seen[NWRITES > 0 ? NWRITES : 1];
    memset(seen, 0, sizeof seen);
    int unexpected = 0, missing = 0;
    char first[160] = "";
    for (int i = 0; i < ldr_count(); i++) {
        const RtLoaded *m = ldr_module(i);
        if (!pre[i]) continue;
        uint8_t *now = (uint8_t *)(uintptr_t)m->base;
        for (uint64_t off = 0; off < m->size; off += PAGE) {
            if (!memcmp(now + off, pre[i] + off, PAGE)) continue;
            for (uint64_t w = off; w < off + PAGE; w += 4) {
                uint32_t a, b;
                memcpy(&a, pre[i] + w, 4);
                memcpy(&b, now + w, 4);
                if (a == b) continue;
                int k = plugin_write_find(write_tab, NWRITES, plugin, m->name, w, a, b);
                if (k >= 0) {
                    seen[k] = 1;
                } else if (!unexpected++) {
                    snprintf(first, sizeof first, "%s+%#llx %08x -> %08x", m->name, (unsigned long long)w, a, b);
                }
            }
            if (off < m->data_off) restore_page(m, off, pre[i] + off);
            else memcpy(pre[i] + off, now + off, PAGE);
        }
    }
    for (int k = 0; k < NWRITES; k++) missing += write_tab[k].plugin == plugin && !seen[k] && !write_tab[k].any;
    if (why[0]) return;
    if (unexpected)
        snprintf(why, cap, "its init wrote %d words the build did not record (first %s)", unexpected, first);
    else if (missing)
        snprintf(why, cap, "%d of the words the build recorded were not written", missing);
}
#endif

int plugins_start(Cpu *c, const RtLoaded *sdk) {
#if RT_PLUGIN_COUNT
    if (!play) return 0;
    int run[RT_PLUGIN_COUNT] = {0}, nrun = 0;
    char why[RT_PLUGIN_COUNT][256];
    for (int i = 0; i < RT_PLUGIN_COUNT; i++) {
        why[i][0] = 0;
        const RtLoaded *m = loaded(plugin_tab[i].module);
        if (!rt_patch_enabled(plugin_tab[i].set)) continue;
        if (!m) {   // loader.c left it out (its file is gone or changed): its set cannot run
            mods_plugin_off(plugin_tab[i].set, "its file is not the one the build lifted");
            continue;
        }
        if (!m->init) {
            mods_plugin_off(plugin_tab[i].set, "it has no DT_INIT");
            continue;
        }
        run[i] = 1, nrun++;
    }
    if (!nrun) {   // every plugin's mod off or gone: the game sees its address space as without plugins
        mem_query_hook = NULL;
        rt_process_memory = NULL;
        return 0;
    }
    uint64_t t0 = plat_now_ms();
    rt_log("boot: ENTER %s DT_INIT first: a mod plugin's init runs before the game's constructors", sdk->name);
    rt_call(c, sdk->init, 0);
    pre = calloc((size_t)ldr_count(), sizeof *pre);
    if (!pre) rt_fatal("plugins: out of memory");
    for (int i = 0; i < ldr_count(); i++) {   // the game modules before the plugins run
        const RtLoaded *m = ldr_module(i);
        if (m->plugin) continue;
        pre[i] = malloc(m->size);
        if (!pre[i]) rt_fatal("plugins: out of memory for a copy of %s", m->name);
        memcpy(pre[i], (const void *)(uintptr_t)m->base, m->size);
    }
    for (int i = RT_PLUGIN_COUNT - 1; i >= 0; i--) {
        if (!run[i]) continue;
        const RtLoaded *m = loaded(plugin_tab[i].module);
        rt_log("plugins: ENTER %s DT_INIT at %#llx", m->name, (unsigned long long)m->init);
        failed[0] = 0;
        int first_alias = naliases;
        running = 1;
        rt_trace |= RT_TRACE_PLUGIN;
        rt_call(c, m->init, 0);
        rt_trace &= ~(unsigned)RT_TRACE_PLUGIN;
        running = 0;
        if (failed[0]) snprintf(why[i], sizeof why[i], "its init asked for what the port does not serve: %s", failed);
        check_writes(i, first_alias, why[i], sizeof why[i]);
        char hex[65];
        sha256_hex((const void *)(uintptr_t)m->base, m->text_end, hex);
        if (!why[i][0] && strcmp(hex, plugin_tab[i].sha256))
            snprintf(why[i], sizeof why[i], "its code after the init is not the one the build lifted");
    }
    for (int i = 0; i < ldr_count(); i++) free(pre[i]);
    free(pre);
    pre = NULL;
    mem_query_hook = NULL;    // the game's own QueryMemory sees its address space as without plugins
    rt_process_memory = NULL; // a callback that maps process memory stops with a named fatal (svc.c)
    int on = 0;
    for (int i = 0; i < RT_PLUGIN_COUNT; i++) {
        if (!run[i]) continue;
        if (why[i][0]) {
            rt_log("plugins: %s stays off: %s", plugin_tab[i].module, why[i]);
            mods_plugin_off(plugin_tab[i].set, why[i]);
            continue;
        }
        int hooks = 0;
        for (int k = 0; k < NWRITES; k++) hooks += write_tab[k].plugin == i;
        rt_log("plugins: %s: init ran, %d hooks on (set %s)", plugin_tab[i].module, hooks,
               rt_patch_name(plugin_tab[i].set));
        on++;
    }
    int live = 0;
    for (int i = 0; i < naliases; i++) live += aliases[i].live;
    rt_log("plugins: %d of %d plugins on in %llu ms (%d process-memory maps, %d kept)", on, nrun,
           (unsigned long long)(plat_now_ms() - t0), naliases, live);
    if (on && cfg_bool("TOTK_PLUGIN_TRACE")) trace_entries();
    return 1;
#else
    (void)c, (void)sdk;
    return 0;
#endif
}
