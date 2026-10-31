// Reserve guest address ranges without replacing existing host mappings.
#ifndef TOTK_NATIVE_GUEST_MEMORY_H
#define TOTK_NATIVE_GUEST_MEMORY_H
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdint.h>

static uint8_t *native_guest_reserve_gap(uint64_t size, uint64_t first, uint64_t limit) {
    const uint64_t alignment = 2ULL << 20;
    if (!size || first >= limit || size > limit - first) return NULL;
    unsigned races = 0;
    while (first < limit) {
        mach_vm_address_t region = first;
        mach_vm_size_t length = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t status = mach_vm_region(mach_task_self(), &region, &length,
            VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (status != KERN_SUCCESS && status != KERN_INVALID_ADDRESS) return NULL;
        uint64_t end = status == KERN_SUCCESS && region < limit ? region : limit;
        if (first > UINT64_MAX - (alignment - 1)) return NULL;
        uint64_t start = (first + alignment - 1) & ~(alignment - 1);
        if (start < end && size <= end - start) {
            mach_vm_address_t address = start;
            kern_return_t mapped = mach_vm_map(mach_task_self(), &address, size, 0, VM_FLAGS_FIXED,
                MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_ALL,
                VM_INHERIT_DEFAULT);
            if (mapped == KERN_SUCCESS) return (uint8_t *)(uintptr_t)address;
            // Another thread may have occupied just the beginning of this gap.
            // Refresh the map before skipping its still-usable remainder.
            if (mapped == KERN_NO_SPACE && races++ < 3) continue;
        }
        if (status != KERN_SUCCESS || region >= limit || length >= limit - region) break;
        if (region + length <= first) break;
        first = region + length;
        races = 0;
    }
    return NULL;
}
#endif
