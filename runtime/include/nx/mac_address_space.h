#pragma once

#include <cstdint>

namespace nx {

// Reserved before generated ModuleInfo tables capture their load addresses.
struct MacAddressSpace {
    uint64_t modules, heap, alias, stack, kernel, thunks, alias_size;
    bool mapped_thunks;
    MacAddressSpace();
};

inline const MacAddressSpace& mac_address_space() {
    static const MacAddressSpace layout;
    return layout;
}
bool mac_address_reserved(uint64_t addr, uint64_t size);

}  // namespace nx
