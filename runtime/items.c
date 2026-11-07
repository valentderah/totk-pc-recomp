// Actor creation through the lifted guest function. Address/layout leads: kreptiliri/lonk d321a19c (MIT),
// docs/how-it-works.md and patches/lonk.patch. Resident Player lookup follows MacNativeGames world_survey.c
// at b75be8cf (MIT). No ARM-native function pointers are called by this x86-64 runtime.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "host.h"
#include "layout.h"
#include "gfx.h"
#include "cfg.h"
#include "items.h"
#include "platform/platform.h"
#include "ui/menu.h"

static PlatMutex lock = PLAT_MUTEX_INIT;
static char request[96], message[256] = "Choose an item.";
static _Atomic int remaining;
static _Atomic uint64_t system_object;
static uint64_t preactor;
static _Atomic unsigned watch;
static _Thread_local int inside;
static _Atomic uint64_t grant_manager;
static _Atomic int inventory_request;
static uint64_t inventory_actor;
static void status(const char *text);
int items_busy(void) { return remaining || watch; }

static uint64_t ptr(uint64_t at) { uint64_t value = 0; if (at) plat_mem_read(at, &value, 8); return value; }
static void status(const char *text) { plat_mutex_lock(&lock); snprintf(message, sizeof message, "%s", text); plat_mutex_unlock(&lock); }
void items_status(char *out, size_t cap) { plat_mutex_lock(&lock); snprintf(out, cap, "%s", message); plat_mutex_unlock(&lock); }
void items_capture_grant(Cpu *c) {
    if (rt_game_version != 143 || !c->x[0] || !c->x[1] ||
        (c->x[30] != MOD_MAIN_BASE + 0x1bd7df4 && c->x[30] != MOD_MAIN_BASE + 0x1bd7ea8)) return;
    uint64_t previous = atomic_exchange(&grant_manager, c->x[0]);
    if (previous != c->x[0])
        rt_log("items: captured pickup grant manager %#llx", (unsigned long long)c->x[0]);
}
int items_inventory_ready(void) { return rt_game_version == 143 && atomic_load(&grant_manager) != 0; }
int items_supported(void) { return rt_game_version == 143; }
int items_request(const char *actor, int count) {
    if (rt_game_version != 143 || !actor || !*actor || strlen(actor) >= sizeof request || count < 1 || count > 10) return 0;
    for (const char *p = actor; *p; ++p) if (!isalnum((unsigned char)*p) && *p != '_') return 0;
    // System actors require bespoke lifecycle handling; never permit a duplicate Player through this interface.
    if (!strcmp(actor, "Player") || !strcmp(actor, "PouchPlayer")) return 0;
    plat_mutex_lock(&lock);
    if (remaining || watch) { plat_mutex_unlock(&lock); return 0; }
    strcpy(request, actor);
    atomic_store(&inventory_request, 0);
    remaining = count;
    snprintf(message, sizeof message, "Spawning %d x %s when play resumes.", count, actor);
    plat_mutex_unlock(&lock);
    rt_log("items: queued %d x %s", count, actor);
    return 1;
}

int items_inventory_request(const char *actor) {
    if (!items_inventory_ready() || !actor || !*actor || strlen(actor) >= sizeof request) return 0;
    for (const char *p = actor; *p; ++p) if (!isalnum((unsigned char)*p) && *p != '_') return 0;
    if (!strcmp(actor, "Player") || !strcmp(actor, "PouchPlayer")) return 0;
    plat_mutex_lock(&lock);
    if (remaining || watch) { plat_mutex_unlock(&lock); return 0; }
    strcpy(request, actor);
    inventory_actor = 0;
    atomic_store(&inventory_request, 1);
    remaining = 1;
    snprintf(message, sizeof message, "Adding %s when play resumes.", actor);
    plat_mutex_unlock(&lock);
    rt_log("items: queued live inventory grant for %s", actor);
    return 1;
}

// The present-interval controller's caller saves the game system pointer in its parent frame.
// Use guest frame pointers, not the host's x86-64 call stack.
void items_capture_frame(Cpu *c) {
    if (rt_game_version != 143 || system_object) return;
    static unsigned debug;
    if (cfg_get("TOTK_ITEM_TEST") && debug++ < 4)
        rt_log("items: interval caller lr %#llx fp %#llx", (unsigned long long)c->x[30], (unsigned long long)c->x[29]);
    if (c->x[30] == MOD_MAIN_BASE + 0x2db2ec) {
        system_object = ptr(c->x[29] + 0x38);
        if (system_object) rt_log("items: captured game frame system %#llx", (unsigned long long)system_object);
        return;
    }
    uint64_t fp = c->x[29];
    for (int i = 0; i < 24 && fp && !(fp & 7); ++i) {
        uint64_t parent = ptr(fp), ret = ptr(fp + 8);
        if (ret == MOD_MAIN_BASE + 0x2db2ec) {
            system_object = ptr(parent + 0x38);
            if (system_object) rt_log("items: captured game frame system %#llx", (unsigned long long)system_object);
            return;
        }
        if (parent <= fp || parent - fp > 0x100000) return;
        fp = parent;
    }
}

static uint64_t player(void) {
    uint64_t module = ptr(MOD_MAIN_BASE + 0x3add238), scene = ptr(module ? module + 0x1e8 : 0);
    int32_t count = 0;
    if (!scene || !plat_mem_read(scene + 0x50, &count, 4) || count < 14 || count > 256) return 0;
    uint64_t components = ptr(scene + 0x58), resident = ptr(components ? components + 13 * 8 : 0);
    if (!resident || !plat_mem_read(resident + 0x20, &count, 4) || count < 1 || count > 64) return 0;
    uint64_t links = ptr(resident + 0x28);
    for (int i = 0; links && i < count; ++i) {
        uint64_t data = ptr(links + i * 0x70 + 8), actor = ptr(data ? data + 0x40 : 0);
        char name[7];
        uint64_t text = ptr(actor ? actor + 0x218 : 0);
        if (text && plat_mem_read(text, name, sizeof name) && !memcmp(name, "Player", sizeof name)) {
            float xyz[3];
            if (plat_mem_read(actor + 0x2b4, xyz, sizeof xyz) && isfinite(xyz[0]) && isfinite(xyz[1]) &&
                isfinite(xyz[2]) && fabsf(xyz[0]) + fabsf(xyz[2]) > 1) return actor;
        }
    }
    return 0;
}

// Persistent guest-visible arguments outlive the asynchronous creation request. Only one request is outstanding.
typedef struct {
    uint8_t arg[0x98];
    char name[96], empty[8];
    uint64_t safe_string, pre;
    uint32_t result;
} Scratch;
static Scratch *scratch;

static void spawn(Cpu *c, uint64_t actor) {
    const uint32_t expected[] = {0xa9ba7bfd, 0xa9016ffc, 0x910003fd, 0xa90267fa, 0xa9035ff8,
                                0xa90457f6, 0xa9054ff4, 0xd10ac3ff, 0xf94037a9, 0x39438408};
    uint32_t actual[10];
    uint64_t mgr = ptr(MOD_MAIN_BASE + 0x3ad7450);
    if (!mgr || !plat_mem_read(MOD_MAIN_BASE + 0x599670, actual, sizeof actual) || memcmp(actual, expected, sizeof actual)) {
        status("Spawn unavailable: game build or actor manager does not match.");
        remaining = 0;
        return;
    }
    float pos[3], rot[9];
    if (!plat_mem_read(actor + 0x2b4, pos, sizeof pos) || !plat_mem_read(actor + 0x2c0, rot, sizeof rot)) return;
    if (!isfinite(pos[0]) || !isfinite(pos[1]) || !isfinite(pos[2])) return;
    if (!scratch) scratch = mem_kernel_alloc(sizeof *scratch, 16);
    if (!scratch) { status("Could not allocate spawn arguments."); remaining = 0; return; }
    memset(scratch, 0, sizeof *scratch);
    strcpy(scratch->name, request);
    float len = hypotf(rot[2], rot[8]);
    float fx = len > 0.1f && isfinite(len) ? rot[2] / len : 0, fz = len > 0.1f && isfinite(len) ? rot[8] / len : 1;
    if (atomic_load(&inventory_request)) pos[1] -= 20.0f;
    else { pos[0] += 2 * fx; pos[1] += 1.5f; pos[2] += 2 * fz; }
    memcpy(scratch->arg, pos, sizeof pos);
    const float rotation[9] = {1,0,0,0,1,0,0,0,1}, scale[3] = {1,1,1};
    memcpy(scratch->arg + 0x0c, rotation, sizeof rotation);
    memcpy(scratch->arg + 0x30, scale, sizeof scale);
    uint64_t empty = (uintptr_t)scratch->empty;
    memcpy(scratch->arg + 0x40, &empty, 8);
    scratch->arg[0x90] = 7;
    scratch->safe_string = (uintptr_t)scratch->name;
    scratch->result = ~0u;
    Cpu saved = *c;
    c->sp = (c->sp - 0x40) & ~15ull;
    ST64(c->sp, 0); ST64(c->sp + 8, (uintptr_t)&scratch->result); ST64(c->sp + 16, (uintptr_t)&scratch->pre);
    rt_log("items: creating %s at %.2f %.2f %.2f", request, pos[0], pos[1], pos[2]);
    uint64_t ok = rt_call(c, MOD_MAIN_BASE + 0x599670, 8, mgr, (uint64_t)(uintptr_t)&scratch->safe_string,
                          (uint64_t)(uintptr_t)scratch->arg, (uint64_t)0, (uint64_t)1, (uint64_t)0, (uint64_t)0, (uint64_t)0);
    *c = saved;
    rt_fpcr_sync(c);
    rt_log("items: request returned %llu result %u preactor %#llx", (unsigned long long)ok, scratch->result,
           (unsigned long long)scratch->pre);
    if (ok && scratch->pre) { preactor = scratch->pre; watch = 300; status("Creating item..."); }
    else { remaining = 0; status("Game could not create this item. Remaining copies were cancelled."); }
}

// Use the grant and actor cleanup calls from the game's normal pickup path.
static void grant_spawned_item(Cpu *c, uint64_t actor, uint64_t component) {
    uint64_t manager = atomic_load(&grant_manager);
    char name[96] = {0};
    uint64_t text = ptr(actor + 0x218);
    if (!manager || !text || !plat_mem_read(text, name, sizeof name) || strcmp(name, request)) {
        status("Could not add the item. Pick up an item and try again.");
        remaining = 0;
        return;
    }
    Cpu saved = *c;
    c->sp = (c->sp - 0x40) & ~15ull;
    uint64_t ok = rt_call(c, MOD_MAIN_BASE + 0x209ce1c, 4, manager, actor, (uint64_t)0, (uint64_t)0);
    uint64_t cleanup = ~0ull;
    if (ok) cleanup = rt_call(c, MOD_MAIN_BASE + 0x1b4b738, 4,
                              component, (uint64_t)7, (uint64_t)0, (uint64_t)0);
    *c = saved;
    rt_fpcr_sync(c);
    rt_log("items: live inventory grant actor %s manager %#llx result %llu pickup cleanup %llu",
           request, (unsigned long long)manager, (unsigned long long)ok, (unsigned long long)cleanup);
    status(ok ? "Item added to inventory." : "Could not add the item. It remains near Link.");
    remaining = 0;
}

void items_sync_tick(Cpu *c, uint64_t sync) {
    if (inside || rt_game_version != 143 || !system_object) return;
    uint64_t wait = ptr(system_object + 0x150);
    if (!wait || ptr(wait + 0x78) != sync) return;
    static int reached;
    if (!reached) { reached = 1; rt_log("items: reached game frame-end callback"); }
    if (atomic_load(&grant_manager) && !player()) atomic_store(&grant_manager, 0);
    static int test;
    if (!test && cfg_get("TOTK_ITEM_TEST") && frame_step_world_us() >= 2000000 && player()) {
        const char *count = cfg_get("TOTK_ITEM_TEST_COUNT");
        test = 1; items_request(cfg_get("TOTK_ITEM_TEST"), count ? atoi(count) : 1);
    }
    if (menu_is_open()) return;
    inside = 1;
    if (watch) {
        if (atomic_load(&inventory_request) && inventory_actor) {
            uint64_t actor_state = ptr(inventory_actor + 0x228);
            uint64_t component = ptr(actor_state ? actor_state + 0x1c8 : 0);
            if (component) {
                grant_spawned_item(c, inventory_actor, component);
                inventory_actor = 0;
                watch = 0;
            }
        } else {
            for (unsigned off = 0; off <= 0x100; off += 8) {
                uint64_t actor = ptr(preactor + off), name = ptr(actor ? actor + 0x218 : 0);
                char text[96] = {0};
                if (name && plat_mem_read(name, text, strlen(request) + 1) && !strcmp(text, request)) {
                    float xyz[3] = {0}; plat_mem_read(actor + 0x2b4, xyz, sizeof xyz);
                    rt_log("items: spawned %s actor %#llx at %.2f %.2f %.2f", request, (unsigned long long)actor, xyz[0], xyz[1], xyz[2]);
                    if (atomic_load(&inventory_request)) {
                        uint64_t actor_state = ptr(actor + 0x228);
                        uint64_t component = ptr(actor_state ? actor_state + 0x1c8 : 0);
                        if (component) {
                            grant_spawned_item(c, actor, component);
                            watch = 0;
                        } else {
                            inventory_actor = actor;
                            status("Adding item to inventory...");
                        }
                    } else {
                        char line[256]; snprintf(line, sizeof line, "Spawned %s near Link.", request); status(line);
                        plat_mutex_lock(&lock); watch = 0; --remaining; plat_mutex_unlock(&lock);
                    }
                    break;
                }
            }
        }
        if (watch && !--watch) { remaining = 0; status("Creation timed out; no actor completion was confirmed."); }
    } else {
        plat_mutex_lock(&lock); int pending = remaining; plat_mutex_unlock(&lock);
        if (pending) { uint64_t actor = player(); if (actor) spawn(c, actor); else status("Waiting for Link. Resume a playable world first."); }
    }
    inside = 0;
}
