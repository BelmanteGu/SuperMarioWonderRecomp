// Where a frame's threads run, and how evenly frames reach the display (docs/nvn/android-perf/PLAN.md, phase 0 and
// R2). Every 600 ticks each frame thread logs which kind of core it was found on (Android: clusters by their highest
// clock, little/mid/prime on a Snapdragon 8 Gen 2) and how much of the wall clock it spent on a CPU; the presenting
// thread also logs a histogram of the intervals between presents and the thermal headroom.
//
// NX_AFFINITY (Android): 1 puts the thread that replays and presents on the fastest cluster and the game's main
// thread on the middle ones; 2 puts both on everything but the slowest cluster. Re-applied every 60 ticks (Android
// moves an app's threads between cpusets). Unset or 0: the scheduler decides, as before.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif
#include <string>
#include <vector>

#include "platform.h"

#if defined(__linux__) && !defined(__ANDROID__)
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <unistd.h>
extern char** environ;
#endif
#if defined(__ANDROID__)
#include <dlfcn.h>
#include <sched.h>
#include <unistd.h>
#endif

#include <nx/runtime.h>

namespace nx::plat {

namespace {

uint64_t thread_cpu_ns() {
#if defined(CLOCK_THREAD_CPUTIME_ID)
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
#else
    return 0;
#endif
}

#if defined(__ANDROID__)
// Cluster of each CPU, 0 = slowest, from cpufreq's highest clock.
struct Cpus {
    std::vector<int> cluster;
    int clusters = 0;
};

const Cpus& cpus() {
    static const Cpus c = [] {
        Cpus x;
        long n = sysconf(_SC_NPROCESSORS_CONF);
        std::vector<long> freq;
        for (long k = 0; k < n && k < 32; k++) {
            char path[96];
            std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq", k);
            long f = 0;
            if (FILE* fp = std::fopen(path, "r")) {
                if (std::fscanf(fp, "%ld", &f) != 1) f = 0;
                std::fclose(fp);
            }
            freq.push_back(f);
        }
        std::vector<long> distinct = freq;
        std::sort(distinct.begin(), distinct.end());
        distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
        for (long f : freq)
            x.cluster.push_back(int(std::lower_bound(distinct.begin(), distinct.end(), f) - distinct.begin()));
        x.clusters = int(distinct.size());
        std::string desc;
        for (size_t k = 0; k < freq.size(); k++)
            desc += (k ? " " : "") + std::to_string(freq[k] / 1000) + "/" + std::to_string(x.cluster[k]);
        log("threads: %ld CPUs, MHz/cluster: %s", n, desc.c_str());
        return x;
    }();
    return c;
}

int affinity_mode() {
    static const int m = [] {
        const char* e = std::getenv("NX_AFFINITY");
        return e ? std::atoi(e) : 0;
    }();
    return m;
}

void place(int role) {
    const Cpus& c = cpus();
    int mode = affinity_mode();
    if (!mode || c.clusters < 2) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    int top = c.clusters - 1;
    for (size_t k = 0; k < c.cluster.size(); k++) {
        int cl = c.cluster[k];
        bool in = mode == 2 ? cl > 0 : role == 0 ? cl == top : (c.clusters > 2 ? cl > 0 && cl < top : cl == top);
        if (in) CPU_SET(int(k), &set);
    }
    if (sched_setaffinity(0, sizeof set, &set) == 0) return;
    // The prime core paused (Android takes it offline when the phone is hot): every big core instead, not wherever
    // the scheduler left the thread (the little ones included).
    CPU_ZERO(&set);
    for (size_t k = 0; k < c.cluster.size(); k++)
        if (c.cluster[k] > 0) CPU_SET(int(k), &set);
    static uint64_t failed = 0;
    if (failed++ % 100 == 0)
        log("threads: sched_setaffinity to role %d's cores failed %llu times: all big cores instead", role,
            (unsigned long long)failed);
    sched_setaffinity(0, sizeof set, &set);
}

float thermal_headroom() {
    using Acquire = void* (*)();
    using Headroom = float (*)(void*, int);
    static void* mgr = nullptr;
    static Headroom headroom = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (void* lib = dlopen("libandroid.so", RTLD_NOW)) {
            auto acquire = reinterpret_cast<Acquire>(dlsym(lib, "AThermal_acquireManager"));
            headroom = reinterpret_cast<Headroom>(dlsym(lib, "AThermal_getThermalHeadroom"));
            if (acquire && headroom) mgr = acquire();
        }
    }
    return mgr && headroom ? headroom(mgr, 0) : -1.0f;
}
#endif

struct Role {
    const char* name;
    uint64_t ticks = 0, cpu_from = 0, wall_from = 0;
    uint64_t on[4] = {};  // ticks found on each cluster (Android)
    long faults_from[2] = {}, switches_from[2] = {};  // major/minor page faults, voluntary/involuntary switches
};

// This thread's page faults and context switches (Linux and Android: RUSAGE_THREAD; elsewhere none).
void thread_usage(long faults[2], long switches[2]) {
    faults[0] = faults[1] = switches[0] = switches[1] = 0;
#if defined(RUSAGE_THREAD)
    rusage u{};
    if (getrusage(RUSAGE_THREAD, &u) == 0)
        faults[0] = u.ru_majflt, faults[1] = u.ru_minflt, switches[0] = u.ru_nvcsw, switches[1] = u.ru_nivcsw;
#endif
}
Role g_roles[2] = {{"replay+present"}, {"game main"}};

}  // namespace

void thread_counters(uint64_t& cpu_ns, long faults[2], long switches[2]) {
    cpu_ns = thread_cpu_ns();
    thread_usage(faults, switches);
}


// Linux desktop (power-profiles-daemon): under its power-saver profile (and a laptop's "low-power" platform profile)
// the CPU's energy preference is "power" and the package power limit low; the game's threads, which wake every few
// milliseconds and sleep again, then run at a fraction of the clock: on the Intel laptop of docs/nvn/studies/
// intel-60fps-1710.md the same scripted scene ran at 54 fps under power-saver and at 60 under performance. A helper
// (`powerprofilesctl launch -p performance -- tail --pid=<us> -f /dev/null`, the way GameMode and Steam do it) holds
// the profile until this process ends; the daemon puts the user's profile back by itself. Nothing happens when
// powerprofilesctl or the daemon is missing. NX_POWER_PROFILE=0 never asks.
void request_performance_profile() {
#if defined(__linux__) && !defined(__ANDROID__)
    static bool done = false;
    if (done) return;
    done = true;
    const char* e = std::getenv("NX_POWER_PROFILE");
    if (e && e[0] == '0') return;
    char pid[32];
    std::snprintf(pid, sizeof pid, "--pid=%d", int(getpid()));
    char* argv[] = {const_cast<char*>("powerprofilesctl"), const_cast<char*>("launch"), const_cast<char*>("-p"),
                    const_cast<char*>("performance"), const_cast<char*>("-r"),
                    const_cast<char*>("a game is running (nx runtime)"), const_cast<char*>("--"),
                    const_cast<char*>("tail"), pid, const_cast<char*>("-f"), const_cast<char*>("/dev/null"), nullptr};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t child = 0;
    int rc = posix_spawnp(&child, "powerprofilesctl", &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    log("power: performance profile requested through powerprofilesctl (%s)", rc == 0 ? "started" : "not available");
#endif
}

void frame_thread_tick(int role) {
    Role& r = g_roles[role & 1];
#if defined(__ANDROID__)
    if (r.ticks % 60 == 0) place(role);
    int cpu = sched_getcpu();
    const Cpus& c = cpus();
    if (cpu >= 0 && size_t(cpu) < c.cluster.size()) r.on[std::min(c.cluster[size_t(cpu)], 3)]++;
#endif
    uint64_t now = monotonic_ns();
    if (!r.wall_from) r.wall_from = now, r.cpu_from = thread_cpu_ns(), thread_usage(r.faults_from, r.switches_from);
    if (++r.ticks % 600) return;
    uint64_t cpu_ns = thread_cpu_ns();
    long faults[2], switches[2];
    thread_usage(faults, switches);
    log("threads: %s: %ld major and %ld minor page faults, %ld voluntary and %ld involuntary switches over 600", r.name,
        faults[0] - r.faults_from[0], faults[1] - r.faults_from[1], switches[0] - r.switches_from[0],
        switches[1] - r.switches_from[1]);
    for (int k = 0; k < 2; k++) r.faults_from[k] = faults[k], r.switches_from[k] = switches[k];
    double busy = now > r.wall_from ? 100.0 * double(cpu_ns - r.cpu_from) / double(now - r.wall_from) : 0.0;
#if defined(__ANDROID__)
    log("threads: %s (tid %lld) on clusters 0/1/2/3: %llu/%llu/%llu/%llu of 600, on a CPU %.0f%% of the time, "
        "thermal headroom %.2f",
        r.name, (long long)current_thread_id(), (unsigned long long)r.on[0], (unsigned long long)r.on[1],
        (unsigned long long)r.on[2], (unsigned long long)r.on[3], busy, double(thermal_headroom()));
#else
    log("threads: %s (tid %lld) on a CPU %.0f%% of the time", r.name, (long long)current_thread_id(), busy);
#endif
    for (auto& o : r.on) o = 0;
    r.wall_from = now, r.cpu_from = cpu_ns;
}

void frame_presented() {
    frame_thread_tick(0);
    static uint64_t last = 0, n = 0, bins[7] = {};
    uint64_t now = monotonic_ns();
    if (last) {
        double ms = double(now - last) / 1e6;
        bins[ms < 14 ? 0 : ms < 19 ? 1 : ms < 25 ? 2 : ms < 31 ? 3 : ms < 36 ? 4 : ms < 50 ? 5 : 6]++;
    }
    last = now;
    if (++n % 600) return;
    log("present intervals over 600: <14 ms %llu, 14-19 %llu, 19-25 %llu, 25-31 %llu, 31-36 %llu, 36-50 %llu, "
        "more %llu",
        (unsigned long long)bins[0], (unsigned long long)bins[1], (unsigned long long)bins[2],
        (unsigned long long)bins[3], (unsigned long long)bins[4], (unsigned long long)bins[5],
        (unsigned long long)bins[6]);
    for (auto& b : bins) b = 0;
}

}  // namespace nx::plat
