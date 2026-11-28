"""Check the production capture transitions with controlled OS results, without moving the desktop pointer.

python tests/input_capture_backend_test.py --cc clang --deps /path/to/include
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
COMMON = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static int capture_ok=1;
int input_cursor_allowed(void) { return 1; }
void gameplay_tweaks_reset(void) {}
void rt_log(const char *fmt,...) { (void)fmt; }
'''
WIN = r'''
#include <windows.h>
static volatile LONG cur_captured,capture_generation,cur_active,cur_rel,cur_suspend,mouse_dx,mouse_dy;
static SRWLOCK cur_lock=SRWLOCK_INIT;
enum { CUR_REL=1 };
#define GetForegroundWindow() ((HWND)(uintptr_t)1)
#define IsIconic(h) 0
static int fixture_rect(HWND h,RECT *r) { (void)h; *r=(RECT){0,0,100,100}; return 1; }
static int fixture_point(HWND h,POINT *p) { (void)h; (void)p; return 1; }
#define GetClientRect fixture_rect
#define ClientToScreen fixture_point
#define ClipCursor(rect) ((rect)==NULL || capture_ok)
#define APPLY() cursor_update((HWND)(uintptr_t)1)
#define ACTIVE cur_active
#define CAPTURED cur_captured
'''
SDL = r'''
#include <SDL3/SDL.h>
#include "platform/platform.h"
static SDL_Window *win=(SDL_Window *)(uintptr_t)1;
static volatile int32_t cur_pending,focused,cur_rel,cur_suspend,captured,mouse_dx,mouse_dy;
static volatile uint32_t capture_generation;
static float rem_x,rem_y;
enum { CUR_REL=1 };
#define SDL_GetWindowFlags(w) 0
#define SDL_SetWindowRelativeMouseMode(w,on) (capture_ok != 0)
#define SDL_GetError() "fixture capture failure"
#define APPLY() cursor_apply()
#define ACTIVE focused
#define CAPTURED captured
'''
MAIN = r'''
int main(void) {
    ACTIVE=1; APPLY(); assert(CAPTURED && capture_generation==1);
    APPLY(); assert(CAPTURED && capture_generation==1);
    ACTIVE=0; APPLY(); assert(!CAPTURED && capture_generation==1);
    ACTIVE=1; capture_ok=0; APPLY(); assert(!CAPTURED && capture_generation==1);
    capture_ok=1; APPLY(); assert(CAPTURED && capture_generation==2);
    ACTIVE=0; APPLY(); ACTIVE=1; APPLY(); assert(CAPTURED && capture_generation==3);
    puts("capture backend: initial capture, stable state, loss, failed capture and recapture passed");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--deps', type=Path, required=True, help='SDL3 include directory')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='totk-capture-') as temp:
        folder = Path(temp)
        for backend in (('win', 'sdl') if os.name == 'nt' else ('sdl',)):
            path = ROOT / 'runtime' / ('input_win.c' if backend == 'win' else 'input_sdl.c')
            source = path.read_text(encoding='utf-8')
            start = 'static void cursor_update(HWND h)' if backend == 'win' else 'static void cursor_apply(void)'
            end = '// The pointer'
            if source.count(start) != 1:
                raise RuntimeError(f'{path.name}: capture function changed')
            begin = source.index(start)
            body = source[begin:source.index(end, begin)]
            harness = folder / (backend + '.c')
            # Compile the actual transition function; only the surrounding state and OS results are controlled.
            harness.write_text(COMMON + (WIN if backend == 'win' else SDL) + body + MAIN, encoding='utf-8')
            exe = folder / (backend + ('.exe' if os.name == 'nt' else ''))
            subprocess.run([args.cc, '-O2', '-march=x86-64-v3', '-I', str(ROOT/'runtime'),
                            '-I', str(args.deps), str(harness), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__':
    main()