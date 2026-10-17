// Draw traces (docs/nvn/DEBUG.md): NX_TRACE_DRAWS=<first draw> logs draws with their shaders, targets, textures and
// state; NX_TRACE_TARGET, NX_TRACE_FS_HASH, NX_TRACE_COUNT select them; NX_TRACE_VS_BLOCKS and NX_TRACE_GLYPHS add
// uniform blocks and glyph records. Moved out of nvn_draw.cpp, which calls trace_draw() after every draw.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>

#include "nvn_draw_state.h"

namespace nx::nvn::replay {

using namespace nx::vk;

namespace {

// NX_TRACE_CUBE_DRAWS=<first frame>: the draws into cube and cube-array targets from that frame on (3000 at most),
// what makes the light probes. A scene start writes the 6 faces of one level at once (6 colour targets). Per sampled
// texture: `rt` GPU-written, `up` only uploaded from guest memory (GPU writes through an older object are not there),
// `undef` never written (reads black), `SELF` an image this draw renders into (a mip chain reading the level above).
void trace_cube_draw(const Targets& t, const ShaderInfo* vs, const ShaderInfo* fs, const TexBindings& tex_bindings,
                     const std::vector<uint64_t>& handles) {
    static const char* env = std::getenv("NX_TRACE_CUBE_DRAWS");
    static const uint64_t from = env ? std::strtoull(env, nullptr, 10) : 0;
    static int traced = 0;
    if (!env || traced >= 3000 || g_frame < from) return;
    GpuImage* targets[8] = {};
    bool cube = false;
    for (uint32_t k = 0; k < t.color_count && k < 8; k++)
        if ((targets[k] = t.colors[k] ? image_of(t.colors[k]) : nullptr))
            cube |= targets[k]->view_type == VK_IMAGE_VIEW_TYPE_CUBE || targets[k]->view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    if (!cube) return;
    traced++;
    std::string tex;
    for (size_t k = 0; k < tex_bindings.size(); k++) {
        uint64_t texture = 0;
        SamplerInfo si;
        ViewInfo v;
        texture_for(handles[k], texture, si, &v);
        Texture* tx = texture ? get<Texture>(texture) : nullptr;
        GpuImage* img = tx ? tx->image.get() : nullptr;
        bool self = img && std::find(targets, targets + 8, img) != targets + 8;
        char b[160];
        std::snprintf(b, sizeof b, " t%zu=%llx:%llx(%ux%ux%u L%u target %u f%x view L%u+%u layer %u%s%s%s)", k,
                      (unsigned long long)handles[k], (unsigned long long)texture, tx ? tx->width : 0, tx ? tx->height : 0,
                      tx ? tx->depth : 0, tx ? tx->levels : 0, tx ? tx->target : 0, tx ? tx->format : 0, v.base_level,
                      v.levels, v.min_layer, !img ? " no image" : img->rendered ? " rt" : img->uploaded ? " up" : "",
                      img && img->layout == VK_IMAGE_LAYOUT_UNDEFINED ? " undef" : "", self ? " SELF" : "");
        tex += b;
    }
    GpuImage* d = t.depth ? image_of(t.depth) : nullptr;
    log("cube draw %llu frame %llu: vs %s fs %s targets %u:%llx level %u layer %u depth %llx (%ux%u)%s",
        (unsigned long long)g_stats.done, (unsigned long long)g_frame.load(), vs->hash.c_str(), fs ? fs->hash.c_str() : "-",
        t.color_count, (unsigned long long)t.colors[0], t.views.level[0], t.views.layer[0], (unsigned long long)t.depth,
        d ? d->width : 0, d ? d->height : 0, tex.c_str());
    for (int bank : {4, 5}) {  // the probe shaders' axes (c4[1..3]), colours (c5[16..21]) and LUT rows (c5[8..10])
        const Range& u = g_state.uniforms[1][bank - 3];
        const float* f = u.address ? reinterpret_cast<const float*>(guest_bytes(u.address)) : nullptr;
        std::string rows;
        for (int r : {0, 1, 2, 3, 8, 16, 17, 18, 19})
            if (f && uint64_t(r) * 16 + 16 <= u.size) {
                char b[96];
                std::snprintf(b, sizeof b, " [%d] %g %g %g %g", r, f[4 * r], f[4 * r + 1], f[4 * r + 2], f[4 * r + 3]);
                rows += b;
            }
        log("  fs c%d (%llx, %llu bytes):%s", bank, (unsigned long long)u.address, (unsigned long long)u.size, rows.c_str());
    }
    if (const Range& u = g_state.uniforms[1][0]; u.address && u.size >= 64)  // fs c3: lod, level, probe index
        if (auto* f = reinterpret_cast<const float*>(guest_bytes(u.address)))
            log("  fs c3: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g", f[0], f[1], f[2], f[3], f[4], f[5],
                f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
}

}  // namespace

namespace {
std::map<uint64_t, std::string> g_nan_writers;  // storage address -> the last compute shader bound to it
bool trace_nan_on() {
    static const bool on = std::getenv("NX_TRACE_NAN") != nullptr;
    return on;
}
}  // namespace

void trace_nan_dispatch(const ShaderInfo* cs) {
    static const bool c8 = std::getenv("NX_TRACE_C8") != nullptr;  // each compute dispatch's storage ranges (and uniform c3..)
    static int dispatches = 0;
    static std::map<std::string, uint64_t> seen;
    if (c8 && seen[cs->hash]++ == 0)
        log("dispatch: first use of cs %s at dispatch %d, frame %llu", cs->hash.c_str(), dispatches, (unsigned long long)g_frame.load());
    if (c8 && dispatches++ < 0) {
        std::string r;
        for (int k = 0; k < 16; k++)
            if (g_state.storage[5][k].address) {
                char b[64];
                std::snprintf(b, sizeof b, " s%d=%llx+%llu", k, (unsigned long long)g_state.storage[5][k].address,
                              (unsigned long long)g_state.storage[5][k].size);
                r += b;
            }
        log("dispatch %d: cs %s%s", dispatches, cs->hash.c_str(), r.c_str());
    }
    if (!trace_nan_on()) return;
    for (const Range& r : g_state.storage[5])
        if (r.address) g_nan_writers[reinterpret_cast<uintptr_t>(guest_bytes(r.address))] = cs->hash;
}

void trace_nan_write_back(const uint8_t* from, const uint8_t* to, uint64_t size) {
    static int logged = 0;
    if (!trace_nan_on() || logged >= 200) return;
    uint64_t bad = 0, first = 0;
    auto* f = reinterpret_cast<const uint32_t*>(from);
    for (uint64_t k = 0; k < size / 4; k++) {
        uint32_t e = f[k] >> 23 & 0xFF;
        if (e == 0xFF || (e >= 127 + 40 && e != 0xFF)) {  // Inf/NaN, or |x| >= 2^40 (also integers that look so)
            if (!bad) first = k;
            bad++;
        }
    }
    if (!bad) return;
    std::string who;
    auto it = g_nan_writers.upper_bound(reinterpret_cast<uintptr_t>(to) + size - 1);
    if (it != g_nan_writers.begin() && (--it)->first + 0 >= reinterpret_cast<uintptr_t>(to)) who = it->second;
    logged++;
    log("nan: frame %llu write-back %p (%llu bytes): %llu suspicious floats, first at word %llu = %08x, cs %s",
        (unsigned long long)g_frame.load(), (void*)to, (unsigned long long)size, (unsigned long long)bad,
        (unsigned long long)first, f[first], who.c_str());
}

void trace_pass(const char* what, const Targets& t, uint32_t a, uint32_t b) {
    static const char* env = std::getenv("NX_TRACE_PASSES");
    static const uint64_t from = env ? std::strtoull(env, nullptr, 10) : 0;
    static int n = 0;
    if (!env || g_stats.done < from || n++ >= 4000) return;
    Texture* c0 = t.color_count && t.colors[0] ? get<Texture>(t.colors[0]) : nullptr;
    log("pass: %s %x %x after draw %llu: %u colours, 0 = %llx (%ux%u f%x L%u l%u view f%x) depth %llx", what, a, b,
        (unsigned long long)g_stats.done, t.color_count, (unsigned long long)t.colors[0], c0 ? c0->width : 0,
        c0 ? c0->height : 0, c0 ? c0->format : 0, t.views.level[0], t.views.layer[0], t.views.format[0],
        (unsigned long long)t.depth);
}

// NX_SNAP_FOLLOW=<fs hash>@<first draw>: from the first draw with that fragment shader, the next 48 draws into its
// colour target 0 are logged (shaders, textures) and snapshotted after (frames/00000_<draw>.ppm): which draw of a
// pass paints what.
bool snap_follow(const Targets& t, const ShaderInfo* vs, const ShaderInfo* fs, const std::vector<uint64_t>& handles) {
    static const std::string env = std::getenv("NX_SNAP_FOLLOW") ? std::getenv("NX_SNAP_FOLLOW") : "";
    static const uint64_t from = env.find('@') != std::string::npos ? std::strtoull(env.c_str() + env.find('@') + 1, nullptr, 10) : 0;
    static uint64_t target = 0;
    static int left = 0;
    if (env.empty() || !t.color_count) return false;
    static const int count = std::getenv("NX_SNAP_FOLLOW_COUNT") ? std::atoi(std::getenv("NX_SNAP_FOLLOW_COUNT")) : 48;
    static const bool by_frame = env.find("@f") != std::string::npos;  // <hash>@f<presented frame>
    static const uint64_t frame_from = by_frame ? std::strtoull(env.c_str() + env.find("@f") + 2, nullptr, 10) : 0;
    bool started = by_frame ? g_frame >= frame_from : g_stats.done >= from;
    if (!target && started && ((fs && env.compare(0, 16, fs->hash) == 0) || env.compare(0, 16, vs->hash) == 0))
        target = t.colors[0], left = count;  // the vertex shader's hash matches too
    if (!target || t.colors[0] != target || left <= 0) return false;
    left--;
    std::string tex;
    for (size_t k = 0; k < handles.size(); k++) {
        uint64_t texture = 0;
        SamplerInfo si;
        texture_for(handles[k], texture, si);
        Texture* tx = texture ? get<Texture>(texture) : nullptr;
        char b[96];
        std::snprintf(b, sizeof b, " t%zu=%llx(%ux%u f%x%s)", k, (unsigned long long)texture, tx ? tx->width : 0,
                      tx ? tx->height : 0, tx ? tx->format : 0, tx && tx->image && tx->image->rendered ? " rt" : "");
        tex += b;
    }
    log("follow: draw %llu vs %s fs %s%s", (unsigned long long)g_stats.done, vs->hash.c_str(), fs ? fs->hash.c_str() : "-",
        tex.c_str());
    static const bool nosnap = std::getenv("NX_SNAP_FOLLOW_NOSNAP") != nullptr;  // log only (8 MB per snapshot)
    return !nosnap;
}

// NX_FRAME_SIG=1 (DEBUG-2 #92): per presented frame, the number of draws, a hash of their fragment shaders in order,
// and every small non-indexed draw (fullscreen passes): fragment shader, colour target, a hash of its fragment
// uniform blocks. Frames that render differently from their neighbours stand out in the log.
struct FrameSig {
    uint32_t draws = 0;
    uint32_t order = 2166136261u;
    std::string passes;
    std::map<std::string, int> keys;  // "<fs>" (draws) and "C<cs>" (dispatches): how many
    struct Block {
        const uint8_t* p;
        uint64_t size;
        uint32_t hash;
        std::string name;
    };
    std::map<uint64_t, Block> blocks;  // uniform blocks looked at
    uint32_t zero = 0;
    std::string zeros;
    std::set<std::string> watched;
    std::string watch;
};
FrameSig g_sig;
std::map<std::string, int> g_sig_last;
bool frame_sig_on() {
    static const bool on = std::getenv("NX_FRAME_SIG") != nullptr;
    return on;
}
uint32_t fnv(uint32_t h, const void* p, size_t n) {
    for (size_t k = 0; k < n; k++) h = (h ^ static_cast<const uint8_t*>(p)[k]) * 16777619u;
    return h;
}
void frame_sig_draw(const Targets& t, bool indexed, uint32_t count, uint32_t instances, const ShaderInfo* fs,
                    const TexBindings& tex_bindings, const std::vector<uint64_t>& handles) {
    if (!frame_sig_on()) return;
    // NX_FRAME_SIG=<fs prefix>,...: the first draw of each of those shaders in the frame: its textures (image, size,
    // format, the serial of the pass that last rendered it) and its fragment uniform blocks (address, content hash).
    static const std::string watch = std::getenv("NX_FRAME_SIG");
    if (fs && watch.size() > 2 && watch.find(fs->hash.substr(0, 6)) != std::string::npos &&
        g_sig.watched.insert(fs->hash.substr(0, 6)).second) {
        std::string s = " [" + fs->hash.substr(0, 6);
        for (size_t k = 0; k < tex_bindings.size() && k < handles.size(); k++) {
            uint64_t texture = 0;
            SamplerInfo si;
            texture_for(handles[k], texture, si);
            GpuImage* img = texture ? image_of(texture) : nullptr;
            char b[96];
            std::snprintf(b, sizeof b, " t%zu=%llx/%llx %ux%u f%x r%llu", k, (unsigned long long)(texture & 0xFFFFF),
                          (unsigned long long)(uintptr_t(img) & 0xFFFFFF), img ? img->width : 0, img ? img->height : 0,
                          img ? img->nvn_format : 0, (unsigned long long)(img && img->rendered ? img->rendered_at : 0));
            s += b;
            if (img && img->rendered && watch.find('+') != std::string::npos) s += explain_texture_slot(handles[k]);
        }
        for (int n = 0; n < 18; n++)
            if (const Range& r = g_state.uniforms[4][n]; r.address && r.size)
                if (const uint8_t* p = guest_bytes(r.address)) {
                    char b[64];
                    std::snprintf(b, sizeof b, " c%d=%llx:%08x", n, (unsigned long long)r.address,
                                  fnv(2166136261u, p, std::min<uint64_t>(r.size, 4096)));
                    s += b;
                }
        g_sig.watch += s + "]";
    }
    g_sig.draws++;
    g_sig.order = fnv(g_sig.order, fs ? fs->hash.data() : "-", fs ? std::min<size_t>(fs->hash.size(), 8) : 1);
    char k[48];
    std::snprintf(k, sizeof k, "%.6s", fs ? fs->hash.c_str() : "-");
    g_sig.keys[k]++;
    // Uniform blocks of the vertex and fragment stages: all zeros, or rewritten between their first use in the frame
    // and its present (a block caught while the game rewrites it).
    for (int st : {0, 4})
        for (int n = 0; n < 18; n++)
            if (const Range& r = g_state.uniforms[st][n]; r.address && r.size >= 16 && !g_sig.blocks.count(r.address))
                if (const uint8_t* p = guest_bytes(r.address)) {
                    uint64_t size = std::min<uint64_t>(r.size, 4096), z = 0;
                    char b[96];
                    std::snprintf(b, sizeof b, " %c%d:%llx+%llx(%.6s)", st ? 'f' : 'v', n,
                                  (unsigned long long)r.address, (unsigned long long)r.size, fs ? fs->hash.c_str() : "-");
                    g_sig.blocks[r.address] = {p, size, fnv(2166136261u, p, size), b};
                    while (z < size && !p[z]) z++;
                    if (z == size && (g_sig.zero++, g_sig.zeros.size() < 600)) g_sig.zeros += b;
                }
    if (indexed || count > 6 || instances != 1 || g_sig.passes.size() > 2000) return;
    uint32_t u = 2166136261u;
    for (int n = 0; n < 18; n++)
        if (const Range& r = g_state.uniforms[4][n]; r.address && r.size)
            if (const uint8_t* p = guest_bytes(r.address)) u = fnv(u, p, std::min<uint64_t>(r.size, 4096));
    char b[64];
    std::snprintf(b, sizeof b, " %.6s>%llx:%08x", fs ? fs->hash.c_str() : "-",
                  (unsigned long long)(t.colors[0] & 0xFFFFF), u);
    g_sig.passes += b;
}

}  // namespace nx::nvn::replay

namespace nx::nvn {
void frame_sig_flush(uint64_t presents) {
    using replay::g_sig;
    if (!replay::frame_sig_on()) return;
    std::string diff;  // against the frame before: +key*n new or more, -key*n gone or fewer
    auto& last = replay::g_sig_last;
    for (auto& [k, n] : g_sig.keys)
        if (auto it = last.find(k); it == last.end() || it->second != n)
            diff += " " + k + "*" + std::to_string(n - (it == last.end() ? 0 : it->second));
    for (auto& [k, n] : last)
        if (!g_sig.keys.count(k)) diff += " " + k + "*-" + std::to_string(n);
    if (diff.size() > 1500) diff.resize(1500);
    static auto t_last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(now - t_last).count();
    t_last = now;
    uint32_t changed = 0;
    std::string ch;
    for (auto& [a, b] : g_sig.blocks)
        if (replay::fnv(2166136261u, b.p, b.size) != b.hash && (changed++, ch.size() < 800)) ch += b.name;
    log("fsig %llu: %.1f ms, %u draws, order %08x:%s | %zu blocks, %u zero:%s | %u rewritten:%s | watch%s | diff%s",
        (unsigned long long)presents, ms, g_sig.draws, g_sig.order, g_sig.passes.c_str(), g_sig.blocks.size(),
        g_sig.zero, g_sig.zeros.c_str(), changed, ch.c_str(), g_sig.watch.c_str(), diff.c_str());
    last = std::move(g_sig.keys);
    g_sig = {};
}
}  // namespace nx::nvn

namespace nx::nvn::replay {

void trace_draw(const Targets& t, uint32_t mode, bool indexed, const Cmd& c, const ShaderInfo* vs,
                const ShaderInfo* fs, const TexBindings& tex_bindings, const std::vector<uint64_t>& handles) {
    // Always: the first draws with a huge vertex or instance count (a GPU hang candidate).
    static int big = 0;
    uint32_t count = uint32_t(indexed ? c.i[0] : c.i[1]), instances = uint32_t(std::max(1, c.i[3]));
    if ((count > (1u << 18) || instances > 1024 || uint64_t(count) * instances > (1u << 20)) && big++ < 40)
        log("nvn: huge draw %llu: vs %s fs %s %s count %u x%u", (unsigned long long)g_stats.done, vs->hash.c_str(),
            fs ? fs->hash.c_str() : "-", indexed ? "indexed" : "arrays", count, instances);
    trace_cube_draw(t, vs, fs, tex_bindings, handles);
    frame_sig_draw(t, indexed, count, instances, fs, tex_bindings, handles);
    // NX_TRACE_DRAWS=<first draw>: logs 80 draws from there.
    static const char* trace_env = std::getenv("NX_TRACE_DRAWS");
    static const uint64_t trace_from = trace_env ? std::strtoull(trace_env, nullptr, 10) : 0;
    // NX_TRACE_TARGET=<hex NVNtexture*>: only draws into that color target.
    static const char* target_env = std::getenv("NX_TRACE_TARGET");
    static const uint64_t trace_target = target_env ? std::strtoull(target_env, nullptr, 16) : 0;
    static uint64_t traced = 0;
    // NX_TRACE_FS_HASH=<hash>[,<hash>...]: only draws with those fragment shaders.
    static const char* fs_env = std::getenv("NX_TRACE_FS_HASH");
    static const uint64_t trace_count = std::getenv("NX_TRACE_COUNT") ? std::strtoull(std::getenv("NX_TRACE_COUNT"), nullptr, 10) : 80;
    if (trace_env && g_stats.done >= trace_from && traced < trace_count && (!trace_target || t.colors[0] == trace_target) &&
        (!fs_env || (fs && std::strstr(fs_env, fs->hash.c_str())))) {  // one hash or a comma list
        traced++;
        // NX_TRACE_VERTS=1: the first 3 vertices of stream 0 as floats and halves (8 words each at most).
        if (std::getenv("NX_TRACE_VERTS") && !g_state.streams.empty())
            if (const Range& vb = g_state.vertex_buffers[0]; vb.address)
                if (const auto* w = reinterpret_cast<const uint32_t*>(guest_bytes(vb.address))) {
                    uint32_t stride = g_state.streams[0].stride, words = std::min<uint32_t>(stride / 4, 8);
                    for (uint32_t v = 0; v < 3 && uint64_t(v + 1) * stride <= vb.size; v++) {
                        std::string line;
                        for (uint32_t k = 0; k < words; k++) {
                            uint32_t x = w[v * stride / 4 + k];
                            float f;
                            std::memcpy(&f, &x, 4);
                            char b[48];
                            std::snprintf(b, sizeof b, " %g[%08x]", f, x);
                            line += b;
                        }
                        log("  vertex %u (stride %u):%s", v, stride, line.c_str());
                    }
                }
        std::string tex;
        for (size_t k = 0; k < tex_bindings.size(); k++) {
            uint64_t texture = 0;
            SamplerInfo si;
            ViewInfo vw;
            texture_for(handles[k], texture, si, &vw);
            Texture* tx = texture ? get<Texture>(texture) : nullptr;
            char b[96];
            if (std::getenv("NX_TRACE_GLYPHS") || std::getenv("NX_TRACE_SAMPLERS"))  // NX_TRACE_SAMPLERS: only these
                log("  sampler for t%zu: min %u mag %u wrap %u/%u/%u compare %u/%u reduction %u lod %g..%g bias %g aniso %g; "
                    "view levels %u+%u layers %u+%u swizzle %08x format %#x; texture levels %u swizzle %u%u%u%u pool %08x",
                    k, si.min_filter, si.mag_filter, si.wrap[0], si.wrap[1], si.wrap[2], si.compare_mode, si.compare_func,
                    si.reduction, si.lod_min, si.lod_max, si.lod_bias, si.max_anisotropy, vw.base_level, vw.levels,
                    vw.min_layer, vw.layers, vw.swizzle, vw.format, tx ? tx->levels : 0, tx ? tx->swizzle[0] : 0,
                    tx ? tx->swizzle[1] : 0, tx ? tx->swizzle[2] : 0, tx ? tx->swizzle[3] : 0,
                    tx ? tx->pool_swizzle.load() : 0);
            std::snprintf(b, sizeof b, " t%zu=%llx:%llx(%ux%u f%x%s)", k, (unsigned long long)handles[k],
                          (unsigned long long)texture, tx ? tx->width : 0, tx ? tx->height : 0, tx ? tx->format : 0,
                          tx && tx->image && tx->image->rendered ? " rt" : "");
            tex += b;
        }
        if (std::getenv("NX_TRACE_VS_BLOCKS"))
            if (const Range& u = g_state.uniforms[0][0]; u.address)
                if (auto* f = reinterpret_cast<const float*>(guest_bytes(u.address)))
                    for (int row = 0; row < std::min<int>(int(u.size / 16), 24); row++)
                        log("  vs c3[%d]: %g %g %g %g", row, f[4 * row], f[4 * row + 1], f[4 * row + 2], f[4 * row + 3]);
        for (int blk = 1; blk <= 3 && std::getenv("NX_TRACE_VS_BLOCKS"); blk++)
            if (const Range& u = g_state.uniforms[0][blk]; u.address)
                if (auto* f = reinterpret_cast<const float*>(guest_bytes(u.address)))
                    log("  vs c%d: %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g", blk + 3, f[0], f[1], f[2], f[3],
                        f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
        if (const Range& u = g_state.uniforms[0][0]; u.address)
            if (auto* f = reinterpret_cast<const float*>(guest_bytes(u.address)))
                log("  vs c3 @%llx: %g %g %g %g | %g %g %g %g", (unsigned long long)u.address, f[0], f[1], f[2], f[3], f[4],
                    f[5], f[6], f[7]);
        if (const Range& u = g_state.uniforms[1][0]; u.address)
            if (auto* f = reinterpret_cast<const float*>(guest_bytes(u.address)))
                for (uint64_t k = 0; k < std::min<uint64_t>(u.size / 16, 8); k++)
                    log("  fs c3[%llu] @%llx: %g %g %g %g", (unsigned long long)k, (unsigned long long)u.address,
                        f[4 * k], f[4 * k + 1], f[4 * k + 2], f[4 * k + 3]);
        if (const Range& r = g_state.storage[0][0]; r.address && std::getenv("NX_TRACE_GLYPHS"))
            if (auto* g = guest_bytes(r.address))
                for (uint64_t k = 0; k < std::min<uint64_t>(r.size / 64, 3); k++) {
                    const uint32_t* w = reinterpret_cast<const uint32_t*>(g + 64 * k);
                    if (Texture* at = get<Texture>(0x4383061d8ull); at && at->memory && at->stride) {
                        float u0, v0, u1, v1;
                        std::memcpy(&u0, w + 4, 4), std::memcpy(&v0, w + 5, 4), std::memcpy(&u1, w + 6, 4), std::memcpy(&v1, w + 7, 4);
                        uint32_t x = uint32_t((u0 + u1) * 0.5f * at->width), y = uint32_t((v0 + v1) * 0.5f * at->height);
                        const uint8_t* m = reinterpret_cast<const uint8_t*>(at->memory);
                        uint32_t nz = 0, mx = 0;
                        for (int dy = -20; dy <= 20; dy++)
                            for (int dx = -20; dx <= 20; dx++) {
                                uint8_t v = m[uint64_t(y + dy) * at->stride + x + dx];
                                nz += v != 0, mx = std::max<uint32_t>(mx, v);
                            }
                        log("  glyph %llu atlas memory at %u,%u: %u of 1681 non-zero, max %u", (unsigned long long)k, x, y, nz, mx);
                    }
                    log("  glyph %llu: %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
                        (unsigned long long)k, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11],
                        w[12], w[13], w[14], w[15]);
                }
        for (int k = 0; k < 2; k++)
            if (const Range& r = g_state.storage[0][k]; r.address) {
                auto* f = reinterpret_cast<const float*>(guest_bytes(r.address));
                log("  vs s%d @%llx+%llx: %g %g %g %g %g %g %g %g", k, (unsigned long long)r.address,
                    (unsigned long long)r.size, f ? f[0] : -1, f ? f[1] : -1, f ? f[2] : -1, f ? f[3] : -1,
                    f ? f[4] : -1, f ? f[5] : -1, f ? f[6] : -1, f ? f[7] : -1);
            }
        {
            const auto& k = vs->constants;
            std::string w;
            for (size_t i = 0; i + 4 <= std::min<size_t>(k.size(), 256); i += 4) {
                char t[16];
                uint32_t v;
                std::memcpy(&v, k.data() + i, 4);
                std::snprintf(t, sizeof t, " %x", v);
                w += t;
            }
            log("  vs constants %zu bytes:%s", k.size(), w.c_str());
        }
        if (fs) {
            const auto& k = fs->constants;
            const float* f = reinterpret_cast<const float*>(k.data());
            log("  fs constants %zu bytes: %g %g %g %g", k.size(), k.size() >= 4 ? f[0] : -1, k.size() >= 8 ? f[1] : -1,
                k.size() >= 12 ? f[2] : -1, k.size() >= 16 ? f[3] : -1);
        }
        const BlendTarget& b0 = g_state.blend[0];
        log("  blend %u (%u %u %u %u eq %u %u) mask %x logic %x alpha %x fmt %d", g_state.color.blend_enable[0], b0.src_rgb,
            b0.dst_rgb, b0.src_a, b0.dst_a, b0.eq_rgb, b0.eq_a, g_state.mask.mask[0], g_state.color.logic_op,
            g_state.color.alpha_test, t.colors[0] ? int(image_of(t.colors[0])->format) : -1);
        log("  depth func %u write %u", g_state.depth.depth_func, g_state.depth.depth_write);
        log("  viewport %d %d %d %d (%d) scissor %d %d %d %d (%d) cull %u front %u depth %g..%g test %u", g_state.viewport[0],
            g_state.viewport[1], g_state.viewport[2], g_state.viewport[3], g_state.has_viewport, g_state.scissor[0],
            g_state.scissor[1], g_state.scissor[2], g_state.scissor[3], g_state.has_scissor, g_state.polygon.cull,
            g_state.polygon.front, g_state.depth_near, g_state.depth_far, g_state.depth.depth_test);
        log("draw %llu: vs %s fs %s mode %u %s count %d x%d targets %u:%llx depth %llx%s", (unsigned long long)g_stats.done,
            vs->hash.c_str(), fs ? fs->hash.c_str() : "-", mode, indexed ? "indexed" : "arrays",
            indexed ? c.i[0] : c.i[1], c.i[3], t.color_count, (unsigned long long)t.colors[0], (unsigned long long)t.depth, tex.c_str());
    }}

// NX_TRACE_SKIN=1: the first 3 draws of every vertex shader with an integer vertex input (skinned models: bone
// indices), 60 at most: each input's format and stream, the vertex uniform blocks, the first vertices' integer input
// and weights (location 4 bytes), and the bone rows (3 vec4 at index * 48 of c8) they select.
namespace {

// Component size and count of an integer vertex format; {0, 0} if it is not one.
std::pair<uint32_t, uint32_t> int_layout(uint32_t format) {
    switch (vk_vertex_format(format)) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: return {1, 1};
        case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT: return {1, 2};
        case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8_SINT: return {1, 3};
        case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: return {1, 4};
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: return {2, 1};
        case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: return {2, 2};
        case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16_SINT: return {2, 3};
        case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT: return {2, 4};
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: return {4, 1};
        case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: return {4, 2};
        case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: return {4, 3};
        case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: return {4, 4};
        default: return {0, 0};
    }
}

const uint8_t* attrib_at(const VertexAttrib& va, uint32_t vertex) {
    if (va.stream >= g_state.streams.size() || va.stream >= 16) return nullptr;
    const Range& vb = g_state.vertex_buffers[va.stream];
    uint64_t at = uint64_t(vertex) * g_state.streams[va.stream].stride + va.offset;
    if (!vb.address || at + 16 > vb.size) return nullptr;
    return guest_bytes(vb.address + at);
}

std::string floats(const uint8_t* p, uint32_t n) {
    std::string s;
    for (uint32_t k = 0; k < n; k++) {
        float f;
        std::memcpy(&f, p + 4 * k, 4);
        char b[24];
        std::snprintf(b, sizeof b, "%s%.4g", k ? " " : "", f);
        s += b;
    }
    return s;
}

uint32_t read_uint(const uint8_t* p, uint32_t size, uint32_t k) {
    uint32_t v = 0;
    std::memcpy(&v, p + size * k, size);
    return v;
}

}  // namespace

void trace_skin(bool indexed, const Cmd& c, const ShaderInfo* vs, const ShaderInfo* fs) {
    static const bool on = std::getenv("NX_TRACE_SKIN") != nullptr;
    static std::map<const ShaderInfo*, int> seen;
    static int total = 0;
    if (!on || !vs || total >= 60) return;
    int int_loc = -1;
    for (const ShaderIo& in : vs->refl.inputs)
        if (in.location < g_state.attribs.size() && int_layout(g_state.attribs[in.location].format).first)
            int_loc = int(in.location);
    if (int_loc < 0 || seen[vs]++ >= 3) return;
    total++;
    uint32_t count = uint32_t(indexed ? c.i[0] : c.i[1]);
    log("skin: vs %s fs %s %s count %u base %d first %d x%d", vs->hash.c_str(), fs ? fs->hash.c_str() : "-",
        indexed ? "indexed" : "arrays", count, indexed ? c.i[1] : 0, indexed ? 0 : c.i[0], c.i[3]);
    std::string line;
    for (const ShaderIo& in : vs->refl.inputs) {
        if (in.location >= g_state.attribs.size()) continue;
        const VertexAttrib& va = g_state.attribs[in.location];
        uint32_t stride = va.stream < g_state.streams.size() ? g_state.streams[va.stream].stride : 0;
        char b[80];
        std::snprintf(b, sizeof b, " %u:%x/s%u+%u/%u", in.location, va.format, va.stream, va.offset, stride);
        line += b;
    }
    log("skin:  inputs (loc:format/stream+offset/stride)%s", line.c_str());
    // The vertex stage's uniform blocks: name, binding n, size, first row.
    const uint8_t* bank[18] = {};
    uint64_t bank_size[18] = {};
    for (const DescriptorBinding& b : vs->refl.bindings) {
        if (b.nvn.source != NvnSource::UniformBlock || b.nvn.index < 0 || b.nvn.index >= 18) continue;
        const Range& r = g_state.uniforms[0][b.nvn.index];
        const uint8_t* p = r.address ? guest_bytes(r.address) : nullptr;
        if (b.nvn.bank >= 0 && b.nvn.bank < 18) bank[b.nvn.bank] = p, bank_size[b.nvn.bank] = p ? r.size : 0;
        log("skin:  %s = block %d bank %d, %llu bytes%s%s", b.name.c_str(), b.nvn.index, b.nvn.bank,
            (unsigned long long)r.size, p ? ", row 0: " : " (unbound)", p ? floats(p, 4).c_str() : "");
    }
    if (bank[4] && bank_size[4] >= 0x90) log("skin:  c4[8] %s", floats(bank[4] + 0x80, 4).c_str());
    // The vertices (through the index buffer): the first 4 in detail, the largest bone index over all.
    const uint8_t* idx = indexed ? guest_bytes(c.a[0]) : nullptr;
    uint32_t isz = indexed ? index_size(c.mask) : 0;
    const VertexAttrib& ia = g_state.attribs[int_loc];
    auto [size, n] = int_layout(ia.format);
    uint32_t max_bone = 0, missing = 0;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t v = idx ? read_uint(idx, isz, k) + uint32_t(c.i[1]) : uint32_t(c.i[0]) + k;
        const uint8_t* p = attrib_at(ia, v);
        if (!p) {
            missing++;
            continue;
        }
        uint32_t bones[4] = {};
        for (uint32_t j = 0; j < n; j++) max_bone = std::max(max_bone, bones[j] = read_uint(p, size, j));
        if (k >= 4) continue;
        std::string w = "-";
        if (g_state.attribs.size() > 4)
            if (const uint8_t* q = attrib_at(g_state.attribs[4], v)) {
                char b[64];
                std::snprintf(b, sizeof b, "%02x%02x%02x%02x %02x%02x%02x%02x", q[0], q[1], q[2], q[3], q[4], q[5],
                              q[6], q[7]);
                w = b;
            }
        log("skin:  vertex %u: loc%d %u %u %u %u, loc4 bytes %s", v, int_loc, bones[0], bones[1], bones[2], bones[3],
            w.c_str());
        for (uint32_t j = 0; j < n && j < 2 && bank[8]; j++)
            if (uint64_t(bones[j]) * 48 + 48 <= bank_size[8])
                log("skin:   bone %u: %s | %s | %s", bones[j], floats(bank[8] + bones[j] * 48, 4).c_str(),
                    floats(bank[8] + bones[j] * 48 + 16, 4).c_str(), floats(bank[8] + bones[j] * 48 + 32, 4).c_str());
            else
                log("skin:   bone %u: past c8 (%llu bytes)", bones[j], (unsigned long long)bank_size[8]);
    }
    log("skin:  max bone index %u (c8 needs %u bytes, bound %llu); %u vertices outside their buffer", max_bone,
        (max_bone + 1) * 48, (unsigned long long)bank_size[8], missing);
}

// The first dispatch of each compute shader: its groups and what its bindings point at.
void trace_compute_once(const ShaderInfo* cs, uint32_t x, uint32_t y, uint32_t z) {
        if (frame_sig_on()) g_sig.keys["C" + cs->hash.substr(0, 6)]++;
        static std::set<std::string> seen;
        if (seen.insert(cs->hash).second) {
            std::string res;
            for (const DescriptorBinding& b : cs->refl.bindings)
                res += " " + b.name + "(s" + std::to_string(b.set) + ")";
            log("nvn: compute %s groups %ux%ux%u local %ux%ux%u:%s", cs->hash.c_str(), x, y, z, cs->local_size[0],
                cs->local_size[1], cs->local_size[2], res.c_str());
            for (const DescriptorBinding& b : cs->refl.bindings) {
                const Range* r = b.nvn.source == NvnSource::StorageBuffer && b.nvn.index >= 0 && b.nvn.index < 16
                                     ? &g_state.storage[5][b.nvn.index]
                                 : (b.nvn.source == NvnSource::UniformBlock || b.nvn.source == NvnSource::UniformAsStorage) &&
                                         b.nvn.index >= 0 && b.nvn.index < 18
                                     ? &g_state.uniforms[5][b.nvn.index]
                                     : nullptr;
                log("  %s: %s %d -> %llx+%llx", b.name.c_str(), source_name(b.nvn.source), b.nvn.index,
                    (unsigned long long)(r ? r->address : 0), (unsigned long long)(r ? r->size : 0));
            }
        }
}

// Debug overrides of a draw's fragment shader; false: drop the draw.
bool debug_shader_overrides(const std::vector<const ShaderInfo*>*& shaders, const ShaderInfo*& fs,
                            std::vector<const ShaderInfo*>& swapped) {
    // NX_SWAP_FS=<from>:<to>: draws with fragment shader <from> use <to> instead (loaded already), to see what a
    // shader's geometry covers or what its textures hold.
    static const std::string swap = std::getenv("NX_SWAP_FS") ? std::getenv("NX_SWAP_FS") : "";
    if (!swap.empty() && fs && swap.compare(0, 16, fs->hash) == 0 && swap.size() > 17)
        if (const ShaderInfo* to = shader_by_hash(swap.substr(17))) {
            swapped = *shaders;
            std::replace(swapped.begin(), swapped.end(), fs, to);
            shaders = &swapped;
            fs = to;
        }
    // NX_SKIP_FS=<hash>,<hash>: draws with those fragment shaders are dropped (to measure what they cost).
    static const std::string skip = std::getenv("NX_SKIP_FS") ? std::getenv("NX_SKIP_FS") : "";
    return skip.empty() || !fs || skip.find(fs->hash) == std::string::npos;
}

}  // namespace nx::nvn::replay
