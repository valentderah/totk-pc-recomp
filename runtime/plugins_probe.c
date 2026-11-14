// --probe-plugins: the exlaunch inline hooks and MSFix's handlers of plugins.c, on a stack and objects of their own,
// with host functions standing in for the game functions MSFix calls. Needs no game files.
#include <stdio.h>
#include <string.h>

#include "host.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { fails++; fprintf(stderr, "FAIL plugins: %s\n", what); } } while (0)

static uint8_t stack[0x2000] __attribute__((aligned(16)));
static uint8_t obj[0x4000] __attribute__((aligned(16)));   // what X19 points to, and C's pointer chain
#define TOP ((uint64_t)(uintptr_t)(stack + sizeof stack))
#define OBJ ((uint64_t)(uintptr_t)obj)

static __uint128_t v_start(int i) {   // a vector register's value before a hook: both halves and bits 32-63 set
    return (__uint128_t)(0xA0A0A0A000000000ull | (uint64_t)i) << 64 | (0xB0B0B0B000000000ull | (uint64_t)i);
}
static __uint128_t v_clobbered(int i) { return (__uint128_t)0xC0B1ull << 64 | (uint64_t)i; }

static void start(Cpu *c) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 31; i++) c->x[i] = 0x1000 + (uint64_t)i;
    for (int i = 0; i < 32; i++) c->v[i] = v_start(i);
    c->x[19] = OBJ;
    c->sp = TOP;
    c->nzcv = 0x60000000;
}

static int x_kept(const Cpu *c) {   // X0-X30, sp and the flags as start() left them
    for (int i = 0; i < 31; i++)
        if (c->x[i] != (i == 19 ? OBJ : 0x1000 + (uint64_t)i)) return 0;
    return c->sp == TOP && c->nzcv == 0x60000000;
}

// The stand-ins record what they got and then change what a called function may: x0-x18, v0-v7, v16-v31, the flags.
// fn2's also advances the time left by step, as the game's timer step would.
static Cpu seen;
static float seen_timer[4], step;
static int calls;

static void clobber(Cpu *c) {
    for (int i = 0; i <= 18; i++) c->x[i] = 0xC0B0000000000000ull | (uint64_t)i;
    for (int i = 0; i < 32; i++)
        if (i < 8 || i >= 16) c->v[i] = v_clobbered(i);
    c->nzcv = 0xF0000000;
}

static void fn1_stand_in(Cpu *c) {
    seen = *c;
    calls++;
    clobber(c);
}

static void fn2_stand_in(Cpu *c) {
    seen = *c;
    calls++;
    float *t = (float *)(uintptr_t)c->x[0];
    memcpy(seen_timer, t, sizeof seen_timer);
    t[0] += step;
    clobber(c);
}

static uint64_t fn1, fn2;

static RtHook hook(const char *handler, uint64_t version) {
    RtHook h = {"probe", handler, 3, {fn1, fn2, version, 0}, NULL, NULL, 0};
    if (!rt_hook_bind(&h)) {
        fails++;
        fprintf(stderr, "FAIL plugins: %s does not bind\n", handler);
    }
    return h;
}

static void run(RtHook *h, Cpu *c) {
    if (h->fn) h->fn(c, h);
}

static void probe_a(void) {
    Cpu c;
    start(&c);
    calls = 0;
    RtHook a2 = hook("msfix2_a", 9);
    run(&a2, &c);
    uint64_t ctx = TOP - 0x100, fp = ctx - 0x20;
    CHECK(calls == 1 && seen.x[0] == 0x1000 && seen.x[1] == fp + 0x1c && seen.x[2] == 0x1002 &&
          seen.x[3] == 0xEE199C3Au && seen.sp == fp && seen.x[29] == fp && LD32(fp + 0x1c) == 0xFFFFFFFFu,
          "A (exl_inline_hook_impl): fn1 gets X0, a word holding -1 in the frame below the context, X2, 0xee199c3a");
    CHECK(LD64(ctx + 8 * 19) == OBJ && LD64(TOP - 0x10) == 0x101e && seen.v[0] == v_start(0),
          "A: the context holds X0-X30 below sp, X30 at sp - 0x10; fn1 gets the hooked code's vector registers");
    CHECK(x_kept(&c), "A: X0-X30, sp and the flags come back");
    CHECK(c.v[0] == v_clobbered(0) && c.v[8] == v_start(8) && c.v[16] == v_clobbered(16),
          "A (exl_inline_hook_impl): the vector registers stay as fn1 left them");

    start(&c);
    calls = 0;
    RtHook a1 = hook("msfix1_a", 5);
    run(&a1, &c);
    ctx = TOP - 0x180;
    fp = ctx - 0x20;
    CHECK(calls == 1 && seen.x[0] == 0x1000 && seen.x[1] == fp + 0x1c && seen.x[2] == 0x1002 && seen.sp == fp &&
          LD64(ctx) == (uint64_t)v_start(0) && LD64(ctx + 0x80) == 0x1000,
          "A (exl_inline_hook_with_fp_impl): D0-D15 then X0-X30 in the context, fn1's arguments");
    int lows = 1, highs = 1;
    for (int i = 0; i < 16; i++) lows &= c.v[i] == (uint64_t)v_start(i);
    for (int i = 16; i < 32; i++) highs &= c.v[i] == v_clobbered(i);
    CHECK(x_kept(&c) && lows && highs,
          "A (with_fp): V0-V15 back as their low halves only, V16-V31 as fn1 left them, the rest as before");
}

static void probe_bd(void) {
    float at74 = 12.5f;
    uint32_t at74_bits;
    memcpy(&at74_bits, &at74, 4);
    memcpy(obj + 0x74, &at74, 4);
    Cpu c;
    RtHook b2 = hook("msfix2_b", 9), d2 = hook("msfix2_d", 9);
    start(&c);
    calls = 0;
    run(&d2, &c);
    CHECK(calls == 0 && x_kept(&c), "D with the timer at 0: no call");
    run(&b2, &c);
    CHECK(x_kept(&c), "B: the registers stay");
    start(&c);
    step = -1.0f;
    run(&d2, &c);
    uint64_t ctx = TOP - 0x300, fp = ctx - 0x20;
    CHECK(calls == 1 && seen.x[1] == ctx && seen.sp == fp && LD64(fp + 0x18) == ctx && seen.v[0] == v_start(0),
          "D (exl_inline_float_hook_impl): fn2 gets the context in x1 and the hooked code's s0");
    CHECK(seen_timer[0] == 90.0f && seen_timer[1] == 90.0f && seen_timer[2] == -1.0f && seen.v[31] == 0x42B40000u,
          "B set the timer to 90; D hands it to fn2 with the time left in s31");
    int same = 1;
    for (int i = 1; i < 32; i++) same &= c.v[i] == v_start(i);
    CHECK(x_kept(&c) && same && c.v[0] == ((v_start(0) & ~(__uint128_t)0xFFFFFFFFu) | at74_bits),
          "D on 1.4.3: S0 takes the float at X19 + 0x74, every other register comes back");
    uint64_t timer = seen.x[0];

    RtHook d2_121 = hook("msfix2_d", 5);
    start(&c);
    run(&d2_121, &c);
    CHECK(calls == 2 && c.v[0] == v_start(0) && c.v[1] == ((v_start(1) & ~(__uint128_t)0xFFFFFFFFu) | at74_bits),
          "D of the 1.4.3 package on 1.2.1: S1 takes it");
    step = -100.0f;   // 88 left: this step takes it below 0
    start(&c);
    run(&d2, &c);
    start(&c);
    run(&d2, &c);
    const float *t = (const float *)(uintptr_t)timer;
    CHECK(calls == 3 && t[0] == 0.0f && t[1] == 0.0f && t[2] == -1.0f && x_kept(&c),
          "D below 0: no call, the time left and the second word cleared");

    RtHook b1 = hook("msfix1_b", 5), d1 = hook("msfix1_d", 5);
    start(&c);
    run(&b1, &c);
    step = -1.0f;
    calls = 0;
    run(&d1, &c);
    ctx = TOP - 0x180;
    fp = ctx - 0x20;
    CHECK(calls == 1 && seen.x[1] == fn2 && seen.v[0] == 0x42B40000u && seen.sp == fp && seen_timer[0] == 90.0f,
          "D of the 1.2.1 package: fn2 gets its own address in x1 and the time left in s0");
    int lows = 1, highs = 1;
    for (int i = 1; i < 16; i++) lows &= c.v[i] == (uint64_t)v_start(i);
    for (int i = 16; i < 32; i++) highs &= c.v[i] == v_clobbered(i);
    CHECK(x_kept(&c) && lows && highs && c.v[0] == ((0xB0B0B0B000000000ull & ~0xFFFFFFFFull) | at74_bits),
          "D of the 1.2.1 package: S0 of the D0 slot takes the float, V16-V31 as fn2 left them");
}

static void probe_c(void) {
    uint64_t p = OBJ + 0x100, q1 = OBJ + 0x400, q2 = OBJ + 0x800, q3 = OBJ + 0x1000;
    static const char sword[] = "Weapon_Sword_070", other[] = "Weapon_Sword_071";
    ST64(OBJ + 0x18, p);
    ST64(p + 0x218, (uint64_t)(uintptr_t)sword);
    ST64(p + 0x228, q1);
    ST64(q1 + 0x2f8, q2);
    ST64(q2 + 0x1840, q3);
    ST32(q3 + 8, 0);
    Cpu c;
    RtHook c2 = hook("msfix2_c", 9);
    start(&c);
    c.x[8] = 0x123456789ull;
    run(&c2, &c);
    CHECK(LD32(q3 + 8) == 0x23456789u && c.x[8] == 0x123456789ull, "C: the Master Sword's word takes X8's low half");
    ST32(q3 + 8, 0);
    ST64(p + 0x218, (uint64_t)(uintptr_t)other);
    run(&c2, &c);
    CHECK(!LD32(q3 + 8), "C: another name stores nothing");
    ST64(p + 0x218, (uint64_t)(uintptr_t)sword);
    ST64(q1 + 0x2f8, 0);
    run(&c2, &c);
    CHECK(!LD32(q3 + 8), "C: a null pointer on the way stores nothing");
    ST64(q1 + 0x2f8, q2);
    ST64(OBJ + 0x18, p + 8);   // 1.0.0: the plugin goes 8 bytes back
    RtHook c1 = hook("msfix1_c", 0);
    start(&c);
    run(&c1, &c);
    CHECK(LD32(q3 + 8) == 0x1008u, "C on 1.0.0 (1.2.1 package): p is 8 bytes lower");
}

int probe_plugins(void) {
    fn1 = hostfn_register("<probe fn1>", fn1_stand_in);
    fn2 = hostfn_register("<probe fn2>", fn2_stand_in);
    probe_a();
    probe_bd();
    probe_c();
    RtHook bad = {"probe", "msfix2_a", 2, {fn1, fn2, 0, 0}, NULL, NULL, 0};
    CHECK(!rt_hook_bind(&bad), "a hook with the wrong argument count does not bind");
    RtHook unknown = {"probe", "no_such_handler", 3, {fn1, fn2, 9, 0}, NULL, NULL, 0};
    CHECK(!rt_hook_bind(&unknown), "an unknown handler does not bind");
    printf("plugins: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
