// NVN entry points (docs/NVN.md, phases N2-N3).
//
// The game links one NVN symbol, nvnBootstrapLoader, and asks it for nvnDeviceGetProcAddress; every other NVN
// function is then fetched by name, the same shape as vkGetInstanceProcAddr. Both are HLE here. For each name:
//   - one we implement (an NX_HLE with the NVN name): our function;
//   - a builder or state object proc (pure struct setters/getters): the SDK driver's own, recompiled (see nvn.h);
//   - anything else: a tracing stub that records the call and returns a neutral value, so the trace goes as far
//     as it can.
// The trace is <data root>/nvn_trace.txt: one line per call (the first kFullCalls of each proc), each proc's
// driver address ("# real"), and a count per proc at exit.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <regex>
#include <string>

#include "nvn.h"
#include "platform.h"

namespace nx {

namespace {

constexpr uint32_t kFullCalls = 32;  // calls of one proc traced with their arguments

std::mutex g_lock;
FILE* g_trace_file = nullptr;
std::map<std::string, uint64_t> g_calls;  // proc -> calls
std::atomic<uint64_t> g_seq{0};

std::string guest_str(uint64_t addr) {
    if (!addr) return "(null)";
    return std::string(reinterpret_cast<const char*>(addr), strnlen(reinterpret_cast<const char*>(addr), 256));
}

void write_line(const char* line) {  // g_lock held
    if (!g_trace_file) {
        g_trace_file = plat::fopen_utf8(plat::data_root() + "/nvn_trace.txt", "w");
        std::atexit([] {
            std::lock_guard lk(g_lock);
            if (!g_trace_file) return;
            std::fprintf(g_trace_file, "\n# calls per proc\n");
            for (auto& [name, n] : g_calls) std::fprintf(g_trace_file, "%10llu %s\n", (unsigned long long)n, name.c_str());
            std::fclose(g_trace_file);
            g_trace_file = nullptr;
        });
    }
    if (g_trace_file) {
        std::fputs(line, g_trace_file);
        std::fputc('\n', g_trace_file);
        std::fflush(g_trace_file);
    }
}

// A proc we do not implement yet: record it, return 0 except for the ones whose 0 means "failed" to the game.
void trace_stub(CPU& c) {
    const char* name = hle_current_name();
    uint64_t n, seq = g_seq++;
    {
        std::lock_guard lk(g_lock);
        n = ++g_calls[name];
        if (n <= kFullCalls) {
            char line[512];
            std::snprintf(line, sizeof line, "%8llu %s(%llx, %llx, %llx, %llx, %llx, %llx) lr %s", (unsigned long long)seq,
                          name, (unsigned long long)c.x[0], (unsigned long long)c.x[1], (unsigned long long)c.x[2],
                          (unsigned long long)c.x[3], (unsigned long long)c.x[4], (unsigned long long)c.x[5],
                          describe(c.x[30]).c_str());
            write_line(line);
        }
    }
    if (n == 1) log("nvn: first call of %s (lr %s)", name, describe(c.x[30]).c_str());
    // Initialize functions return NVNboolean: false would make the engine give up at the first object.
    c.x[0] = std::strstr(name, "Initialize") ? 1 : 0;
}

// Builders, state objects and texture views only store and return fields: the driver's own code serves them.
bool driver_serves(const std::string& name) {
    static const std::regex pure(
        "nvn(\\w+Builder\\w+|(Blend|Color|DepthStencil|Multisample|Polygon|VertexAttrib|VertexStream|ChannelMask)"
        "State\\w+|TextureView\\w+)");
    return std::regex_match(name, pure) && !std::getenv("NX_NVN_NO_DRIVER");
}

uint64_t proc_address(CPU& c, const std::string& name) {
    if (name.empty()) return 0;
    static const bool note_all = std::getenv("NX_NVN_REAL") != nullptr;  // record every proc's driver address
    if (note_all) nvn::real_proc(c, name.c_str());
    if (hle_lookup(name.c_str())) return hle_address(name.c_str());
    if (driver_serves(name))
        if (uint64_t real = nvn::real_proc(c, name.c_str())) return real;
    return hle_address_or(name.c_str(), trace_stub);
}

// PFNNVNGENERICFUNCPROC nvnBootstrapLoader(const char* name)
void nvn_bootstrap_loader(CPU& c) {
    std::string name = guest_str(c.x[0]);
    log("nvnBootstrapLoader(%s)", name.c_str());
    {
        std::lock_guard lk(g_lock);
        write_line(("# nvnBootstrapLoader(" + name + ")").c_str());
    }
    c.x[0] = proc_address(c, name);
}

// PFNNVNGENERICFUNCPROC nvnDeviceGetProcAddress(const NVNdevice* device, const char* name)
void nvn_device_get_proc_address(CPU& c) {
    std::string name = guest_str(c.x[1]);
    if (g_trace) log("nvnDeviceGetProcAddress(%llx, %s)", (unsigned long long)c.x[0], name.c_str());
    c.x[0] = proc_address(c, name);
}

}  // namespace

NX_HLE("nvnBootstrapLoader", nvn_bootstrap_loader);
NX_HLE("nvnDeviceGetProcAddress", nvn_device_get_proc_address);

namespace nvn {

uint64_t guest_call(CPU& c, uint64_t fn, std::initializer_list<uint64_t> args) {
    uint64_t saved[31];
    std::memcpy(saved, c.x, sizeof saved);
    size_t k = 0;
    for (uint64_t a : args) c.x[k++] = a;
    call(c, fn);
    uint64_t r = c.x[0];
    std::memcpy(c.x, saved, sizeof saved);
    return r;
}

float guest_call_f(CPU& c, uint64_t fn, std::initializer_list<uint64_t> args) {
    uint64_t saved[31];
    std::memcpy(saved, c.x, sizeof saved);
    V128 v0 = c.v[0];
    size_t k = 0;
    for (uint64_t a : args) c.x[k++] = a;
    call(c, fn);
    float r = c.v[0].f32[0];
    std::memcpy(c.x, saved, sizeof saved);
    c.v[0] = v0;
    return r;
}

uint64_t scratch(CPU&, size_t size) {
    thread_local uint64_t page = 0;
    if (!page) page = kernel_alloc(0x1000);
    if (size > 0x1000) fatal("nvn scratch: %zu bytes asked", size);
    return page;
}

uint64_t real_proc_slow(CPU& c, const char* name);

uint64_t real_proc(CPU& c, const char* name) {
    // The hot callers (state-object getters, one per bound state) pass string literals: a per-thread cache by the
    // literal's address, verified by content, instead of a global mutex and a string-keyed tree at every call (the
    // game's recording threads contended on that lock).
    struct Hit {
        std::string name;
        uint64_t proc = 0;
    };
    thread_local std::unordered_map<const char*, Hit> fast;
    if (auto it = fast.find(name); it != fast.end() && it->second.name == name) return it->second.proc;
    uint64_t found = real_proc_slow(c, name);
    if (found) fast[name] = {name, found};
    return found;
}

uint64_t real_proc_slow(CPU& c, const char* name) {
    static std::mutex lock;
    static std::map<std::string, uint64_t> cache;
    {
        std::lock_guard lk(lock);
        auto it = cache.find(name);
        if (it != cache.end()) return it->second;
    }
    static uint64_t loader = find_symbol("nvnBootstrapLoader");  // the sdk's export (imports are HLE)
    static uint64_t scratch = kernel_alloc(0x1000);
    uint64_t real = 0;
    size_t len = std::strlen(name);
    if (loader && len < 0x1000) {
        std::lock_guard lk(lock);  // one scratch buffer
        std::memcpy(reinterpret_cast<void*>(scratch), name, len + 1);
        real = guest_call(c, loader, {scratch});
        cache[name] = real;
    }
    {
        std::lock_guard lk(g_lock);
        write_line(("# real " + std::string(name) + " = " + describe(real)).c_str());
    }
    return real;
}

ObjectsLock& objects_lock() {
    static ObjectsLock m;
    return m;
}

std::vector<std::pair<uint64_t, std::unique_ptr<Object>>>& deferred_objects() {  // (epoch when retired, object)
    static std::vector<std::pair<uint64_t, std::unique_ptr<Object>>> v;
    return v;
}

void defer_object(std::unique_ptr<Object> o) { deferred_objects().push_back({current_epoch(), std::move(o)}); }

void drain_objects() {
    // Only what no replay can name any more: retired at or before the last replay's mark (nvn_incarnation.cpp). A
    // later submit's lists were recorded after that, so they cannot mean those objects.
    std::vector<std::unique_ptr<Object>> dead;
    {
        std::lock_guard lk(objects_lock());
        uint64_t upto = replay_floor();
        auto& all = deferred_objects();
        size_t keep = 0;
        for (auto& [epoch, o] : all) {
            if (epoch <= upto)
                dead.push_back(std::move(o));
            else
                all[keep++] = {epoch, std::move(o)};
        }
        all.resize(keep);
        purge_incarnations(upto);  // the records point at objects freed here
    }
}  // destructors run here, outside the lock (a Texture's takes the graveyard lock)

std::unordered_map<uint64_t, std::unique_ptr<Object>>& objects() {
    static std::unordered_map<uint64_t, std::unique_ptr<Object>> m;
    return m;
}

}  // namespace nvn

}  // namespace nx
