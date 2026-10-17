// Guest address space on the host: fixed reserved ranges, commit on demand, and a Horizon-style memory map
// for svcQueryMemory.
#include <map>
#include <mutex>

#if defined(__APPLE__)
#include <sys/mman.h>
#endif

#include <nx/runtime.h>

#include "platform.h"

namespace nx {

uint64_t g_alias_base = kAliasBase;
uint64_t g_alias_size = kAliasSize;
uint64_t g_stack_base = kStackBase;
uint64_t g_kernel_base = kKernelBase;

namespace {

std::mutex g_vm_lock;
std::map<uint64_t, VmInfo> g_vm;  // start -> block (non-overlapping; gaps are free)
uint64_t g_kernel_next = kKernelBase;
bool g_memory_inited = false;

#if defined(__APPLE__)
static bool regions_overlap(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
    return a < b + bs && a + as > b;
}

static void unreserve_macos(uint64_t addr, uint64_t size) {
    constexpr uint64_t kChunk = 0x4000000;
    for (uint64_t off = 0; off < size; off += kChunk)
        munmap(reinterpret_cast<void*>(addr + off), std::min(kChunk, size - off));
}

void reserve_alias_macos() {
    struct {
        uint64_t base, max;
        const char* name;
    } slots[] = {
        // Gap before the heap is often free when the flush-after-heap alias VA is taken (macOS 26+).
        {kModuleBase + 0x20000000ull, kHeapBase - (kModuleBase + 0x20000000ull), "module/heap gap"},
        {kAliasBase, kAliasSize, "after heap"},
        {0x2000000000ull, 0x800000000ull, "low"},
    };
    for (auto& s : slots) {
        for (uint64_t sz = s.max; sz >= 0x08000000; sz -= 0x08000000) {
            if (plat::reserve(s.base, sz)) {
                g_alias_base = s.base;
                g_alias_size = sz;
                log("memory: alias (%s) at %llx (+%llx)", s.name, (unsigned long long)g_alias_base,
                    (unsigned long long)g_alias_size);
                return;
            }
        }
    }
    fatal("cannot reserve alias on this Mac (tried after-heap, module/heap gap, and low region)");
}

void reserve_stack_kernel_macos() {
    constexpr uint64_t align = 0x10000000ull;
    const uint64_t alias_end = g_alias_base + g_alias_size;
    uint64_t after_alias = (alias_end + align - 1) & ~(align - 1);
    uint64_t stack_try[8];
    size_t n = 0;
    stack_try[n++] = kStackBase;
    stack_try[n++] = 0x800000000ull;
    stack_try[n++] = 0xA00000000ull;  // older macOS / if the high VA hole moves
    if (after_alias >= kHeapBase + kHeapMax) stack_try[n++] = after_alias;
    for (size_t i = 0; i < n; i++) {
        uint64_t sb = stack_try[i];
        if (regions_overlap(sb, kStackSize, g_alias_base, g_alias_size)) continue;
        if (regions_overlap(sb, kStackSize, kHeapBase, kHeapMax)) continue;
        if (!plat::reserve(sb, kStackSize)) continue;
        g_stack_base = sb;

        uint64_t kernel_try[6];
        size_t kn = 0;
        kernel_try[kn++] = kKernelBase;
        kernel_try[kn++] = (sb + kStackSize + align - 1) & ~(align - 1);
        kernel_try[kn++] = 0xE00000000ull;
        for (size_t j = 0; j < kn; j++) {
            uint64_t kb = kernel_try[j];
            bool dup = false;
            for (size_t k = 0; k < j; k++)
                if (kernel_try[k] == kb) dup = true;
            if (dup) continue;
            if (regions_overlap(kb, kKernelSize, g_alias_base, g_alias_size)) continue;
            if (regions_overlap(kb, kKernelSize, kHeapBase, kHeapMax)) continue;
            if (regions_overlap(kb, kKernelSize, sb, kStackSize)) continue;
            if (plat::reserve(kb, kKernelSize)) {
                g_kernel_base = kb;
                g_kernel_next = kb;
                log("memory: stack at %llx (+%llx), kernel at %llx", (unsigned long long)g_stack_base,
                    (unsigned long long)kStackSize, (unsigned long long)g_kernel_base);
                return;
            }
        }
        unreserve_macos(sb, kStackSize);
    }
    fatal("cannot reserve stack/kernel on this Mac (tried %zu stack bases)", n);
}
#endif

void reserve(uint64_t addr, uint64_t size, const char* what) {
    if (!plat::reserve(addr, size))
        fatal("cannot reserve %s at %llx (+%llx): the range is in use", what, (unsigned long long)addr,
              (unsigned long long)size);
}

}  // namespace

void memory_init() {
    if (g_memory_inited) return;
    g_memory_inited = true;
    reserve(kModuleBase, 0x20000000, "modules");
    reserve(kHeapBase, kHeapMax, "heap");
#if defined(__APPLE__)
    reserve_alias_macos();
    reserve_stack_kernel_macos();
#else
    reserve(g_alias_base, g_alias_size, "alias");
    reserve(g_stack_base, kStackSize, "stack");
    reserve(g_kernel_base, kKernelSize, "kernel");
#endif
    // The thunk range only names HLE functions: calls to it are dispatched by address (loader.cpp) and nothing is ever
    // mapped, read or run there, so a host mapping already in it is harmless. On a POCO F6 (Android 15, Adreno 735,
    // Xiaomi 24069PC21G) something sat at 0x6F00000000 and this was fatal: the app closed as soon as the game started.
    if (!plat::reserve(kThunkBase, kThunkSize))
        log("memory: thunk range %llx (+%llx) already in use on this device: not reserved (only an address range for "
            "HLE calls, never mapped)", (unsigned long long)kThunkBase, (unsigned long long)kThunkSize);
    // Only the alias and stack regions are free for the guest to pick addresses in (nn::os maps shared memory,
    // transfer memory and thread stacks at random free addresses); everything else is reported inaccessible.
    uint64_t end = kAddressSpaceBase + kAddressSpaceSize;
    vm_set(kAddressSpaceBase, g_alias_base - kAddressSpaceBase, kMemInaccessible, 0);
    if (g_alias_base + g_alias_size < g_stack_base)
        vm_set(g_alias_base + g_alias_size, g_stack_base - (g_alias_base + g_alias_size), kMemInaccessible, 0);
    vm_set(g_stack_base + kStackSize, end - (g_stack_base + kStackSize), kMemInaccessible, 0);
}

bool commit(uint64_t addr, uint64_t size) {
    return plat::commit(addr, size);
}

void decommit(uint64_t addr, uint64_t size) {
    plat::decommit(addr, size);
}

uint64_t kernel_alloc(uint64_t size, uint64_t align) {
    std::lock_guard lk(g_vm_lock);
    uint64_t a = (g_kernel_next + align - 1) & ~(align - 1);
    size = (size + 0xFFF) & ~0xFFFull;
    if (a + size > g_kernel_base + kKernelSize) fatal("kernel region exhausted");
    g_kernel_next = a + size;
    if (!commit(a, size)) fatal("kernel_alloc commit failed");
    return a;
}

void vm_set(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm) {
    std::lock_guard lk(g_vm_lock);
    uint64_t end = addr + size;
    // Cut overlapping blocks.
    auto it = g_vm.lower_bound(addr);
    if (it != g_vm.end()) {
        auto p = std::prev(it);
        if (p->second.addr + p->second.size > addr) it = p;
    }
    while (it != g_vm.end() && it->second.addr < end) {
        VmInfo b = it->second;
        it = g_vm.erase(it);
        if (b.addr < addr) g_vm[b.addr] = {b.addr, addr - b.addr, b.state, b.perm};
        if (b.addr + b.size > end) g_vm[end] = {end, b.addr + b.size - end, b.state, b.perm};
    }
    if (state != kMemFree) g_vm[addr] = {addr, size, state, perm};
}

VmInfo vm_query(uint64_t addr) {
    std::lock_guard lk(g_vm_lock);
    auto it = g_vm.upper_bound(addr);
    uint64_t gap_end = it == g_vm.end() ? kAddressSpaceBase + kAddressSpaceSize : it->first;
    if (it != g_vm.begin()) {
        auto p = std::prev(it);
        if (addr < p->second.addr + p->second.size) return p->second;
        uint64_t gap_start = p->second.addr + p->second.size;
        return {gap_start, gap_end - gap_start, kMemFree, 0};
    }
    uint64_t gap_start = addr < kAddressSpaceBase ? 0 : kAddressSpaceBase;
    if (addr < kAddressSpaceBase) gap_end = kAddressSpaceBase;
    return {gap_start, gap_end - gap_start, kMemFree, 0};
}

}  // namespace nx
