// Importing a .cai or .txt blueprint into the game's own Autobuild history, so the creation appears in the native
// Autobuild page as the newest draft and builds with the native preview, rotation and Zonai costs.
//
// The history is one save flag: the BinaryArray 0xa56722b6 (AutoBuilder.Draft.Content.CombinedActorInfo), 30 items of
// {u32 size = 6688; CmbAct blob}. The Autobuild page draws its cards in the order the items sit in that array: item 0 is
// the card at the top, item 29 the one at the bottom. That was measured on a running game (writing the entry the save's
// Index array calls "oldest" put the card at the bottom of the list; writing item 0 put it at the top), so an import
// writes item 0.
//
// The game holds the loaded save data in guest memory (runtime/memory.c maps guest memory as host memory), so the write
// goes straight into it. Two of the game's in-memory copies of the draft array are written: the serialized array (the
// bytes it writes back to progress.sav) and the parsed page copy it reads when the Autobuild page draws.
//
// Deleting the cached draft pictures (<profile>/cache/<i>/icon/Draft_NNN.png) is what makes the change visible on the
// open page: without it the game keeps drawing the picture it drew before, which looks exactly like a failed import.
//
// The addresses are found by scanning the guest heap for the array's shape. Nothing depends on a fixed address.
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint_cai.h"
#include "blueprint_file.h"
#include "blueprint_import.h"
#include "host.h"
#include "platform/platform.h"

enum {
    ITEM_STRIDE = BPC_BLOB_SIZE + 4,   // a serialized item is its u32 size and then the blob
    SLOT_STRIDE = BPC_BLOB_SIZE * 2,   // a parsed slot is the game's own copy and a second one
    CAND_MAX = 4096,
    N = BP_SLOTS,
};

// Where the arrays are, as found by the scan. 0: not found (yet).
static uint64_t g_cai;      // the serialized array: entry 0's blob (its u32 size is 4 bytes before it)
static uint64_t g_parsed;   // the page copy: entry 0's first blob
static int g_parsed_slots;
static uint64_t g_scan_ns;  // plat_now_ns() of the last scan, so a failed scan is not repeated every frame

static uint64_t now_ns(void) { return plat_now_ns(); }

// Defined below: the entry the save's Index array calls 0, the card the Autobuild page draws first.
static int newest_position_from_save(void);

// Guest memory is host memory: a committed page is read and written with memcpy, after asking its state so an
// uncommitted page is a miss instead of a fault.
static int guest_read(uint64_t a, void *out, uint64_t n) {
    uint64_t end = 0;
    if (!a) return 0;
    int st = plat_mem_state(a, a + n, &end);
    if ((st != PLAT_PROT_RW && st != PLAT_PROT_READ) || end < a + n) return 0;
    memcpy(out, (const void *)(uintptr_t)a, (size_t)n);
    return 1;
}

// A page the runtime froze read-only (write-watch) is made writable for this copy and put back afterwards, as the
// cheats' memory writes do (runtime/cheats.c, wr_main).
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

// The serialized array at base (entry 0's blob): the count word before it, each entry's size field 4 bytes before its
// blob, and the blob's own magic and version. Checking all of it rejects a wrong address instead of writing into it.
static int verify_cai(uint64_t base) {
    uint32_t count = 0;
    if (!guest_read(base - 8, &count, 4) || count != N) return 0;
    for (int i = 0; i < N; i++) {
        uint64_t item = base + (uint64_t)i * ITEM_STRIDE;
        uint32_t size = 0;
        uint8_t head[10];
        if (!guest_read(item - 4, &size, 4) || size != BPC_BLOB_SIZE) return 0;
        if (!guest_read(item, head, sizeof head)) return 0;
        if (memcmp(head, "CmbAct", 6) || head[8] != BPC_VERSION) return 0;
    }
    return 1;
}

// The parsed copies at base: entries of two blobs each with no size field, so neighbours are exactly one blob apart.
static int verify_parsed(uint64_t base, int runs, int *slots) {
    int here = runs / 2;
    if (here > N) here = N;
    for (int i = 0; i < here; i++) {
        uint8_t a[10], b[10];
        if (!guest_read(base + (uint64_t)i * SLOT_STRIDE, a, sizeof a)) return 0;
        if (!guest_read(base + (uint64_t)i * SLOT_STRIDE + BPC_BLOB_SIZE, b, sizeof b)) return 0;
        if (memcmp(a, "CmbAct", 6) || memcmp(b, "CmbAct", 6)) return 0;
    }
    *slots = here;
    return 1;
}

static int cmp_u64(const void *x, const void *y) {
    uint64_t a = *(const uint64_t *)x, b = *(const uint64_t *)y;
    return a < b ? -1 : a > b ? 1 : 0;
}

// Every "CmbAct" hit in the guest heap. The heap is where the loaded save data lives (runtime/memory.c); the alias
// region and the modules hold none, so skipping them leaves most of the address space untouched. The bytes are read
// where they are, after plat_mem_state proved the run readable, so nothing faults.
static int scan_hits(uint64_t *out, int cap) {
    const RtGuestLayout *L = mem_layout();
    if (!L || !L->heap.size) return 0;
    int n = 0;
    uint64_t at = L->heap.base, stop = L->heap.base + L->heap.size;
    while (at < stop && n < cap) {
        uint64_t end = 0;
        int st = plat_mem_state(at, stop, &end);
        if (st != PLAT_PROT_RW && st != PLAT_PROT_READ) {
            at = end > at ? end : stop;
            continue;
        }
        uint64_t to = end > stop ? stop : end;
        const uint8_t *p = (const uint8_t *)(uintptr_t)at;
        uint64_t len = to - at;
        const uint8_t *hit = memchr(p, 'C', (size_t)len);
        while (hit && n < cap) {
            if (!memcmp(hit, "CmbAct", 6)) out[n++] = at + (uint64_t)(hit - p);
            size_t done = (size_t)(hit - p) + 1;
            hit = done < len ? memchr(p + done, 'C', (size_t)(len - done)) : NULL;
        }
        at = to;
    }
    if (n > 1) qsort(out, (size_t)n, sizeof out[0], cmp_u64);
    return n;
}

// Find the arrays. A previously found address is used until it stops verifying; the scan then runs again, at most once
// per second, so a save that is still loading is retried without stalling the page's thread.
static int find_lists(void) {
    if (g_cai && verify_cai(g_cai)) return 1;
    uint64_t t = now_ns();
    if (t < g_scan_ns) return 0;
    g_scan_ns = t + 1000000000ull;
    g_cai = 0;
    g_parsed = 0;
    g_parsed_slots = 0;

    uint64_t t0 = now_ns();
    static uint64_t hits[CAND_MAX];
    const int n = scan_hits(hits, CAND_MAX);
    int i = 0;
    while (i < n) {
        // A run is a group of hits at one uniform stride: the item stride is the serialized array, the blob stride is
        // the parsed copies (two blobs per entry).
        int run = 1;
        uint64_t stride = 0;
        while (i + run < n) {
            uint64_t d = hits[i + run] - hits[i + run - 1];
            if (d != ITEM_STRIDE && d != BPC_BLOB_SIZE) break;
            if (stride && d != stride) break;
            stride = stride ? stride : d;
            run++;
        }
        int slots = 0;
        if (stride == ITEM_STRIDE && run >= N && !g_cai && verify_cai(hits[i])) {
            g_cai = hits[i];
        } else if (stride == BPC_BLOB_SIZE && run >= N && !g_parsed && verify_parsed(hits[i], run, &slots)) {
            g_parsed = hits[i];
            g_parsed_slots = slots;
        }
        i += run;
    }
    rt_log("blueprint: scan %d blobs in %.0f ms: drafts %#llx, page copy %#llx (%d entries)", n,
           (double)(now_ns() - t0) / 1e6, (unsigned long long)g_cai, (unsigned long long)g_parsed, g_parsed_slots);
    if (!g_cai) rt_log("blueprint: no Autobuild draft list in the save data yet (the save is still loading)");
    return g_cai != 0;
}

int bp_favorites(BpFavorites *out) {
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!find_lists()) return 0;
    out->slots = N;
    for (int i = 0; i < N; i++) {
        uint8_t head[10];
        if (guest_read(g_cai + (uint64_t)i * ITEM_STRIDE, head, sizeof head) && !memcmp(head, "CmbAct", 6))
            out->actors[i] = (int)head[9];
    }
    // top is the entry the page draws first, which is the one an import replaces. The page reads it through the save's
    // Index array, so the same lookup is used here instead of assuming entry 0.
    out->top = newest_position_from_save();
    if (out->top < 0) out->top = 0;
    return 1;
}

// The game draws a picture of each draft once and keeps it as <profile>/cache/<i>/icon/Draft_NNN.png. After a change to
// the array those files describe the old contents, so they are removed and the game draws them again. A failure here is
// not an error: a stale picture is the only consequence.
static void drop_thumbnails(void) {
    const RtConfig *c = rt_config();
    if (!c || !c->profile) return;
    int removed = 0;
    for (int index = 0; index < 8; index++) {
        char icons[PLAT_PATH_MAX];
        snprintf(icons, sizeof icons, "%s/cache/%d/icon", c->profile, index);
        if (plat_fs_kind(icons) != PLAT_FS_DIR) continue;
        for (int k = 0; k < 256; k++) {
            char path[PLAT_PATH_MAX];
            snprintf(path, sizeof path, "%s/Draft_%03d.png", icons, k);
            if (plat_fs_kind(path) == PLAT_FS_FILE && plat_fs_unlink(path) == 0) removed++;
        }
    }
    if (removed) rt_log("blueprint: dropped %d cached draft picture(s), the game draws them again", removed);
}

// ---- the save file, for the one array the game does not keep in guest memory
//
// The page draws its history ordered by AutoBuilder.Draft.Content.Index (hash 0xd27f8651), an IntArray of 30 ages where 0
// is the card on top. That array is not in guest memory while the menu runs, so the loaded save file is read for it: the
// game loaded this file, so its values are the ones the page is drawing by. Reading it is what lets an import put the new
// creation on the *first* card instead of wherever entry 0 happens to sort.
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// Whether these 30 ages are present in guest memory. The page can only draw by an array it is holding, so finding a save
// file's ages in the heap says that file is the one the game loaded. Reading the heap is bounded to the region the save
// data lives in (runtime/memory.c) and stops at the first match.
static int ages_present_in_guest(const int32_t *ages) {
    const RtGuestLayout *L = mem_layout();
    if (!L || !L->heap.size) return 0;
    uint8_t key[4 * N];
    memcpy(key, ages, sizeof key);
    uint64_t at = L->heap.base, stop = L->heap.base + L->heap.size;
    while (at < stop) {
        uint64_t end = 0;
        int st = plat_mem_state(at, stop, &end);
        if (st != PLAT_PROT_RW && st != PLAT_PROT_READ) {
            at = end > at ? end : stop;
            continue;
        }
        uint64_t to = end > stop ? stop : end;
        const uint8_t *p = (const uint8_t *)(uintptr_t)at;
        uint64_t len = to - at;
        if (len < sizeof key) {
            at = to;
            continue;
        }
        const uint8_t *hit = memchr(p, key[0], (size_t)len);
        while (hit) {
            size_t off = (size_t)(hit - p);
            if (off + sizeof key <= len && !memcmp(p + off, key, sizeof key)) return 1;
            size_t step = off + 1;
            hit = step < len ? memchr(p + step, key[0], (size_t)(len - step)) : NULL;
        }
        at = to;
    }
    return 0;
}

// The position the save's Index array calls 0 (the card drawn first), or -1 when it cannot be read.
//
// Every save slot has its own Index array, and only one of them belongs to the save the game has loaded; the others are
// the player's other files. Reading whichever came first would order the import by a different save's ages, which is
// how an import lands on card 9 instead of card 1. So each candidate read from a file is checked against the guest
// memory: the array the game is using is the one that is actually present there.
static int newest_position_from_save(void) {
    const RtConfig *c = rt_config();
    if (!c || !c->profile) return -1;
    uint64_t uid[2] = {0, 0};
    account_uid(uid);
    int best = -1;
    int best_is_live = 0;
    for (int slot = 0; slot < 6; slot++) {
        char path[PLAT_PATH_MAX];
        snprintf(path, sizeof path, "%s/save/%016llx%016llx/slot_%02d/progress.sav", c->profile,
                 (unsigned long long)uid[1], (unsigned long long)uid[0], slot);
        PlatFile *f = plat_file_open(path);
        if (!f) continue;
        PlatFileId id;
        if (!plat_file_id(f, &id) || id.size < 0x20 || id.size > (64u << 20)) {
            plat_file_close(f);
            continue;
        }
        uint8_t *d = malloc((size_t)id.size);
        if (!d) {
            plat_file_close(f);
            continue;
        }
        int64_t got = plat_file_pread(f, d, (uint32_t)id.size, 0);
        plat_file_close(f);
        int found = -1;
        int32_t ages[N];
        if (got == (int64_t)id.size) {
            // The declaration block is (hash, offset) pairs from 0x20 up to the data block; a zero hash marks a section
            // and is followed by its number.
            uint32_t data_off = rd32(d + 8);
            for (uint32_t pos = 0x20; pos + 8 <= data_off && pos + 8 <= (uint32_t)id.size;) {
                uint32_t hash = rd32(d + pos);
                pos += 4;
                if (hash == 0) {
                    pos += 4;
                    continue;
                }
                uint32_t off = rd32(d + pos);
                pos += 4;
                if (hash != 0xD27F8651u || (uint64_t)off + 4 + 4 * N > (uint64_t)id.size) continue;
                if (rd32(d + off) != N) continue;
                memcpy(ages, d + off + 4, sizeof ages);
                uint32_t seen = 0;
                int ok = 1;
                for (int i = 0; i < N; i++) {
                    if (ages[i] < 0 || ages[i] >= N || (seen >> ages[i] & 1u)) {
                        ok = 0;
                        break;
                    }
                    seen |= 1u << ages[i];
                }
                if (!ok) continue;
                for (int i = 0; i < N; i++)
                    if (ages[i] == 0) found = i;
                break;
            }
        }
        free(d);
        if (found < 0) continue;
        // Is this array the one the game is holding? The array the page draws by must be in guest memory, and that is
        // what tells this slot's ages apart from the other saves'.
        const int live = ages_present_in_guest(ages);
        if (live || !best_is_live) {
            best = found;
            best_is_live = live;
        }
        if (best_is_live) break;   // the loaded save's own array: no other slot can beat it
    }
    return best;
}

int bp_import_newest(const char *path) {
    if (!path || !*path) return BP_IMPORT_NO_FILE;
    // The file is read whole: a .txt blueprint is a JSON object with the blob gzip-compressed and Base64-encoded inside
    // it (blueprint_file.h), so the blob cannot be read at a fixed offset.
    static uint8_t storage[BPF_READ_MAX];
    BpcBlueprint bp;
    enum BpcResult r = bp_file_read(path, storage, &bp);
    if (r != BPC_OK) return BP_IMPORT_BAD_FILE;
    if (!find_lists()) return BP_IMPORT_NO_SAVE;

    // The card to replace is the one the page draws first, which is the entry the save's Index array calls 0. Writing
    // that entry means the new creation is the first card in the history list, whatever the save's ages happen to be;
    // writing entry 0 instead would leave the creation wherever that entry sorts. Every other entry keeps its data.
    int pos = newest_position_from_save();
    if (pos < 0) {
        rt_log("blueprint: cannot read the Autobuild Index array from the save file, so the import cannot place the "
               "creation first");
        return BP_IMPORT_NO_ORDER;
    }
    const uint64_t dst = g_cai + (uint64_t)pos * ITEM_STRIDE;
    if (!guest_write(dst, bp.blob, BPC_BLOB_SIZE)) {
        rt_log("blueprint: cannot write entry %d at %#llx", pos, (unsigned long long)dst);
        g_cai = 0;
        return BP_IMPORT_NO_ROOM;
    }
    // The parsed copies the page reads: both blobs of that entry. A failure here does not fail the import, because the
    // serialized array (the one the game saves) is already written.
    if (g_parsed && pos < g_parsed_slots) {
        uint64_t page = g_parsed + (uint64_t)pos * SLOT_STRIDE;
        guest_write(page, bp.blob, BPC_BLOB_SIZE);
        guest_write(page + BPC_BLOB_SIZE, bp.blob, BPC_BLOB_SIZE);
    }
    rt_log("blueprint: %s (%d parts) is now the first draft of the Autobuild history (entry %d, the one the save draws "
           "first)", bp.names[0], bp.actors, pos);
    drop_thumbnails();
    return BP_IMPORT_OK;
}

const char *bp_import_text(int r) {
    switch (r) {
    case BP_IMPORT_OK: return "ok";
    case BP_IMPORT_NO_FILE: return "cannot read the blueprint file";
    case BP_IMPORT_BAD_FILE: return "the file is not a blueprint this port can import";
    case BP_IMPORT_NO_SAVE: return "no loaded save data: load a save first";
    case BP_IMPORT_NO_ORDER: return "the Autobuild history is not in memory";
    case BP_IMPORT_BAD_SLOT: return "the entry is outside the list the save holds";
    case BP_IMPORT_NO_ROOM: return "the save data could not be written in guest memory";
    }
    return "unknown import result";
}
