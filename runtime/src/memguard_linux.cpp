// Memory guard (Linux), as memguard_mac.cpp: the process's resident memory (/proc/self/statm; the guest heap, host
// allocations and what the GPU driver maps into us) is checked every 250 ms; past NX_MEM_LIMIT_GB (default 8) the
// process ends at once, so a leak can never push the machine into swap until it freezes. The footprint is logged
// every 30 s so growth shows early.
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <nx/runtime.h>

#include "platform.h"

namespace nx::plat {

namespace {

uint64_t footprint() {
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long long size = 0, resident = 0;
    int n = std::fscanf(f, "%llu %llu", &size, &resident);
    std::fclose(f);
    return n == 2 ? uint64_t(resident) * uint64_t(sysconf(_SC_PAGESIZE)) : 0;
}

}  // namespace

void start_memory_guard() {
    const char* e = std::getenv("NX_MEM_LIMIT_GB");
    double gb = e ? std::atof(e) : 8.0;
    if (gb <= 0) gb = 8.0;
    const uint64_t limit = uint64_t(gb * double(1ull << 30));
    log("memory: guard on, limit %.1f GB (NX_MEM_LIMIT_GB)", gb);
    std::thread([limit, gb] {
        uint64_t ticks = 0, peak = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            uint64_t f = footprint();
            if (f > peak) peak = f;
            if (++ticks % 120 == 0)
                log("memory: %.2f GB (peak %.2f GB, limit %.1f GB)", double(f) / double(1ull << 30),
                    double(peak) / double(1ull << 30), gb);
            if (f > limit) {
                char msg[160];
                std::snprintf(msg, sizeof msg, "memory: %.2f GB is over the %.1f GB limit (NX_MEM_LIMIT_GB): stopping",
                              double(f) / double(1ull << 30), gb);
                log("%s", msg);
                std::fprintf(stderr, "%s\n", msg);
                std::fflush(nullptr);
                _exit(3);
            }
        }
    }).detach();
}

uint64_t memguard_overlap_ns(uint64_t, uint64_t) { return 0; }

}  // namespace nx::plat
