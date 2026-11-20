"""Check the production player-position walk used to start the game-time clock.

    python tests/frame_world_check.py --cc path/to/clang
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MOD_MAIN_BASE 0x7000000000ull
#define RT_V110 (version == 110)
#define RT_V143 (version >= 143)
static int version;
static uint8_t memory[0x8000];
static uint64_t singleton;
static int plat_mem_read(uint64_t address, void *out, size_t size) {
    uint64_t slot = MOD_MAIN_BASE + (RT_V143 ? 0x3add238 : RT_V110 ? 0x4630a68 : 0x4728538);
    if (address == slot && size == 8) { memcpy(out, &singleton, 8); return 1; }
    if (address > sizeof memory || size > sizeof memory - address) return 0;
    memcpy(out, memory + address, size);
    return 1;
}
static void put64(uint64_t address, uint64_t value) { memcpy(memory + address, &value, 8); }
static void put32(uint64_t address, int32_t value) { memcpy(memory + address, &value, 4); }
PRODUCTION
static void fixture(void) {
    memset(memory, 0, sizeof memory);
    singleton = RT_V110 ? 0x800 : 0x1000;
    put64(0x800, 0x1000);   // 1.1.0's GOT entry points to the singleton's storage
    put64(0x1000 + 0x1e8, 0x2000);
    put32(0x2000 + 0x50, 14); put64(0x2000 + 0x58, 0x3000);
    put64(0x3000 + 13 * 8, 0x4000);
    put32(0x4000 + 0x20, 2); put64(0x4000 + 0x28, 0x5000);
    // A missing first actor must not hide the valid player in the second entry.
    put64(0x5000 + 0x70 + 8, 0x5800); put64(0x5800 + 0x40, 0x6000);
    put64(0x6000 + 0x218, 0x7000); memcpy(memory + 0x7000, "Player", 7);
    float position[3] = {-700.0f, 140.0f, 870.0f};
    memcpy(memory + 0x6000 + 0x2b4, position, sizeof position);
}
int main(void) {
    const int versions[] = {110, 121, 143};
    for (unsigned i = 0; i < sizeof versions / sizeof *versions; i++) {
        version = versions[i]; fixture();
        float position[3] = {0};
        assert(player_pos(position));
        assert(position[0] == -700.0f && position[1] == 140.0f && position[2] == 870.0f);
        singleton = 0; assert(!player_pos(position));
        fixture(); put64(0x800, 0); if (RT_V110) assert(!player_pos(position));
        fixture(); put32(0x2000 + 0x50, 13); assert(!player_pos(position));
        fixture(); put32(0x4000 + 0x20, 0); assert(!player_pos(position));
        fixture(); put64(0x4000 + 0x28, 0); assert(!player_pos(position));
        fixture(); memory[0x7000] = 'X'; assert(!player_pos(position));
        fixture(); float bad = NAN; memcpy(memory + 0x6000 + 0x2b4, &bad, 4);
        assert(!player_pos(position));
        printf("world-clock player walk: version %d passed\n", version);
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cc", default=shutil.which("clang") or r"C:\msys64\ucrt64\bin\clang.exe")
    parser.add_argument("--source", type=Path, default=ROOT / "runtime/frame_rate.c")
    args = parser.parse_args()
    source = args.source.read_text()
    start = source.index("static int read_guest(")
    end = source.index("static void speed_report(", start)
    with tempfile.TemporaryDirectory(prefix="world-clock-", dir=ROOT / "build") as directory:
        directory = Path(directory)
        path, exe = directory / "check.c", directory / "check.exe"
        path.write_text(HARNESS.replace("PRODUCTION", source[start:end]))
        subprocess.run([args.cc, "-std=c11", "-O2", "-Wall", "-Werror", str(path), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
