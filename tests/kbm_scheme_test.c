// tests/kbm_scheme_test.c: the PC scheme's routing (runtime/kbm_scheme.c) against the cases the port must hold:
// the table's size, the inherited and overridden rows of each scope, a 'T' row's tap and its hold, the walk
// modifier, the menu operations' bits, and an 'X' row that kills an inherited key. Built and run by
// tests/kbm_scheme_test.py; prints the first mismatches and exits 1 on any.
//
// Esc is the game's key while the scheme runs (KEY_ESC; the host menu keeps F1), so the table's Esc rows fire as
// upstream writes them: the pause menu in the world, back in a menu, the cancel while aiming.
#include <stdio.h>
#include <string.h>

#include "input_map.h"
#include "kbm_scheme.h"

// kbm_scheme.c reads two settings (TOTK_KBM_PULSE_MS, TOTK_KBM_MATERIALS_KEY). This binary links it without cfg.c, so
// they answer as an unset totk.ini does for the test's purpose: a press' bits stay in the frame of the press, and the
// materials row keeps the table's own key. Routing is what is under test here, not the length of a pulse.
int cfg_int(const char *name) {
    (void)name;
    return 0;
}
const char *cfg_value(const char *name) {
    (void)name;
    return "";
}

static int fails;

static void expect_u(const char *what, unsigned long long got, unsigned long long want) {
    if (got != want && fails++ < 20)
        printf("%s: got %llx want %llx\n", what, got, want);
}

static void expect_i(const char *what, long long got, long long want) {
    if (got != want && fails++ < 20) printf("%s: got %lld want %lld\n", what, got, want);
}

static uint8_t down[KEY_COUNT];

static uint64_t frame(uint64_t now, int k1, int k2, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    memset(down, 0, sizeof down);
    if (k1 > 0) down[k1] = 1;
    if (k2 > 0) down[k2] = 1;
    uint64_t b = 0;
    int32_t l[2], r[2];
    kbm_scheme_frame(down, now, &b, l, r);
    if (lx) *lx = l[0];
    if (ly) *ly = l[1];
    if (rx) *rx = r[0];
    if (ry) *ry = r[1];
    return b;
}

int main(void) {
    kbm_scheme_init();
    int32_t lx, ly, rx, ry;
    uint64_t b;

    // The whole upstream table is in (255 rows), plus the four AIM rows that turn the inherited strips off while the
    // bow is drawn (kbm_scheme_gameplay.inc); every key name in it is one this port knows. Ten tokens are Esc, which
    // this branch gives to the host: they are counted apart, not as unknown.
    expect_i("rows", kbm_scheme_row_count(), 259);
    expect_i("unknown keys", kbm_scheme_unknown_key_count(), 0);

    // WORLD, on foot: W drives the left stick up; Alt (the walk row) scales it; Space is the jump button.
    kbm_scheme_set_ctx(KBM_SC_WORLD);
    b = frame(1000, KEY_W, 0, &lx, &ly, &rx, &ry);
    expect_i("W stick y", ly, 32767);
    expect_i("W stick x", lx, 0);
    expect_u("W buttons", b, 0);
    frame(1010, 0, 0, &lx, &ly, &rx, &ry);
    b = frame(1020, KEY_W, KEY_LALT, &lx, &ly, &rx, &ry);
    expect_i("walk stick y", ly, (int32_t)(32767 * KBM_WALK_SCALE));
    b = frame(1030, KEY_SPACE, 0, &lx, &ly, &rx, &ry);
    expect_u("Space is X", b >> B_X & 1, 1);
    expect_u("Space alone", b & ~(1ull << B_X), 0);

    // A 'T' row: F tapped is the pair's bits for a pulse; F held past KBM_TAP_MS is the pair held, and the release
    // that ends a hold is not a tap.
    frame(1100, 0, 0, 0, 0, 0, 0);
    b = frame(1110, KEY_F, 0, 0, 0, 0, 0);
    expect_u("F down alone", b, 0);   // nothing before the tap or the hold is decided
    b = frame(1150, 0, 0, 0, 0, 0, 0);   // released 40 ms after the press: a tap
    expect_u("F tap pulse", b >> B_ZR & 1, 1);
    b = frame(1230, 0, 0, 0, 0, 0, 0);   // the pulse is 70 ms
    expect_u("F tap pulse ends", b, 0);
    b = frame(1300, KEY_F, 0, 0, 0, 0, 0);
    expect_u("F hold before 250 ms", b, 0);
    b = frame(1560, KEY_F, 0, 0, 0, 0, 0);
    expect_u("F hold after 250 ms", b >> B_ZR & 1, 1);
    b = frame(1600, 0, 0, 0, 0, 0, 0);
    expect_u("F hold release is not a tap", b, 0);

    // A menu: Enter confirms, the right button backs out, the wheel scrolls the D-pad. The keyboard drives the
    // menu's focus through the left stick (the game's own), which is why W is not dead there; a world button is.
    kbm_scheme_set_ctx(KBM_SC_PAUSE);
    frame(2000, 0, 0, 0, 0, 0, 0);
    b = frame(2010, KEY_ENTER, 0, 0, 0, 0, 0);
    expect_u("Enter confirms", b >> B_A & 1, 1);
    frame(2100, 0, 0, 0, 0, 0, 0);
    b = frame(2110, KEY_ESC, 0, 0, 0, 0, 0);
    expect_u("Esc backs out", b >> B_B & 1, 1);
    frame(2150, 0, 0, 0, 0, 0, 0);
    b = frame(2160, KEY_MOUSE_R, 0, 0, 0, 0, 0);   // the row's other key
    expect_u("the right button backs out too", b >> B_B & 1, 1);
    b = frame(2200, KEY_WHEEL_UP, 0, 0, 0, 0, 0);
    expect_u("wheel up scrolls", b >> B_UP & 1, 1);
    b = frame(2210, KEY_W, 0, &lx, &ly, &rx, &ry);
    expect_i("W moves the menu focus", ly, 32767);
    frame(2300, 0, 0, 0, 0, 0, 0);   // the wheel's 70 ms pulse is over
    b = frame(2310, KEY_MOUSE_L, 0, 0, 0, 0, 0);
    expect_u("the world's attack is gone in a menu", b, 0);
    b = frame(2400, KEY_SPACE, 0, 0, 0, 0, 0);
    expect_u("... and the jump too", b, 0);

    // Esc in the world is the pause menu (PCOP_MENU_GOTO, no cursor hook: the row's bits open it); I is the same
    // action's other key, and Tab holds the wheel's L.
    kbm_scheme_set_ctx(KBM_SC_WORLD);
    frame(3000, 0, 0, 0, 0, 0, 0);
    b = frame(3010, KEY_ESC, 0, 0, 0, 0, 0);
    expect_u("Esc opens the pause menu in the world", b >> B_PLUS & 1, 1);
    frame(3050, 0, 0, 0, 0, 0, 0);
    b = frame(3060, KEY_I, 0, 0, 0, 0, 0);
    expect_u("I opens it too", b >> B_PLUS & 1, 1);
    b = frame(3100, KEY_TAB, 0, 0, 0, 0, 0);
    expect_u("Tab holds the ability wheel (L)", b >> B_L & 1, 1);

    // An 'X' row kills an inherited key: Esc is the pause menu in GAMEPLAY, the aim's cancel in AIM.
    kbm_scheme_set_ctx(KBM_SC_AIM);
    frame(3200, 0, 0, 0, 0, 0, 0);
    b = frame(3210, KEY_ESC, 0, 0, 0, 0, 0);
    expect_u("Esc is B in AIM (lower the bow)", b >> B_B & 1, 1);
    expect_u("... and no longer the pause menu", b >> B_PLUS & 1, 0);

    // An 'X' row kills an inherited key: RMB shields in the world, the quick menu's rows take that away, and so is
    // Ctrl's crouch there ('G' rows of the world).
    kbm_scheme_set_ctx(KBM_SC_WORLD);
    frame(4000, 0, 0, 0, 0, 0, 0);
    b = frame(4010, KEY_MOUSE_R, 0, 0, 0, 0, 0);
    expect_u("RMB shields in the world", b >> B_ZL & 1, 1);
    b = frame(4020, KEY_LCTRL, 0, 0, 0, 0, 0);
    expect_u("Ctrl crouches there", b >> B_STICK_L & 1, 1);
    kbm_scheme_set_ctx(KBM_SC_QUICKMENU);
    frame(4100, 0, 0, 0, 0, 0, 0);
    b = frame(4110, KEY_MOUSE_R, 0, 0, 0, 0, 0);
    expect_u("... not in the quick menu", b, 0);
    b = frame(4200, KEY_LCTRL, 0, 0, 0, 0, 0);
    expect_u("... and no crouch there either", b, 0);

    // The wheel's strip step (MAC_NEXT / MAC_PREV), counted in game frames as upstream's macro is: 6 frames on the
    // d-pad while the strip comes in front, a 2-frame right-stick flick, 2 frames more on the d-pad, then the release
    // that equips (pc_gameplay.cpp). A frame here is 16 ms (kbm_scheme_set_frame_ms's default), so 96 + 32 + 32 ms.
    kbm_scheme_set_ctx(KBM_SC_WORLD);
    frame(7000, 0, 0, 0, 0, 0, 0);
    b = frame(7010, KEY_WHEEL_DOWN, 0, 0, 0, &rx, &ry);
    expect_u("a notch holds the strip's d-pad", b >> B_RIGHT & 1, 1);
    expect_i("... the stick at rest as it opens", rx, 0);
    b = frame(7075, 0, 0, 0, 0, &rx, &ry);   // 65 ms: inside the 6 frames
    expect_u("the strip is still open", b >> B_RIGHT & 1, 1);
    expect_i("... and the stick has not moved yet", rx, 0);
    b = frame(7106, 0, 0, 0, 0, &rx, &ry);   // 96 ms: the flick
    expect_i("... the flick steps the strip right", rx, 32767);
    b = frame(7140, 0, 0, 0, 0, &rx, &ry);   // 130 ms: the d-pad's 2 frames after it
    expect_u("the d-pad alone before the release", b >> B_RIGHT & 1, 1);
    expect_i("... the stick back at rest", rx, 0);
    b = frame(7180, 0, 0, 0, 0, &rx, &ry);   // 160 ms: over
    expect_u("the step ends by releasing the d-pad", b, 0);

    // Two notches are two steps, never one: the second waits for the first, and 'up' walks the strip the other way.
    frame(8000, 0, 0, 0, 0, 0, 0);
    b = frame(8010, KEY_WHEEL_UP, 0, 0, 0, &rx, &ry);
    expect_u("the first notch opens the strip", b >> B_RIGHT & 1, 1);
    frame(8030, 0, 0, 0, 0, 0, 0);                       // the key is up between two notches
    b = frame(8050, KEY_WHEEL_UP, 0, 0, 0, &rx, &ry);   // queued: one strip step at a time
    expect_i("... which is still at rest", rx, 0);
    b = frame(8106, 0, 0, 0, 0, &rx, &ry);
    expect_i("... and then flicks left for 'previous'", rx, -32767);
    b = frame(8180, 0, 0, 0, 0, &rx, &ry);   // 170 ms: the first step is over, the queued one starts
    expect_u("the queued notch runs its own step", b >> B_RIGHT & 1, 1);
    expect_i("... from rest again", rx, 0);
    b = frame(8280, 0, 0, 0, 0, &rx, &ry);
    expect_i("... and flicks left as well", rx, -32767);

    // While aiming the same wheel steps the arrows: ZR + D-pad up, the bow already in hand.
    kbm_scheme_set_ctx(KBM_SC_AIM);
    frame(10000, 0, 0, 0, 0, 0, 0);
    b = frame(10010, KEY_WHEEL_DOWN, 0, 0, 0, &rx, &ry);
    expect_u("a notch opens the arrow strip", b >> B_UP & 1, 1);
    expect_i("... at rest as it opens", rx, 0);
    b = frame(10075, 0, 0, 0, 0, &rx, &ry);
    expect_i("... still at rest inside the 6 frames", rx, 0);
    b = frame(10106, 0, 0, 0, 0, &rx, &ry);
    expect_i("... the stick steps it right", rx, 32767);
    b = frame(10200, 0, 0, 0, 0, &rx, &ry);
    expect_u("... and the step ends", b, 0);

    // G and the wheel: the material page. The first notch takes D-pad up and keeps it (the game holds the page open
    // only while up is held), later notches flick the right stick inside that hold, and releasing the throw key drops
    // both -- nothing here presses A, so the player's own release is the confirm.
    kbm_scheme_set_ctx(KBM_SC_WORLD);
    frame(11000, 0, 0, 0, 0, 0, 0);
    b = frame(11010, KEY_G, 0, 0, 0, 0, 0);
    expect_u("the throw key alone keeps the page shut", b >> B_UP & 1, 0);
    b = frame(11020, KEY_G, KEY_WHEEL_DOWN, 0, 0, &rx, &ry);
    expect_u("the first notch takes up", b >> B_UP & 1, 1);
    expect_i("... without walking the list yet", rx, 0);
    b = frame(11300, KEY_G, 0, 0, 0, &rx, &ry);
    expect_u("up is still held 280 ms later", b >> B_UP & 1, 1);
    b = frame(11310, KEY_G, KEY_WHEEL_DOWN, 0, 0, &rx, &ry);
    b = frame(11320, KEY_G, 0, 0, 0, &rx, &ry);
    expect_i("the next notch flicks the stick right", rx, 32767);
    expect_u("... with the page still held open", b >> B_UP & 1, 1);
    b = frame(11400, KEY_G, 0, 0, 0, &rx, &ry);
    expect_i("... and the flick is over", rx, 0);
    b = frame(11410, KEY_G, KEY_WHEEL_UP, 0, 0, &rx, &ry);
    b = frame(11420, KEY_G, 0, 0, 0, &rx, &ry);
    expect_i("a notch back walks the other way", rx, -32767);
    b = frame(12000, 0, 0, 0, 0, 0, 0);
    expect_u("releasing the throw drops the page too", b, 0);

    // While the throw key is down the left button belongs to the page and to nothing else: it closes the page (the
    // material stays in hand), it never reaches the attack, and it stays inert after the page is gone -- the button
    // comes back with the key. The page's sort is on the middle button while it is up.
    kbm_scheme_set_ctx(KBM_SC_WORLD);
    frame(13000, 0, 0, 0, 0, 0, 0);
    b = frame(13010, KEY_G, KEY_WHEEL_DOWN, 0, 0, &rx, &ry);
    expect_u("the notch opens the page", b >> B_UP & 1, 1);
    b = frame(13020, KEY_G, KEY_MOUSE_M, 0, 0, &rx, &ry);
    expect_u("the middle button sorts with Y", b >> B_Y & 1, 1);   // the stubbed pulse is the press frame only
    expect_u("... and the page stays up", b >> B_UP & 1, 1);
    b = frame(13030, KEY_G, 0, 0, 0, &rx, &ry);
    expect_u("the middle button did not reach the Scope", b >> B_STICK_R & 1, 0);
    b = frame(13200, KEY_G, KEY_MOUSE_L, 0, 0, &rx, &ry);
    expect_u("left closes the page", b >> B_UP & 1, 0);
    expect_u("... without reaching the attack", b >> B_Y & 1, 0);
    expect_u("... and the throw stays held", b >> B_R & 1, 1);
    b = frame(13210, KEY_G, KEY_MOUSE_L, 0, 0, &rx, &ry);   // the same press, still down
    expect_u("... still inert with the key down", b >> B_Y & 1, 0);
    b = frame(13220, KEY_G, 0, 0, 0, &rx, &ry);
    expect_u("... and inert after the release", b >> B_Y & 1, 0);
    b = frame(13300, 0, KEY_MOUSE_L, 0, 0, &rx, &ry);       // the throw key is up: the button is the attack again
    expect_u("left attacks once the throw key is up", b >> B_Y & 1, 1);
    frame(14000, 0, 0, 0, 0, 0, 0);

    // Whatever is held, in every context, the button word carries buttons only: the stick directions (16..23) are
    // the caller's to derive from the sticks it ends up with (kbm_scheme.h). Every key down is the loudest case.
    kbm_scheme_set_ctx(KBM_SC_WORLD);
    frame(9000, 0, 0, 0, 0, 0, 0);
    memset(down, 1, sizeof down);
    for (int sc = 0; sc < KBM_SC_COUNT; sc++) {
        uint64_t bits = 0;
        int32_t l[2], r[2];
        kbm_scheme_set_ctx(sc);
        kbm_scheme_frame(down, 6000, &bits, l, r);
        expect_u(kbm_scheme_scope_name(sc), bits & ~0xFFFFull, 0);
    }

    if (fails) {
        printf("%d checks failed\n", fails);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
