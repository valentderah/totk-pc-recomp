// Importing a .cai or .txt blueprint into the game's own Autobuild history, so the creation appears in the native
// Autobuild page as the newest draft and builds with the native preview, rotation and Zonai costs.
//
// The history is one save flag: the BinaryArray 0xa56722b6 (AutoBuilder.Draft.Content.CombinedActorInfo), 30 items of
// {u32 size = 6688; CmbAct blob}. The order the page draws them in is a second array, AutoBuilder.Draft.Content.Index
// (0xd27f8651): 30 ages, where age 0 is the card at the top. Both live in guest memory, and an import writes both.
//
// The write copies what the game does for a creation built in game instead of overwriting the first card: favourites
// keep their age, every other entry's age rises by one, and the oldest free entry receives the new blob at age 0. That
// rule is bp_rotate_ages, kept pure so tests/blueprint_rotation_test.py can check it without a running game.
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
static uint64_t g_ages_at;  // the live Index array (the 30 ages), 0 when it is not in guest memory
static uint32_t g_fav_bits;  // bit i: entry i is a favorite, from the save's IsFavorite BoolArray
static int g_fav_seen;       // 1 when that BoolArray was actually read; 0 means g_fav_bits is "unknown", not "none"

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
            // The magic is six bytes, and the readable run ends at `to`: a 'C' in its last five bytes has no magic to
            // compare, and reading past the run would touch a page plat_mem_state did not vouch for.
            const size_t off = (size_t)(hit - p);
            if (off + 6 <= (size_t)len && !memcmp(hit, "CmbAct", 6)) out[n++] = at + (uint64_t)off;
            const size_t done = off + 1;
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

// Where these 30 ages sit in guest memory, or 0 when they are nowhere. The page can only draw by an array it is holding,
// so finding a save file's ages in the heap says both that this file is the one the game loaded and where the live array
// is. Returns the address so an insert-style import can rotate that array, rather than only a yes/no.
//
// This is one exact memchr match for the 120-byte run (like scan_hits, ~0.3 s), bounded to the region the save data
// lives in (runtime/memory.c). It is called only when the page is drawn or an import runs, never per frame, and does not
// walk the heap for every 0..29 permutation: an unbounded shape scan there starves the render thread.
static uint64_t ages_present_in_guest(const int32_t *ages) {
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
            if (off + sizeof key <= len && !memcmp(p + off, key, sizeof key)) return at + off;
            size_t step = off + 1;
            hit = step < len ? memchr(p + step, key[0], (size_t)(len - step)) : NULL;
        }
        at = to;
    }
    return 0;
}

// ---- finding the history arrays by shape -----------------------------------------------------------------------------
//
// The Index array and the IsFavorite mask are part of the same save block as the draft blobs, so the search runs in a window
// around a blob address instead of across the whole heap. A run of 30 int32 that are exactly the ranks 0..29 once each is a
// shape the game always produces and that no save file can make wrong; the same goes for the {count=30, mask} pair, which
// holds at most the eight favourites the game allows.

// Calls `fn` for each readable range inside [lo, hi). Returns the number of ranges visited.
static int each_readable(uint64_t lo, uint64_t hi,
                         int (*fn)(uint64_t at, uint64_t len, void *ctx), void *ctx) {
    int n = 0;
    uint64_t at = lo;
    while (at < hi) {
        uint64_t end = 0;
        const int st = plat_mem_state(at, hi, &end);
        if (st != PLAT_PROT_RW && st != PLAT_PROT_READ) {
            at = end > at ? end : hi;
            continue;
        }
        const uint64_t to = end > hi ? hi : end;
        if (to > at && fn(at, to - at, ctx)) return n + 1;
        n++;
        at = to;
    }
    return n;
}

typedef struct {
    uint64_t hit;      // the address found, 0 when none
    uint64_t nearest;  // the distance of the closest candidate so far
    uint64_t anchor;
} PermCtx;

// One readable range: every 4-byte-aligned run of 30 int32 that is a permutation of 0..29. The run closest to the anchor
// wins, because the save block's arrays are neighbours.
static int perm_in_range(uint64_t at, uint64_t len, void *vctx) {
    PermCtx *c = vctx;
    if (len < (uint64_t)N * 4) return 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)at;
    for (uint64_t o = 0; o + (uint64_t)N * 4 <= len; o += 4) {
        uint32_t seen = 0;
        int ok = 1;
        for (int i = 0; i < N; i++) {
            int32_t v;
            memcpy(&v, p + o + (uint64_t)i * 4, 4);
            if (v < 0 || v >= N || (seen >> v & 1u)) {
                ok = 0;
                break;
            }
            seen |= 1u << v;
        }
        if (!ok) continue;
        const uint64_t here = at + o;
        const uint64_t dist = here > c->anchor ? here - c->anchor : c->anchor - here;
        if (!c->hit || dist < c->nearest) {
            c->hit = here;
            c->nearest = dist;
        }
        o += (uint64_t)N * 4 - 4;   // the run is one array: skip past it
    }
    return 0;
}

typedef struct {
    int found;
    uint32_t mask;
    uint64_t nearest;
    uint64_t anchor;
} FavCtx;

// One readable range: a {count, mask} pair with count == 30 and a mask of at most eight bits below 30.
static int fav_in_range(uint64_t at, uint64_t len, void *vctx) {
    FavCtx *c = vctx;
    if (len < 8) return 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)at;
    for (uint64_t o = 0; o + 8 <= len; o += 4) {
        int32_t count;
        uint32_t mask;
        memcpy(&count, p + o, 4);
        memcpy(&mask, p + o + 4, 4);
        if (count != N) continue;
        if (mask & ~((1u << N) - 1u)) continue;   // a bit outside the 30 entries is not this array
        int bits = 0;
        for (uint32_t m = mask; m; m &= m - 1u) bits++;
        if (bits > 8) continue;                   // the game pins at most eight favourites
        const uint64_t here = at + o;
        const uint64_t dist = here > c->anchor ? here - c->anchor : c->anchor - here;
        if (!c->found || dist < c->nearest) {
            c->found = 1;
            c->mask = mask;
            c->nearest = dist;
        }
    }
    return 0;
}

static uint64_t find_index_by_shape(uint64_t anchor, uint64_t radius) {
    if (!anchor) return 0;
    PermCtx c = {0, 0, anchor};
    const uint64_t lo = anchor > radius ? anchor - radius : 0;
    each_readable(lo, anchor + radius, perm_in_range, &c);
    return c.hit;
}

static int find_fav_by_shape(uint64_t anchor, uint32_t *out) {
    if (!anchor || !out) return 0;
    FavCtx c = {0, 0, 0, anchor};
    const uint64_t radius = 64u << 10;
    const uint64_t lo = anchor > radius ? anchor - radius : 0;
    each_readable(lo, anchor + radius, fav_in_range, &c);
    if (!c.found) return 0;
    *out = c.mask;
    return 1;
}

// The position the save's Index array calls 0 (the card drawn first), or -1 when it cannot be read.
//
// Every save slot has its own Index array, and only one of them belongs to the save the game has loaded; the others are
// the player's other files. Reading whichever came first would order the import by a different save's ages, which is
// how an import lands on card 9 instead of card 1. So each candidate read from a file is checked against the guest
// memory: the array the game is using is the one that is actually present there.
static int newest_position_from_save(void) {
    // Once the live Index array has been located it stays put for the process. Re-reading the save file every import
    // would try to match the file's (older) ages against guest memory after an earlier import rotated them, which fails
    // and silently falls back to replacing the first card. Reading the live array directly is both faster and correct
    // for a run of successive imports.
    if (g_ages_at) {
        int32_t live[N];
        if (guest_read(g_ages_at, live, sizeof live)) {
            int newest = -1;
            uint32_t seen = 0;
            int ok = 1;
            for (int i = 0; i < N; i++) {
                if (live[i] < 0 || live[i] >= N || (seen >> live[i] & 1u)) { ok = 0; break; }
                seen |= 1u << live[i];
                if (live[i] == 0) newest = i;
            }
            if (ok && newest >= 0) {
                rt_log("blueprint: Index: cached live array at %#llx, newest entry %d",
                       (unsigned long long)g_ages_at, newest);
                return newest;
            }
        }
        // The address went stale (the save was reloaded): re-locate it below.
        g_ages_at = 0;
        g_fav_bits = 0;
        g_fav_seen = 0;
    }
    // Shape first: the arrays are next to the draft blobs, which the scan already located, so this needs no save file and
    // works whatever the save's ages happen to be.
    {
        const uint64_t anchor = g_parsed ? g_parsed : g_cai;
        const uint64_t at = find_index_by_shape(anchor, 1u << 20);
        if (at) {
            int32_t live[N];
            if (guest_read(at, live, sizeof live)) {
                uint32_t seen = 0;
                int ok = 1, newest = -1;
                for (int i = 0; i < N; i++) {
                    if (live[i] < 0 || live[i] >= N || (seen >> live[i] & 1u)) { ok = 0; break; }
                    seen |= 1u << live[i];
                    if (live[i] == 0) newest = i;
                }
                if (ok && newest >= 0) {
                    g_ages_at = at;
                    if (!g_fav_seen && find_fav_by_shape(at, &g_fav_bits)) g_fav_seen = 1;
                    rt_log("blueprint: Index: history array found by its shape at %#llx, newest entry %d%s",
                           (unsigned long long)at, newest, g_fav_seen ? "" : " (no favourite mask nearby)");
                    return newest;
                }
            }
        }
    }
    const RtConfig *c = rt_config();
    if (!c || !c->profile) return -1;
    uint64_t uid[2] = {0, 0};
    account_uid(uid);
    int best = -1;
    int best_is_live = 0;
    uint64_t best_ages_at = 0;
    uint32_t best_fav_bits = 0;
    int best_fav_seen = 0;
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
        uint32_t fav_bits = 0;
        // Whether the save actually carried the IsFavorite array. Without it fav_bits stays 0, which is indistinguishable
        // from "no entry is a favourite" - and reading it as that would let an import evict a card the player starred.
        // The two cases are told apart so a save this code cannot read the mask from is refused instead of guessed at.
        int fav_seen = 0;
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
                // IsFavorite (0x67f4b46b) is a BoolArray of 30 entries held as {count=30; u32 bitmask}: bit i says the
                // entry i is a favorite. Those entries keep their age and are never the one an insert replaces.
                if (hash == 0x67F4B46Bu && (uint64_t)off + 8 <= (uint64_t)id.size && rd32(d + off) == N) {
                    fav_bits = rd32(d + off + 4);
                    fav_seen = 1;
                    continue;
                }
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
            }
        }
        free(d);
        if (found < 0) continue;
        // Is this array the one the game is holding? The array the page draws by must be in guest memory, and that is
        // what tells this slot's ages apart from the other saves'. Remember where it is: an insert-style import rotates
        // this very array so the change is visible at once and persists with the game's own save.
        const uint64_t live = ages_present_in_guest(ages);
        if (live || !best_is_live) {
            best = found;
            best_is_live = live != 0;
            best_ages_at = live;
            best_fav_bits = fav_bits;
            best_fav_seen = fav_seen;
        }
        if (best_is_live) break;   // the loaded save's own array: no other slot can beat it
    }
    g_ages_at = best_ages_at;
    g_fav_bits = best_fav_bits;
    g_fav_seen = best_fav_seen;
    rt_log("blueprint: Index: newest entry %d, live array %s%#llx, %d favorite(s)%s", best,
           best_ages_at ? "at " : "not in guest memory ", (unsigned long long)best_ages_at,
           __builtin_popcount(best_fav_bits),
           best_fav_seen ? "" : " (the save carries no IsFavorite array)");
    return best;
}

// The history rotation a new creation causes (blueprint_import.h). Kept free of save-memory access so the rule can be
// tested on its own; bp_import_newest applies the result. Returns the evicted entry, or -1 when the input does not fit
// the rule and nothing may be written.
int bp_rotate_ages(const int ages[BP_SLOTS], uint32_t fav_bits, int rotated[BP_SLOTS]) {
    if (!ages || !rotated) return -1;
    // The ages must already be a permutation of 0..N-1; anything else is a save this rule does not describe.
    uint32_t seen = 0;
    for (int i = 0; i < BP_SLOTS; i++) {
        if (ages[i] < 0 || ages[i] >= BP_SLOTS || (seen >> ages[i] & 1u)) return -1;
        seen |= 1u << ages[i];
    }
    // The entry a new creation evicts is the oldest free one: the largest age among the non-favorites.
    int victim = -1, max_free_age = -1;
    for (int i = 0; i < BP_SLOTS; i++) {
        if ((fav_bits >> i) & 1u) continue;
        if (ages[i] > max_free_age) {
            max_free_age = ages[i];
            victim = i;
        }
    }
    if (victim < 0) return -1;   // every entry is a favorite: there is nothing to spend
    // The ages are ranks, so the insert has to leave them a permutation: the new creation takes rank 0, the cards ABOVE the
    // evicted one move down by one, and the cards BELOW it keep their rank. Moving every free card +1 instead is what breaks
    // a save whose favourites sit at the old end: a free card at age 27 becomes 28 and collides with a favourite at 28, the
    // result stops being a permutation, and the import is refused (which is exactly what a 100% save with two favourites
    // did). Favourites keep their rank for the same reason - they are cards, and a card that does not move keeps its rank.
    const int32_t evicted_age = ages[victim];
    for (int i = 0; i < BP_SLOTS; i++) {
        if (i == victim) rotated[i] = 0;
        else rotated[i] = ages[i] < evicted_age ? ages[i] + 1 : ages[i];
    }
    // The result must still be a permutation, or the page would draw an order the save cannot mean.
    seen = 0;
    for (int i = 0; i < BP_SLOTS; i++) {
        if (rotated[i] < 0 || rotated[i] >= BP_SLOTS || (seen >> rotated[i] & 1u)) return -1;
        seen |= 1u << rotated[i];
    }
    return victim;
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

    // The game's own rule for a new creation: it takes the oldest entry that is NOT a favorite, and the favorites keep
    // their age (they are pinned to the oldest ages, up to 8 of them). The free entries' ages then each move down one
    // place, so the card that was first becomes second and the oldest free card is gone. That is the "insert" behaviour
    // the port has to copy, rather than overwriting the first card and destroying it.
    //
    // The Index array is in guest memory (g_ages_at), so rotating it takes effect on the open page at once and is
    // written back by the game's own save. The blob goes into the entry the rotation frees.
    int pos = newest_position_from_save();
    if (pos < 0) {
        rt_log("blueprint: cannot read the Autobuild Index array from the save file, so the import cannot place the "
               "creation first");
        return BP_IMPORT_NO_ORDER;
    }
    int32_t ages[N];
    if (!g_ages_at || !guest_read(g_ages_at, ages, sizeof ages)) {
        rt_log("blueprint: the live Index array is not in guest memory; falling back to replacing the first card");
        g_ages_at = 0;   // no rotation: the legacy replace path below
    }
    // The entry the new blob goes into, and the rotation that makes it the newest. They are worked out together but
    // applied in a fixed order: the blob first, the Index last. The page draws its cards sorted by Index, so the new
    // order must never become visible before the content it points at is in place, and the game must never see the new
    // blob while an old age still marks it as the discarded card. Writing in that order also makes the rotation the
    // last, atomic-looking change, which is what the game notices and redraws from.
    int32_t rotated[N];
    int victim = pos, fav_count = 0;
    int do_rotate = 0;
    if (g_ages_at) {
        for (int i = 0; i < N; i++) fav_count += (g_fav_bits >> i) & 1u;
        // Without the IsFavorite array there is no way to tell a starred card from a free one, and evicting the wrong
        // entry would destroy something the player marked. Refuse the rotation instead: the blob still lands on the
        // newest card, which is the documented fallback, and nothing is dropped.
        if (!g_fav_seen) {
            rt_log("blueprint: the save carries no IsFavorite array, so a starred card cannot be told from a free one; "
                   "keeping the history order and replacing the newest card instead of rotating");
            g_ages_at = 0;
        }
    }
    if (g_ages_at) {
        // The rule lives in bp_rotate_ages so it can be tested without a running game (tests/blueprint_rotation_test.py).
        int rot[N];
        const int evicted = bp_rotate_ages(ages, g_fav_bits, rot);
        if (evicted < 0) {
            if (fav_count >= N)
                rt_log("blueprint: every history entry is a favorite; there is no free entry to replace");
            else
                rt_log("blueprint: the Index array does not have the shape an insert needs; no write made");
            return evicted < 0 && fav_count >= N ? BP_IMPORT_NO_ROOM : BP_IMPORT_NO_ORDER;
        }
        victim = evicted;
        for (int i = 0; i < N; i++) rotated[i] = rot[i];
        do_rotate = 1;
    }
    // The new blob, first: the serialized array (what the game saves) and the parsed copies the page draws from. The
    // parsed failure does not fail the import, because the serialized array is the one that matters for persistence.
    //
    // The entry's old blob is kept until the rotation has been written: the rotation can still fail, and a blob written
    // into an entry whose age was never rotated shows up as some old card having turned into the new creation while the
    // card the rotation meant to spend is gone. Restoring it on failure leaves the history exactly as it was.
    const uint64_t dst = g_cai + (uint64_t)victim * ITEM_STRIDE;
    static uint8_t undo[BPC_BLOB_SIZE];
    const int have_undo = guest_read(dst, undo, BPC_BLOB_SIZE);
    if (!guest_write(dst, bp.blob, BPC_BLOB_SIZE)) {
        rt_log("blueprint: cannot write entry %d at %#llx", victim, (unsigned long long)dst);
        g_cai = 0;
        return BP_IMPORT_NO_ROOM;
    }
    if (g_parsed && victim < g_parsed_slots) {
        uint64_t page = g_parsed + (uint64_t)victim * SLOT_STRIDE;
        guest_write(page, bp.blob, BPC_BLOB_SIZE);
        guest_write(page + BPC_BLOB_SIZE, bp.blob, BPC_BLOB_SIZE);
    }
    // The rotation, last, so it is the change the game notices after the content is in place.
    if (do_rotate) {
        if (!guest_write(g_ages_at, rotated, sizeof rotated)) {
            rt_log("blueprint: cannot rotate the live Index array at %#llx", (unsigned long long)g_ages_at);
            // Put the entry's own blob back, so a rotation that failed leaves no half-import behind.
            if (have_undo && guest_write(dst, undo, BPC_BLOB_SIZE)) {
                if (g_parsed && victim < g_parsed_slots) {
                    uint64_t page = g_parsed + (uint64_t)victim * SLOT_STRIDE;
                    guest_write(page, undo, BPC_BLOB_SIZE);
                    guest_write(page + BPC_BLOB_SIZE, undo, BPC_BLOB_SIZE);
                }
                rt_log("blueprint: entry %d restored; the history is unchanged", victim);
            } else {
                rt_log("blueprint: entry %d could not be restored; it holds the new creation at its old age", victim);
            }
            return BP_IMPORT_NO_ROOM;
        }
        rt_log("blueprint: Index: %d favorite(s), evicted free entry %d, %d free entries moved down one", fav_count,
               victim, N - fav_count - 1);
    }
    rt_log("blueprint: %s (%d parts) is the first draft of the Autobuild history (entry %d)", bp.names[0], bp.actors,
           victim);
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
    case BP_IMPORT_NO_ROOM: return "the save data could not be written in guest memory";
    }
    return "unknown import result";
}
