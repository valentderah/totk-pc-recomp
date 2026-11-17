// --probe-mods: the mods folder scan, its order and switches (mods.c). Builds a mods folder of its own next to totk.exe,
// scans it and removes it. Also the resource size table's entries of the members of the mods' packs (restbl_probe.c).
// Needs no game files.
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "mods.h"
#include "platform/platform.h"
#include "restbl.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { fails++; fprintf(stderr, "FAIL mods: %s\n", what); } } while (0)
#define EQ(a, b) ((a) && !strcmp((a), (b)))

// root/rel as a file of a few bytes (rel ends in '/': a folder), with every folder above it.
static void put(const char *root, const char *rel) {
    char p[2048];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    char *slash = strrchr(p, '/');
    *slash = 0;
    plat_file_mkdir_p(p);
    *slash = '/';
    if (slash[1]) plat_file_write(p, "probe", 5);
}

static int remove_cb(const char *name, int kind, uint64_t size, void *ctx);
static void remove_tree(const char *dir) {
    plat_fs_list(dir, remove_cb, (void *)dir);
    plat_fs_rmdir(dir);
}
static int remove_cb(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    char sub[2048];
    snprintf(sub, sizeof sub, "%s/%s", (const char *)ctx, name);
    if (kind == PLAT_FS_DIR) remove_tree(sub);
    else plat_fs_unlink(sub);
    return 1;
}

static uint32_t fnv(const char *p, size_t n) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)p[i]) * 0x01000193u;
    return h;
}

static ModInfo *by_name(const char *name) {
    for (int i = 0; mods_at(i); i++)
        if (!strcmp(mods_at(i)->name, name)) return mods_at(i);
    return NULL;
}

// mods_exefs_data's store into a buffer standing in for main
static int store_buffer(uint64_t a, const uint8_t *p, int n) {
    memcpy((void *)(uintptr_t)a, p, (size_t)n);
    return 1;
}

// A second scan of a folder of its own: data writes of ExeFS patch files (build-cache/exefs.data, given as text).
static void probe_data(const char *root) {
    remove_tree(root);
    plat_file_mkdir_p(root);
    const char *bid = *mods_main_build_id() ? mods_main_build_id() : "0";
    char n_ips[160], p_ips[160], q_ips[160], x_ips[160], y_ips[160];
    snprintf(n_ips, sizeof n_ips, "N/exefs/%s.ips", bid);
    snprintf(p_ips, sizeof p_ips, "P/exefs/%s.ips", bid);
    snprintf(q_ips, sizeof q_ips, "Q/exefs/%s.ips", bid);
    snprintf(x_ips, sizeof x_ips, "X/exefs/%s.ips", bid);
    snprintf(y_ips, sizeof y_ips, "Y/exefs/%s.ips", bid);
    const char *files[] = {n_ips, p_ips, q_ips, x_ips, y_ips};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) put(root, files[i]);
    uint32_t kn = fnv(n_ips, strlen(n_ips)), kp = fnv(p_ips, strlen(p_ips)), kq = fnv(q_ips, strlen(q_ips));
    uint32_t kx = fnv(x_ips, strlen(x_ips)), ky = fnv(y_ips, strlen(y_ips));
    uint32_t c = fnv("probe", 5);
    char status[512], data[768];
    snprintf(status, sizeof status,
             "%08X\t%08X\tdata\t-\tdata 11\n%08X\t%08X\tdata\t-\tdata 1\n%08X\t%08X\tdata\t-\tdata 4\n"
             "%08X\t%08X\tdata\t-\tdata 1\n%08X\t%08X\tdata\t-\tdata 1\n",
             kn, c, kp, c, kq, c, kx, c, ky, c);
    snprintf(data, sizeof data,
             "# file hash, content hash, offset in main, bytes[, base]\n"
             "%08X\t%08X\t10\t414243\n"                   // N: three bytes of data
             "%08X\t%08X\t18\t0800000000000000\tbase\r\n"  // N: a DT_RELR word, 8 plus main's base (a CRLF line)
             "DEADBEEF\t%08X\t20\t99\n"                   // another file's: not N's
             "%08X\t%08X\t4\t77\n"                         // P: below main's data
             "%08X\t%08X\t30\t66\n"                        // Q: 1 of the 4 bytes its status counts
             "%08X\t%08X\t28\tBB\n"                        // X: the byte Y writes too
             "%08X\t%08X\t28\tAA\n",                       // Y, above X in the order: its byte stays
             kn, c, kn, c, c, kp, c, kq, c, kx, c, ky, c);
    mods_probe_cache(status, data);
    cfg_set("TOTK_MODS_DIR", root);
    cfg_set("TOTK_MODS_ORDER", "Y");   // Y on top, the rest by name: Y N P Q X
    cfg_set("TOTK_MODS_DISABLED", "");
    mods_rescan();
    mods_scan();
    ModInfo *n = by_name("N"), *p = by_name("P"), *q = by_name("Q");
    CHECK(n && p && q && by_name("X") && by_name("Y"), "second scan: five mods");
    if (n && p && q) {
        CHECK(n->kind == MOD_EXEFS && n->problem == MOD_OK && n->ndata == 1 && !n->nsets,
              "an ExeFS patch file with data writes only makes a usable mod");
        uint8_t img[0x40];
        memset(img, 0, sizeof img);
        uint64_t base = (uint64_t)(uintptr_t)img, word;
        long stored = mods_exefs_data(n, base, sizeof img, 8, store_buffer);
        memcpy(&word, img + 0x18, 8);
        CHECK(stored == 11 && !memcmp(img + 0x10, "ABC", 3) && word == base + 8 && !img[0x20],
              "data writes: bytes as they are, a DT_RELR word rebased, other files' lines left");
        memset(img, 0x5A, sizeof img);
        int untouched = 1;
        CHECK(mods_exefs_data(p, base, sizeof img, 8, store_buffer) == -1, "a line below main's data keeps all out");
        CHECK(n->data_bytes == 11 && q->data_bytes == 4, "the status lines count the data bytes");
        CHECK(mods_exefs_data(q, base, sizeof img, 8, store_buffer) == -1,
              "a data file holding fewer bytes than the status counts keeps all out");
        for (size_t i = 0; i < sizeof img; i++) untouched &= img[i] == 0x5A;
        CHECK(untouched, "nothing stored when a line does not fit or bytes are missing");
        // every enabled mod as at start: X and Y write the same byte, and Y, the higher one, wins as its RomFS files
        // would; P and Q stay out as above
        memset(img, 0, sizeof img);
        CHECK(EQ(mods_at(0)->name, "Y") && EQ(mods_at(4)->name, "X"), "mods_order puts Y on top and X at the bottom");
        mods_exefs_apply(base, sizeof img, 8, store_buffer);
        CHECK(img[0x28] == 0xAA, "two mods write the same byte: the top mod's byte stays");
        CHECK(!memcmp(img + 0x10, "ABC", 3) && !img[0x30], "the other mods' data as each one stores it alone");
        const ModInfo *xm = by_name("X"), *ym = by_name("Y");
        CHECK(mods_conflict_count() == 1 && xm && ym && mods_conflict_at(0)->winner == ym->id &&
                  mods_conflict_at(0)->loser == xm->id && mods_conflict_at(0)->part == MOD_PART_DATA &&
                  mods_conflict_at(0)->count == 1,
              "the conflict table: Y overrides X at one byte of data");
    }
    mods_probe_cache(NULL, NULL);
    remove_tree(root);
}

// "mod_<key>": the name tools/exefs_patches.py gives the patch set of the file at rel (mod folder name / path in it).
static void set_name(char *out, size_t cap, const char *rel) { snprintf(out, cap, "mod_%08x", fnv(rel, strlen(rel))); }

// Code sites several mods patch (patches.c rt_patch_picks): a set table of the probe's own, with the sets of the ExeFS
// patch files of A (one file), B (two files) and C (one, switched off) and three contested sites:
//   0: A and B's first file    the higher mod's set runs; a conflict of B over A or A over B
//   1: B's two files           the later file in the mod runs; no conflict
//   2: C and A                 C is off, so A's runs; no conflict
static void probe_code(const char *root) {
    remove_tree(root);
    plat_file_mkdir_p(root);
    const char *bid = *mods_main_build_id() ? mods_main_build_id() : "0";
    char a[160], b1[160], b2[160], c[160];
    snprintf(a, sizeof a, "A/exefs/%s.ips", bid);
    snprintf(b1, sizeof b1, "B/atmosphere/exefs_patches/one/%s.ips", bid);
    snprintf(b2, sizeof b2, "B/atmosphere/exefs_patches/two/%s.ips", bid);
    snprintf(c, sizeof c, "C/exefs/%s.ips", bid);
    const char *files[] = {a, b1, b2, c};
    static char names[5][16];
    static const char *const table[] = {"fps60", names[1], names[2], names[3], names[4]};
    char status[512] = "";
    for (int i = 0; i < 4; i++) {
        put(root, files[i]);
        set_name(names[i + 1], sizeof names[i + 1], files[i]);
        size_t at = strlen(status);
        snprintf(status + at, sizeof status - at, "%08X\t%08X\tcode\t-\t\n", fnv(files[i], strlen(files[i])),
                 fnv("probe", 5));
    }
    static const int16_t s0[] = {1, 2}, s1[] = {2, 3}, s2[] = {4, 1};
    static const RtPatchPick picks[] = {{"main", 0x100, s0, 2}, {"main", 0x200, s1, 2}, {"main", 0x300, s2, 2}};
    mods_probe_cache(status, "");
    cfg_set("TOTK_MODS_DIR", root);
    cfg_set("TOTK_MODS_DISABLED", "C");
    for (int round = 0; round < 2; round++) {
        cfg_set("TOTK_MODS_ORDER", round ? "A|B" : "B|A");
        mods_rescan();
        mods_scan();
        rt_patch_probe_table(table, 5, picks, 3);
        mods_exefs_apply(0, 0, 0, NULL);
        const ModInfo *am = by_name("A"), *bm = by_name("B");
        CHECK(am && bm && am->nsets == 1 && bm->nsets == 2 && am->problem == MOD_OK, "code scan: A one set, B two");
        if (!am || !bm) break;
        const ModInfo *hi = round ? am : bm, *lo = round ? bm : am;
        CHECK(rt_patch_pick_winner(0) == (round ? 0 : 1), round ? "A above B: A's code runs at their site"
                                                                : "B above A: B's code runs at their site");
        CHECK(rt_patch_pick_winner(1) == 1, "two files of one mod on a site: the later file's code runs");
        CHECK(rt_patch_pick_winner(2) == 1 && !rt_patch_enabled(4), "a switched-off mod's set stays off");
        CHECK(mods_conflict_count() == 1 && mods_conflict_at(0)->winner == hi->id &&
                  mods_conflict_at(0)->loser == lo->id && mods_conflict_at(0)->part == MOD_PART_CODE &&
                  mods_conflict_at(0)->count == 1,
              "the conflict table: the higher mod overrides the lower one at one code site, and only there");
        // the winner switched off at run time: the site takes the other mod's set
        rt_patch_switch(round ? 1 : 2, 0);
        CHECK(rt_patch_pick_winner(0) == (round ? 1 : 0), "a contested site follows a switch of its winner");
    }
    // the table itself: one entry per pair and part, counts adding up
    mods_conflict(7, 8, MOD_PART_FILES, 2);
    mods_conflict(7, 8, MOD_PART_FILES, 3);
    const ModConflict *last = mods_conflict_at(mods_conflict_count() - 1);
    CHECK(last && last->winner == 7 && last->count == 5, "mods_conflict adds to the pair's entry");
    rt_patch_probe_table(NULL, 0, NULL, 0);
    cfg_set("TOTK_MODS_DISABLED", "");
    mods_probe_cache(NULL, NULL);
    remove_tree(root);
}

// build-cache/plugins.status lines (tools/exefs_plugins.py) that refuse the probe's subsdk9 files of E and H: they stay
// code the port cannot run.
static const char *refused_plugins(void) {
    static char text[256];
    snprintf(text, sizeof text, "%08X\t%08X\tunsupported\t-\tthe init called svc 0x40\n"
             "%08X\t%08X\tunsupported\t-\tthe init called svc 0x40\n", fnv("E/exefs/subsdk9", 15), fnv("probe", 5),
             fnv("H/exefs/subsdk9", 15), fnv("probe", 5));
    return text;
}

// Mod plugins (build-cache/plugins.status, plugins.stale; mods.c plugin_file, mods_plugin_off) in a folder of their own:
//   P  RomFS + a built plugin (7 hooks)       its set on, ranked by the order; a contested site with Q's patch file
//   Q  an ExeFS patch file on P's site
//   R  a plugin the build has no line for     waits for the rebuild
//   S  RomFS + a refused plugin               off, the reason kept; used without its code, the rest runs
// Then P's start differs from the build (mods_plugin_off): its set goes off, Q's code runs at the site, the conflict
// goes, a stale line marks P's file, and the next scan has P wait for the rebuild.
static void probe_plugins(const char *root) {
    remove_tree(root);
    plat_file_mkdir_p(root);
    const char *bid = *mods_main_build_id() ? mods_main_build_id() : "0";
    char q[160];
    snprintf(q, sizeof q, "Q/exefs/%s.ips", bid);
    static const char *const files[] = {"P/romfs/p.bin", "P/exefs/subsdk9", "R/exefs/subsdk9", "S/romfs/s.bin",
                                        "S/exefs/subsdk9"};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) put(root, files[i]);
    put(root, q);
    static char names[3][16];
    static const char *const table[] = {"fps60", names[1], names[2]};
    set_name(names[1], sizeof names[1], "P/exefs/subsdk9");
    set_name(names[2], sizeof names[2], q);
    char status[256], plugins[256];
    snprintf(status, sizeof status, "%08X\t%08X\tcode\t-\t\n", fnv(q, strlen(q)), fnv("probe", 5));
    snprintf(plugins, sizeof plugins, "%08X\t%08X\tplugin\tfps60\tplugin0, 7 hooks\n"
             "%08X\t%08X\tunsupported\t-\tthe code its hooks reach has svc 0x40\n", fnv("P/exefs/subsdk9", 15),
             fnv("probe", 5), fnv("S/exefs/subsdk9", 15), fnv("probe", 5));
    static const int16_t s0[] = {1, 2};
    static const RtPatchPick picks[] = {{"main", 0x100, s0, 2}};
    mods_probe_cache(status, "");
    mods_probe_plugins(plugins, "");
    cfg_set("TOTK_MODS_DIR", root);
    cfg_set("TOTK_MODS_DISABLED", "");
    cfg_set("TOTK_MODS_NO_CODE", "");
    for (int round = 0; round < 2; round++) {
        cfg_set("TOTK_MODS_ORDER", round ? "Q|P" : "P|Q");
        mods_rescan();
        mods_scan();
        rt_patch_probe_table(table, 3, picks, 1);
        rt_patch_switch(0, 1);
        mods_exefs_apply(0, 0, 0, NULL);
        const ModInfo *pm = by_name("P"), *qm = by_name("Q"), *rm = by_name("R"), *sm = by_name("S");
        CHECK(pm && qm && rm && sm, "plugin scan: four mods");
        if (!pm || !qm || !rm || !sm) break;
        CHECK(pm->problem == MOD_OK && pm->nsets == 1 && pm->nplugins == 1 && pm->plugin_hooks == 7 && !pm->code[0] &&
                  EQ(pm->overrides, "fps60"), "a built plugin: its set, its hooks, the built-in set it replaces");
        CHECK(rt_patch_enabled(1) && !rt_patch_enabled(0), "the plugin's set on, the built-in set it replaces off");
        CHECK(rt_patch_pick_winner(0) == (round ? 1 : 0), round ? "Q above P: Q's patch runs at their site"
                                                                : "P above Q: P's plugin hook runs at their site");
        CHECK(mods_conflict_count() == 1 && mods_conflict_at(0)->part == MOD_PART_CODE &&
                  mods_conflict_at(0)->winner == (round ? qm : pm)->id,
              "the conflict table: a plugin site against a patch file goes by the order");
        CHECK(rm->problem == MOD_EXEFS_LATER && rm->pending == 1, "a plugin the build lacks waits for the rebuild");
        CHECK(sm->problem == MOD_PLUGIN && EQ(sm->code, "subsdk9") && strstr(sm->plugin_note, "svc 0x40"),
              "a refused plugin keeps its mod off and says why");
    }
    // P above Q, then P's start differs from the build
    cfg_set("TOTK_MODS_ORDER", "P|Q");
    mods_rescan();
    mods_scan();
    rt_patch_probe_table(table, 3, picks, 1);
    mods_exefs_apply(0, 0, 0, NULL);
    mods_plugin_off(1, "probe: 1 of the words the build recorded were not written");
    const ModInfo *pm = by_name("P");
    CHECK(!rt_patch_enabled(1) && rt_patch_pick_winner(0) == 1, "a plugin off at start: the other mod's patch runs");
    CHECK(pm && strstr(pm->plugin_off, "were not written"), "its row says why");
    CHECK(mods_conflict_count() == 0, "its conflict goes with it");
    cfg_set("TOTK_MODS_NO_CODE", "S");
    mods_rescan();
    mods_scan();
    pm = by_name("P");
    const ModInfo *sm = by_name("S");
    CHECK(pm && pm->problem == MOD_EXEFS_LATER && pm->pending == 1, "the next start: its file waits for the rebuild");
    CHECK(sm && sm->problem == MOD_OK && sm->plugin_note[0], "a refused plugin used without its code: the rest runs");
    rt_patch_probe_table(NULL, 0, NULL, 0);
    cfg_set("TOTK_MODS_NO_CODE", "");
    cfg_set("TOTK_MODS_ORDER", "");
    mods_probe_cache(NULL, NULL);
    mods_probe_plugins(refused_plugins(), "");
    remove_tree(root);
}

// Mods with code the port cannot run (a code module, an ExeFS patch file it cannot apply): used without that code
// by their own switch (mods_no_code) or by mods_without_code for every mod.
static void probe_without_code(const char *root) {
    remove_tree(root);
    plat_file_mkdir_p(root);
    const char *bid = *mods_main_build_id() ? mods_main_build_id() : "0";
    char u[160], v[160];
    snprintf(u, sizeof u, "U/exefs/%s.ips", bid);   // with RomFS files: the rest is usable
    snprintf(v, sizeof v, "V/exefs/%s.ips", bid);   // nothing else
    static const char *const files[] = {"E/exefs/subsdk9", "H/romfs/h.bin", "H/exefs/subsdk9", "I/romfs/i.bin",
                                        "I/exefs/main", "U/romfs/u.bin"};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) put(root, files[i]);
    put(root, u);
    put(root, v);
    char status[256];
    snprintf(status, sizeof status, "%08X\t%08X\tunsupported\t-\tit writes outside main\n"
             "%08X\t%08X\tunsupported\t-\tit writes outside main\n", fnv(u, strlen(u)), fnv("probe", 5),
             fnv(v, strlen(v)), fnv("probe", 5));
    mods_probe_cache(status, "");
    cfg_set("TOTK_MODS_NO_CODE", "H|U|V");
    cfg_set("TOTK_MODS_DIR", root);
    cfg_set("TOTK_MODS_ORDER", "");
    cfg_set("TOTK_MODS_DISABLED", "");
    mods_rescan();
    mods_scan();
    {
        ModInfo *h = by_name("H"), *im = by_name("I"), *um = by_name("U"), *vm = by_name("V");
        CHECK(h && im && um && vm, "per-mod scan: four mods");
        if (h && im && um && vm) {
            CHECK(h->problem == MOD_OK && h->no_code && EQ(h->code, "subsdk9"),
                  "its own switch: RomFS + subsdk9 is used without the code module");
            CHECK(im->problem == MOD_PLUGIN && !im->no_code, "a mod without the switch stays off");
            CHECK(um->problem == MOD_OK && um->unsupported == 1 && um->note[0],
                  "its own switch: RomFS + an unsupported patch file is used without that file");
            CHECK(vm->problem == MOD_EXEFS_UNSUPPORTED, "an unsupported patch file and nothing else stays off");
            int ii = 0;
            while (mods_at(ii) != im) ii++;
            mods_set_no_code(ii, 1);
            CHECK(EQ(cfg_get("TOTK_MODS_NO_CODE"), "H|I|U|V") && mods_changed(), "the switch writes mods_no_code");
        }
    }
    cfg_set("TOTK_MODS_NO_CODE", "");
    cfg_set("TOTK_MODS_WITHOUT_CODE", "0");
    mods_rescan();
    mods_scan();
    CHECK(by_name("U") && by_name("U")->problem == MOD_EXEFS_UNSUPPORTED, "without the switch an unsupported file "
                                                                           "keeps the whole mod off");
    remove_tree(root);
    plat_file_mkdir_p(root);
    for (size_t i = 0; i < 5; i++) put(root, files[i]);
    mods_probe_cache("", "");
    cfg_set("TOTK_MODS_DIR", root);
    cfg_set("TOTK_MODS_ORDER", "");
    cfg_set("TOTK_MODS_DISABLED", "");
    cfg_set("TOTK_MODS_WITHOUT_CODE", "1");
    mods_rescan();
    mods_scan();
    ModInfo *e = by_name("E"), *h = by_name("H"), *im = by_name("I");
    CHECK(e && h && im, "without-code scan: three mods");
    if (e && h && im) {
        CHECK(e->problem == MOD_PLUGIN, "mods_without_code: a mod with only a code module stays off");
        CHECK(h->problem == MOD_OK && EQ(h->code, "subsdk9"), "mods_without_code: RomFS + subsdk9 is used without it");
        CHECK(im->problem == MOD_OK && EQ(im->code, "main"), "mods_without_code: RomFS + replaced main is used");
    }
    cfg_set("TOTK_MODS_WITHOUT_CODE", "0");
    mods_rescan();
    mods_scan();
    h = by_name("H");
    CHECK(h && h->problem == MOD_PLUGIN && !h->without_code, "without mods_without_code a mod with a code module stays "
                                                              "off");
    cfg_set("TOTK_MODS_WITHOUT_CODE", "");
    mods_probe_cache(NULL, NULL);
    remove_tree(root);
}

// s ends in tail, either path separator matching either.
static int ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), t = strlen(tail);
    if (n < t) return 0;
    for (size_t i = 0; i < t; i++) {
        char a = s[n - t + i], b = tail[i];
        if (a == '\\') a = '/';
        if (b == '\\') b = '/';
        if (a != b) return 0;
    }
    return 1;
}

int probe_mods(void) {
    char root[1024];
    if (!plat_exe_path(root, sizeof root)) return 1;
    char *cut = strrchr(root, '/'), *back = strrchr(root, '\\');
    if (back > cut) cut = back;
    if (!cut) return 1;
    strcpy(cut + 1, "mods-probe");
    mods_probe_plugins(refused_plugins(), "");   // not the build's own plugins.status
    remove_tree(root);
    plat_file_mkdir_p(root);
    static const char *const files[] = {
        "A/romfs/x.bin", "B/atmosphere/contents/0100F2C0115B6000/romfs/x.bin",
        "C/contents/0100F2C0115B6000/romfs/y.bin", "D/exefs/main.pchtxt", "E/exefs/subsdk9", "F/romfs/",
        "G/romfs/z.bin", "G/exefs/1234.ips", "Ultra \xD0\x96/romfs/u.bin",
        "H/romfs/h.bin", "H/exefs/subsdk9", "I/romfs/i.bin", "I/exefs/main", "J/cheats/0123456789ABCDEF.txt",
        "K/romfslite/k.bin", "L/romfs/l.bin", "L/cheats/0123456789ABCDEF.txt", "L/exefs/main.npdm",
    };
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) put(root, files[i]);
    // an IPS file for this game's main (any file counts as one when main cannot be read), in Atmosphere's layout
    char ips[160];
    snprintf(ips, sizeof ips, "M/atmosphere/exefs_patches/x/%s.ips", *mods_main_build_id() ? mods_main_build_id() : "0");
    put(root, ips);

    cfg_set("TOTK_MODS_DIR", root);
    cfg_set("TOTK_MODS_ORDER", "B|A|Gone");
    cfg_set("TOTK_MODS_DISABLED", "C");
    mods_scan();

    static const char *const order[] = {"B", "A", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                        "Ultra \xD0\x96"};
    enum { N = sizeof order / sizeof *order };
    CHECK(mods_count() == N, "fourteen mod folders");
    int in_order = mods_count() == N;
    for (int i = 0; in_order && i < N; i++) in_order = EQ(mods_at(i)->name, order[i]);
    CHECK(in_order, "order: mods_order first, the rest by name");
    if (mods_count() == N && in_order) {
        ModInfo *a = mods_at(1), *b = mods_at(0), *c = mods_at(2), *d = mods_at(3), *e = mods_at(4), *f = mods_at(5),
                *g = mods_at(6), *hm = mods_at(7), *im = mods_at(8), *jm = mods_at(9), *km = mods_at(10),
                *lm = mods_at(11), *mm = mods_at(12), *u = mods_at(13);
        CHECK(hm->kind == (MOD_ROMFS | MOD_SUBSDK) && hm->problem == MOD_PLUGIN, "RomFS + subsdk plugin: unusable");
        CHECK(im->kind == (MOD_ROMFS | MOD_SUBSDK) && im->problem == MOD_PLUGIN, "RomFS + replaced exefs/main");
        CHECK(ends_with(b->romfs, "B/atmosphere/contents/0100F2C0115B6000/romfs"), "Atmosphere RomFS root");
        CHECK(ends_with(c->romfs, "C/contents/0100F2C0115B6000/romfs"), "contents RomFS root");
        CHECK(ends_with(a->romfs, "A/romfs") && a->kind == MOD_ROMFS && a->problem == MOD_OK, "plain RomFS mod");
        CHECK(!c->enabled && a->enabled && b->enabled && g->enabled && u->enabled, "mods_disabled switches C off");
        CHECK(d->kind == MOD_EXEFS && d->problem == MOD_EXEFS_OTHER && d->other_version == 1,
              "ExeFS patch mod without a build id: another version's");
        CHECK(e->problem == MOD_PLUGIN, "subsdk-only mod is a plugin mod");
        CHECK(f->problem == MOD_EMPTY, "empty RomFS folder");
        CHECK(g->kind == (MOD_ROMFS | MOD_EXEFS) && g->problem == MOD_OK && g->other_version == 1,
              "RomFS + another version's ExeFS patch: the RomFS is used");
        CHECK(mm->kind == MOD_EXEFS && mm->problem == MOD_EXEFS_LATER && mm->pending == 1,
              "an ExeFS patch for main the build lacks waits for the rebuild");
        CHECK(jm->kind == MOD_CHEATS && jm->problem == MOD_OK && ends_with(jm->cheats, "J/cheats"),
              "cheats-only mod");
        CHECK(km->kind == MOD_ROMFS && km->problem == MOD_OK && ends_with(km->romfs, "K/romfslite"), "TKMM romfslite");
        CHECK(lm->kind == (MOD_ROMFS | MOD_CHEATS) && lm->problem == MOD_OK, "RomFS + cheats, main.npdm is no code");
        CHECK(u->kind == MOD_ROMFS && ends_with(u->romfs, "Ultra \xD0\x96/romfs"), "Unicode folder name");
        CHECK(!mods_changed(), "nothing changed after the scan");

        mods_move(1, -1);
        CHECK(EQ(mods_at(0)->name, "A") && EQ(mods_at(1)->name, "B"), "move up");
        CHECK(EQ(cfg_get("TOTK_MODS_ORDER"), "A|B|C|D|E|F|G|H|I|J|K|L|M|Ultra \xD0\x96"),
              "order written without the gone name");
        mods_move(0, -1);
        CHECK(EQ(mods_at(0)->name, "A"), "move up at the top does nothing");
        mods_move(N - 1, 1);
        CHECK(EQ(mods_at(N - 1)->name, "Ultra \xD0\x96"), "move down at the bottom does nothing");
        mods_set_enabled(2, 1);
        const char *dis = cfg_get("TOTK_MODS_DISABLED");
        CHECK(mods_at(2)->enabled && (!dis || !*dis), "switching C on empties mods_disabled");
        mods_set_enabled(0, 0);
        CHECK(EQ(cfg_get("TOTK_MODS_DISABLED"), "A"), "switching A off writes it");
        CHECK(mods_changed(), "changes seen");
    }
    probe_data(root);
    probe_without_code(root);
    probe_code(root);
    probe_plugins(root);
    mods_probe_plugins(NULL, NULL);
    printf("mods: %s\n", fails ? "FAIL" : "ok");
    int table = restbl_probe();
    return fails || table ? 1 : 0;
}
