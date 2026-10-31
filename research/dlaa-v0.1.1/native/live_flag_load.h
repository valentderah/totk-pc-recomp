#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
// A read must not perform a locked read-modify-write. Writers remain Interlocked
// exchanges; acquire observes their publication without changing the flag.
static LONG lf_load(volatile LONG *flag,int acquire){
    return acquire?__atomic_load_n(flag,__ATOMIC_ACQUIRE):InterlockedCompareExchange(flag,0,0);
}
