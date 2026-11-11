// Recall and Ultrahand target check (patch set "ability", recomp/patches-v143.txt and patches-v121.txt).
//
// main+0x1bd0138 on 1.4.3 (main+0x1622b60 on 1.2.1, the same code and actor offsets) runs while an ability picks its
// target. Once the owner's request says ready, it loads the result holder from the owner (x8 = [x19+0x6b0]), the
// target actor from the holder (x21 = [x8]), links the actor (main+0x155ec), reads [[x21+0x228]+0xb0]
// (main+0x9a89d4) and takes a reference on that (main+0x2b8464, an atomic write at +0x90).
// The holder can be reused memory by then: the actor pointer lands in .rodata, past the address space or on heap
// memory that holds other data, and the game crashes reading 0xffffffffffffffff (Windows' report of a non-canonical
// address) minutes to hours into play. UltraTears (kuraeow/ultratears, "ability target guard") found the class and
// the test below; this code is ours.
//
// The first site replaces `ldr x21, [x8]` with a call here; the second, at the game's `cbz x21` right after it, becomes
// `mov x21, x9; cbz x21`. The answer travels in x9 because the lifted caller keeps x21 (callee-saved) in a C local
// across the call and reloads only caller-saved registers; x9 is dead there on every path. x9 gets the holder's first
// word when the holder is readable, and 0 when the holder is unreadable or the actor fails the live-actor test: the
// actor's link record [actor+0x1a8] points back to it (+0x40) and carries its id (+0x48 == [actor+0x10]), and every
// pointer the game then follows is readable. The cbz skips a 0 target for this frame, as it skips an empty one. A null
// or tagged (bit 0) word goes back unchanged: the game skips those itself. No other register changes.
// Only an ability's target pick reaches this (hundreds of calls an hour), so the memory queries cost nothing.
#include <stdint.h>

#include "host.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

#ifdef RT_PATCH_PAGE

#define HOST_ABILITY_TARGET (RT_PATCH_PAGE + 0xf60)

// 0 when [a, a+8) cannot be read.
static int rd64(uint64_t a, uint64_t *v) {
    *v = 0;
    if (!a || (a & 7) || !mem_guest_readable(a, 8)) return 0;
    *v = *(volatile uint64_t *)(uintptr_t)a;
    return 1;
}

static int rd32(uint64_t a, uint32_t *v) {
    *v = 0;
    if (!a || (a & 3) || !mem_guest_readable(a, 4)) return 0;
    *v = *(volatile uint32_t *)(uintptr_t)a;
    return 1;
}

// NULL when the actor passes, else why it does not.
static const char *stale(uint64_t actor) {
    uint64_t link, back, comp, sub, ref;
    uint32_t link_id, actor_id;
    if (!rd64(actor + 0x1a8, &link) || !rd32(actor + 0x10, &actor_id)) return "actor not readable";
    if (!rd64(link + 0x40, &back)) return "link record not readable";
    if (back != actor) return "link record points elsewhere";
    if (!rd32(link + 0x48, &link_id) || link_id != actor_id) return "link record has another id";
    if (!rd64(actor + 0x228, &comp)) return "actor not readable";
    if (!comp) return NULL;   // main+0x9a89d4 returns 0 for it and the game stops there
    if (!rd64(comp + 0xb0, &sub)) return "component not readable";
    if (sub && !rd64(sub + 0x90, &ref)) return "component target not readable";
    return NULL;
}

static void host_ability_target(Cpu *c) {
    static uint64_t checked, skipped;
    uint64_t holder = c->x[8], actor = 0;
    const char *why = NULL;
    if (!rd64(holder, &actor)) why = "holder not readable";
    else if (actor && !(actor & 1)) why = stale(actor);
    if (!why) {
        c->x[9] = actor;
        if (actor && !(actor & 1) && __atomic_add_fetch(&checked, 1, __ATOMIC_RELAXED) == 1)
            rt_log("ability: first target checked, live (TOTK_ABILITY_GUARD=0: no check)");
        return;
    }
    c->x[9] = 0;
    uint64_t n = __atomic_add_fetch(&skipped, 1, __ATOMIC_RELAXED);
    if (n <= 20 || !(n & (n - 1)))
        rt_log("ability: stale target skipped (%s): holder %#llx actor %#llx; %llu skipped", why,
               (unsigned long long)holder, (unsigned long long)actor, (unsigned long long)n);
}

__attribute__((constructor)) static void ability_register(void) {
    rt_register_host(HOST_ABILITY_TARGET, host_ability_target);
}

#endif
