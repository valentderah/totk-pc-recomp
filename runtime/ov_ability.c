// Recall and Ultrahand target check (patch set "ability", recomp/patches-v143.txt and patches-v121.txt).
//
// main+0x1bd0138 on 1.4.3 (main+0x1622b60 on 1.2.1, the same code and actor offsets) runs while an ability picks its
// target. Once the owner's request says ready, it loads the result holder from the owner (x8 = [x19+0x6b0]), the
// target actor from the holder (x21 = [x8]), links the actor (main+0x155ec: reads the actor's +0x10, +0x24 and
// +0x1a8), reads [[x21+0x228]+0xb0] (main+0x9a89d4) and takes a reference on that (main+0x2b8464: an atomic
// read-modify-write of the word at +0x90). The holder can be reused memory by then, and the game crashes reading
// 0xffffffffffffffff (Windows' report of a non-canonical address) minutes to hours into play. UltraTears
// (kuraeow/ultratears, "ability target guard") found the class; this code is ours.
//
// A target is dropped only where the game's own code would fault on it: the holder or the actor unreadable, a link
// record the game follows unreadable or read-only, the component pointer unreadable, or the reference word
// unreadable or in a module's code or read-only data. Every
// other target goes on exactly as the game reads it, so no target the game can handle is ever lost. UltraTears' test
// of a live actor (its link record [actor+0x1a8] points back to it at +0x40 and carries its id at +0x48) is not in the
// game's code: the game links an actor whose link record is null without complaint. It runs here as a log line only,
// to show in players' logs whether it would ever reject a live target.
//
// The first site replaces `ldr x21, [x8]` with a call here; the second, at the game's `cbz x21` right after it, becomes
// `mov x21, x9; cbz x21`. The answer travels in x9 because the lifted caller keeps x21 (callee-saved) in a C local
// across the call and reloads only caller-saved registers; x9 is dead there on every path. x9 gets the holder's first
// word, or 0 for a dropped target, which the cbz skips for this frame as it skips an empty one. A null or tagged
// (bit 0) word goes back unchanged: the game skips those itself. No other register changes.
// Only an ability's target pick reaches this (hundreds of calls an hour), so the memory queries cost nothing.
#include <stdint.h>

#include "host.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

#ifdef RT_PATCH_PAGE

#define HOST_ABILITY_TARGET (RT_PATCH_PAGE + 0xf60)
#define ACTOR_SPAN 0x230   // the actor fields the function and its callees read: +0x10 .. +0x22f

static int readable(uint64_t a, uint64_t n) { return a && !(a & 3) && mem_guest_readable(a, n); }

static uint64_t u64_at(uint64_t a) { return *(volatile uint64_t *)(uintptr_t)a; }
static uint32_t u32_at(uint64_t a) { return *(volatile uint32_t *)(uintptr_t)a; }

// [a, a+4) inside a module's code or read-only data: a write there faults.
static int read_only_module(uint64_t a) {
    for (int i = 0; i < ldr_count(); i++) {
        const RtLoaded *m = ldr_module(i);
        if (a - m->base < m->size) return a - m->base < m->data_off;
    }
    return 0;
}

// NULL when the game's code can follow the actor without a fault, else what would fault.
static const char *faults(uint64_t actor) {
    if (!readable(actor, ACTOR_SPAN)) return "actor not readable";
    // main+0x155ec (1.2.1: main+0x809ecc) follows a non-null link record unless the actor's state word +0x24 is 7..11:
    // it stores the record in the owner, updates +0x50 atomically (main+0x15838) and later compares +0x40 and +0x48.
    uint64_t link = u64_at(actor + 0x1a8);
    if (link && u32_at(actor + 0x24) - 7 >= 5 &&
        (!readable(link + 0x40, 0x18) || read_only_module(link + 0x50)))
        return "link record not writable";
    uint64_t comp = u64_at(actor + 0x228);
    if (!comp) return NULL;   // main+0x9a89d4 returns 0 for it and the game stops there
    if (!readable(comp + 0xb0, 8)) return "component not readable";
    uint64_t ref = u64_at(comp + 0xb0);
    if (!ref) return NULL;    // the game's cbz x0 skips the reference
    if (!readable(ref + 0x90, 4) || read_only_module(ref + 0x90)) return "reference word not writable";
    return NULL;
}

// UltraTears' live-actor test, for the log: NULL when it passes.
static const char *not_live(uint64_t actor) {
    uint64_t link = u64_at(actor + 0x1a8);
    if (!readable(link + 0x40, 0x10)) return "link record not readable";
    if (u64_at(link + 0x40) != actor) return "link record points elsewhere";
    if (u32_at(link + 0x48) != u32_at(actor + 0x10)) return "link record has another id";
    return NULL;
}

static void host_ability_target(Cpu *c) {
    static uint64_t checked, dropped, mismatched;
    uint64_t holder = c->x[8], actor = 0;
    const char *why = NULL;
    if (!readable(holder, 8)) why = "holder not readable";
    else if ((actor = u64_at(holder)) && !(actor & 1)) why = faults(actor);
    if (why) {
        c->x[9] = 0;
        uint64_t n = __atomic_add_fetch(&dropped, 1, __ATOMIC_RELAXED);
        if (n <= 20 || !(n & (n - 1)))
            rt_log("ability: target dropped, the game would fault on it (%s): holder %#llx actor %#llx; %llu dropped",
                   why, (unsigned long long)holder, (unsigned long long)actor, (unsigned long long)n);
        return;
    }
    c->x[9] = actor;
    if (!actor || (actor & 1)) return;
    uint64_t k = __atomic_add_fetch(&checked, 1, __ATOMIC_RELAXED);
    const char *odd = not_live(actor);
    if (!odd) {
        if (k == 1) rt_log("ability: first target checked (TOTK_ABILITY_GUARD=0: no check)");
        return;
    }
    uint64_t n = __atomic_add_fetch(&mismatched, 1, __ATOMIC_RELAXED);
    if (n <= 20 || !(n & (n - 1)))
        rt_log("ability: target kept although the live-actor test fails (%s): actor %#llx; %llu of %llu targets", odd,
               (unsigned long long)actor, (unsigned long long)n, (unsigned long long)k);
}

__attribute__((constructor)) static void ability_register(void) {
    rt_register_host(HOST_ABILITY_TARGET, host_ability_target);
}

#endif
