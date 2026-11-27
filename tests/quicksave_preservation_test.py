"""Fault-test the actual quicksave publication/recovery functions with synthetic data.

The platform tree/copy/rename seams use an in-memory two-file filesystem. A copy
fault occurs after its first file. No real profile, game files or filesystem fault
is needed. Run with --cc <clang> --out build/<new-directory>; --source selects a
baseline checkout. This checks process-interruption states, not power-loss durability.
"""
import argparse
from pathlib import Path
import subprocess
import sys


STUBS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { PLAT_FS_MISSING = -1, PLAT_FS_FILE, PLAT_FS_DIR, MOVE_TRIES = 2 };
static char save_root[] = "save";
static int tx_lock, copy_fault, publish_fault, rollback_fault;
// file_mask bits: 1 = first file, 2 = second file, 3 = both complete files.
typedef struct { char path[128]; unsigned file_mask, contents; } Node;
static Node nodes[12];
static Node *find(const char *path) {
    for (int i = 0; i < 12; i++) if (nodes[i].path[0] && !strcmp(nodes[i].path, path)) return &nodes[i];
    return NULL;
}
static Node *add(const char *path, unsigned file_mask, unsigned contents) {
    Node *n = find(path);
    if (!n) for (int i = 0; i < 12; i++) if (!nodes[i].path[0]) { n = &nodes[i]; break; }
    if (!n || strlen(path) >= sizeof n->path) abort();
    strcpy(n->path, path); n->file_mask = file_mask; n->contents = contents;
    return n;
}
static void need(int ok, const char *what) { if (!ok) { fprintf(stderr, "%s\n", what); abort(); } }
static void rt_log(const char *fmt, ...) { (void)fmt; }
static void plat_mutex_lock(int *lock) { (void)lock; }
static void plat_mutex_unlock(int *lock) { (void)lock; }
static void save_wait_commit(void) {}
static const char *newest_save(void) { return save_root; }
static char *join(const char *a, const char *b) {
    size_t n = strlen(a) + strlen(b) + 2;
    char *p = malloc(n); need(p != NULL, "join"); snprintf(p, n, "%s/%s", a, b); return p;
}
static int plat_fs_kind(const char *path) { return find(path) ? PLAT_FS_DIR : PLAT_FS_MISSING; }
static int exists(const char *path) { return find(path) != NULL; }
static int remove_tree(const char *path) { Node *n = find(path); if (n) memset(n, 0, sizeof *n); return 0; }
static int copy_tree(const char *src, const char *dst, const char *base, int durable,
                     int *nfiles, int *nlinked, uint64_t *bytes) {
    (void)base; (void)durable; (void)nlinked;
    Node *s = find(src); if (!s || find(dst)) return -1;
    Node *d = add(dst, 1, s->contents); (*nfiles)++; (*bytes)++;
    if (copy_fault) return -1; /* failure while writing the second synthetic file */
    d->file_mask = s->file_mask; (*nfiles)++; (*bytes)++; return 0;
}
static int move_dir(const char *from, const char *to, int tries) {
    (void)tries;
    if ((publish_fault && !strcmp(from, "quick/slot.new") && !strcmp(to, "quick/slot")) ||
        (rollback_fault && !strcmp(from, "quick/slot.old") && !strcmp(to, "quick/slot"))) return -1;
    Node *n = find(from); if (!n || find(to)) return -1;
    strcpy(n->path, to); return 0;
}
'''

CASES = r'''
static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) exit(1);
}
static int complete(const char *path, unsigned contents) {
    Node *n = find(path); return n && n->file_mask == 3 && n->contents == contents;
}
static void setup(void) {
    memset(nodes, 0, sizeof nodes); copy_fault = publish_fault = rollback_fault = 0;
    add("save/slot_00", 3, 2); add("quick/slot", 3, 1);
}
int main(void) {
    setup(); copy_fault = 1;
    check(fs_save_copy_slot("slot_00", "quick/slot") == -1, "second-file copy failure is reported");
    check(complete("quick/slot", 1), "copy failure preserves the previous two-file quicksave");
    setup(); publish_fault = 1;
    check(fs_save_copy_slot("slot_00", "quick/slot") == -1, "publication rename failure is reported");
    check(complete("quick/slot", 1), "publication failure restores the previous quicksave");
    setup();
    check(fs_save_copy_slot("slot_00", "quick/slot") == 0 && complete("quick/slot", 2), "successful copy publishes both new files");
    check(!exists("quick/slot.new") && !exists("quick/slot.old"), "successful publication cleans its siblings");
    if (HAS_RECOVERY) {
        setup(); publish_fault = rollback_fault = 1;
        check(fs_save_copy_slot("slot_00", "quick/slot") == -1 && complete("quick/slot.old", 1),
              "failed rollback retains the complete previous copy");
        check(fs_save_recover_slot("quick/slot") == -1 && !exists("quick/slot"), "blocked recovery refuses a missing canonical slot");
        publish_fault = rollback_fault = 0;
        check(fs_save_recover_slot("quick/slot") == 0 && complete("quick/slot", 1), "later recovery restores the preserved copy");
        setup(); move_dir("quick/slot", "quick/slot.old", 1); add("quick/slot.new", 1, 2);
        check(fs_save_recover_slot("quick/slot") == 0 && complete("quick/slot", 1) && exists("quick/slot.new"),
              "load recovery restores old without deleting partial staging");
        setup(); add("quick/slot.old", 3, 9); add("quick/slot.new", 1, 2);
        check(fs_save_recover_slot("quick/slot") == 0 && complete("quick/slot", 1) && exists("quick/slot.old") &&
                  exists("quick/slot.new"), "load recovery leaves cleanup to the worker");
        check(fs_save_copy_slot("slot_00", "quick/slot") == 0 && complete("quick/slot", 2) &&
                  !exists("quick/slot.old") && !exists("quick/slot.new"), "copy worker cleans stale siblings");
        setup(); remove_tree("quick/slot"); add("quick/slot.new", 1, 2);
        check(fs_save_recover_slot("quick/slot") == 0 && !exists("quick/slot") && exists("quick/slot.new"),
              "unfinished first copy never becomes a quicksave");
        remove_tree("quick/slot.new");
        check(fs_save_recover_slot("quick/slot") == 0, "ordinary start without a quicksave succeeds");
    }
    puts("Quicksave preservation checks passed"); return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root, out = args.source.resolve(), args.out.resolve()
    if not any(out.is_relative_to(root / name) for name in ('build', 'generated')):
        parser.error('--out must be below the source checkout build/ or generated/')
    if out.exists():
        parser.error('--out must name a new directory')
    source = (root / 'runtime/fs_host.c').read_text(encoding='utf-8')
    start = source.index('static int slot_name_ok(')
    end = source.index('static uint32_t commit_run(', start)
    functions = source[start:end]
    recovery = 'int fs_save_recover_slot(' in functions
    # The baseline fails the preservation assertion before reaching recovery-only cases.
    fallback = '' if recovery else 'static int fs_save_recover_slot(const char *dst) { (void)dst; return -1; }\n'
    out.mkdir(parents=True)
    driver = out / 'quicksave_preservation.c'
    driver.write_text(STUBS + functions + fallback + CASES.replace('HAS_RECOVERY', '1' if recovery else '0'), encoding='utf-8')
    executable = out / 'quicksave_preservation.exe'
    result = subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', str(driver), '-o', str(executable)])
    return result.returncode if result.returncode else subprocess.run([str(executable)]).returncode


if __name__ == '__main__':
    sys.exit(main())
