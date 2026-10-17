// Real Darwin reservations with the same SDL frameworks as the game; no game data or generated code needed.
#include <SDL3/SDL.h>
#include <sys/mman.h>

#include <cstdio>

#include <nx/module.h>
#include <nx/runtime.h>

// Generated tables capture NX_BASE during ordinary global initialization, before main().
static const nx::ModuleInfo captured_module{"test", NX_BASE};

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) return 1;
    using namespace nx;
    if (captured_module.base != kModuleBase || kModuleBase != mac_address_space().modules) return 2;
    struct Range { const char* name; uint64_t addr, size; };
    const Range ranges[] = {{"modules", kModuleBase, 0x20000000}, {"heap", kHeapBase, kHeapMax},
                            {"alias", kAliasBase, kAliasSize}, {"stack", kStackBase, kStackSize},
                            {"kernel", kKernelBase, kKernelSize}};
    for (const auto& r : ranges) {
        if (r.addr < 0x100000000ull || r.addr >= (1ull << 39) || r.size > (1ull << 39) - r.addr) return 3;
        if (!mac_address_reserved(r.addr, r.size) || mac_address_reserved(r.addr, r.size + 0x4000)) return 4;
        for (const auto& other : ranges)
            if (&r != &other && r.addr < other.addr + other.size && other.addr < r.addr + r.size) return 5;
        if (mprotect(reinterpret_cast<void*>(r.addr), 0x4000, PROT_READ | PROT_WRITE)) return 6;
        auto* value = reinterpret_cast<volatile uint64_t*>(r.addr);
        *value = r.addr;
        if (*value != r.addr) return 7;
        std::printf("%s: %llx + %llx reserved and writable\n", r.name,
                    static_cast<unsigned long long>(r.addr), static_cast<unsigned long long>(r.size));
    }
    if (kAliasBase + kAliasSize > kStackBase || kStackBase + kStackSize > kKernelBase ||
        kHeapBase + kHeapMax > kKernelBase || kModuleBase > kHeapBase) return 8;
    SDL_Quit();
    std::puts("PASS: initialization order, non-overlap, 39-bit bounds and page commits");
}
