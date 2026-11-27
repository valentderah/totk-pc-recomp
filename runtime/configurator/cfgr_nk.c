// totk-config: the implementations of Nuklear and of its SDL3 renderer back end, both upstream files unchanged
// (toolchain/nuklear, THIRD_PARTY_NOTICES.md). Their own warnings are not ours to fix.
#pragma clang diagnostic ignored "-Weverything"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define NK_IMPLEMENTATION
#define NK_SDL3_RENDERER_IMPLEMENTATION
#define NK_MEMSET memset
#define NK_MEMCPY memcpy
#define NK_SQRT sqrtf
#define NK_SIN sinf
#define NK_COS cosf
#define NK_STRTOD strtod
#include "configurator/cfgr_nk.h"
