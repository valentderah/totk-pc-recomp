#pragma once
#include <string.h>
// Process-start selection only; the bridge refuses to reconfigure an existing feature.
static int dp_select(const char *value,unsigned *out){
    if(!value||!*value||!strcmp(value,"L")||!strcmp(value,"12")){*out=12;return 1;}
    if(!strcmp(value,"K")||!strcmp(value,"11")){*out=11;return 1;}
    return 0;
}
