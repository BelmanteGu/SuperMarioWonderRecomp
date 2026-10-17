#include "nvn_ultrawide.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "nvn.h"
#include "nvn_pipeline.h"
#include <nx/module.h>
#include <nx/runtime.h>

#include "kernel.h"
#include "platform.h"

namespace nx::nvn {

namespace {

constexpr double k169 = 16.0 / 9.0;

float init_factor() {
    const char* e = std::getenv("NX_ULTRAWIDE");
    if (!e || !*e || e[0] == '0') return 1.0f;
    double aspect = 0;
    unsigned w = 0, h = 0;
    if (std::sscanf(e, "%ux%u", &w, &h) == 2 && h) {
        aspect = double(w) / h;
    } else if (std::sscanf(e, "%u:%u", &w, &h) == 2 && h) {
        aspect = double(w) / h;
    } else {  // "1" / "auto": the window's shape
        uint32_t ww = 0, wh = 0;
        plat::window_size(ww, wh);
        if (wh) aspect = double(ww) / wh;
    }
    float k = float(aspect / k169);
    if (k < 1.02f) k = 1.0f;  // 16:9 and narrower: nothing to do
    if (k > 3.0f) k = 3.0f;
    log("nvn: ultrawide: display aspect %.4f, render targets widened by %.4f", aspect, double(k));
    return k;
}

}  // namespace

float uw_factor() {
    static const float k = init_factor();
    return k;
}

uint32_t uw_widen(uint32_t w) {
    uint32_t v = uint32_t(std::lround(double(w) * uw_factor()));
    return v & ~1u;
}

bool uw_widen_texture(Texture& t) {
    // Render targets (flags 8), the window textures (display, 1) and the scene colour (flags 0); not packaged assets (4).
    if (!uw_on() || t.target != 1 || (t.flags & 4) || t.width < 16 || t.height < 9) return false;
    double r = double(t.width) / double(t.height);
    if (std::fabs(r - k169) / k169 > 0.03) return false;
    static std::map<std::string, int> seen;  // each distinct widened shape once, for the study log
    char key[64];
    std::snprintf(key, sizeof key, "%ux%u f%x fmt%x", t.width, t.height, t.flags, t.format);
    if (seen[key]++ == 0) log("nvn: ultrawide: widening %s", key);
    t.width = uw_widen(t.width);
    t.uw = true;
    return true;
}

bool uw_crop(int32_t crop[4]) {
    if (!uw_on() || crop[0] != 0 || crop[1] != 0 || crop[2] <= 0 || crop[3] <= 0) return false;
    double r = double(crop[2]) / double(crop[3]);
    if (std::fabs(r - k169) / k169 > 0.03) return false;
    crop[2] = int32_t(uw_widen(uint32_t(crop[2])));
    return true;
}


// NX_UW_SCAN=<present>: once, at that present, lists where guest memory (heap, main's data and bss) holds the numbers
// of the 16:9 picture (aspect 16/9 and 9/16 floats, 1920x1080 and 1280x720 as int and float pairs), with context.
void uw_scan(uint64_t present) {
    static const char* e = std::getenv("NX_UW_SCAN");
    static const uint64_t at = e ? std::strtoull(e, nullptr, 10) : 0;
    static bool done = false;
    if (!e || done || present < at) return;
    done = true;
    auto f32 = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
    struct Pat { const char* name; uint32_t a, b; bool pair; };
    const Pat pats[] = {{"f32 16/9", f32(16.f / 9.f), 0, false}, {"f32 9/16", f32(9.f / 16.f), 0, false},
                        {"u32 1920,1080", 1920, 1080, true}, {"f32 1920,1080", f32(1920.f), f32(1080.f), true},
                        {"u32 1280,720", 1280, 720, true}, {"f32 1280,720", f32(1280.f), f32(720.f), true},
                        {"u32 1080,1920", 1080, 1920, true}, {"f32 960,540", f32(960.f), f32(540.f), true}};
    struct Range { uint64_t lo, hi; const char* name; };
    std::vector<Range> ranges{{kHeapBase, kHeapBase + heap_size(), "heap"}};
    if (const LoadedModule* m = module_at(kModuleBase)) {
        const ModuleInfo* mi = m->info;
        ranges.push_back({mi->base + mi->data, mi->base + mi->bss + mi->bss_size, "main data/bss"});
    }
    for (const Range& r : ranges)
        for (const Pat& p : pats) {
            int hits = 0;
            for (uint64_t a = r.lo; a + 8 <= r.hi; a += 4) {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(a);
                if (w[0] != p.a || (p.pair && w[1] != p.b)) continue;
                if (hits++ >= 40) break;
                std::string ctx;
                for (int k = -4; k < 8; k++) {
                    uint64_t q = a + 4 * k;
                    if (q < r.lo || q + 4 > r.hi) continue;
                    uint32_t u = *reinterpret_cast<const uint32_t*>(q);
                    float f;
                    std::memcpy(&f, &u, 4);
                    char b[40];
                    std::snprintf(b, sizeof b, " %s%g/%x", k == 0 ? "[" : "", double(f), u);
                    ctx += b;
                }
                log("uwscan %s %s at %llx (main+%llx):%s", r.name, p.name, (unsigned long long)a,
                    (unsigned long long)(a - kModuleBase), ctx.c_str());
            }
            log("uwscan %s %s: %d hits%s", r.name, p.name, hits, hits > 40 ? "+" : "");
        }
}

// ---- camera aspect -----------------------------------------------------------------------------------------------
// The game keeps its cameras as objects holding {.., far 1000.0, fovy 0.785398, aspect 16/9, ..} (measured with
// NX_UW_SCAN: 40+ of them in a pool). The aspect is a plain float the game turns into the projection, so setting it
// to k * 16/9 gives the Hor+ camera (same vertical field of view, wider horizontal). Found by scanning the heap for
// the three floats in a row (rescanned every 2 s, a worker thread), rewritten at every present.
namespace {
std::mutex g_cam_lock;
std::vector<uint64_t> g_cams;  // addresses of the aspect floats

void cam_scan() {
    const uint32_t far_bits = 0x447a0000u, fov_bits = 0x3f490fdbu, asp_bits = 0x3fe38e39u;
    uint64_t lo = kHeapBase, hi = kHeapBase + heap_size();
    std::vector<uint64_t> found;
    for (uint64_t a = lo; a + 12 <= hi; a += 4) {
        const uint32_t* w = reinterpret_cast<const uint32_t*>(a);
        if (w[0] != far_bits) continue;
        if (w[1] == fov_bits && (w[2] == asp_bits)) found.push_back(a + 8);
    }
    std::lock_guard lk(g_cam_lock);
    for (uint64_t a : found)
        if (std::find(g_cams.begin(), g_cams.end(), a) == g_cams.end() && g_cams.size() < 8192) g_cams.push_back(a);
}
}  // namespace

void uw_cameras(uint64_t present) {
    if (!uw_on()) return;
    static const bool off = std::getenv("NX_UW_CAMERA") && std::getenv("NX_UW_CAMERA")[0] == '0';
    if (off) return;
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            plat::set_thread_name("uw-camscan");
            for (;;) {
                uint64_t t0 = plat::monotonic_ns();
                cam_scan();
                static int logged = 0;
                if (logged++ < 5) {
                    std::lock_guard lk(g_cam_lock);
                    log("nvn: ultrawide: camera scan %.0f ms, %zu cameras", double(plat::monotonic_ns() - t0) / 1e6, g_cams.size());
                }
                plat::sleep_ns(2'000'000'000ll);
            }
        }).detach();
    });
    const uint32_t asp_bits = 0x3fe38e39u;
    float wide = float(16.0 / 9.0) * uw_factor();
    uint32_t wide_bits;
    std::memcpy(&wide_bits, &wide, 4);
    std::lock_guard lk(g_cam_lock);
    for (uint64_t a : g_cams) {
        auto* p = reinterpret_cast<volatile uint32_t*>(a);
        if (*p == asp_bits) *p = wide_bits;
    }
}

bool g_uw_wide = false;

void uw_span(int32_t& x, int32_t& w, uint32_t extent_w) {
    int64_t x0 = std::llround(double(x) * uw_factor());
    int64_t ww = int64_t(uw_widen(uint32_t(std::max(0, w))));
    if (std::llabs(int64_t(extent_w) - ww) <= 2 && x == 0) ww = extent_w;  // the whole target
    if (x0 + ww > int64_t(extent_w)) ww = std::max<int64_t>(0, int64_t(extent_w) - x0);
    x = int32_t(x0), w = int32_t(ww);
}

namespace {
std::mutex g_ui_lock;
std::set<const ShaderInfo*> g_ui_vs;
}  // namespace
void uw_ui_vertex_shader(const ShaderInfo* vs) {
    std::lock_guard lk(g_ui_lock);
    if (g_ui_vs.insert(vs).second) log("nvn: ultrawide: UI vertex shader %s", vs->hash.c_str());
}
bool uw_is_ui_vertex_shader(const ShaderInfo* vs) {
    std::lock_guard lk(g_ui_lock);
    return g_ui_vs.count(vs) != 0;
}

bool uw_vs_scaled(const ShaderInfo* vs) {
    static std::map<const ShaderInfo*, bool> cache;
    static const std::string off = std::getenv("NX_UW_NOSCALE") ? std::getenv("NX_UW_NOSCALE") : "";
    static const std::string only = std::getenv("NX_UW_SCALE") ? std::getenv("NX_UW_SCALE") : "";
    auto it = cache.find(vs);
    if (it != cache.end()) return it->second;
    bool scaled = false;
    for (const DescriptorBinding& b : vs->refl.bindings)
        if (b.type == DescriptorType::UniformBuffer && b.name != "support_buffer") scaled = true;
    if (!off.empty() && off.find(vs->hash) != std::string::npos) scaled = false;
    if (!only.empty()) scaled = only.find(vs->hash) != std::string::npos;
    return cache[vs] = scaled;
}


// ---- gl_Position.x * spec constant (modelled on patch_clip_z, nvn_spirv_clip.cpp) -----------------------------------

namespace {
enum : uint32_t {
    OpEntryPoint = 15, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32, OpSpecConstant = 50,
    OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpStore = 62, OpAccessChain = 65,
    OpDecorate = 71, OpMemberDecorate = 72, OpCompositeExtract = 81, OpCompositeInsert = 82, OpConstant = 43,
    OpFMul = 133, OpReturn = 253, OpStructType = 30,
    DecorationBuiltIn = 11, DecorationSpecId = 1, BuiltInPosition = 0, StorageOutput = 3, ModelVertex = 0,
};
bool is_header_op(uint32_t op) {  // before the types: capabilities .. annotations
    switch (op) {
        case 17: case 10: case 11: case 14: case 15: case 16: case 330: case 3: case 4: case 5: case 6: case 7: case 8:
        case 71: case 72: case 73: case 74: case 75: return true;
        default: return false;
    }
}
}  // namespace

bool patch_x_scale(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    if (in.size() < 5) return false;
    struct Inst { size_t at; uint32_t op, len; };
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    uint32_t bound = in[3], entry = 0, f32 = 0, u32 = 0, v4f = 0, ptr_v4f = 0, zero = 0;
    uint32_t block = 0, member = 0, position_var = 0;
    std::map<uint32_t, uint32_t> struct_ptr;
    size_t first_function = in.size(), first_type = in.size();
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (first_type == in.size() && !is_header_op(n.op)) first_type = n.at;
        if (n.op == OpEntryPoint && n.len >= 3 && w[1] == ModelVertex && !entry) entry = w[2];
        else if (n.op == OpMemberDecorate && n.len >= 5 && w[3] == DecorationBuiltIn && w[4] == BuiltInPosition)
            block = w[1], member = w[2];
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationBuiltIn && w[3] == BuiltInPosition)
            position_var = w[1];
        else if (n.op == OpTypeFloat && n.len == 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeInt && n.len == 4 && w[2] == 32 && w[3] == 0) u32 = w[1];
        else if (n.op == OpTypeVector && n.len == 4 && w[2] == f32 && w[3] == 4) v4f = w[1];
        else if (n.op == OpTypePointer && n.len == 4 && w[2] == StorageOutput) {
            if (w[3] == v4f && v4f) ptr_v4f = w[1];
            struct_ptr[w[1]] = w[3];
        } else if (n.op == OpConstant && n.len == 4 && u32 && w[1] == u32 && w[3] == member) {
            zero = w[2];
        } else if (n.op == OpFunction && first_function == in.size()) {
            first_function = n.at;
        }
    }
    if (!entry || !f32 || !v4f || first_type == in.size()) return false;
    uint32_t var = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op != OpVariable || n.len < 4 || w[3] != StorageOutput) continue;
        auto p = struct_ptr.find(w[1]);
        if (p == struct_ptr.end()) continue;
        if (block && p->second == block) var = w[2];
        if (!block && w[2] == position_var && p->second == v4f) var = w[2];
    }
    if (!var) return false;
    uint32_t spec = bound++;
    std::vector<uint32_t> globals;
    if (!ptr_v4f) ptr_v4f = bound++, globals.insert(globals.end(), {(4u << 16) | OpTypePointer, ptr_v4f, StorageOutput, v4f});
    if (block) {
        if (!u32) u32 = bound++, globals.insert(globals.end(), {(4u << 16) | OpTypeInt, u32, 32, 0});
        if (!zero) zero = bound++, globals.insert(globals.end(), {(4u << 16) | OpConstant, u32, zero, member});
    }
    globals.insert(globals.end(), {(4u << 16) | OpSpecConstant, f32, spec, 0x3f800000u});

    out.assign(in.begin(), in.begin() + 5);
    uint32_t function = 0;
    int returns = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.at == first_type) out.insert(out.end(), {(4u << 16) | OpDecorate, spec, DecorationSpecId, 0u});
        if (n.at == first_function) out.insert(out.end(), globals.begin(), globals.end());
        if (n.op == OpFunction) function = w[2];
        if (n.op == OpReturn && function == entry) {
            uint32_t p = var;
            if (block) {
                p = bound++;
                out.insert(out.end(), {(5u << 16) | OpAccessChain, ptr_v4f, p, var, zero});
            }
            uint32_t v = bound++, x = bound++, m = bound++, r = bound++;
            out.insert(out.end(), {(4u << 16) | OpLoad, v4f, v, p});
            out.insert(out.end(), {(5u << 16) | OpCompositeExtract, f32, x, v, 0});
            out.insert(out.end(), {(5u << 16) | OpFMul, f32, m, x, spec});
            out.insert(out.end(), {(6u << 16) | OpCompositeInsert, v4f, r, m, v, 0});
            out.insert(out.end(), {(3u << 16) | OpStore, p, r});
            returns++;
        }
        if (n.op == OpFunctionEnd) function = 0;
        out.insert(out.end(), w, w + n.len);
    }
    if (!returns) return false;
    out[3] = bound;
    return true;
}

}  // namespace nx::nvn
