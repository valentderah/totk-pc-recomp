"""Exercise the production sync-ahead wait selection with a deterministic back end (TOTK_SYNC_AHEAD 0, 1, check).

    python tests/sync_ahead_check.py [--cc path/to/clang]
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent

HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint64_t x[31]; } Cpu;
typedef struct { uint32_t magic, fenced; uint64_t fence; } sync_t;
enum { NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED, NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED,
       NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED };
static int rt_game_version, enabled, shadow, succeed, relaxed;
static uint64_t completed, last, replay_value, synced, waited, waited_timeout;
static int replay_calls, shadow_calls, sync_calls, wait_calls;
uint64_t nvn_sync_ahead_guard(void);
static uint64_t done(void) { return completed; }
static void cpu_synced(uint64_t value) { synced = value; sync_calls++; }
static int wait_for(uint64_t value, uint64_t timeout) {
    assert(shadow_calls == relaxed);
    wait_calls++; waited = value; waited_timeout = timeout;
    if (succeed) completed = value;
    return succeed;
}
typedef struct { uint64_t (*fence_done)(void); void (*cpu_synced)(uint64_t); int (*fence_wait)(uint64_t, uint64_t); } GfxBackend;
static const GfxBackend backend = {done, cpu_synced, wait_for};
static const GfxBackend *gfx_backend(void) { return &backend; }
// enabled: 0 off, 1 on, 2 check (the wait stays exact; vk_ahead.c is told what the late wait would have waited for)
static const char *cfg_value(const char *name) {
    assert(!strcmp(name, "TOTK_SYNC_AHEAD"));
    return enabled == 2 ? "check" : enabled ? "1" : "0";
}
static int entry_calls, lag_calls, done_calls, done_ok;
static uint64_t lag_asked, lag_value;
static int lag_behind;
static void vk_ahead_frame_entry(void) { assert(!lag_calls && !done_calls); entry_calls++; }
static void vk_ahead_frame_lag(uint64_t asked, uint64_t lagged, int behind) {
    assert(entry_calls == 1 && !sync_calls);   // before cpu_synced: the small write-backs see the lag
    lag_calls++, lag_asked = asked, lag_value = lagged, lag_behind = behind;
}
static void vk_ahead_frame_done(int ok) { assert(lag_calls == 1); done_calls++, done_ok = ok; }
static int vk_mem_shadow_on(void) { return shadow; }
static uint64_t nvn_fence_last(void) { return last; }
static void rt_log(const char *format, ...) { (void)format; }
// The patch table: set qstamp exists on 1.4.3 only and starts as TOTK_SYNC_AHEAD says (runtime/patches.c).
static int qstamp_on;
static int rt_patch_set(const char *name) { return !strcmp(name, "qstamp") && rt_game_version == 143 ? 20 : -1; }
static void rt_patch_switch(int set, int on) { if (set == 20) qstamp_on = on; else assert(set == -1); }
static void gfx_replay_wait_value(uint64_t value) { replay_value = value; replay_calls++; }
static void vk_mem_shadow_fence(void) {
    assert(nvn_sync_ahead_guard() >= replay_value);
    shadow_calls++;
}
#define NVN_BLOCKING() ((void)0)
#define NVN_API(name) static void name(Cpu *c)
#define arg_p(c, i) ((void *)(uintptr_t)(c)->x[i])
#define arg_x(c, i) ((c)->x[i])
#define ret_x(c, value) ((c)->x[0] = (value))
PRODUCTION
static uint32_t seed = 0x19ed634b;
static uint32_t random_word(void) { seed ^= seed << 13; seed ^= seed >> 17; return seed ^= seed << 5; }
int main(int argc, char **argv) {
    assert(argc == 4);
    rt_game_version = atoi(argv[1]); enabled = atoi(argv[2]); shadow = atoi(argv[3]);
    int supported = rt_game_version == 110 || rt_game_version == 121 || rt_game_version == 143;
    uint64_t site = rt_game_version == 110 ? 0x7000b39aa4ull : rt_game_version == 121 ? 0x7000b23dfcull :
                    rt_game_version == 143 ? 0x70002db99cull : 0;
    assert(frame_wait_lr() == site);
    qstamp_on = rt_game_version == 143 && enabled;
    last = 20; nvn_sync_ahead_present();
    last = 40; nvn_sync_ahead_present();
    int on = enabled == 1 && shadow && supported, check = enabled == 2 && supported;
    assert(pa_fence[0] == (on || check ? 40 : 0));
    assert(pa_fence[1] == (on || check ? 20 : 0));
    assert(qstamp_on == (rt_game_version == 143 && on));   // a refused switch and check leave the game's check
    int relaxed_cases = 0, polls = 0, other_waits = 0;
    for (int i = 0; i < 100000; i++) {
        sync_t sync = {.fenced = random_word() % 2, .fence = random_word() % 65};
        uint64_t timeout = i % 3 == 0 ? 0 : i % 3 == 1 ? 1 : UINT64_MAX;
        uint64_t lr = i / 3 % 3 == 0 ? site : i / 3 % 3 == 1 ? site + 4 : site - 0x18;
        completed = random_word() % 65;
        uint64_t initial_done = completed, target = sync.fence;
        succeed = random_word() % 2;
        int lagcase = sync.fenced && timeout && lr == site && completed < target && target > 20 && target <= 40;
        relaxed = on && lagcase;
        int checked = check && sync.fenced && timeout && lr == site;
        int checkcase = checked && target > 20 && target <= 40;   // check: as if the GPU were behind
        relaxed_cases += relaxed; polls += timeout == 0; other_waits += lr != site;
        if (relaxed) target = 20;
        replay_calls = shadow_calls = sync_calls = wait_calls = 0;
        entry_calls = lag_calls = done_calls = 0;
        synced = waited = waited_timeout = replay_value = UINT64_MAX;
        pa_guard = 0;
        Cpu cpu = {0}; cpu.x[0] = (uintptr_t)&sync; cpu.x[1] = timeout; cpu.x[30] = lr;
        nvnSyncWait(&cpu);
        assert(replay_calls == relaxed && shadow_calls == relaxed);
        assert(entry_calls == checked && lag_calls == checked && done_calls == checked);
        if (checked) {   // exact wait, the late one's value told to the check
            assert(lag_asked == sync.fence && lag_value == (checkcase ? 20 : sync.fence));
            assert(lag_behind == (initial_done < sync.fence));
            assert(done_ok == (initial_done >= target || succeed));
        }
        assert(pa_guard == (relaxed ? sync.fence : 0));
        if (relaxed) assert(replay_value == sync.fence);
        if (!sync.fenced) {
            assert(cpu.x[0] == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED && !sync_calls && !wait_calls);
        } else if (initial_done >= target) {
            assert(cpu.x[0] == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED && sync_calls == 1 && !wait_calls);
            assert(synced == (relaxed ? initial_done : sync.fence));
        } else {
            assert(wait_calls == 1 && waited == target && waited_timeout == timeout);
            assert(cpu.x[0] == (succeed ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED));
            assert(sync_calls == succeed);
            if (succeed) assert(synced == target);
        }
    }
    assert(polls > 0 && other_waits > 0);
    assert((relaxed_cases > 0) == on);
    // Raising the guard for an older request must not lower a guard already published by another waiter.
    if (on) {
        sync_t sync = {.fenced = 1, .fence = 35};
        completed = 25; pa_guard = 39; relaxed = 1;
        Cpu cpu = {0}; cpu.x[0] = (uintptr_t)&sync; cpu.x[1] = UINT64_MAX; cpu.x[30] = site;
        nvnSyncWait(&cpu);
        assert(pa_guard == 39);
    }
    printf("sync-ahead version %d enabled %d shadow %d: 100000 wait selections passed (%d relaxed)\n", rt_game_version, enabled, shadow, relaxed_cases);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cc", default=shutil.which("clang") or r"C:\msys64\ucrt64\bin\clang.exe")
    args = parser.parse_args()
    source = (ROOT / "runtime/nvn_api_core.c").read_text()
    start = source.index("static uint64_t pa_fence[2], pa_guard;")
    end = source.index("// void nvnEventBuilderSetDefaults", start)
    (ROOT / "build").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="sync-ahead-", dir=ROOT / "build") as directory:
        directory = Path(directory)
        path, exe = directory / "check.c", directory / "check.exe"
        path.write_text(HARNESS.replace("PRODUCTION", source[start:end]))
        subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Werror", str(path), "-o", str(exe)], check=True)
        for version in (110, 121, 143, 999):
            for enabled in (0, 1, 2):
                for shadow in (0, 1):
                    subprocess.run([str(exe), str(version), str(enabled), str(shadow)], check=True)


if __name__ == "__main__":
    main()
