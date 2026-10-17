// NVN queues, command buffers, windows and syncs (docs/NVN.md, N3). Synchronously every command completes at
// submit: fences are signaled right away, the window hands out its textures in turn, and presents are counted. With
// NX_ASYNC_GPU a fence is the submit it follows, and waiting for it waits for that submit (nvn_frames.cpp). With
// NX_PIPELINED the GPU work is in flight when a submit returns but its compute results are in guest memory, so
// syncs and nvnQueueFinish have nothing to wait for, as synchronously (gpu_serial() is 0). Return
// values follow the SDK driver (nvnSyncWait sdk+45a960: 0 once the fence has passed; nvnWindowAcquireTexture
// sdk+45e050: 0 on success).
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <atomic>
#include <vector>
#include <map>
#ifdef __linux__
#include <dirent.h>
#include <unistd.h>
#endif

#include "nvn.h"
#include "nvn_ultrawide.h"
#include "platform.h"

namespace nx {
void svc_profile_mark(int role);
void svc_profile_report(uint64_t frames, int role);
uint64_t display_period_ns();
extern std::atomic<uint64_t> g_tick_calls, g_yield_calls, g_sleep_calls;
}
namespace nx::nvn {

// Submits the game made and submits whose replay (and GPU work, write-backs) finished. The game records the next frame
// on one thread while another submits the last one, and our replay runs inside nvnQueueSubmitCommands: a fence the
// game waits on must not pass before the submits it follows were replayed, or the game rewrites the next frame's
// vertex and uniform data while this one is still read (the horizon band, exploded meshes, DEBUG.md #59).
// NX_SYNC_LOOSE=1: the old behaviour (fences passed at once).
namespace {
// NX_TRACE_TIMELINE=<present>: from that present on, the queue events (submit, replay, sync wait, present) of 8 frames
// with their times and threads are logged (where a frame's wall clock goes).
struct TlEvent { uint64_t ns; int64_t tid; const char* what; uint64_t arg; };
TlEvent g_tl[2048];
std::atomic<uint64_t> g_tl_n{0}, g_presents{0};
uint64_t g_tl_from = UINT64_MAX;
void tl(const char* what, uint64_t arg = 0) {
    static const bool init = [] {
        if (const char* e = std::getenv("NX_TRACE_TIMELINE")) g_tl_from = uint64_t(std::atoll(e));
        return true;
    }();
    (void)init;
    if (g_tl_from == UINT64_MAX || g_presents < g_tl_from) return;
    uint64_t n = g_tl_n.fetch_add(1);
    if (n < 2048) g_tl[n] = {plat::monotonic_ns(), plat::current_thread_id(), what, arg};
}
std::mutex g_sub_lock;
std::condition_variable g_sub_cv;
uint64_t g_sub_called = 0, g_sub_done = 0;
std::atomic<uint64_t> g_sub_blocked{0};

bool sync_loose() {
    static const bool on = std::getenv("NX_SYNC_LOOSE") != nullptr;
    return on;
}

void submit_done() {
    {
        std::lock_guard lk(g_sub_lock);
        g_sub_done++;
    }
    g_sub_cv.notify_all();
}

// Where the game waits for the replay (docs/nvn/android-perf/PHASE3.md I1): calls, the ones that blocked and for how
// long, per 600 presents; frame_ns is this main-thread frame's blocked time, for the hitch lines (frame_time_probe).
struct WaitSite {
    const char* name;
    std::atomic<uint64_t> calls{0}, blocked{0}, ns{0}, frame_ns{0};
};
WaitSite g_wait_sites[kWaitSites] = {{"sync wait"}, {"queue finish"}, {"pool invalidate"}, {"pool finalize"}};

// Waits until the submits up to `seq` were replayed (2 s at most: a hung replay is logged, not waited forever).
void wait_submits(uint64_t seq, int site) {
    if (sync_loose() || !seq) return;
    WaitSite& w = g_wait_sites[site];
    w.calls++;
    std::unique_lock lk(g_sub_lock);
    if (g_sub_done >= seq) return;
    uint64_t n = ++g_sub_blocked;
    if (n == 1 || n % 1000 == 0) log("nvn: %llu CPU waits blocked on a submit still being replayed", (unsigned long long)n);
    uint64_t t0 = plat::monotonic_ns();
    if (!g_sub_cv.wait_for(lk, std::chrono::seconds(2), [seq] { return g_sub_done >= seq; }))
        log("nvn: a CPU wait gave up after 2 s on submit %llu (%llu done)", (unsigned long long)seq,
            (unsigned long long)g_sub_done);
    uint64_t dt = plat::monotonic_ns() - t0;
    w.blocked++, w.ns += dt, w.frame_ns += dt;
}

void log_wait_sites() {
    std::string line;
    for (WaitSite& w : g_wait_sites) {
        char b[128];
        std::snprintf(b, sizeof b, "; %s %llu calls, %llu blocked %.1f ms", w.name, (unsigned long long)w.calls.exchange(0),
                      (unsigned long long)w.blocked.exchange(0), double(w.ns.exchange(0)) / 1e6);
        line += b;
    }
    log("nvn: game waits on the replay over 600 presents%s", line.c_str());
}

uint64_t submits_called() {
    std::lock_guard lk(g_sub_lock);
    return g_sub_called;
}
}  // namespace

// MemoryPoolInvalidateMappedRange / Finalize (nvn_objects.cpp): the CPU reads or frees what submits made so far use.
void wait_submits_so_far(int site) { wait_submits(submits_called(), site); }

namespace {

// ---- syncs ------------------------------------------------------------------------------------------------------

struct Sync : Object {
    std::atomic<bool> signaled{false};
    std::atomic<uint64_t> serial{0};  // NX_ASYNC_GPU: the submit it waits for (0: none; always 0 otherwise)
    std::atomic<uint64_t> job{0};     // the replay-thread job that fences it (0: none)
    std::atomic<uint64_t> seq{0};     // the submits it follows (wait_submits); 0: none
};

void SyncInitialize(CPU& c) {  // NVNboolean (NVNsync*, NVNdevice*)
    make<Sync>(c.x[0]);
    c.x[0] = 1;
}

void SyncFinalize(CPU& c) { destroy(c.x[0]); }

void signal_sync(uint64_t guest) {
    if (auto* s = get<Sync>(guest)) s->serial = 0, s->job = 0, s->seq = 0, s->signaled = true;
}

// NVNsyncWaitResult (const NVNsync*, u64 timeout). Always 0 (already signaled), as before: a fenced submit is waited
// for whatever the timeout, 2 s at most (then the GPU hung and the process ends). Correct results before latency.
void SyncWait(CPU& c) {
    auto* s = get<Sync>(c.x[0]);
    tl("syncwait>");
    if (s) wait_submits(s->seq.load(), kWaitSync);
    if (s) worker_wait(s->job);
    tl("syncwait<");
    if (uint64_t serial = s ? s->serial.load() : 0) gpu_wait(serial);
    c.x[0] = 0;
}

// ---- queues -----------------------------------------------------------------------------------------------------


void QueueInitialize(CPU& c) { c.x[0] = 1; }  // NVNboolean (NVNqueue*, const NVNqueueBuilder*)
void QueueFinalize(CPU&) {}
void QueueSubmitCommands(CPU& c) {  // (NVNqueue*, int count, const NVNcommandHandle*)
    plat::perf_hint_thread();  // this thread replays: part of every frame (perf_hint.cpp)
    std::vector<std::shared_ptr<CmdList>> lists;
    static uint64_t submitted = 0, missing = 0;
    for (uint32_t k = 0; k < uint32_t(c.x[1]); k++) {
        submitted++;
        if (auto l = command_list(ld<uint64_t>(c.x[2] + 8 * k)))
            lists.push_back(std::move(l));
        else if (++missing <= 5)
            log("nvn: submitted handle %llx is unknown", (unsigned long long)ld<uint64_t>(c.x[2] + 8 * k));
    }
    static const bool trace_lists = std::getenv("NX_NVN_LISTS") != nullptr;
    if (trace_lists && g_presents > 900 && g_presents < 905)
        for (uint32_t k = 0; k < uint32_t(c.x[1]); k++) {
            auto l = command_list(ld<uint64_t>(c.x[2] + 8 * k));
            log("nvn list: submit %llx handle %llu (%zu cmds)", (unsigned long long)c.x[0],
                (unsigned long long)ld<uint64_t>(c.x[2] + 8 * k), l ? l->cmds.size() : 0);
        }
    if ((submitted & 1023) == 0) log("nvn: %llu handles submitted, %llu unknown", (unsigned long long)submitted, (unsigned long long)missing);
    // NX_DEFERRED_SNAPSHOT=1: read the Deferred arrays here; by default they are read when the submit is replayed.
    static const bool snap = std::getenv("NX_DEFERRED_SNAPSHOT") != nullptr;
    auto deferred = snap ? std::make_shared<const DeferredValues>(deferred_snapshot(lists)) : nullptr;
    {
        std::lock_guard lk(g_sub_lock);
        g_sub_called++;
    }
    tl("submit-call");
    worker_run([lists = std::move(lists), deferred] {
        tl("replay>");
        gpu_submit(lists, deferred);
        tl("replay<");
        submit_done();
    });
}
void QueueFlush(CPU&) {}
void QueueFinish(CPU&) {
    wait_submits(submits_called(), kWaitFinish);
    worker_finish();
    gpu_wait(gpu_serial());
}
// (NVNqueue*, NVNsync*, NVNsyncCondition, int flags): every condition taken as "all previous work complete".
void QueueFenceSync(CPU& c) {
    uint64_t guest = c.x[1];  // the fence goes after the work queued so far: its serial is taken on the replay thread
    if (auto* s = get<Sync>(guest)) {
        s->signaled = true;
        s->seq = submits_called();
        s->job = worker_run([guest] {
            if (auto* s = get<Sync>(guest)) s->serial = gpu_serial();
        });
    }
}
void QueueWaitSync(CPU& c) { c.x[0] = 1; }  // NVNboolean (NVNqueue*, const NVNsync*): one queue, in order

// ---- windows ----------------------------------------------------------------------------------------------------

void WindowInitialize(CPU& c) {  // NVNboolean (NVNwindow*, const NVNwindowBuilder*)
    uint64_t guest = c.x[0], builder = c.x[1];
    auto& w = make<Window>(guest);
    int n = int(driver(c, "nvnWindowBuilderGetNumTextures", {builder}));
    for (int k = 0; k < n; k++) w.textures.push_back(driver(c, "nvnWindowBuilderGetTexture", {builder, uint64_t(k)}));
    w.active = int(driver(c, "nvnWindowBuilderGetNumActiveTextures", {builder}));
    if (w.active <= 0 || w.active > n) w.active = n;
    w.interval = int(driver(c, "nvnWindowBuilderGetPresentInterval", {builder}));
    log("nvn: window %llx: %d textures (%d active), present interval %d", (unsigned long long)guest, n, w.active, w.interval);
    c.x[0] = 1;
}

void WindowFinalize(CPU& c) { destroy(c.x[0]); }

// NVNwindowAcquireTextureResult (NVNwindow*, NVNsync* textureAvailableSync, int* textureIndex)
void WindowAcquireTexture(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    int index = 0;
    if (w && w->active > 0) {
        index = w->next;
        w->next = (w->next + 1) % w->active;
    }
    if (c.x[2]) st<int32_t>(c.x[2], index);
    signal_sync(c.x[1]);
    c.x[0] = 0;
}

void WindowGetNumTextures(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    c.x[0] = w ? w->textures.size() : 0;
}

void WindowGetNumActiveTextures(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    c.x[0] = w ? uint64_t(w->active) : 0;
}

void WindowSetNumActiveTextures(CPU& c) {
    if (auto* w = get<Window>(c.x[0])) w->active = std::max(1, std::min(int(c.x[1]), int(w->textures.size())));
}

void WindowGetPresentInterval(CPU& c) {
    auto* w = get<Window>(c.x[0]);
    c.x[0] = w ? uint64_t(w->interval) : 1;
}

// Our governor over the game's (NX_GOVERNOR; on by default on Android unless NX_MAX_INTERVAL is set). The game's own
// rule (below) locks 30 fps after 3 frames over 15.83 ms; at 30 fps a phone lowers its clocks, every frame then takes
// longer, and it never leaves 30 (a Galaxy S23 sat at 30 fps in course with frames that run at 60 when allowed).
// This one holds 60 (the game's maximum clamped to 1) while it is kept: it watches the main thread's loop period over
// 120-frame windows, and when two windows in a row had over 20% of their frames past 20 ms (the game would run
// visibly slow; one window is a scene load), it hands the choice back to the game (30 fps at the right speed) for a
// while, then tries 60 again, skipping the first 90 frames of a try (the clocks ramp up from the 30-fps level). The
// wait doubles after each failed try (10 s, 20 s, ... 80 s) and resets once a try holds. Returns 1 (clamp) or 0.
uint32_t governor_max(uint64_t now) {
    static const bool on = [] {
        const char* e = std::getenv("NX_GOVERNOR");
#if defined(__ANDROID__)
        return !(e && e[0] == '0');
#else
        return e && e[0] == '1';
#endif
    }();
    if (!on) return 0;
    static uint64_t last = 0, frames = 0, slow = 0, game_until = 0, warm_until = 0, backoff = 600, n = 0;
    static int bad_windows = 0, held_windows = 0;
    uint64_t period = last ? now - last : 0;
    last = now;
    n++;
    if (n < game_until) return 0;
    if (n == game_until) frames = slow = 0, bad_windows = 0, warm_until = n + 90;
    if (n < warm_until) return 1;
    frames++, slow += period > 20'000'000ull;
    if (frames < 120) return 1;
    bool held = slow * 5 <= frames;
    if (!held || held_windows++ % 30 == 0)
        log("nvn: governor: %llu of %llu frames over 20 ms%s", (unsigned long long)slow, (unsigned long long)frames,
            held ? ": 60 fps holds" : bad_windows ? ": back to the game's choice" : " (once: still 60)");
    frames = slow = 0;
    if (held) {
        bad_windows = 0, backoff = 600;
        return 1;
    }
    held_windows = 0;
    if (++bad_windows < 2) return 1;
    game_until = n + backoff;
    backoff = std::min<uint64_t>(backoff * 2, 4800);
    return 0;
}


#ifdef __linux__
// One line per 300 main-thread frames: wall time, CPU seconds used since the last line, % of one core, per thread.
static std::map<std::string, uint64_t> g_runq_wait, g_runq_prev;
static void cpu_log_tick() {
    static int n = 0;
    static uint64_t wall0 = 0;
    static std::map<std::string, uint64_t> prev, prev_sys;
    if (++n % 300) return;
    uint64_t now = plat::monotonic_ns();
    std::map<std::string, uint64_t> cur, cur_sys;
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            char path[96], buf[512];
            snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
            FILE* f = fopen(path, "r");
            if (!f) continue;
            size_t r = fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
            buf[r] = 0;
            char* lp = strchr(buf, '(');
            char* rp = strrchr(buf, ')');
            if (!lp || !rp) continue;
            std::string name(lp + 1, rp);
            unsigned long ut = 0, st = 0;
            sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st);
            cur[name + "/" + e->d_name] = ut + st;
            cur_sys[name + "/" + e->d_name] = st;
            {  // schedstat: ns on a CPU, ns waiting on a run queue (ready but no CPU free), timeslices
                snprintf(path, sizeof path, "/proc/self/task/%s/schedstat", e->d_name);
                if (FILE* sf = fopen(path, "r")) {
                    unsigned long long run = 0, wait = 0, slices = 0;
                    if (fscanf(sf, "%llu %llu %llu", &run, &wait, &slices) == 3) g_runq_wait[name + "/" + e->d_name] = wait;
                    fclose(sf);
                }
            }
        }
        closedir(d);
    }
    double tick = 1.0 / double(sysconf(_SC_CLK_TCK));
    double dt = wall0 ? double(now - wall0) / 1e9 : 0;
    std::string line;
    double sum = 0;
    for (auto& [k, v] : cur) {
        double used = double(v - (prev.count(k) ? prev[k] : 0)) * tick;
        sum += used;
        if (dt > 0 && used / dt > 0.03) {
            char b[96];
            double sys = double(cur_sys[k] - (prev_sys.count(k) ? prev_sys[k] : 0)) * tick;
            double wq = double(g_runq_wait[k] - g_runq_prev[k]) / 1e9;
            snprintf(b, sizeof b, " %s=%.0f%%(sys %.0f, runq wait %.0f)", k.substr(0, k.find('/')).c_str(), 100 * used / dt, 100 * sys / dt, 100 * wq / dt);
            line += b;
        }
    }
    static uint64_t pt = 0, py = 0, ps = 0;
    uint64_t t_ = g_tick_calls, y_ = g_yield_calls, s_ = g_sleep_calls;
    if (dt > 0) log("cpu: per second: GetSystemTick %.0f, yield %.0f, SleepThread(ns>0) %.0f", double(t_ - pt) / dt, double(y_ - py) / dt, double(s_ - ps) / dt);
    pt = t_, py = y_, ps = s_;
    if (dt > 0) log("cpu: %.1f s wall, process %.0f%% of one core (%.2f cores);%s", dt, 100 * sum / dt, sum / dt, line.c_str());
    prev = cur;
    prev_sys = cur_sys;
    g_runq_prev = g_runq_wait;
    wall0 = now;
}
#else
static void cpu_log_tick() {}
#endif

// Wonder's sead::GameFramework::procFrame_ override (main+e15ec) picks the interval every frame from the main
// thread's last frame time, fw[0x118] (GetSystemTick ticks from the end of the previous present wait to the end of
// this frame's calc and draw recording): 2 when it reaches 15.83 ms, locked after 3 slow frames, back to 1 after 15
// fast ones (docs/nvn/DEBUG.md #43). From that call site (lr main+e16ac, x19 = the framework): a histogram of that
// time every 600 frames, and NX_MAX_INTERVAL=1 clamps the game's own maximum to 1 (fields 0x24c/0x264/0x268).
void frame_time_probe(CPU& c) {
    const LoadedModule* m = module_at(c.x[30]);
    if (!m || c.x[30] - m->base != 0xe16ac || !c.x[19]) return;
    plat::perf_hint_thread();  // the game's main thread makes the frame too (ADPF, PLAN.md R2)
    plat::frame_thread_tick(1);
    {
        static thread_local bool marked = false;
        if (!marked) marked = true, svc_profile_mark(1);
        static uint64_t n = 0;
        if (++n % 600 == 0) svc_profile_report(600, 1);
    }
    // NX_FPS_CAP=30 (the launcher's "FPS limit"): the game is held at its own 30 fps mode, interval 2, always. The
    // game's logic steps by the interval it asked for, so the speed stays right; only the governors below are off.
    // 60 or unset: the game's own behaviour (60 when it can, 30 when heavy) plus our governor.
    static const bool cap30 = [] {
        const char* e = std::getenv("NX_FPS_CAP");
        bool on = e && std::atoi(e) > 0 && std::atoi(e) <= 30;
        if (on) log("nvn: NX_FPS_CAP=%s: present interval held at 2 (30 fps)", e);
        return on;
    }();
    if (cap30) {
        c.x[1] = 2;
        st<uint32_t>(c.x[19] + 0x264, 2);
        st<uint32_t>(c.x[19] + 0x268, 2);
    }
    static const char* max_env = std::getenv("NX_MAX_INTERVAL");
    static uint32_t game_max = 0;  // the game's own maximum (fw[0x24c]), restored when the governor lets go
    if (!game_max) game_max = ld<uint32_t>(c.x[19] + 0x24c);
    uint32_t max = cap30 ? 0 : max_env && std::atoi(max_env) >= 1 ? uint32_t(std::atoi(max_env)) : governor_max(plat::monotonic_ns());
    if (!max && !max_env && game_max) st<uint32_t>(c.x[19] + 0x24c, game_max);
    if (max >= 1) {
        st<uint32_t>(c.x[19] + 0x24c, max);
        if (ld<uint32_t>(c.x[19] + 0x268) > max) st<uint32_t>(c.x[19] + 0x268, max);
        if (ld<uint32_t>(c.x[19] + 0x264) > max) st<uint32_t>(c.x[19] + 0x264, max);
        if (int(c.x[1]) > int(max)) c.x[1] = max;
    }
    // NX_FW_PROBE=1: which words of the framework object count with the frames (study fps-unlock-1710).
    static const bool fwprobe = std::getenv("NX_FW_PROBE") != nullptr;
    if (fwprobe) {
        static uint32_t prev[0xc0] = {};
        static uint32_t changed[0xc0] = {}, sum[0xc0] = {};
        static int n = 0;
        for (int k = 0; k < 0xc0; k++) {
            uint32_t v = ld<uint32_t>(c.x[19] + 4 * k);
            if (v != prev[k]) changed[k]++, sum[k] += v - prev[k];
            prev[k] = v;
        }
        if (++n % 300 == 0) {
            for (int k = 0; k < 0xc0; k++)
                if (changed[k] > 150 && sum[k] < 100000)
                    log("fwprobe: fw[0x%x] changed %u/300, sum of deltas %u, now %u", 4 * k, changed[k], sum[k], prev[k]);
            memset(changed, 0, sizeof changed), memset(sum, 0, sizeof sum);
        }
        static uint64_t t0 = 0, ivsum = 0;
        if (!t0) t0 = plat::monotonic_ns();
        ivsum += uint32_t(c.x[1]);
        if (n % 300 == 0)
            log("fwprobe: %.2f s wall, %d frames, interval sum %llu, step %u", double(plat::monotonic_ns() - t0) / 1e9, n,
                (unsigned long long)ivsum, ld<uint32_t>(c.x[19] + 0x260));
    }
    // NX_CPU_LOG=1: process CPU use per 300 frames, total and by thread (Linux /proc), for the low-end study.
    static const bool cpulog = std::getenv("NX_CPU_LOG") != nullptr;
    if (cpulog) cpu_log_tick();
    static uint64_t bins[5] = {}, frames = 0;
    double ms = double(ld<uint64_t>(c.x[19] + 0x118)) / 19200.0;
    // A main-thread frame over 33 ms: what it waited on (PHASE3.md I1), its CPU time, faults and switches since the
    // last call, and how much of it a memory-guard read covered (PHASE4.md H0). NX_HITCH_LINES lines at most (300).
    static int hitches = 0;
    static const int max_hitches = std::getenv("NX_HITCH_LINES") ? std::atoi(std::getenv("NX_HITCH_LINES")) : 300;
    static uint64_t cpu_prev = 0;
    static long faults_prev[2] = {}, switches_prev[2] = {};
    uint64_t cpu = 0, now = plat::monotonic_ns();
    long faults[2], switches[2];
    plat::thread_counters(cpu, faults, switches);
    if (ms >= 33 && hitches < max_hitches && cpu_prev) {
        hitches++;
        uint64_t span = uint64_t(ms * 1e6);
        log("hitch: frame %llu, main thread %.1f ms (cpu %.1f), blocked: sync %.1f, finish %.1f, invalidate %.1f, "
            "finalize %.1f ms; faults %ld/%ld, switches %ld/%ld; guard read %.1f ms",
            (unsigned long long)g_frame.load(), ms, double(cpu - cpu_prev) / 1e6,
            double(g_wait_sites[0].frame_ns) / 1e6, double(g_wait_sites[1].frame_ns) / 1e6,
            double(g_wait_sites[2].frame_ns) / 1e6, double(g_wait_sites[3].frame_ns) / 1e6, faults[0] - faults_prev[0],
            faults[1] - faults_prev[1], switches[0] - switches_prev[0], switches[1] - switches_prev[1],
            double(plat::memguard_overlap_ns(now > span ? now - span : 0, now)) / 1e6);
    }
    cpu_prev = cpu;
    for (int k = 0; k < 2; k++) faults_prev[k] = faults[k], switches_prev[k] = switches[k];
    for (WaitSite& w : g_wait_sites) w.frame_ns = 0;
    bins[ms < 12 ? 0 : ms < 15.83 ? 1 : ms < 20 ? 2 : ms < 33 ? 3 : 4]++;
    if (++frames % 600 == 0) {
        log("nvn: main-thread frame time over 600 frames: <12 ms %llu, <15.8 %llu, <20 %llu, <33 %llu, more %llu",
            (unsigned long long)bins[0], (unsigned long long)bins[1], (unsigned long long)bins[2],
            (unsigned long long)bins[3], (unsigned long long)bins[4]);
        for (auto& b : bins) b = 0;
    }
}

void WindowSetPresentInterval(CPU& c) {
    frame_time_probe(c);
    if (auto* w = get<Window>(c.x[0])) {
        static int logged = 0;
        if (w->interval != int(c.x[1]) && logged++ < 50)
            log("nvn: window present interval %d -> %d (frame %llu)", w->interval, int(c.x[1]), (unsigned long long)g_frame.load());
        w->interval = int(c.x[1]);
    }
}

// (NVNqueue*, NVNwindow*, int textureIndex)
// Frame pacing: a present returns no earlier than the window's present interval (in 60 Hz vsyncs) after the previous
// one. On the Switch the texture-available sync of the next acquire does this; ours is signaled at once, so without
// it the game runs as fast as it can once it is faster than 60 fps. A late frame is not snapped to the next vsync
// (an 18 ms frame stays 18 ms, not 33). NX_NO_FRAME_LIMIT=1: unpaced.
void pace_present(int interval) {
    static const bool off = std::getenv("NX_NO_FRAME_LIMIT") != nullptr;
    static uint64_t next = 0;
    if (off || interval <= 0) return;
    uint64_t now = plat::monotonic_ns();
    if (next > now) plat::sleep_ns(next - now), now = plat::monotonic_ns();
    static uint64_t presents = 0, at2 = 0;  // how often the game asks for 30 fps (interval 2), per 600 presents
    at2 += interval >= 2;
    if (++presents % 600 == 0)
        log("nvn: %llu of the last 600 presents at interval >= 2", (unsigned long long)at2), at2 = 0, log_wait_sites();
    uint64_t period = uint64_t(interval) * display_period_ns();
    next = (next > now || now - next < period ? next : now) + period;
}

void QueuePresentTexture(CPU& c) {
    // The frame's work, from the end of the last present's pacing to now (perf_hint.cpp: ADPF on Android).
    static uint64_t work_from = 0;
    uint64_t t_in = plat::monotonic_ns();
    tl("present>");
    {
        static thread_local bool marked = false;
        if (!marked) marked = true, svc_profile_mark(2);
        static uint64_t n = 0;
        if (++n % 600 == 0) svc_profile_report(600, 2);
    }
    plat::perf_hint_thread();
    if (work_from) plat::perf_hint_frame(int64_t(t_in - work_from));
    if (auto* w = get<Window>(c.x[1])) {
        // At most two presents queued: the game runs at most two frames ahead of the replay.
        static uint64_t queued[2] = {};
        static int next_slot = 0;
        worker_wait(queued[next_slot]);
        if (int(c.x[2]) >= 0 && size_t(c.x[2]) < w->textures.size()) {
            uint64_t texture = w->textures[size_t(c.x[2])];
            std::array<int32_t, 4> crop{w->crop[0], w->crop[1], w->crop[2], w->crop[3]};
            queued[next_slot] = worker_run([texture, crop] { gpu_present(texture, crop.data()); });
            next_slot ^= 1;
        }
        pace_present(w->interval);
    }
    work_from = plat::monotonic_ns();
    uint64_t n = ++g_presents;
    uw_scan(n);
    uw_cameras(n);
    tl("present<", n);
    if (g_tl_from != UINT64_MAX && n == g_tl_from + 8) {
        uint64_t cnt = std::min<uint64_t>(g_tl_n.load(), 2048), t0 = g_tl[0].ns;
        for (uint64_t k = 0; k < cnt; k++)
            log("timeline: %9.3f ms  t%lld  %s %llu", double(g_tl[k].ns - t0) / 1e6, (long long)g_tl[k].tid, g_tl[k].what,
                (unsigned long long)g_tl[k].arg);
    }
    if (n == 1 || (n & (n - 1)) == 0) log("nvn: present #%llu (texture %d)", (unsigned long long)n, int(c.x[2]));
    g_frame++;
}

// (NVNwindow*, int x, int y, int width, int height): the part of the window textures shown. In handheld mode
// (NX_HANDHELD) the game renders 1280x720 into its 1920x1080 window textures and crops to it.
void WindowSetCrop(CPU& c) {
    if (auto* w = get<Window>(c.x[0])) {
        int32_t v[4] = {int32_t(c.x[1]), int32_t(c.x[2]), int32_t(c.x[3]), int32_t(c.x[4])};
        uw_crop(v);  // NX_ULTRAWIDE
        if (std::memcmp(v, w->crop, sizeof v) != 0) {
            static int logged = 0;
            if (logged++ < 20) log("nvn: window crop %d,%d %dx%d", v[0], v[1], v[2], v[3]);
            std::memcpy(w->crop, v, sizeof v);
        }
    }
}

}  // namespace

NX_HLE("nvnWindowSetCrop", WindowSetCrop);
NX_HLE("nvnSyncInitialize", SyncInitialize);
NX_HLE("nvnSyncFinalize", SyncFinalize);
NX_HLE("nvnSyncWait", SyncWait);
NX_HLE("nvnQueueInitialize", QueueInitialize);
NX_HLE("nvnQueueFinalize", QueueFinalize);
NX_HLE("nvnQueueSubmitCommands", QueueSubmitCommands);
NX_HLE("nvnQueueFlush", QueueFlush);
NX_HLE("nvnQueueFinish", QueueFinish);
NX_HLE("nvnQueueFenceSync", QueueFenceSync);
NX_HLE("nvnQueueWaitSync", QueueWaitSync);
NX_HLE("nvnQueuePresentTexture", QueuePresentTexture);
NX_HLE("nvnWindowInitialize", WindowInitialize);
NX_HLE("nvnWindowFinalize", WindowFinalize);
NX_HLE("nvnWindowAcquireTexture", WindowAcquireTexture);
NX_HLE("nvnWindowGetNumTextures", WindowGetNumTextures);
NX_HLE("nvnWindowGetNumActiveTextures", WindowGetNumActiveTextures);
NX_HLE("nvnWindowSetNumActiveTextures", WindowSetNumActiveTextures);
NX_HLE("nvnWindowGetPresentInterval", WindowGetPresentInterval);
NX_HLE("nvnWindowSetPresentInterval", WindowSetPresentInterval);

}  // namespace nx::nvn
