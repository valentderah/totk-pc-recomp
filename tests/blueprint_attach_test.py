"""Check runtime/blueprint_attach.c against the game's own AttachmentParam data.

The module reads a material's actor pack, so the test needs that one game file: it extracts it out of the packed RomFS into
build/ (never into the repository) and stubs the game's RomFS calls to serve it from disk. Everything else - the pack
reader, the SARC walk, the BYML reader and the attachment chain - is the real code.

Usage: python tests\\blueprint_attach_test.py [--build-only]
"""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "build" / "attach_test"
ROMFS = ROOT / "build" / "extracted" / "v143" / "romfs"
CLANG = Path("C:/msys64/ucrt64/bin/clang.exe")

WANT = "Pack/Actor/Item_Enemy_151.pack.zs"   # the Lynel horn: an attachment record with a parent chain
# The actor packs are compressed with the game's own zstd dictionary, so the reader needs this too.
DICT = "Pack/ZsDic.pack.zs"
# The horn: a material whose records carry a rotation and an ExScale. The statue: a material whose records for a shield
# carry neither, so it must come out at its own size and in the weapon's own orientation.
NEEDED = (WANT, DICT, "Pack/Actor/Obj_GerudoHoleCover_A_03.pack.zs",
          "Pack/Actor/Weapon_Shield_103.pack.zs", "Pack/Actor/Weapon_Sword_001.pack.zs")


def romfs_entry(entries, want):
    for e in entries:
        if isinstance(e, dict):
            p, off, size = e.get("path") or e.get("name"), e.get("offset", 0), e.get("size", 0)
        elif isinstance(e, (list, tuple)) and len(e) >= 3:
            p, off, size = e[0], e[-2], e[-1]
        else:
            continue
        if p == want:
            return off, size
    return None


STUBS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "fs.h"

// Serve RomFS reads from the extracted tree, so the real bp_romfs_file and SARC walk run unchanged.
static const char *g_root = ".";
void harness_set_root(const char *root) { g_root = root; }

typedef struct { FILE *f; int64_t size; } StubFile;
#define MAX_OPEN 8
static StubFile g_open[MAX_OPEN];

uint32_t romfs_open(uint64_t *out, const char *path, int mode) {
    (void)mode;
    // The caller asks for "<mount>:/Pack/...", so drop the mount prefix to find the extracted file.
    const char *rel = path;
    const char *sep = strstr(path, ":/");
    if (sep) rel = sep + 2;
    char full[512];
    snprintf(full, sizeof full, "%s/%s", g_root, rel);
    FILE *f = fopen(full, "rb");
    if (!f) return 1;
    for (int i = 0; i < MAX_OPEN; i++) {
        if (g_open[i].f) continue;
        fseek(f, 0, SEEK_END);
        g_open[i].f = f;
        g_open[i].size = ftell(f);
        rewind(f);
        *out = (uint64_t)(i + 1);
        return 0;
    }
    fclose(f);
    return 2;
}

uint32_t romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    StubFile *s = &g_open[h - 1];
    if (!s->f) return 3;
    if (fseek(s->f, (long)offset, SEEK_SET)) return 4;
    const size_t got = fread(buffer, 1, (size_t)size, s->f);
    if (out) *out = (uint64_t)got;   // the caller compares this against what it asked for
    return got == (size_t)size ? 0 : 5;
}

uint32_t romfs_size(int64_t *out, uint64_t h) {
    StubFile *s = &g_open[h - 1];
    if (!s->f) return 3;
    *out = s->size;
    return 0;
}

void romfs_close(uint64_t h) {
    StubFile *s = &g_open[h - 1];
    if (s->f) fclose(s->f);
    s->f = NULL;
}

void rt_log(const char *fmt, ...) { (void)fmt; }
"""


def main():
    if not (ROMFS / "index.json").is_file():
        print("blueprint attach: no extracted RomFS at %s, skipping" % ROMFS)
        return 0
    idx = json.loads((ROMFS / "index.json").read_text(encoding="utf-8"))
    entries = idx if isinstance(idx, list) else (idx.get("files") or idx.get("entries") or [])
    for want in NEEDED:
        hit = romfs_entry(entries, want)
        if not hit:
            print("blueprint attach: %s is not in this RomFS, skipping" % want)
            return 0
        off, size = hit
        dst = WORK / want
        dst.parent.mkdir(parents=True, exist_ok=True)
        with (ROMFS / "romfs.bin").open("rb") as f:
            f.seek(off)
            dst.write_bytes(f.read(size))

    stub_src = WORK / "romfs_stubs.c"
    stub_src.write_text(STUBS, encoding="utf-8")

    exe = WORK / "blueprint_attach_test.exe"
    units = [
        ROOT / "tests" / "blueprint_attach_main.c",
        ROOT / "runtime" / "blueprint_attach.c",
        ROOT / "runtime" / "blueprint_shape.c",
        ROOT / "runtime" / "byml.c",
        ROOT / "runtime" / "hull.c",
    ]
    build = [
        str(CLANG), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "runtime"), "-I", str(ROOT / "generated" / "v143"),
        "-I", r"C:\msys64\ucrt64\include",
        *map(str, units), str(stub_src),
        "-L", r"C:\msys64\ucrt64\lib", "-lzstd", "-o", str(exe),
    ]
    r = subprocess.run(build, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr[-3000:])
        print("blueprint attach: build failed")
        return 1
    if "--build-only" in sys.argv:
        print("blueprint attach: built")
        return 0

    run = subprocess.run([str(exe), str(WORK)], capture_output=True, text=True)
    print(run.stdout.strip())
    if run.stderr.strip():
        print(run.stderr.strip()[-1500:])
    return run.returncode


if __name__ == "__main__":
    sys.exit(main())