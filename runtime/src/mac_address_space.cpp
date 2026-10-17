// Darwin's shared cache and malloc reservations vary across OS releases and launches. Reserve the guest before
// the generated module tables initialize; never overwrite a host mapping or leave Horizon's 39-bit address space.
#include <mach/mach.h>
#include <mach/mach_vm.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include <nx/runtime.h>

namespace nx {
namespace {

struct Range { uint64_t addr, size; };

bool reserve_at(uint64_t addr, uint64_t size) {
    mach_vm_address_t a = addr;
    return mach_vm_map(mach_task_self(), &a, size, 0, VM_FLAGS_FIXED, MEMORY_OBJECT_NULL, 0, FALSE,
                       VM_PROT_NONE, VM_PROT_READ | VM_PROT_WRITE, VM_INHERIT_DEFAULT) == KERN_SUCCESS;
}

void release(const Range* ranges, size_t count) {
    while (count) {
        const auto& r = ranges[--count];
        mach_vm_deallocate(mach_task_self(), r.addr, r.size);
    }
}

uint64_t reserve_free(uint64_t begin, uint64_t end, uint64_t size) {
    constexpr uint64_t alignment = 0x100000;
    uint64_t cursor = (begin + alignment - 1) & ~(alignment - 1);
    while (cursor <= end && size <= end - cursor) {
        mach_vm_address_t next = cursor;
        mach_vm_size_t length = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        auto result = mach_vm_region(mach_task_self(), &next, &length, VM_REGION_BASIC_INFO_64,
                                     reinterpret_cast<vm_region_info_t>(&info), &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (result != KERN_SUCCESS && result != KERN_INVALID_ADDRESS) return 0;
        uint64_t gap_end = result == KERN_SUCCESS ? std::min(uint64_t(next), end) : end;
        if (gap_end >= cursor && size <= gap_end - cursor && reserve_at(cursor, size)) return cursor;
        if (result != KERN_SUCCESS || next >= end || length > end - next) return 0;
        cursor = (next + length + alignment - 1) & ~(alignment - 1);
    }
    return 0;
}

}  // namespace

MacAddressSpace::MacAddressSpace() {
    constexpr uint64_t legacy_alias_size = 0x400000000ull;
    const Range legacy[] = {{0x300000000ull, 0x20000000}, {0x400000000ull, kHeapMax},
                            {0x600000000ull, legacy_alias_size}, {0xA00000000ull, kStackSize},
                            {0xB00000000ull, kKernelSize}, {0xE00000000ull, kThunkSize}};
    size_t count = 0;
    while (count < 6 && reserve_at(legacy[count].addr, legacy[count].size)) ++count;
    if (count >= 5) {
        modules = legacy[0].addr; heap = legacy[1].addr; alias = legacy[2].addr;
        stack = legacy[3].addr; kernel = legacy[4].addr; thunks = legacy[5].addr;
        mapped_thunks = count == 6;
        alias_size = legacy_alias_size;
        return;
    }
    release(legacy, count);

    // The low window fits the modules, while large anonymous mappings belong above Darwin's VA hole. Try the
    // heap on either side of the alias/stack pair so a malloc reservation cannot fragment the only usable order.
    modules = reserve_free(0x100000000ull, 0x180000000ull, 0x20000000);
    if (!modules) modules = reserve_free(0x7000000000ull, 0x8000000000ull, 0x20000000);
    if (modules) {
        const unsigned orders[][4] = {{0, 1, 2, 3}, {1, 0, 2, 3}, {1, 2, 0, 3}};
        // The alias region is virtual address space advertised through svcGetInfo, not physical memory. Keep
        // 16 GiB when it fits; 8 GiB also leaves room for the game's mappings when host malloc fragments that gap.
        for (uint64_t alias_bytes : {legacy_alias_size, uint64_t(0x200000000ull)}) {
            const uint64_t sizes[] = {kHeapMax, alias_bytes, kStackSize, kKernelSize};
            for (const auto& order : orders) {
                Range reserved[4]{};
                uint64_t addresses[4]{};
                uint64_t cursor = std::max(0x7000000000ull, modules + 0x20000000);
                size_t n = 0;
                for (unsigned kind : order) {
                    uint64_t addr = reserve_free(cursor, 0x8000000000ull, sizes[kind]);
                    if (!addr) break;
                    reserved[n++] = {addr, sizes[kind]};
                    addresses[kind] = addr;
                    cursor = addr + sizes[kind];
                }
                if (n == 4) {
                    heap = addresses[0]; alias = addresses[1]; stack = addresses[2]; kernel = addresses[3];
                    alias_size = alias_bytes;
                    // Thunks name HLE functions only: no memory is read or executed at these addresses.
                    thunks = 0x6F00000000ull;
                    mapped_thunks = reserve_at(thunks, kThunkSize);
                    return;
                }
                release(reserved, n);
            }
        }
        const Range module{modules, 0x20000000};
        release(&module, 1);
    }
    std::fputs("Cannot reserve a non-overlapping 39-bit guest address space on this Mac.\n", stderr);
    std::abort();
}

bool mac_address_reserved(uint64_t addr, uint64_t size) {
    const auto& m = mac_address_space();
    return (addr == m.modules && size == 0x20000000) || (addr == m.heap && size == kHeapMax) ||
           (addr == m.alias && size == kAliasSize) || (addr == m.stack && size == kStackSize) ||
           (addr == m.kernel && size == kKernelSize) || (m.mapped_thunks && addr == m.thunks && size == kThunkSize);
}

}  // namespace nx
