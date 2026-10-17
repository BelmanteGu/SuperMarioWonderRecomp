// NX_SAMPLE=<hz> (default 997): a SIGPROF sampling profiler for hunting CPU cost where `perf` is not allowed
// (kernel.perf_event_paranoid). Every sample is the interrupted thread and its program counter; every 20 s the top
// buckets per thread are logged as `module+0xoffset` (resolve with `addr2line -f -e <module> 0x...`).
// Linux x86-64 hosts only (glibc backtrace, REG_RIP): an empty file on Android, macOS and Windows.
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <nx/runtime.h>

namespace {

constexpr size_t kRing = 1 << 18;
constexpr int kDepth = 14;
struct Sample { uint32_t tid; uint64_t pc; uint64_t bt[kDepth]; int n; };
Sample g_ring[kRing];
std::atomic<uint64_t> g_head{0};

void handler(int, siginfo_t*, void* uc) {
    auto* c = static_cast<ucontext_t*>(uc);
    uint64_t k = g_head.fetch_add(1, std::memory_order_relaxed);
    Sample& sm = g_ring[k % kRing];
    sm.tid = uint32_t(syscall(SYS_gettid));
    sm.pc = uint64_t(c->uc_mcontext.gregs[REG_RIP]);
    void* frames[kDepth + 3];
    int n = backtrace(frames, kDepth + 3);  // the handler, the trampoline, then the interrupted code
    n = n > 2 ? n - 2 : 0;
    for (int i = 0; i < n && i < kDepth; i++) sm.bt[i] = uint64_t(frames[i + 2]);
    sm.n = n < kDepth ? n : kDepth;
}

std::string comm(uint32_t tid) {
    char p[64], b[64] = "?";
    std::snprintf(p, sizeof p, "/proc/self/task/%u/comm", tid);
    if (FILE* f = std::fopen(p, "r")) {
        if (std::fgets(b, sizeof b, f)) b[std::strcspn(b, "\n")] = 0;
        std::fclose(f);
    }
    return b;
}

void dump_stacks(uint64_t from, uint64_t to) {
    // NX_SAMPLE_STACKS=<file>: raw samples (tid, comm, module+offset per frame) appended for offline aggregation.
    const char* path = std::getenv("NX_SAMPLE_STACKS");
    if (!path) return;
    FILE* f = std::fopen(path, "a");
    if (!f) return;
    std::fprintf(f, "WINDOW\n");
    for (uint64_t k = from; k < to; k++) {
        const Sample& s = g_ring[k % kRing];
        std::fprintf(f, "S %u %s", s.tid, comm(s.tid).c_str());
        uint64_t a = s.pc;
        for (int i = -1; i < s.n; i++) {
            uint64_t pc = i < 0 ? s.pc : s.bt[i];
            Dl_info di{};
            if (dladdr(reinterpret_cast<void*>(pc), &di) && di.dli_fname)
                std::fprintf(f, " %s+%llx", (std::strrchr(di.dli_fname, '/') ? std::strrchr(di.dli_fname, '/') + 1 : di.dli_fname), (unsigned long long)(pc - uint64_t(di.dli_fbase)));
            else std::fprintf(f, " ?+%llx", (unsigned long long)pc);
        }
        (void)a;
        std::fprintf(f, "\n");
    }
    std::fclose(f);
}

void report(uint64_t from, uint64_t to) {
    dump_stacks(from, to);
    std::map<uint32_t, uint64_t> per_thread;
    std::map<std::pair<uint32_t, uint64_t>, uint64_t> buckets;
    std::map<std::pair<uint32_t, std::string>, uint64_t> modules;
    for (uint64_t k = from; k < to; k++) {
        const Sample& s = g_ring[k % kRing];
        per_thread[s.tid]++;
        buckets[{s.tid, s.pc >> 4}]++;
        Dl_info di{};
        std::string m = dladdr(reinterpret_cast<void*>(s.pc), &di) && di.dli_fname ? di.dli_fname : "?";
        modules[{s.tid, m.substr(m.rfind('/') + 1)}]++;
    }
    nx::log("sample: %llu samples in the window", (unsigned long long)(to - from));
    for (auto& [tid, n] : per_thread) {
        if (n < 20) continue;
        nx::log("sample: thread %u (%s): %llu samples", tid, comm(tid).c_str(), (unsigned long long)n);
        for (auto& [k, v] : modules) if (k.first == tid && v * 50 >= n) nx::log("sample:   module %s: %llu (%.0f%%)", k.second.c_str(), (unsigned long long)v, 100.0 * v / n);
        std::vector<std::pair<uint64_t, uint64_t>> top;
        for (auto& [k, v] : buckets) if (k.first == tid) top.push_back({v, k.second << 4});
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 400; i++) {
            Dl_info di{};
            const void* a = reinterpret_cast<void*>(top[i].second);
            if (dladdr(a, &di) && di.dli_fname) {
                const char* nm = di.dli_sname ? di.dli_sname : "";
                nx::log("sample:   %5.1f%% %s+0x%llx (%s)", 100.0 * top[i].first / n, di.dli_fname,
                        (unsigned long long)(top[i].second - uint64_t(di.dli_fbase)), nm);
            } else nx::log("sample:   %5.1f%% ?0x%llx", 100.0 * top[i].first / n, (unsigned long long)top[i].second);
        }
    }
}

struct Init {
    Init() {
        const char* e = std::getenv("NX_SAMPLE");
        if (!e) return;
        int hz = std::atoi(e);
        if (hz <= 1) hz = 997;
        { void* warm[4]; backtrace(warm, 4); }
        struct sigaction sa{};
        sa.sa_sigaction = handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGPROF, &sa, nullptr);
        itimerval it{{0, 1000000 / hz}, {0, 1000000 / hz}};
        setitimer(ITIMER_PROF, &it, nullptr);
        std::thread([] {
            uint64_t from = 0;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(20));
                uint64_t to = g_head.load();
                if (to - from > kRing) from = to - kRing;
                report(from, to);
                from = to;
            }
        }).detach();
    }
} g_init;

}  // namespace

#endif  // Linux x86-64
