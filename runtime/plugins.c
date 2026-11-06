// Code modules of mods that the port runs as its own code: the handlers of the host hooks tools/plugins.py makes from
// them (recomp/patches.py `hook` lines; patches.c binds them through rt_hook_bind).
//
// A mod's exefs/subsdk<n> plugin is loaded into the game's process on the Switch and patches main's code from its own
// start-up. Code lifted ahead of time cannot be patched while it runs, and the port loads no such module (mods.c).
// For a plugin build it knows, tools/plugins.py turns the plugin's hooks into hook sites of the lifted game, and the
// handlers below do what the plugin's callbacks do, on the guest state the plugin would see there.
//
// exlaunch inline hooks. The plugins are built on exlaunch. An inline hook replaces the hooked instruction with a
// branch to an entry stub, which stores x30 at sp - 0x10 and calls exlaunch's save routine. That routine stores the
// registers into a context on the stack below the hooked code's sp, calls the plugin's callback with the context's
// address in x0 and sp at the context, and loads the registers back from it, so the callback's changes take effect.
// The stub then loads x30 from the context and runs the hooked instruction, and the code after it continues. Each
// exlaunch build has its own context layout (exl_ctx). The flags are not in the context, so exlaunch leaves the flags
// the callback's own code set; here they stay as the hooked code had them, the only value that code can rely on.
//
// MSFix: the plugin of the mod The Second Quest (GameBanana 479009). It ships in two builds: one in the mod's 1.2.1
// package, for game versions 1.0.0 to 1.2.1, and one in the 1.4.3 package, for 1.0.0 to 1.4.3. Each build puts seven
// inline hooks into main, at addresses its own tables give per game version (tools/plugins.py has those addresses), and
// runs one of four callbacks at each:
//   A, at two sites: calls a game function (fn1) with X0, the address of a word holding -1, X2 and 0xee199c3a.
//   B, at three sites: sets MSFix's timer to 90.
//   C, at one site: when the object X19 refers to is the Master Sword (named "Weapon_Sword_070"), stores the low word
//      of X8 into a structure that object reaches through three pointers.
//   D, at one site: while the timer is above 0, advances it with the game's timer step (fn2) and replaces a float
//      register with the float at X19 + 0x74; at or below 0, clears it.
// The 1.2.1 package's build saves its context with exl_inline_hook_with_fp_impl at every hook. The 1.4.3 package's
// build uses exl_inline_hook_impl for A to C and exl_inline_float_hook_impl for D. The two builds' D differ (msfix1_d,
// msfix2_d).
// The handlers follow each build's callbacks instruction by instruction where the game can see the difference: the
// arguments and registers a game function gets, the frame below the context, the timer's words and the context's
// registers. The hooks of both builds take three arguments (tools/plugins.py): fn1, fn2 and MSFix's index of the game
// version (1.0.0 = 0, 1.1.0 = 1, 1.2.1 = 5, 1.4.3 = 9).
#include <stdio.h>
#include <string.h>

#include "host.h"

// ---- exlaunch inline hooks

enum { EXL_INT, EXL_FLOAT, EXL_WITH_FP };

// The context of each exlaunch save routine: its size below the hooked code's sp and the offset of X0 in it. X0-X29
// follow one another and X30 comes next, at sp - 0x10, where the entry stub stored it; the 8 bytes above stay as they
// were.
static const struct { uint64_t size, x; } exl_ctx[] = {
    [EXL_INT] = {0x100, 0},          // exl_inline_hook_impl: X0-X30
    [EXL_FLOAT] = {0x300, 0x200},    // exl_inline_float_hook_impl: Q0-Q31, then X0-X30
    [EXL_WITH_FP] = {0x180, 0x80},   // exl_inline_hook_with_fp_impl: D0-D15 (the low halves of V0-V15), then X0-X30
};

// A callback: ctx is the context's address, x the address of X0 in it, state the plugin's data (exl_hook).
typedef void (*ExlCallback)(Cpu *c, uint64_t ctx, uint64_t x, const RtHook *h, void *state);

// One exlaunch inline hook: the context written below sp, the callback run with sp at the context, then the registers
// the save routine loads back. Game code the callback calls leaves its changes in the registers the routine does not
// load back: V0-V31 for EXL_INT, V16-V31 for EXL_WITH_FP, whose D loads also clear the top halves of V0-V15.
static void exl_inline(Cpu *c, int kind, ExlCallback cb, const RtHook *h, void *state) {
    uint64_t sp = c->sp, nzcv = c->nzcv, ctx = sp - exl_ctx[kind].size, x = ctx + exl_ctx[kind].x;
    for (int i = 0; i < 31; i++) ST64(x + 8 * (uint64_t)i, c->x[i]);
    if (kind == EXL_FLOAT) memcpy((void *)(uintptr_t)ctx, c->v, sizeof c->v);
    if (kind == EXL_WITH_FP)
        for (int i = 0; i < 16; i++) ST64(ctx + 8 * (uint64_t)i, (uint64_t)c->v[i]);
    c->sp = ctx;
    cb(c, ctx, x, h, state);
    for (int i = 0; i < 31; i++) c->x[i] = LD64(x + 8 * (uint64_t)i);
    if (kind == EXL_FLOAT) memcpy(c->v, (const void *)(uintptr_t)ctx, sizeof c->v);
    if (kind == EXL_WITH_FP)
        for (int i = 0; i < 16; i++) c->v[i] = LD64(ctx + 8 * (uint64_t)i);
    c->sp = sp;
    c->nzcv = nzcv;
}

// The frame of 0x20 bytes a callback that calls game code opens below sp (stp x29, x30, [sp, #-0x20]!; mov x29, sp):
// x29 and x30 stored at its bottom, x29 and sp pointing at it. Returns its address; the callback sets sp back to
// address + 0x20.
static uint64_t exl_frame(Cpu *c) {
    uint64_t fp = c->sp - 0x20;
    ST64(fp, c->x[29]);
    ST64(fp + 8, c->x[30]);
    c->sp = c->x[29] = fp;
    return fp;
}

// ---- MSFix

// MSFix's four words after its timer, as both builds lay them out (the 1.2.1 package's build at subsdk9+0x12518, the
// 1.4.3 package's at subsdk9+0x134b0): the time left, which the game's timer step advances; a second word B sets to 90
// with it and D clears with it; -1.0, from the plugin's static initialiser; MSFix's index of the game version. D hands
// the timer's address to the game's timer step, so it lies in guest memory (rt_hook_bind).
typedef struct {
    float left, set, rate;
    int32_t version;
} MsfixTimer;

static uint32_t float_bits(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

// A (1.2.1 package's subsdk9+0x5104, 1.4.3 package's +0x66f0): fn1 (argument 0) with X0, the address of a word of the
// callback's frame holding -1, X2 and 0xee199c3a. Its result is not used.
static void msfix_a(Cpu *c, uint64_t ctx, uint64_t x, const RtHook *h, void *state) {
    (void)ctx;
    (void)state;
    uint64_t fp = exl_frame(c);
    ST32(fp + 0x1c, 0xFFFFFFFFu);
    rt_call(c, h->args[0], 4, LD64(x), fp + 0x1c, LD64(x + 16), (uint64_t)0xEE199C3Au);
    c->sp = fp + 0x20;
}

// B (+0x513c, +0x6730): the time left and the second word to 90.
static void msfix_b(Cpu *c, uint64_t ctx, uint64_t x, const RtHook *h, void *state) {
    (void)c;
    (void)ctx;
    (void)x;
    (void)h;
    MsfixTimer *t = state;
    t->left = t->set = 90.0f;
}

// C (+0x5150, +0x6750): p = [X19 + 0x18], 8 bytes lower on 1.0.0 (index 0). When the C string at [p + 0x218] is
// "Weapon_Sword_070", the low word of X8 goes to [[[p + 0x228] + 0x2f8] + 0x1840] + 8. A null p, a null pointer on the
// way or a null name (where the plugin would read address 0) ends it.
static void msfix_c(Cpu *c, uint64_t ctx, uint64_t x, const RtHook *h, void *state) {
    (void)c;
    (void)ctx;
    (void)h;
    const MsfixTimer *t = state;
    uint64_t p = LD64(LD64(x + 8 * 19) + 0x18);
    if (!p) return;
    if (!t->version) p -= 8;
    const char *name = (const char *)(uintptr_t)LD64(p + 0x218);
    if (!name || strcmp(name, "Weapon_Sword_070")) return;
    uint64_t q = LD64(p + 0x228);
    if (q) q = LD64(q + 0x2f8);
    if (q) q = LD64(q + 0x1840);
    if (q) ST32(q + 8, (uint32_t)LD64(x + 8 * 8));
}

// D of the 1.2.1 package's build (+0x51bc, context with D0-D15): V0 takes the time left (an S load clears the rest of
// the register). At or below 0 (fcmpe, b.ls: NaN goes on) the time left and the second word are cleared. Otherwise fn2
// (argument 1) gets the timer's address in x0, its own address in x1 and the time left in s0, and then S0 of the
// context, the low word of the D0 slot, takes the float at X19 + 0x74.
static void msfix1_d(Cpu *c, uint64_t ctx, uint64_t x, const RtHook *h, void *state) {
    MsfixTimer *t = state;
    uint64_t fp = exl_frame(c);
    c->v[0] = float_bits(t->left);
    if (t->left <= 0.0f) {
        t->left = t->set = 0.0f;
    } else {
        rt_call(c, h->args[1], 2, (uint64_t)(uintptr_t)t, h->args[1]);
        ST32(ctx, LD32(LD64(x + 8 * 19) + 0x74));
    }
    c->sp = fp + 0x20;
}

// D of the 1.4.3 package's build (+0x67d0, context with Q0-Q31): V31 takes the time left. At or below 0 the time left
// and the second word are cleared. Otherwise fn2 gets the timer's address in x0 and the context's in x1 (s0 is the
// hooked code's), and then the float at X19 + 0x74 goes to the context's S1 on game versions up to 1.2.1 (index 5 and
// below), to its S0 on later ones.
static void msfix2_d(Cpu *c, uint64_t ctx, uint64_t x, const RtHook *h, void *state) {
    MsfixTimer *t = state;
    c->v[31] = float_bits(t->left);
    if (t->left <= 0.0f) {
        t->left = t->set = 0.0f;
        return;
    }
    uint64_t fp = exl_frame(c);
    ST64(fp + 0x18, ctx);
    rt_call(c, h->args[1], 2, (uint64_t)(uintptr_t)t, ctx);
    ST32(ctx + (t->version <= 5 ? 16 : 0), LD32(LD64(x + 8 * 19) + 0x74));
    c->sp = fp + 0x20;
}

// ---- handlers

enum { MSFIX_121, MSFIX_143, PLUGINS };   // the plugin builds, each with its own state

// The builds, by the SHA-256 of their module file, as tools/plugins.py KNOWN lists them: mods.c tells a known build the
// game was built without (it waits for the rebuild) from a module the port cannot run.
static const struct { const char *sha256, *name; } builds[PLUGINS] = {
    [MSFIX_121] = {"0e2d3b75dfa135155c8ab0da1dcbcf2da74ffd5455434214e214e0511a6758da",
                   "MSFix of The Second Quest 1.2.1"},
    [MSFIX_143] = {"1490a83ee46e33fe1937cc1d5a70dacc6f1732cd5b9b1e0d4460b028c26eb921",
                   "MSFix of The Second Quest 1.4.3"},
};

static char probe_sha[65], probe_name[64];   // plugins_probe_known

const char *plugins_known(const uint8_t sha256[32]) {
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", sha256[i]);
    for (int i = 0; i < PLUGINS; i++)
        if (!strcmp(builds[i].sha256, hex)) return builds[i].name;
    return probe_sha[0] && !strcmp(probe_sha, hex) ? probe_name : NULL;
}

void plugins_probe_known(const char *sha256, const char *name) {
    snprintf(probe_sha, sizeof probe_sha, "%s", sha256 ? sha256 : "");
    snprintf(probe_name, sizeof probe_name, "%s", name ? name : "");
}

typedef struct {
    const char *name;
    ExlCallback cb;
    int kind, plugin;
} ExlHandler;

static const ExlHandler handlers[] = {
    {"msfix1_a", msfix_a, EXL_WITH_FP, MSFIX_121}, {"msfix1_b", msfix_b, EXL_WITH_FP, MSFIX_121},
    {"msfix1_c", msfix_c, EXL_WITH_FP, MSFIX_121}, {"msfix1_d", msfix1_d, EXL_WITH_FP, MSFIX_121},
    {"msfix2_a", msfix_a, EXL_INT, MSFIX_143},     {"msfix2_b", msfix_b, EXL_INT, MSFIX_143},
    {"msfix2_c", msfix_c, EXL_INT, MSFIX_143},     {"msfix2_d", msfix2_d, EXL_FLOAT, MSFIX_143},
};

static MsfixTimer *timers[PLUGINS];

static void exl_hook(Cpu *c, const RtHook *h) {
    const ExlHandler *e = h->data;
    exl_inline(c, e->kind, e->cb, h, timers[e->plugin]);
}

int rt_hook_bind(RtHook *h) {
    for (size_t i = 0; i < sizeof handlers / sizeof *handlers; i++) {
        const ExlHandler *e = &handlers[i];
        if (strcmp(e->name, h->handler)) continue;
        if (h->nargs != 3) {
            rt_log("hooks: %s: %s takes 3 arguments (fn1, fn2, version index), not %d", h->name, h->handler, h->nargs);
            return 0;
        }
        MsfixTimer **t = &timers[e->plugin];
        if (!*t) {   // zeroed, as the static initialiser leaves the first two words
            *t = mem_kernel_alloc(sizeof **t, 16);
            (*t)->rate = -1.0f;
        }
        (*t)->version = (int32_t)h->args[2];
        h->fn = exl_hook;
        h->data = e;
        return 1;
    }
    rt_log("hooks: %s: this runtime has no handler %s", h->name, h->handler);
    return 0;
}
