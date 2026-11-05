// Imports a .cai blueprint into the game's own Autobuild favourites: the 30-slot BinaryArray of the loaded save data, held
// in guest memory for the whole run. The game keeps that list in two shapes, and both are written:
//
//   serialized  u32 count = 30, then 30 items of {u32 size = 6688; CmbAct blob}, 6692 bytes apart. This is the array the
//               game writes back to progress.sav, so this write is what persists.
//   parsed      30 slots of two 6688-byte blobs in a row (13376 bytes per slot, no size fields): the copies the Autobuild
//               page reads when it draws. Writing only the serialized copy leaves the page showing the old creation until
//               the save is loaded again.
//
// The addresses are found by scanning the guest heap for the blob magic (memchr, so ten GiB takes well under a second),
// grouping the hits by the item stride, and checking the group's shape. The result is cached for the rest of the run: the
// list does not move once the save is loaded, and a cached address that stops verifying is simply found again.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "blueprint_cai.h"
#include "blueprint_import.h"
#include "host.h"
#include "platform/platform.h"

enum {
    ITEM_STRIDE = BPC_BLOB_SIZE + 4,   // a serialized item is its u32 size and then the blob
    SLOT_STRIDE = BPC_BLOB_SIZE * 2,   // a parsed slot is the game's own copy and a second one
    CAND_MAX = 4096,                   // blob hits kept from one scan; the arrays alone are 30 to 60
};

// Where the lists are, as found by the scan. 0: not found (yet).
static uint64_t g_serialized;   // the address of slot 0's blob (its u32 size is 4 bytes before it)
static int g_serialized_slots;
static uint64_t g_parsed;       // the address of slot 0's first blob
static int g_parsed_slots;
static uint64_t g_scan_ns;      // plat_now_ns() of the last scan, so a failed scan is not repeated every frame

static uint64_t now_ns(void) { return plat_now_ns(); }

// Guest memory is host memory: a committed page is read and written with memcpy, after asking its state so an uncommitted
// page is a miss instead of a fault.
static int guest_read(uint64_t a, void *out, uint64_t n) {
    uint64_t end = 0;
    if (!a) return 0;
    int st = plat_mem_state(a, a + n, &end);
    if ((st != PLAT_PROT_RW && st != PLAT_PROT_READ) || end < a + n) return 0;
    memcpy(out, (const void *)(uintptr_t)a, (size_t)n);
    return 1;
}

// A page the runtime froze read-only (write-watch) is made writable for this copy and put back afterwards, as the cheats'
// memory writes do (runtime/cheats.c, wr_main).
static int guest_write(uint64_t a, const void *from, uint64_t n) {
    const uint8_t *p = from;
    for (uint64_t done = 0; done < n;) {
        uint64_t page = (a + done) & ~0xFFFull;
        uint64_t part = page + 0x1000 - (a + done);
        if (part > n - done) part = n - done;
        uint64_t end = 0;
        int st = plat_mem_state(page, page + 1, &end);
        if (st != PLAT_PROT_RW && st != PLAT_PROT_READ) return 0;
        int frozen = st == PLAT_PROT_READ;
        if (frozen && !plat_mem_protect(page, 0x1000, PLAT_PROT_RW)) return 0;
        memcpy((void *)(uintptr_t)(a + done), p + done, (size_t)part);
        if (frozen) plat_mem_protect(page, 0x1000, PLAT_PROT_READ);
        done += part;
    }
    return 1;
}

// The serialized list at base (slot 0's blob): count word before it, each item's size field 4 bytes before its blob, each
// blob's magic and version. Checks all of it, so a wrong address is rejected rather than written.
static int verify_serialized(uint64_t base, int *slots) {
    uint32_t count = 0;
    if (!guest_read(base - 8, &count, 4) || count == 0 || count > BP_SLOTS) return 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t item = base + (uint64_t)i * ITEM_STRIDE;
        uint32_t size = 0;
        uint8_t head[12];
        if (!guest_read(item - 4, &size, 4) || size != BPC_BLOB_SIZE) return 0;
        if (!guest_read(item, head, sizeof head)) return 0;
        if (memcmp(head, "CmbAct", 6) || head[8] != BPC_VERSION) return 0;
    }
    *slots = (int)count;
    return 1;
}

// The parsed arrays at base: slots of two blobs each; the game may hold a second copy elsewhere, so the length is what
// this array itself has. Only the magic is checked (the parsed records carry no size field).
static int verify_parsed(uint64_t base, int runs, int *slots) {
    int slots_here = runs / 2;
    if (slots_here > BP_SLOTS) slots_here = BP_SLOTS;
    for (int i = 0; i < slots_here; i++) {
        uint8_t a[12], b[12];
        if (!guest_read(base + (uint64_t)i * SLOT_STRIDE, a, sizeof a)) return 0;
        if (!guest_read(base + (uint64_t)i * SLOT_STRIDE + BPC_BLOB_SIZE, b, sizeof b)) return 0;
        if (memcmp(a, "CmbAct", 6) || memcmp(b, "CmbAct", 6)) return 0;
    }
    *slots = slots_here;
    return 1;
}

// One blob address found by a scan.
struct cand { uint64_t a; };

static int cand_cmp(const void *x, const void *y) {
    const struct cand *a = x, *b = y;
    return a->a < b->a ? -1 : a->a > b->a ? 1 : 0;
}

// Collect every "CmbAct" hit in the guest heap. The heap is the one region the save data lives in (a reservation the game
// grows into, runtime/memory.c), so the scan skips the alias region and the modules: about a third of the address space a
// whole-process scan would touch. The bytes are read where they are (guest memory is host memory) after plat_mem_state
// proved the run committed and readable, so nothing faults and nothing is copied.
static int scan_hits(struct cand *out, int cap) {
    const RtGuestLayout *L = mem_layout();
    if (!L || !L->heap.size) return 0;
    int n = 0;
    uint64_t at = L->heap.base, stop = L->heap.base + L->heap.size;
    while (at < stop && n < cap) {
        uint64_t end = 0;
        int st = plat_mem_state(at, stop, &end);
        if (st != PLAT_PROT_RW && st != PLAT_PROT_READ) {
            at = end > at ? end : stop;   // reserved, freed or inaccessible: skip the run whole
            continue;
        }
        uint64_t to = end > stop ? stop : end;
        const uint8_t *p = (const uint8_t *)(uintptr_t)at;
        uint64_t len = to - at;
        const uint8_t *hit = memchr(p, 'C', (size_t)len);
        while (hit && n < cap) {
            if (!memcmp(hit, "CmbAct", 6)) {
                struct cand c = {at + (uint64_t)(hit - p)};
                out[n++] = c;
            }
            size_t done = (size_t)(hit - p) + 1;
            hit = done < len ? memchr(p + done, 'C', (size_t)(len - done)) : NULL;
        }
        at = to;
    }
    if (n > 1) qsort(out, (size_t)n, sizeof out[0], cand_cmp);
    return n;
}

// Find both lists. Returns 1 when the serialized one (the one the save writes) is known. A previously found address is
// used until it stops verifying; the scan then runs again, at most once per second, so a save that is still loading is
// retried without stalling the page's thread.
static int find_lists(void) {
    if (g_serialized) {
        int slots = 0;
        if (verify_serialized(g_serialized, &slots)) {
            g_serialized_slots = slots;
            return 1;
        }
        g_serialized = 0;   // stale: the list moved (a reload), scan again
    }
    uint64_t t = now_ns();
    if (t < g_scan_ns) return 0;
    g_scan_ns = t + 1000000000ull;
    uint64_t t0 = now_ns();
    static struct cand hits[CAND_MAX];
    int n = scan_hits(hits, CAND_MAX);
    rt_log("blueprint: heap scan: %d candidates in %.0f ms", n, (double)(now_ns() - t0) / 1e6);
    if (n <= 1) return 0;
    // Runs at the item stride are the serialized list; runs at the slot stride are the parsed copies. A run is taken from
    // its first blob, and a candidate inside a run already taken is skipped so the parsed array is not read twice.
    g_serialized = 0;
    g_parsed = 0;
    g_serialized_slots = 0;
    g_parsed_slots = 0;
    int i = 0;
    while (i < n) {
        int run = 1;
        uint64_t stride = 0;
        while (i + run < n) {
            uint64_t d = hits[i + run].a - hits[i + run - 1].a;
            if (d != ITEM_STRIDE && d != BPC_BLOB_SIZE) break;
            if (!stride) stride = d;
            else if (d != stride) break;
            run++;
        }
        int slots = 0;
        if (stride == ITEM_STRIDE && run >= 3 && !g_serialized && verify_serialized(hits[i].a, &slots)) {
            g_serialized = hits[i].a;
            g_serialized_slots = slots;
            rt_log("blueprint: favourites list (save data) at %#llx, %d slots", (unsigned long long)hits[i].a, slots);
        } else if (!stride && !g_serialized && verify_serialized(hits[i].a, &slots)) {
            // A list of one slot: no neighbour to give it a stride, so it is checked on its own shape.
            g_serialized = hits[i].a;
            g_serialized_slots = slots;
            rt_log("blueprint: favourites list (save data, one slot) at %#llx", (unsigned long long)hits[i].a);
        } else if (stride == BPC_BLOB_SIZE && run >= BP_SLOTS && !g_parsed && verify_parsed(hits[i].a, run, &slots)) {
            // The parsed copy: the game's own records for the page, two blobs per slot with no size field, so neighbours
            // are exactly one blob apart.
            g_parsed = hits[i].a;
            g_parsed_slots = slots;
            rt_log("blueprint: favourites list (page copy) at %#llx, %d slots", (unsigned long long)hits[i].a, slots);
        }
        i += run;
    }
    if (!g_serialized) rt_log("blueprint: no favourites list in the save data yet (the save is still loading)");
    return g_serialized != 0;
}

int bp_favorites(BpFavorites *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!find_lists()) return 0;
    out->slots = g_serialized_slots;
    for (int i = 0; i < g_serialized_slots; i++) {
        uint8_t head[12];
        uint64_t blob = g_serialized + (uint64_t)i * ITEM_STRIDE;
        // Actor count is byte 9 of the blob's header: magic, two pad bytes, version, count.
        if (guest_read(blob, head, sizeof head) && !memcmp(head, "CmbAct", 6)) out->actors[i] = (int)head[9];
    }
    return 1;
}

// The game's cached draft thumbnails are <profile>/cache/<index>/icon/Draft_NNN.png, drawn once for the creation a slot
// held. A replaced slot would keep showing the old creation's picture, so the cache is cleared and the game draws them
// again. Failure here is not an error: a stale picture is the only consequence.
static void drop_thumbnails(void) {
    const RtConfig *c = rt_config();
    if (!c || !c->profile) return;
    char dir[PLAT_PATH_MAX];
    snprintf(dir, sizeof dir, "%s/cache", c->profile);
    int removed = 0;
    for (int index = 0; index < 8; index++) {
        char icons[PLAT_PATH_MAX];
        snprintf(icons, sizeof icons, "%s/%d/icon", dir, index);
        if (plat_fs_kind(icons) != PLAT_FS_DIR) continue;
        char name[64];
        for (int k = 0; k < 256; k++) {
            snprintf(name, sizeof name, "Draft_%03d.png", k);
            char path[PLAT_PATH_MAX];
            snprintf(path, sizeof path, "%s/%s", icons, name);
            if (plat_fs_kind(path) == PLAT_FS_FILE && plat_fs_unlink(path) == 0) removed++;
        }
    }
    if (removed) rt_log("blueprint: dropped %d cached draft picture(s), the game draws them again", removed);
}

int bp_import(const char *path, int slot) {
    if (!path || !*path) return BP_IMPORT_NO_FILE;
    if (slot < 0 || slot >= BP_SLOTS) return BP_IMPORT_BAD_SLOT;
    // Read and check the file first: nothing is written when it is not a blueprint this port accepts.
    uint8_t buf[BPC_BLOB_SIZE + 24];
    PlatFile *f = plat_file_open(path);
    if (!f) {
        rt_log("blueprint: cannot open %s", path);
        return BP_IMPORT_NO_FILE;
    }
    int64_t got = plat_file_pread(f, buf, sizeof buf, 0);
    plat_file_close(f);
    BpcBlueprint bp;
    enum BpcResult r = bpc_parse(buf, got > 0 ? (size_t)got : 0, &bp);
    if (r != BPC_OK) {
        rt_log("blueprint: %s: %s", path, bpc_result_text(r));
        return got < (int64_t)BPC_BLOB_SIZE ? BP_IMPORT_NO_FILE : BP_IMPORT_BAD_FILE;
    }
    if (!find_lists()) return BP_IMPORT_NO_SAVE;
    if (slot >= g_serialized_slots) return BP_IMPORT_BAD_SLOT;
    uint64_t dst = g_serialized + (uint64_t)slot * ITEM_STRIDE;
    if (!guest_write(dst, bp.blob, BPC_BLOB_SIZE)) {
        rt_log("blueprint: cannot write slot %d at %#llx", slot + 1, (unsigned long long)dst);
        g_serialized = 0;
        return BP_IMPORT_NO_ROOM;
    }
    // The parsed copies the Autobuild page reads: both blobs of the slot, so the page shows the new creation the next
    // time it opens without a save reload. A failure here does not fail the import: the save data is already written.
    if (g_parsed && slot < g_parsed_slots) {
        uint64_t p0 = g_parsed + (uint64_t)slot * SLOT_STRIDE;
        guest_write(p0, bp.blob, BPC_BLOB_SIZE);
        guest_write(p0 + BPC_BLOB_SIZE, bp.blob, BPC_BLOB_SIZE);
    }
    rt_log("blueprint: slot %d now holds %s (%d parts); the game saves it with the rest of the progress", slot + 1,
           bp.names[0], bp.actors);
    drop_thumbnails();
    return BP_IMPORT_OK;
}

const char *bp_import_text(int r) {
    switch (r) {
    case BP_IMPORT_OK: return "ok";
    case BP_IMPORT_NO_FILE: return "cannot read the blueprint file";
    case BP_IMPORT_BAD_FILE: return "the file is not a .cai blueprint this port can import";
    case BP_IMPORT_NO_SAVE: return "no loaded save data: load a save first";
    case BP_IMPORT_BAD_SLOT: return "the slot is outside the list the save holds";
    case BP_IMPORT_NO_ROOM: return "the save data could not be written in guest memory";
    }
    return "unknown import result";
}
