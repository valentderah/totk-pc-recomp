#pragma once
#include <stdint.h>
#define TOTK_NGX_EVAL_ABI 1u
// Fixed-width C ABI between MinGW host and MSVC SDK bridge. Images order: color, depth, MV, bias, output.
typedef struct {
    uint32_t abi_version,struct_size,width,height;
    uint64_t command_buffer,image[5],view[5];
    uint32_t format[5],reset;
    float jitter_x,jitter_y,frame_time_ms;
    uint32_t reserved;
} TotkNgxEvalFrame;
#ifdef __cplusplus
static_assert(sizeof(TotkNgxEvalFrame)==144,"NGX frame ABI");
#else
_Static_assert(sizeof(TotkNgxEvalFrame)==144,"NGX frame ABI");
#endif
