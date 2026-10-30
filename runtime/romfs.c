// Adapted from MacNativeGames games/totk/runtime/romfs.c @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames
// contributors).
// Read-only game RomFS backed by the player's extracted image.
//
// Input: <romfs>/romfs.bin (rt_config()->romfs), whose six IVFC levels and SHA-256 tools/extract.py verified, and
// romfs-index.bin next to totk.exe (TOTK_ROMFS_INDEX overrides), which tools/romfs_index.py wrote together with
// romfs_config.h. fs_init checks the index's SHA-256 and layout and the image size. The full 17 GB hash runs only with
// TOTK_ROMFS_VERIFY=full; a proof file (.romfs-verified-v1 next to the index, or in TOTK_ROMFS_PROOF_DIR) keyed by
// the file's volume, file id, size and times skips it on later starts, TOTK_ROMFS_VERIFY=force ignores the proof.
//
// Reads are positional (ReadFile with an OVERLAPPED offset on a FILE_FLAG_OVERLAPPED handle, one event per thread),
// so threads read the image in parallel without a file-pointer lock. TOTK_ROMFS_MMAP=1 maps the image instead.
//
// Overlays: files listed in a directory's manifest.tsv are served in place of the originals. Each line names a RomFS
// path, the overlay's size and SHA-256 and the original's SHA-256 ("-" adds a file the RomFS lacks; it is numbered
// after the RomFS's own files, file_count + k in path order). The directories, in this order: TOTK_ROMFS_PATCHES and
// TOTK_ROMFS_MODS; unset or "0" = none. A missing path, a size or hash mismatch or a changed original stops the start.
// An overlay file is opened when the game opens it; its identity (volume, file id, size, last write) is kept from the
// start check, and an open that finds another file there stops the game.
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "fs.h"
#include "host.h"
#include "romfs_config.h"
#include "romfs_path.h"

#define NH 65536                     // RomFS handle slots
#define READ_CHUNK (64u << 20)       // largest single ReadFile
#define HASH_CHUNK (8u << 20)
#define ADDED UINT64_MAX             // an added overlay file's index until overlay_init numbers it

typedef struct { uint64_t offset, size; uint32_t name, len; } Entry;
_Static_assert(sizeof(Entry) == 24, "index entry layout");

typedef struct { uint32_t volume, pad; uint64_t file_id, size, write_time, create_time; } Identity;   // no padding
typedef struct { uint64_t index, size; char *path, *name; Identity id; } Overlay;

static uint8_t *index_bytes;
static size_t index_size;
static uint64_t file_count, strings_offset;
static HANDLE image = INVALID_HANDLE_VALUE;
static const uint8_t *image_map;
static char image_file[2048];
static char index_dir[2048];         // the directory of romfs-index.bin: the proof file's default place
static char mount_name[32];
static SRWLOCK lock = SRWLOCK_INIT;
static uint64_t handles[NH];         // file index + 1; 0 is free
static uint32_t handle_ovl[NH];      // overlay slot + 1 of an open handle, 0 = original
static HANDLE handle_file[NH];       // the handle's open overlay file, when handle_ovl is set
static Overlay *overlays;            // sorted by RomFS index after overlay_init
static size_t noverlay, overlay_cap;
static char **added;                 // RomFS paths of the added files, sorted; added[k] is index file_count + k
static size_t nadded;

static void need(int c, const char *m) {
    if (!c) rt_fatal("RomFS: %s", m);
}

static Entry entry(uint64_t i) {
    Entry e;
    memcpy(&e, index_bytes + 24 + i * 24, 24);
    return e;
}
static const char *entry_name(Entry e) { return (const char *)index_bytes + strings_offset + e.name; }

static double now_s(void) {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart / (double)f.QuadPart;
}

// ---- positional reads
// Bytes read at off (0 at or past the end of the file), -1 on an I/O error. Works on synchronous and
// FILE_FLAG_OVERLAPPED handles; the event is per thread, so concurrent reads on one handle do not share it.
static int64_t read_at(HANDLE f, void *buf, uint32_t n, uint64_t off) {
    static _Thread_local HANDLE ev;
    if (!ev && !(ev = CreateEventW(NULL, TRUE, FALSE, NULL)))
        rt_fatal("RomFS: CreateEvent failed (%lu)", GetLastError());
    OVERLAPPED ov = {.Offset = (DWORD)off, .OffsetHigh = (DWORD)(off >> 32), .hEvent = ev};
    DWORD got = 0;
    mem_thaw((uintptr_t)buf, n);   // the kernel's write would fail on a frozen page instead of faulting
    if (!ReadFile(f, buf, n, &got, &ov)) {
        DWORD e = GetLastError();
        if (e == ERROR_HANDLE_EOF) return 0;
        if (e == ERROR_IO_PENDING && !GetOverlappedResult(f, &ov, &got, TRUE)) e = GetLastError();
        else if (e == ERROR_IO_PENDING) return got;
        if (e == ERROR_HANDLE_EOF) return 0;
        if (e == ERROR_NOACCESS)   // a read-only page in the buffer: a frozen region missed its thaw
            rt_log("RomFS: read of %u bytes into %p refused: the buffer is not writable", n, buf);
        return -1;
    }
    return got;
}

static HANDLE open_read(const char *path, int overlapped) {
    wchar_t *w = fs_wide(path);
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | (overlapped ? FILE_FLAG_OVERLAPPED : 0), NULL);
    free(w);
    return h;
}

static Identity identity_of(HANDLE f) {
    BY_HANDLE_FILE_INFORMATION i;
    need(GetFileInformationByHandle(f, &i), "file information");
    return (Identity){i.dwVolumeSerialNumber, 0, (uint64_t)i.nFileIndexHigh << 32 | i.nFileIndexLow,
                      (uint64_t)i.nFileSizeHigh << 32 | i.nFileSizeLow,
                      (uint64_t)i.ftLastWriteTime.dwHighDateTime << 32 | i.ftLastWriteTime.dwLowDateTime,
                      (uint64_t)i.ftCreationTime.dwHighDateTime << 32 | i.ftCreationTime.dwLowDateTime};
}

// ---- SHA-256 over a file range (BCrypt, streaming)
static void hex32(const uint8_t d[32], char out[65]) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static void sha_range(HANDLE f, uint64_t offset, uint64_t size, char hex[65], int progress) {
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE h;
    need(!BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) &&
         !BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0), "SHA-256 provider");
    uint8_t *buf = malloc(HASH_CHUNK);
    need(buf != NULL, "hash buffer");
    double t0 = now_s();
    for (uint64_t done = 0; done < size;) {
        uint32_t want = size - done > HASH_CHUNK ? HASH_CHUNK : (uint32_t)(size - done);
        int64_t n = read_at(f, buf, want, offset + done);
        need(n > 0, "read of hash input");
        need(!BCryptHashData(h, buf, (ULONG)n, 0), "SHA-256 update");
        done += (uint64_t)n;
        if (progress && (done % (2ull << 30) < (uint64_t)n || done == size))
            rt_log("RomFS: hashed %.1f of %.1f GiB (%.0f MiB/s)", done / 1073741824.0, size / 1073741824.0,
                   done / 1048576.0 / (now_s() - t0 + 1e-9));
    }
    uint8_t d[32];
    need(!BCryptFinishHash(h, d, 32, 0), "SHA-256 final");
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    free(buf);
    hex32(d, hex);
}

// ---- full image verification with a proof file
typedef struct { char magic[16], sha256[65]; uint8_t pad[7]; Identity id; } RomfsProof;

static void verify_image(const char *dir) {
    const char *mode = cfg_get("TOTK_ROMFS_VERIFY");
    if (!mode || (strcmp(mode, "full") && strcmp(mode, "force"))) return;
    const char *pdir = cfg_get("TOTK_ROMFS_PROOF_DIR");
    if (pdir && *pdir) dir = pdir;
    RomfsProof want = {0}, saved;
    memcpy(want.magic, "TOTKRFS-PROOF-1", 16);
    memcpy(want.sha256, ROMFS_IMAGE_SHA256, sizeof ROMFS_IMAGE_SHA256 < 65 ? sizeof ROMFS_IMAGE_SHA256 : 65);
    want.id = identity_of(image);
    char path[2100], tmp[2200];
    need(snprintf(path, sizeof path, "%s/.romfs-verified-v1", dir) < (int)sizeof path, "proof path");
    if (strcmp(mode, "force")) {
        HANDLE p = open_read(path, 0);
        DWORD got = 0;
        if (p != INVALID_HANDLE_VALUE) {
            int ok = ReadFile(p, &saved, sizeof saved, &got, NULL) && got == sizeof saved;
            CloseHandle(p);
            if (ok && !memcmp(&saved, &want, sizeof want)) {
                rt_log("RomFS unchanged since its full SHA-256 check (%s)", path);
                return;
            }
        }
    }
    rt_log("RomFS: verifying the SHA-256 of %s (%llu bytes)", image_file, (unsigned long long)want.id.size);
    char hex[65];
    sha_range(image, 0, want.id.size, hex, 1);
    if (strcmp(hex, ROMFS_IMAGE_SHA256))
        rt_fatal("RomFS image %s has SHA-256 %s, the extraction recorded %s (extract it again)", image_file, hex,
                 ROMFS_IMAGE_SHA256);
    Identity after = identity_of(image);
    need(!memcmp(&after, &want.id, sizeof after), "image changed during verification");
    snprintf(tmp, sizeof tmp, "%s.%lu.tmp", path, GetCurrentProcessId());
    wchar_t *wt = fs_wide(tmp), *wp = fs_wide(path);
    HANDLE out = CreateFileW(wt, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out != INVALID_HANDLE_VALUE) {
        DWORD put = 0;
        int ok = WriteFile(out, &want, sizeof want, &put, NULL) && put == sizeof want && FlushFileBuffers(out);
        CloseHandle(out);
        if (!ok || !MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) DeleteFileW(wt);
    }
    free(wt);
    free(wp);
    rt_log("RomFS: image SHA-256 ok");
}

// ---- index lookup
static uint64_t lower(const char *name) {
    uint64_t lo = 0, hi = file_count;
    while (lo < hi) {
        uint64_t m = lo + (hi - lo) / 2;
        if (strcmp(entry_name(entry(m)), name) < 0) lo = m + 1;
        else hi = m;
    }
    return lo;
}

static size_t added_lower(const char *p) {
    size_t lo = 0, hi = nadded;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (strcmp(added[m], p) < 0) lo = m + 1;
        else hi = m;
    }
    return lo;
}

// The index of RomFS path p (an added file: file_count + k), or UINT64_MAX.
static uint64_t file_index(const char *p) {
    uint64_t i = lower(p);
    if (i < file_count && !strcmp(p, entry_name(entry(i)))) return i;
    size_t k = added_lower(p);
    return k < nadded && !strcmp(added[k], p) ? file_count + k : UINT64_MAX;
}

// 1 when a RomFS file or an added one lies under folder (a path ending in '/').
static int folder_exists(const char *folder) {
    size_t n = strlen(folder);
    uint64_t i = lower(folder);
    if (i < file_count && !strncmp(entry_name(entry(i)), folder, n)) return 1;
    size_t k = added_lower(folder);
    return k < nadded && !strncmp(added[k], folder, n);
}

// The overlay slot + 1 that replaces RomFS file i, or 0.
static uint32_t overlay_slot(uint64_t i) {
    size_t lo = 0, hi = noverlay;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (overlays[m].index < i) lo = m + 1;
        else hi = m;
    }
    return lo < noverlay && overlays[lo].index == i ? (uint32_t)(lo + 1) : 0;
}

// ---- overlays
static void overlay_load(const char *dir) {
    char path[2100];
    need(snprintf(path, sizeof path, "%s/manifest.tsv", dir) < (int)sizeof path, "overlay path");
    wchar_t *w = fs_wide(path);
    FILE *m = _wfopen(w, L"rb");
    free(w);
    if (!m) {
        rt_log("RomFS overlay: none (%s)", path);
        return;
    }
    char line[8192];
    size_t first = noverlay;
    uint64_t hashed = 0;
    double t0 = now_s();
    while (fgets(line, sizeof line, m)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char name[4096], sha[65], orig[65], hex[65];
        unsigned long long size = 0;
        need(sscanf(line, "%4095[^\t]\t%llu\t%64[0-9a-f]\t%64[0-9a-f-]", name, &size, sha, orig) == 4 &&
             strlen(sha) == 64 && (strlen(orig) == 64 || !strcmp(orig, "-")), "overlay manifest line");
        need(romfs_path_valid_relative(name), "overlay path");
        uint64_t i = lower(name);
        int known = i < file_count && !strcmp(entry_name(entry(i)), name), add = !strcmp(orig, "-");
        if (!known && !add)
            rt_fatal("RomFS overlay: %s/manifest.tsv names %s, which the RomFS lacks; a new file has \"-\" as its "
                     "original's SHA-256", dir, name);
        if (known && add)
            rt_fatal("RomFS overlay: %s/manifest.tsv adds %s, which the RomFS already has; give its original's SHA-256",
                     dir, name);
        Entry e = {0};
        if (known) {
            e = entry(i);
            sha_range(image, e.offset, e.size, hex, 0);
            if (strcmp(hex, orig)) rt_fatal("RomFS overlay: the original of %s has SHA-256 %s, %s/manifest.tsv says %s",
                                            name, hex, dir, orig);
        } else {
            i = ADDED;
        }
        char file[8200];
        need(snprintf(file, sizeof file, "%s/%s", dir, name) < (int)sizeof file, "overlay file path");
        HANDLE f = open_read(file, 1);
        if (f == INVALID_HANDLE_VALUE) rt_fatal("RomFS overlay: %s missing", file);
        Identity id = identity_of(f);
        if (id.size != size) rt_fatal("RomFS overlay: %s has %llu bytes, its manifest says %llu", file,
                                      (unsigned long long)id.size, size);
        sha_range(f, 0, size, hex, 0);
        CloseHandle(f);
        if (strcmp(hex, sha)) rt_fatal("RomFS overlay: %s has SHA-256 %s, its manifest says %s", file, hex, sha);
        hashed += e.size + size;
        if (noverlay == overlay_cap) {
            overlay_cap = overlay_cap ? overlay_cap * 2 : 16;
            overlays = realloc(overlays, overlay_cap * sizeof *overlays);
            need(overlays != NULL, "overlay table");
        }
        char *copy = _strdup(file), *rel = _strdup(name);
        need(copy && rel, "overlay path copy");
        overlays[noverlay++] = (Overlay){i, size, copy, rel, id};
        if (noverlay - first <= 16)
            rt_log("RomFS overlay: %s (%llu bytes) %s", name, size, add ? "is added" : "replaces the original");
    }
    fclose(m);
    if (noverlay - first > 16) rt_log("RomFS overlay: %zu files from %s (%zu not listed)", noverlay - first, dir,
                                      noverlay - first - 16);
    if (noverlay > first) rt_log("RomFS overlay: %s checked, %zu files, %.1f MiB hashed in %.2f s", dir,
                                 noverlay - first, hashed / 1048576.0, now_s() - t0);
}

static int overlay_order(const void *a, const void *b) {
    uint64_t x = ((const Overlay *)a)->index, y = ((const Overlay *)b)->index;
    return (x > y) - (x < y);
}

static int added_order(const void *a, const void *b) {
    return strcmp((*(Overlay *const *)a)->name, (*(Overlay *const *)b)->name);
}

// Number the added files in path order after the RomFS's own (file_count + k); two overlays adding one path stop.
static void added_init(void) {
    Overlay **list = NULL;
    for (size_t k = 0; k < noverlay; k++)
        if (overlays[k].index == ADDED) {
            list = realloc(list, (nadded + 1) * sizeof *list);
            need(list != NULL, "added files");
            list[nadded++] = &overlays[k];
        }
    if (!nadded) return;
    qsort(list, nadded, sizeof *list, added_order);
    added = malloc(nadded * sizeof *added);
    need(added != NULL, "added files");
    for (size_t k = 0; k < nadded; k++) {
        if (k && !strcmp(list[k]->name, list[k - 1]->name))
            rt_fatal("RomFS overlay: %s and %s both add %s", list[k - 1]->path, list[k]->path, list[k]->name);
        list[k]->index = file_count + k;
        added[k] = list[k]->name;
    }
    free(list);
    rt_log("RomFS overlay: %zu files added to the RomFS", nadded);
}

// The HeapDef table a bigger game resolution needs (resolution.c): its texture heap and that heap's parent grow. The
// base is an overlay that replaces the table (a mod's heap sizes stay), else the RomFS original; the grown table is
// written to <profile>/resolution/ and served in the base's place.
#define HEAPDEF "System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs"
static void heapdef_overlay(void) {
    if (!res_heap_extra_kib()) return;
    uint64_t i = lower(HEAPDEF);
    if (i >= file_count || strcmp(entry_name(entry(i)), HEAPDEF)) {
        rt_log("RomFS: %s missing; the game keeps its own resolution", HEAPDEF);
        return;
    }
    Overlay *base = NULL;
    for (size_t k = 0; k < noverlay; k++)
        if (overlays[k].index == i) base = &overlays[k];
    HANDLE f = base ? open_read(base->path, 1) : image;
    uint64_t off = base ? 0 : entry(i).offset, n = base ? base->size : entry(i).size;
    void *in = n && n < (16u << 20) && f != INVALID_HANDLE_VALUE ? malloc((size_t)n) : NULL;
    int ok = in && read_at(f, in, (uint32_t)n, off) == (int64_t)n;
    if (base && f != INVALID_HANDLE_VALUE) CloseHandle(f);
    void *out = NULL;
    size_t out_n = 0;
    ok = ok && res_heapdef_build(in, (size_t)n, &out, &out_n);
    free(in);
    if (!ok) return;
    char dir[2100], file[2200];
    need(snprintf(dir, sizeof dir, "%s/resolution", rt_config()->profile) < (int)sizeof dir, "resolution path");
    need(snprintf(file, sizeof file, "%s/Product.Nin_NX_NVN.HeapDef.byml.zs", dir) < (int)sizeof file, "resolution path");
    wchar_t *wd = fs_wide(dir), *wf = fs_wide(file);
    CreateDirectoryW(wd, NULL);
    HANDLE w = CreateFileW(wf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    ok = w != INVALID_HANDLE_VALUE && WriteFile(w, out, (DWORD)out_n, &put, NULL) && put == out_n;
    if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
    free(wd);
    free(wf);
    free(out);
    HANDLE r = ok ? open_read(file, 1) : INVALID_HANDLE_VALUE;
    if (r == INVALID_HANDLE_VALUE) {
        rt_log("RomFS: cannot write %s (error %lu); the game keeps its own resolution", file, GetLastError());
        return;
    }
    Identity id = identity_of(r);
    CloseHandle(r);
    char *copy = _strdup(file);
    need(copy != NULL, "overlay path copy");
    if (base) {
        free(base->path);
        base->path = copy;
        base->size = out_n;
        base->id = id;
    } else {
        if (noverlay == overlay_cap) {
            overlay_cap = overlay_cap ? overlay_cap * 2 : 16;
            overlays = realloc(overlays, overlay_cap * sizeof *overlays);
            need(overlays != NULL, "overlay table");
        }
        char *rel = _strdup(HEAPDEF);
        need(rel != NULL, "overlay path copy");
        overlays[noverlay++] = (Overlay){i, out_n, copy, rel, id};
    }
    rt_log("RomFS overlay: %s (%zu bytes, grown heaps) replaces %s", file, out_n, base ? "the mod's table" : "the original");
    res_heapdef_served();
}

static void overlay_init(void) {
    // TOTK_ROMFS_PATCHES unset: romfs-patches next to romfs-index.bin if it has a manifest (tools/make_romfs_patches.py).
    // A game resolution that is not 16:9 takes romfs-patches-<w>x<h> instead when it is there: the same overlay with
    // the UI laid out for that shape (make_romfs_patches.py --aspect).
    char def[2100];
    const char *patches = cfg_get("TOTK_ROMFS_PATCHES");
    unsigned out[2];
    res_wanted(out);
    if (!patches && (unsigned long long)out[0] * 9 != (unsigned long long)out[1] * 16) {
        if (snprintf(def, sizeof def, "%s/romfs-patches-%ux%u/manifest.tsv", index_dir, out[0], out[1]) < (int)sizeof def &&
            GetFileAttributesA(def) != INVALID_FILE_ATTRIBUTES) {
            def[strlen(def) - strlen("/manifest.tsv")] = 0;
            patches = def;
        } else {
            rt_log("RomFS overlay: no romfs-patches-%ux%u for this game resolution, the UI keeps its 16:9 layout "
                   "(tools/make_romfs_patches.py --aspect %ux%u)", out[0], out[1], out[0], out[1]);
        }
    }
    if (!patches && snprintf(def, sizeof def, "%s/romfs-patches/manifest.tsv", index_dir) < (int)sizeof def &&
        GetFileAttributesA(def) != INVALID_FILE_ATTRIBUTES) {
        def[strlen(def) - strlen("/manifest.tsv")] = 0;
        patches = def;
    }
    const char *dirs[2] = {patches, cfg_get("TOTK_ROMFS_MODS")};
    for (int i = 0; i < 2; i++)
        if (dirs[i] && *dirs[i] && strcmp(dirs[i], "0")) overlay_load(dirs[i]);
    heapdef_overlay();
    added_init();
    if (noverlay) qsort(overlays, noverlay, sizeof *overlays, overlay_order);
    for (size_t k = 1; k < noverlay; k++)
        if (overlays[k].index == overlays[k - 1].index)
            rt_fatal("RomFS overlay: %s and %s replace the same file %s", overlays[k - 1].path, overlays[k].path,
                     overlays[k].name);
}

// ---- start
// romfs-index.bin next to totk.exe, or TOTK_ROMFS_INDEX.
static void index_path(char *out, size_t cap) {
    const char *e = cfg_get("TOTK_ROMFS_INDEX");
    if (e && *e) {
        need(snprintf(out, cap, "%s", e) < (int)cap, "index path");
        return;
    }
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    need(n && n < MAX_PATH * 2, "executable path");
    while (n && exe[n - 1] != L'\\' && exe[n - 1] != L'/') n--;
    exe[n] = 0;
    int k = WideCharToMultiByte(CP_UTF8, 0, exe, -1, out, (int)cap, NULL, NULL);
    need(k > 0 && (size_t)k + 16 < cap, "executable path");
    strcat(out, "romfs-index.bin");
}

static void load_index(void) {
    char path[2100];
    index_path(path, sizeof path);
    snprintf(index_dir, sizeof index_dir, "%s", path);
    char *sep = strrchr(index_dir, '\\'), *slash = strrchr(index_dir, '/');
    if (slash > sep) sep = slash;
    if (sep) *sep = 0;
    else strcpy(index_dir, ".");
    HANDLE f = open_read(path, 0);
    if (f == INVALID_HANDLE_VALUE)
        rt_fatal("RomFS index %s missing (tools/build_game.py writes it next to totk.exe)", path);
    LARGE_INTEGER sz;
    need(GetFileSizeEx(f, &sz) && sz.QuadPart >= 24 && sz.QuadPart < (128 << 20), "index size");
    index_size = (size_t)sz.QuadPart;
    index_bytes = malloc(index_size);
    need(index_bytes != NULL, "index allocation");
    for (size_t done = 0; done < index_size;) {
        DWORD got = 0;
        need(ReadFile(f, index_bytes + done, (DWORD)(index_size - done), &got, NULL) && got, "index read");
        done += got;
    }
    CloseHandle(f);
    uint8_t d[32];
    char hex[65];
    rt_sha256(index_bytes, index_size, d);
    hex32(d, hex);
    if (strcmp(hex, ROMFS_INDEX_SHA256))
        rt_fatal("RomFS index %s has SHA-256 %s, this build expects %s (rebuild with tools/build_game.py)", path, hex,
                 ROMFS_INDEX_SHA256);
    need(!memcmp(index_bytes, "TOTKRFS1", 8), "index format");
    memcpy(&file_count, index_bytes + 8, 8);
    memcpy(&strings_offset, index_bytes + 16, 8);
    need(file_count == ROMFS_FILE_COUNT && file_count < 1000000 && strings_offset == 24 + file_count * 24 &&
         strings_offset < index_size, "index layout");
    const char *previous = NULL;
    for (uint64_t i = 0; i < file_count; i++) {
        Entry e = entry(i);
        need(e.offset <= ROMFS_IMAGE_SIZE && e.size <= ROMFS_IMAGE_SIZE - e.offset, "file bounds");
        need(e.name < index_size - strings_offset && e.len > 1 && e.len <= index_size - strings_offset - e.name,
             "name bounds");
        const char *name = entry_name(e);
        need(name[e.len - 1] == 0 && strlen(name) == e.len - 1 && (!previous || strcmp(previous, name) < 0),
             "sorted paths");
        previous = name;
    }
    rt_log("RomFS index %s: SHA-256 ok, %llu files, %zu bytes", path, (unsigned long long)file_count, index_size);
}

static BOOL CALLBACK init_once(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once, (void)param, (void)ctx;
#ifdef ROMFS_MISSING
    rt_fatal("this build has no RomFS: tools/build_game.py found no index.json in its --romfs directory");
#else
    double t0 = now_s();
    load_index();
    const char *dir = rt_config()->romfs;
    need(snprintf(image_file, sizeof image_file, "%s/romfs.bin", dir) < (int)sizeof image_file, "image path");
    image = open_read(image_file, 1);
    if (image == INVALID_HANDLE_VALUE) rt_fatal("RomFS image %s missing (error %lu); put romfs.bin into the romfs folder next to totk.exe or set TOTK_ROMFS",
                                          image_file, GetLastError());
    LARGE_INTEGER sz;
    need(GetFileSizeEx(image, &sz), "image size");
    if ((uint64_t)sz.QuadPart != ROMFS_IMAGE_SIZE)
        rt_fatal("RomFS image %s has %llu bytes, the index was built for %llu", image_file,
                 (unsigned long long)sz.QuadPart, (unsigned long long)ROMFS_IMAGE_SIZE);
    rt_log("RomFS image %s: size ok (%llu bytes)", image_file, (unsigned long long)ROMFS_IMAGE_SIZE);
    verify_image(index_dir);
    const char *mm = cfg_get("TOTK_ROMFS_MMAP");
    if (mm && strcmp(mm, "0")) {
        HANDLE map = CreateFileMappingW(image, NULL, PAGE_READONLY, 0, 0, NULL);
        const void *v = map ? MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0) : NULL;
        if (map) CloseHandle(map);   // the view keeps the mapping alive
        if (v) {
            image_map = v;
            rt_log("RomFS: mapped read path enabled");
        } else {
            rt_log("RomFS: mapping failed (error %lu); using ReadFile", GetLastError());
        }
    }
    overlay_init();
    rt_log("RomFS ready: %llu files, %zu overlays, %zu added, %.2f s", (unsigned long long)file_count, noverlay,
           nadded, now_s() - t0);
#endif
    return TRUE;
}

void fs_init(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, init_once, NULL, NULL);
}

// ---- the RomFS interface (fs.h)
void fs_trace(const char *what, const char *path) {
    static double t0;
    if (!(rt_trace & RT_TRACE_FS)) return;
    if (!t0) t0 = now_s();
    rt_log("fs %8.3f %s %s", now_s() - t0, what, path ? path : "(null)");
}

uint64_t romfs_file_count(void) { return file_count; }

const char *romfs_entry(uint64_t i, uint64_t *offset, uint64_t *size) {
    if (i >= file_count) return NULL;
    Entry e = entry(i);
    if (offset) *offset = e.offset;
    if (size) *size = e.size;
    return entry_name(e);
}

const char *romfs_image_path(void) { return image_file; }

size_t romfs_overlay_count(void) { return noverlay; }

const char *romfs_overlay(size_t k, const char **file, uint64_t *size, int *is_added) {
    if (k >= noverlay) return NULL;
    *file = overlays[k].path;
    *size = overlays[k].size;
    *is_added = overlays[k].index >= file_count;
    return overlays[k].name;
}

int romfs_owns(const char *path) {
    size_t n = strlen(mount_name);
    return n && path && !strncmp(path, mount_name, n) && path[n] == ':';
}

int romfs_handle(uint64_t h) { return h > ROMFS_HANDLE_BASE && h < ROMFS_HANDLE_BASE + NH; }

uint32_t romfs_mount(const char *name) {
    fs_init();
    if (!name || !romfs_path_valid_relative(name) || strlen(name) >= sizeof mount_name || strchr(name, '/') ||
        strchr(name, ':'))
        return FS_R_INVALID;
    AcquireSRWLockExclusive(&lock);
    if (*mount_name && strcmp(name, mount_name)) rt_fatal("second RomFS mount %s (%s is mounted)", name, mount_name);
    strcpy(mount_name, name);
    ReleaseSRWLockExclusive(&lock);
    rt_log("fs mount %s: -> RomFS", name);
    return 0;
}

uint32_t romfs_open(uint64_t *out, const char *path, int mode) {
    fs_trace("rom", path);
    if (!out || mode != FS_OPEN_READ) return FS_R_UNSUPPORTED_OPEN;
    const char *p = romfs_path_relative(mount_name, path);
    if (!p) return FS_R_PATH_NOT_FOUND;
    uint64_t i = file_index(p);
    if (i == UINT64_MAX) return FS_R_PATH_NOT_FOUND;
    uint32_t ov = overlay_slot(i);
    HANDLE f = INVALID_HANDLE_VALUE;
    if (ov) {
        const Overlay *o = &overlays[ov - 1];
        f = open_read(o->path, 1);
        Identity id;
        if (f != INVALID_HANDLE_VALUE) id = identity_of(f);
        if (f == INVALID_HANDLE_VALUE || memcmp(&id, &o->id, sizeof id))
            rt_fatal("RomFS overlay %s is missing or changed since the start check (restart the game)", o->path);
    }
    AcquireSRWLockExclusive(&lock);
    for (unsigned j = 1; j < NH; j++)
        if (!handles[j]) {
            handles[j] = i + 1;
            handle_ovl[j] = ov;
            handle_file[j] = f;
            *out = ROMFS_HANDLE_BASE + j;
            ReleaseSRWLockExclusive(&lock);
            return 0;
        }
    ReleaseSRWLockExclusive(&lock);
    rt_fatal("RomFS file handle table full (%d)", NH);
}

// The handle's index row (an added file's is empty: its bytes are all in its overlay), and where its bytes come from:
// the overlay file, or the original's range of the image.
static int handle_source(uint64_t h, HANDLE *f, uint64_t *base, uint64_t *size) {
    if (!romfs_handle(h)) return 0;
    uint64_t j = h - ROMFS_HANDLE_BASE;
    AcquireSRWLockShared(&lock);
    uint64_t i = handles[j];
    uint32_t ov = handle_ovl[j];
    HANDLE of = handle_file[j];
    ReleaseSRWLockShared(&lock);
    if (!i) return 0;
    if (ov) {
        *f = of;
        *base = 0;
        *size = overlays[ov - 1].size;
    } else {
        Entry e = entry(i - 1);
        *f = image;
        *base = e.offset;
        *size = e.size;
    }
    return 1;
}

uint32_t romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    HANDLE f;
    uint64_t base, fsize;
    if (!out || offset < 0 || !handle_source(h, &f, &base, &fsize)) return FS_R_INVALID;
    if ((uint64_t)offset > fsize || (!buffer && size)) return FS_R_INVALID;
    uint64_t n = size < fsize - (uint64_t)offset ? size : fsize - (uint64_t)offset, done = 0;
    if (f == image && image_map) {
        memcpy(buffer, image_map + base + (uint64_t)offset, (size_t)n);
        done = n;
    }
    while (done < n) {
        uint32_t chunk = n - done > READ_CHUNK ? READ_CHUNK : (uint32_t)(n - done);
        int64_t r = read_at(f, (uint8_t *)buffer + done, chunk, base + (uint64_t)offset + done);
        if (r <= 0) rt_fatal("RomFS read failure at %#llx (error %lu)", (unsigned long long)(base + offset + done),
                             GetLastError());
        done += (uint64_t)r;
    }
    *out = done;
    return 0;
}

uint32_t romfs_size(int64_t *out, uint64_t h) {
    HANDLE f;
    uint64_t base, size;
    if (!out || !handle_source(h, &f, &base, &size)) return FS_R_INVALID;
    *out = (int64_t)size;
    return 0;
}

void romfs_close(uint64_t h) {
    HANDLE f;
    uint64_t base, size;
    if (!handle_source(h, &f, &base, &size))
        rt_fatal("RomFS: close of invalid file handle %#llx", (unsigned long long)h);
    uint64_t j = h - ROMFS_HANDLE_BASE;
    AcquireSRWLockExclusive(&lock);
    HANDLE of = handle_ovl[j] ? handle_file[j] : INVALID_HANDLE_VALUE;
    handles[j] = 0;
    handle_ovl[j] = 0;
    handle_file[j] = INVALID_HANDLE_VALUE;
    ReleaseSRWLockExclusive(&lock);
    if (of != INVALID_HANDLE_VALUE) CloseHandle(of);
}

uint32_t romfs_entry_type(int *out, const char *path) {
    if (!out) return FS_R_INVALID;
    const char *p = romfs_path_relative(mount_name, path);
    if (!p) return FS_R_PATH_NOT_FOUND;
    if (file_index(p) != UINT64_MAX) {
        *out = FS_ENTRY_FILE;
        return 0;
    }
    char dir[4096];
    if (snprintf(dir, sizeof dir, "%s/", p) >= (int)sizeof dir) return FS_R_INVALID;
    if (folder_exists(dir)) {
        *out = FS_ENTRY_DIRECTORY;
        return 0;
    }
    return FS_R_PATH_NOT_FOUND;
}

// ---- overrides
// nn::Result nn::fs::MountRom(const char *name, void *cache, size_t cache_size)
static void ov_mount_rom(Cpu *c) { ret_x(c, romfs_mount(arg_p(c, 0))); }
// nn::Result nn::fs::QueryMountRomCacheSize(size_t *out): no cache needed, the host reads the image directly
static void ov_query_cache(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    if (out) *out = 0;
    ret_x(c, out ? 0 : FS_R_INVALID);
}
// void nn::fs::InitializeForApplication(): SDK-internal; opens the index instead of the fs service session
static void ov_initialize(Cpu *c) {
    (void)c;
    fs_init();
}

HOST_OVERRIDE("_ZN2nn2fs8MountRomEPKcPvm", ov_mount_rom);
HOST_OVERRIDE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", ov_query_cache);
HOST_OVERRIDE("_ZN2nn2fs24InitializeForApplicationEv", ov_initialize);
