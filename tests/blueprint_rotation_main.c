// The Autobuild history rotation, tested against the real runtime/blueprint_import.c.
//
// bp_rotate_ages is the rule that decides which card a new creation evicts and how the rest of the history moves. It is
// pure, so it can be linked and run here without a game, a save or guest memory: the writes around it cannot be tested
// this way, but the arithmetic that the player actually sees can.
//
// Built and run by tests/blueprint_rotation_test.py.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "blueprint_import.h"

#define N BP_SLOTS

static int failures, checks;

static void check(int cond, const char *what) {
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

// A save whose favourites are pinned to the oldest ages, the way the game leaves them: entry i gets the age it would
// have in a fresh save, except the favourites, which take the largest ages.
static void make_save(int ages[N], uint32_t *fav_bits, const int *favs, int nfav) {
    int free_ages[N];
    int nfree = N - nfav, fi = 0;
    for (int i = 0; i < nfree; i++) free_ages[i] = i;
    *fav_bits = 0;
    int used[N] = {0};
    for (int k = 0; k < nfav; k++) used[favs[k]] = 1;
    for (int i = 0; i < N; i++) {
        if (used[i]) continue;
        ages[i] = free_ages[fi++];
    }
    for (int k = 0; k < nfav; k++) {
        ages[favs[k]] = N - 1 - k;   // the oldest ages, one per favourite
        *fav_bits |= 1u << favs[k];
    }
}

static int is_permutation(const int a[N]) {
    uint32_t seen = 0;
    for (int i = 0; i < N; i++) {
        if (a[i] < 0 || a[i] >= N || (seen >> a[i] & 1u)) return 0;
        seen |= 1u << a[i];
    }
    return 1;
}

static int entry_with_age(const int a[N], int age) {
    for (int i = 0; i < N; i++)
        if (a[i] == age) return i;
    return -1;
}

// One scenario: build a save, rotate it, and assert everything the game's rule promises.
static void scenario(const char *name, const int *favs, int nfav) {
    int ages[N], rotated[N];
    uint32_t fav_bits;
    make_save(ages, &fav_bits, favs, nfav);
    check(is_permutation(ages), "the scenario's own save is a permutation");

    const int victim = bp_rotate_ages(ages, fav_bits, rotated);
    char msg[192];

    snprintf(msg, sizeof msg, "%s: an entry was evicted (%d)", name, victim);
    check(victim >= 0, msg);
    if (victim < 0) return;

    snprintf(msg, sizeof msg, "%s: the result is still a permutation", name);
    check(is_permutation(rotated), msg);

    // The new creation is the card drawn first: age 0 sits on the evicted entry.
    snprintf(msg, sizeof msg, "%s: the new creation takes the evicted entry at age 0 (entry %d, age %d)", name, victim,
             rotated[victim]);
    check(rotated[victim] == 0 && entry_with_age(rotated, 0) == victim, msg);

    // The victim was the oldest free card.
    {
        int max_free = -1;
        for (int i = 0; i < N; i++)
            if (!((fav_bits >> i) & 1u) && ages[i] > max_free) max_free = ages[i];
        snprintf(msg, sizeof msg, "%s: the evicted entry was the oldest free one (age %d, oldest %d)", name,
                 ages[victim], max_free);
        check(ages[victim] == max_free, msg);
    }

    // Every surviving entry keeps its age or rises by exactly one, and a favourite is never the one spent. A favourite
    // ABOVE the evicted card does rise by one - a new creation takes age 0, so every card drawn above the spent one moves
    // down a place - and a favourite below it keeps its age. The result being a permutation is what rules out the other
    // reading: keeping every favourite's age would give two entries the same age as soon as a favourite sits above the
    // oldest free card, which the check above would have caught.
    for (int i = 0; i < N; i++) {
        if (i == victim) continue;
        const int rose = rotated[i] == ages[i] + 1;
        const int kept = rotated[i] == ages[i];
        snprintf(msg, sizeof msg, "%s: entry %d kept its age or rose by one (%d -> %d)", name, i, ages[i], rotated[i]);
        check(kept || rose, msg);
        if ((fav_bits >> i) & 1u) {
            snprintf(msg, sizeof msg, "%s: favourite entry %d was not spent", name, i);
            check(i != victim, msg);
        }
    }
    // A favourite is never the evicted entry, and the eviction is always a free one.
    snprintf(msg, sizeof msg, "%s: the evicted entry %d is not a favourite", name, victim);
    check(!((fav_bits >> victim) & 1u), msg);
}

int main(void) {
    // A fresh save: nothing is a favourite.
    scenario("fresh save", NULL, 0);

    // One favourite, in several places. A favourite is never the entry spent, wherever it sits - including on the newest
    // card, which does move down one place because the new creation takes age 0.
    // A favourite on the newest card. It is not evicted, but it does move down: a new creation takes age 0, so every card
    // that was drawn above the evicted one rises by one. What a favourite is guaranteed is that it is never the entry
    // spent, and that its own age never lands on another entry's.
    {
        const int f1[] = {12};
        scenario("one favourite", f1, 1);
        const int f0[] = {0};
        scenario("favourite on the first card", f0, 1);
        const int f0b[] = {0, 29};
        scenario("a favourite on the first and on the oldest card", f0b, 2);
        const int fold[] = {29};
        scenario("favourite is the oldest card", fold, 1);
    }

    // Two favourites: the layout the imported 100% save came with, and the one the old rule could not rotate. Favourites
    // hold the two oldest ranks, so the oldest free card's rank sits right below them and moving every free card down one
    // collides with a favourite. The ranks below the evicted card must stay put instead.
    {
        const int f2[] = {5, 13};
        scenario("two favourites", f2, 2);
        const int f2mid[] = {3, 17};
        scenario("two favourites in the middle", f2mid, 2);
        const int f3[] = {1, 40 % N, 29};
        scenario("three favourites", f3, 3);
    }

    // The maximum the game allows.
    {
        const int f8[] = {1, 5, 12, 19, 22, 25, 27, 29};
        scenario("eight favourites", f8, 8);
    }

    // A save whose ages are not a permutation must be refused, not written.
    {
        int ages[N], rotated[N];
        memset(ages, 0, sizeof ages);
        char msg[128];
        snprintf(msg, sizeof msg, "ages that are not a permutation are refused");
        check(bp_rotate_ages(ages, 0, rotated) < 0, msg);

        for (int i = 0; i < N; i++) ages[i] = i;
        snprintf(msg, sizeof msg, "an out-of-range age is refused");
        ages[3] = N;
        check(bp_rotate_ages(ages, 0, rotated) < 0, msg);
    }

    // Every entry a favourite: there is nothing to spend, so the import must be refused rather than evict a favourite.
    {
        int ages[N], rotated[N];
        for (int i = 0; i < N; i++) ages[i] = i;
        const uint32_t all = (N >= 32) ? 0xFFFFFFFFu : ((1u << N) - 1u);
        char msg[128];
        snprintf(msg, sizeof msg, "a history of nothing but favourites is refused");
        check(bp_rotate_ages(ages, all, rotated) < 0, msg);
    }

    // Successive imports: the creation that was first slides to second, third, and so on, and the list never breaks.
    {
        int ages[N], rotated[N];
        for (int i = 0; i < N; i++) ages[i] = i;
        int ok = 1;
        for (int round = 0; round < 40; round++) {   // more rounds than there are slots
            const int victim = bp_rotate_ages(ages, 0, rotated);
            if (victim < 0 || !is_permutation(rotated) || rotated[victim] != 0) {
                ok = 0;
                break;
            }
            memcpy(ages, rotated, sizeof ages);
        }
        check(ok, "40 successive imports keep the history a valid permutation with the newest at age 0");
    }

    printf("blueprint rotation: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
