// Fixed scene resolution preserves each effect's own downsample ratio.
#include "native_policy.h"
#include <stdio.h>
#include <string.h>

extern _Noreturn void runtime_fail(const char *message);

void native_patch_main(void *image, size_t size) {
    if (!native_policy_strict()) return;
    const struct { size_t offset; uint32_t original, replacement; } sites[] = {
#if defined(TOTK_GAME_V121)
        // The adaptive and emergency quality paths publish renderer+0xccc.
        // The original division publishes a reduced scene/canvas ratio.
        // A fixed 4K scene and canvas require unity on both paths.
        {0xcc114c, 0x1e211800, 0x1e2e1000}, // fdiv s0,s0,s1 -> fmov s0,#1.0
        {0xcc17e8, 0x1e211800, 0x1e2e1000},
#elif defined(TOTK_GAME_V143)
        // This compiler applies DRS inside view sizing. Preserve view scale,
        // clamp and alignment; replace only the dynamic multiplier.
        {0xd5d80, 0xbd402906, 0x1e2e1006}, // ldr s6,[x8,#0x28] -> fmov s6,#1.0
#else
#error Unsupported native game version
#endif
    };
    for (size_t i=0; i<sizeof sites/sizeof *sites; ++i) {
        uint32_t word;
        if (sites[i].offset > size || size - sites[i].offset < sizeof word)
            runtime_fail("fixed resolution patch is outside the main module");
        memcpy(&word, (char *)image + sites[i].offset, sizeof word);
        if (word != sites[i].original) runtime_fail("fixed resolution patch does not match this game build");
    }
    for (size_t i=0; i<sizeof sites/sizeof *sites; ++i)
        memcpy((char *)image + sites[i].offset, &sites[i].replacement, sizeof sites[i].replacement);
    fprintf(stderr, "[totk] fixed scene resolution: dynamic reduction disabled at %zu guarded sites\n",
            sizeof sites/sizeof *sites);
}
