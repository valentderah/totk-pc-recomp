// totk-config: Nuklear (toolchain/nuklear, fetched by tools/build_configurator.py) with the options the window uses;
// cfgr_nk.c compiles it once with its SDL3 renderer.
#pragma once
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_COMMAND_USERDATA
#include <SDL3/SDL.h>
#include "nuklear.h"
#include "nuklear_sdl3_renderer.h"
