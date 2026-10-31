"""Exercise the guest allocator inside a deliberately fragmented Mach VM map."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix='totk-guest-memory-') as tmp:
        work = Path(tmp)
        source = work / 'memory.c'
        source.write_text(r'''
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <assert.h>
#include <stdio.h>
static int occupy_before_map;
static kern_return_t racing_map(vm_map_t task, mach_vm_address_t *address,
    mach_vm_size_t size, mach_vm_offset_t mask, int flags, mem_entry_name_port_t object,
    memory_object_offset_t offset, boolean_t copy, vm_prot_t current,
    vm_prot_t maximum, vm_inherit_t inheritance) {
    if (occupy_before_map) {
        occupy_before_map = 0;
        mach_vm_address_t occupied = *address;
        assert(mach_vm_map(task, &occupied, 16384, 0, VM_FLAGS_FIXED,
            MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_ALL,
            VM_INHERIT_DEFAULT) == KERN_SUCCESS);
    }
    return mach_vm_map(task, address, size, mask, flags, object, offset, copy,
                       current, maximum, inheritance);
}
#define mach_vm_map racing_map
#include "native_guest_memory.h"
#undef mach_vm_map
int main(void) {
    const uint64_t gib = 1ULL << 30, mib = 1ULL << 20;
    mach_vm_address_t base = 0;
    assert(mach_vm_map(mach_task_self(), &base, 10*gib, gib-1, VM_FLAGS_ANYWHERE,
        MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_ALL,
        VM_INHERIT_DEFAULT) == KERN_SUCCESS);
    uint64_t hole = base + 16*mib, hole_size = 6*gib + 32*mib;
    assert(mach_vm_deallocate(mach_task_self(), hole, hole_size) == KERN_SUCCESS);
    // A 1 GiB search grid skips this usable 6 GiB gap.
    assert(((hole+gib-1)&~(gib-1)) + 6*gib > hole+hole_size);
    uint8_t *found = native_guest_reserve_gap(6*gib, base, base+10*gib);
    assert((uintptr_t)found == hole);
    assert(!native_guest_reserve_gap(6*gib, base, base+10*gib));
    assert(mach_vm_deallocate(mach_task_self(), hole, 6*gib) == KERN_SUCCESS);
    assert(!native_guest_reserve_gap(6*gib, base, hole+6*gib-1));
    assert((uintptr_t)native_guest_reserve_gap(6*gib, base, hole+6*gib) == hole);
    assert(!native_guest_reserve_gap(0, base, base+10*gib));
    assert(!native_guest_reserve_gap(1, base+10*gib, base));
    assert(!native_guest_reserve_gap(UINT64_MAX, base, base+10*gib));
    assert(!native_guest_reserve_gap(1, UINT64_MAX-1, UINT64_MAX));
    assert(mach_vm_deallocate(mach_task_self(), hole, 6*gib) == KERN_SUCCESS);
    occupy_before_map = 1;
    assert((uintptr_t)native_guest_reserve_gap(6*gib, base, base+10*gib) == hole+2*mib);
    assert(!occupy_before_map);
    assert(mach_vm_deallocate(mach_task_self(), base, 10*gib) == KERN_SUCCESS);
    puts("{\"probe\":\"guest-memory-gaps\",\"status\":\"pass\",\"checks\":11}");
}
''')
        executable = work / 'memory'
        subprocess.run(['xcrun', 'clang', '-O2', '-I', ROOT / 'runtime/macos',
                        source, '-o', executable], check=True)
        subprocess.run([executable], check=True)


if __name__ == '__main__':
    main()
