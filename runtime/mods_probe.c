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
#include "sha256.h"

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

// A second scan of a folder of its own: data writes of ExeFS patch files (build-cache/exefs.data, given as text) and
// mods with a code module under mods_without_code.
static void probe_data_and_code(const char *root) {
    remove_tree(root);
    plat_file_mkdir_p(root);
    const char *bid = *mods_main_build_id() ? mods_main_build_id() : "0";
    char n_ips[160], p_ips[160], q_ips[160];
    snprintf(n_ips, sizeof n_ips, "N/exefs/%s.ips", bid);
    snprintf(p_ips, sizeof p_ips, "P/exefs/%s.ips", bid);
    snprintf(q_ips, sizeof q_ips, "Q/exefs/%s.ips", bid);
    const char *files[] = {"E/exefs/subsdk9", "H/romfs/h.bin", "H/exefs/subsdk9", "I/romfs/i.bin", "I/exefs/main",
                           n_ips, p_ips, q_ips};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) put(root, files[i]);
    uint32_t kn = fnv(n_ips, strlen(n_ips)), kp = fnv(p_ips, strlen(p_ips)), kq = fnv(q_ips, strlen(q_ips));
    uint32_t c = fnv("probe", 5);
    char status[256], data[512];
    snprintf(status, sizeof status,
             "%08X\t%08X\tdata\t-\tdata 11\n%08X\t%08X\tdata\t-\tdata 1\n%08X\t%08X\tdata\t-\tdata 4\n", kn, c, kp, c,
             kq, c);
    snprintf(data, sizeof data,
             "# file hash, content hash, offset in main, bytes[, base]\n"
             "%08X\t%08X\t10\t414243\n"                   // N: three bytes of data
             "%08X\t%08X\t18\t0800000000000000\tbase\r\n"  // N: a DT_RELR word, 8 plus main's base (a CRLF line)
             "DEADBEEF\t%08X\t20\t99\n"                   // another file's: not N's
             "%08X\t%08X\t4\t77\n"                         // P: below main's data
             "%08X\t%08X\t30\t66\n",                       // Q: 1 of the 4 bytes its status counts
             kn, c, kn, c, c, kp, c, kq, c);
    mods_probe_cache(status, data);
    cfg_set("TOTK_MODS_DIR", root);
    cfg_set("TOTK_MODS_ORDER", "");
    cfg_set("TOTK_MODS_DISABLED", "");
    cfg_set("TOTK_MODS_WITHOUT_CODE", "1");
    mods_rescan();
    mods_scan();
    ModInfo *e = by_name("E"), *h = by_name("H"), *im = by_name("I"), *n = by_name("N"), *p = by_name("P"),
            *q = by_name("Q");
    CHECK(e && h && im && n && p && q, "second scan: six mods");
    if (e && h && im && n && p && q) {
        CHECK(e->problem == MOD_PLUGIN, "mods_without_code: a mod with only a code module stays off");
        CHECK(h->problem == MOD_OK && EQ(h->code, "subsdk9"), "mods_without_code: RomFS + subsdk9 is used without it");
        CHECK(im->problem == MOD_OK && EQ(im->code, "main"), "mods_without_code: RomFS + replaced main is used");
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
    }
    cfg_set("TOTK_MODS_WITHOUT_CODE", "0");
    mods_rescan();
    mods_scan();
    h = by_name("H");
    CHECK(h && h->problem == MOD_PLUGIN, "without mods_without_code a mod with a code module stays off");
    cfg_set("TOTK_MODS_WITHOUT_CODE", "");
    mods_probe_cache(NULL, NULL);
    remove_tree(root);
}

// root/rel with the n bytes of data, with every folder above it.
static void put_data(const char *root, const char *rel, const char *data, size_t n) {
    char p[2048];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    char *slash = strrchr(p, '/');
    *slash = 0;
    plat_file_mkdir_p(p);
    *slash = '/';
    plat_file_write(p, data, n);
}

// A third scan, of code modules: two plugins the build has (status kind plugin), one whose build installs no hooks on
// this game version, one the build calls unsupported, a known build the build lacks and an unknown module; the
// Switch loads one exefs/subsdk9, so only the top mod's plugin of that name runs.
static void probe_plugin_scan(const char *root) {
    remove_tree(root);
    plat_file_mkdir_p(root);
    static const char *const files[] = {"P1/romfs/p.bin", "P1/exefs/subsdk9", "P2/exefs/subsdk9", "P3/romfs/p.bin",
                                        "P3/exefs/subsdk9", "P5/romfs/p.bin", "P5/exefs/subsdk9", "P6/exefs/subsdk9"};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) put(root, files[i]);
    static const char known[] = "a build plugins.c knows";
    put_data(root, "P4/exefs/subsdk9", known, sizeof known - 1);
    uint8_t sha[32];
    char hex[65];
    sha256(known, sizeof known - 1, sha);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", sha[i]);
    plugins_probe_known(hex, "Probe build");
    uint32_t c = fnv("probe", 5), k1 = fnv("P1/exefs/subsdk9", 16), k2 = fnv("P2/exefs/subsdk9", 16),
             k3 = fnv("P3/exefs/subsdk9", 16), k6 = fnv("P6/exefs/subsdk9", 16);
    char status[512];
    snprintf(status, sizeof status,
             "%08X\t%08X\tplugin\t-\thooks 7 Probe MSFix\n%08X\t%08X\tplugin\t-\thooks 0 Idle build\n"
             "%08X\t%08X\tunsupported\t-\ta code module the port does not know\n"
             "%08X\t%08X\tplugin\tfps60\thooks 3 Probe MSFix\n",
             k1, c, k2, c, k3, c, k6, c);
    mods_probe_cache(status, "");
    cfg_set("TOTK_MODS_DIR", root);
    cfg_set("TOTK_MODS_ORDER", "");
    cfg_set("TOTK_MODS_DISABLED", "");
    cfg_set("TOTK_MODS_WITHOUT_CODE", "0");
    mods_rescan();
    mods_scan();
    ModInfo *p1 = by_name("P1"), *p2 = by_name("P2"), *p3 = by_name("P3"), *p4 = by_name("P4"), *p5 = by_name("P5"),
            *p6 = by_name("P6");
    CHECK(p1 && p2 && p3 && p4 && p5 && p6, "plugin scan: six mods");
    if (p1 && p2 && p3 && p4 && p5 && p6) {
        CHECK(p1->problem == MOD_OK && p1->nplugins == 1 && !p1->code[0] && EQ(p1->plugin[0].module, "subsdk9") &&
                  EQ(p1->plugin[0].name, "Probe MSFix") && p1->plugin[0].hooks == 7 && p1->plugin[0].set == k1,
              "a plugin the build has: the mod is used, its hooks are the set of its file");
        CHECK(p2->problem == MOD_OK && p2->nplugins == 1 && !p2->plugin[0].hooks,
              "a plugin whose build installs no hooks on this game version: the mod is used");
        CHECK(p3->problem == MOD_PLUGIN && EQ(p3->code, "subsdk9"), "a code module the build calls unsupported");
        uint32_t key = 0;
        CHECK(p4->problem == MOD_EXEFS_LATER && p4->pending == 1 && mods_exefs_pending(&key) == 1,
              "a known build the build lacks waits for the rebuild");
        CHECK(p5->problem == MOD_PLUGIN && EQ(p5->code, "subsdk9"), "an unknown code module without a status line");
        CHECK(p6->problem == MOD_OK && EQ(p6->overrides, "fps60"), "a plugin's hooks over the port's own set");
        mods_exefs_switch();
        CHECK(!p1->plugin[0].shadowed[0] && EQ(p2->plugin[0].shadowed, "P1") && EQ(p6->plugin[0].shadowed, "P1"),
              "one exefs/subsdk9 runs: the top mod's");
    }
    plugins_probe_known(NULL, NULL);
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
    probe_data_and_code(root);
    probe_plugin_scan(root);
    printf("mods: %s\n", fails ? "FAIL" : "ok");
    int table = restbl_probe();
    return fails || table ? 1 : 0;
}
