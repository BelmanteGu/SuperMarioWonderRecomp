// NVN objects, CPU side (docs/NVN.md, N3): memory pools and GPU addresses, buffers, textures, samplers, texture and
// sampler pools. Signatures come from the game's call sites (nvn_trace.txt); builder state is read back
// through the SDK driver's getters (nvn.h). The Vulkan side hangs off these objects.
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <set>

#include "nvn.h"
#include "nvn_ultrawide.h"
#include "platform.h"

namespace nx::nvn {

std::recursive_mutex& gpu_lock();  // nvn_vk.cpp

// ---- GPU virtual addresses --------------------------------------------------------------------------------------

namespace {

std::mutex g_gpu_lock;
std::map<uint64_t, MemoryPool*> g_pools_by_gpu;  // gpu start -> pool
std::atomic<uint64_t> g_pools_generation{1};     // bumped with every change of the map, under g_gpu_lock
uint64_t g_gpu_next = 0x100000000ull;  // NVN GPU addresses are 40-bit; keep 0 and low values invalid

}  // namespace

uint64_t gpu_alloc(uint64_t size) {
    std::lock_guard lk(g_gpu_lock);
    uint64_t a = g_gpu_next;
    g_gpu_next = (a + size + 0x1FFFF) & ~0xFFFFull;  // 64 KiB aligned, a guard gap between pools
    return a;
}

void pool_register(MemoryPool* p) {
    std::lock_guard lk(g_gpu_lock);
    g_pools_by_gpu[p->gpu] = p, g_pools_generation.fetch_add(1);
}

void pool_unregister(MemoryPool* p) {
    std::lock_guard lk(g_gpu_lock);
    g_pools_by_gpu.erase(p->gpu), g_pools_generation.fetch_add(1);
}

// Asked for every buffer a replayed draw binds, mostly in the same few pools: each thread keeps the last ranges it
// found, good until a pool is registered or unregistered (a pool's address and size are final when it registers).
MemoryPool* pool_at_gpu(uint64_t gpu, uint64_t* offset) {
    thread_local struct { uint64_t generation, start, size; MemoryPool* pool; } hits[8] = {};
    thread_local uint32_t next = 0;
    uint64_t generation = g_pools_generation.load(std::memory_order_acquire);
    for (const auto& h : hits)
        if (h.generation == generation && gpu - h.start < h.size) {
            if (offset) *offset = gpu - h.start;
            return h.pool;
        }
    std::lock_guard lk(g_gpu_lock);
    auto it = g_pools_by_gpu.upper_bound(gpu);
    if (it == g_pools_by_gpu.begin()) return nullptr;
    --it;
    MemoryPool* p = it->second;
    if (gpu - p->gpu >= p->size) return nullptr;
    hits[next++ & 7] = {g_pools_generation.load(std::memory_order_relaxed), p->gpu, p->size, p};
    if (offset) *offset = gpu - p->gpu;
    return p;
}

ViewInfo read_view(CPU& c, uint64_t view) {
    ViewInfo v;
    if (!view) return v;
    uint64_t tmp = scratch(c, 16);
    // Each getter returns whether the view sets that property.
    if (driver(c, "nvnTextureViewGetLevels", {view, tmp, tmp + 4}) & 1)
        v.base_level = ld<uint32_t>(tmp), v.levels = ld<uint32_t>(tmp + 4);
    if (driver(c, "nvnTextureViewGetLayers", {view, tmp, tmp + 4}) & 1)
        v.min_layer = ld<uint32_t>(tmp), v.layers = ld<uint32_t>(tmp + 4);
    for (int k = 0; k < 4; k++) st<uint32_t>(tmp + 4 * k, 2u + k);
    if (driver(c, "nvnTextureViewGetSwizzle", {view, tmp, tmp + 4, tmp + 8, tmp + 12}) & 1)
        for (int k = 0; k < 4; k++) v.swizzle |= std::min<uint32_t>(ld<uint32_t>(tmp + 4 * k), 5) << (8 * k);
    if (driver(c, "nvnTextureViewGetFormat", {view, tmp}) & 1) v.format = ld<uint32_t>(tmp);
    return v;
}

uint64_t pool_host(const MemoryPool& pool, uint64_t offset) {
    if (pool.memory) return pool.memory + offset;
    std::lock_guard lk(objects_lock());
    auto it = pool.mappings.upper_bound(offset);
    if (it == pool.mappings.begin()) return 0;
    --it;
    if (offset - it->first >= it->second.size) return 0;
    auto* phys = dynamic_cast<MemoryPool*>(objects().count(it->second.physical) ? objects()[it->second.physical].get() : nullptr);
    return phys && phys->memory ? phys->memory + it->second.physical_offset + (offset - it->first) : 0;
}

namespace {

// ---- memory pools -----------------------------------------------------------------------------------------------

// NVNboolean nvnMemoryPoolInitialize(NVNmemoryPool*, const NVNmemoryPoolBuilder*)
void MemoryPoolInitialize(CPU& c) {
    uint64_t guest = c.x[0], builder = c.x[1];
    auto& p = make<MemoryPool>(guest);
    p.memory = driver(c, "nvnMemoryPoolBuilderGetMemory", {builder});
    p.size = driver(c, "nvnMemoryPoolBuilderGetSize", {builder});
    p.flags = uint32_t(driver(c, "nvnMemoryPoolBuilderGetFlags", {builder}));
    p.gpu = gpu_alloc(p.size);
    pool_register(&p);
    if (g_trace)
        log("nvn: pool %llx: memory %llx size %llx flags %x gpu %llx", (unsigned long long)guest,
            (unsigned long long)p.memory, (unsigned long long)p.size, p.flags, (unsigned long long)p.gpu);
    c.x[0] = 1;
}

void MemoryPoolFinalize(CPU& c) {
    wait_submits_so_far(kWaitInvalidate);  // a submit replayed on another thread may still read it
    worker_finish();                    // the replay thread may still read it
    gpu_wait(gpu_write_back_serial());  // NX_ASYNC_GPU: no compute result lands in freed memory
    if (auto* p = get<MemoryPool>(c.x[0])) pool_unregister(p), command_memory_released(p->gpu, p->size);
    destroy(c.x[0]);
}

void MemoryPoolMap(CPU& c) {  // void* (const NVNmemoryPool*)
    auto* p = get<MemoryPool>(c.x[0]);
    c.x[0] = p ? p->memory : 0;
}

void MemoryPoolGetBufferAddress(CPU& c) {  // NVNbufferAddress (const NVNmemoryPool*)
    auto* p = get<MemoryPool>(c.x[0]);
    c.x[0] = p ? p->gpu : 0;
}

void MemoryPoolGetSize(CPU& c) {
    auto* p = get<MemoryPool>(c.x[0]);
    c.x[0] = p ? p->size : 0;
}

void MemoryPoolGetFlags(CPU& c) {
    auto* p = get<MemoryPool>(c.x[0]);
    c.x[0] = p ? p->flags : 0;
}

// NVNboolean nvnMemoryPoolMapVirtual(NVNmemoryPool* virtualPool, int count, const NVNmappingRequest* requests):
// each request maps [virtualOffset, +size) of the virtual pool onto [physicalOffset, +size) of a physical pool.
// Request layout (0x28 bytes, logged once to check): {NVNmemoryPool* physicalPool; s64 physicalOffset;
// s64 virtualOffset; u64 size; NVNstorageClass storageClass}.
void MemoryPoolMapVirtual(CPU& c) {
    auto* v = get<MemoryPool>(c.x[0]);
    uint32_t n = uint32_t(c.x[1]);
    static int logged = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint64_t r = c.x[2] + 0x28ull * k;
        uint64_t phys = ld<uint64_t>(r), poff = ld<uint64_t>(r + 8), voff = ld<uint64_t>(r + 16), size = ld<uint64_t>(r + 24);
        if (logged < 4) {
            logged++;
            log("nvn: map virtual %llx: phys pool %llx +%llx -> virtual +%llx size %llx (class %x)", (unsigned long long)c.x[0],
                (unsigned long long)phys, (unsigned long long)poff, (unsigned long long)voff, (unsigned long long)size,
                ld<uint32_t>(r + 32));
        }
        if (!v) continue;
        std::lock_guard lk(objects_lock());
        if (phys)
            v->mappings[voff] = {phys, poff, size};
        else
            v->mappings.erase(voff);  // unmap
    }
    c.x[0] = 1;
}

// Textures by memory pool, to find the ones a flushed range covers.
std::map<uint64_t, std::set<uint64_t>>& pool_textures() {
    static std::map<uint64_t, std::set<uint64_t>> m;
    return m;
}

// Guest memory is host memory, so buffers need nothing. Textures are copies on the GPU (untiled at first use): a
// flush over one means the CPU wrote it (a glyph cache rasterises text this way), so it is uploaded again.
// (NVNmemoryPool*, ptrdiff_t offset, size_t size)
void MemoryPoolFlushMappedRange(CPU& c) {
    uint64_t pool = c.x[0], off = c.x[1], size = c.x[2];
    std::lock_guard lk(objects_lock());
    auto it = pool_textures().find(pool);
    if (it == pool_textures().end()) return;
    for (uint64_t guest : it->second) {
        auto o = objects().find(guest);
        auto* t = o == objects().end() ? nullptr : dynamic_cast<Texture*>(o->second.get());
        if (!t || t->offset >= off + size || t->offset + t->storage_size <= off) continue;
        if (t->image) {  // rendered images ignore it (nvn_pipeline.cpp)
            uint64_t base = std::max(off, t->offset), end = std::min(off + size, t->offset + t->storage_size);
            t->mark_dirty(base - t->offset, end - t->offset);
        }
    }
}
// The CPU is about to read what the GPU wrote: with NX_ASYNC_GPU, compute results still in flight are waited for.
void MemoryPoolInvalidateMappedRange(CPU&) {
    wait_submits_so_far(kWaitFinalize);  // a submit replayed on another thread: its results first (nvn_queue.cpp)
    worker_finish();  // compute results reach guest memory when their submit is replayed and done
    gpu_wait(gpu_write_back_serial());
}

// ---- buffers ----------------------------------------------------------------------------------------------------

struct Buffer : Object {
    uint64_t pool = 0;  // guest NVNmemoryPool*
    uint64_t offset = 0, size = 0;
    uint64_t memory = 0, gpu = 0;
};

void BufferInitialize(CPU& c) {  // NVNboolean (NVNbuffer*, const NVNbufferBuilder*)
    uint64_t guest = c.x[0], builder = c.x[1];
    auto& b = make<Buffer>(guest);
    b.pool = driver(c, "nvnBufferBuilderGetMemoryPool", {builder});
    b.offset = driver(c, "nvnBufferBuilderGetMemoryOffset", {builder});
    b.size = driver(c, "nvnBufferBuilderGetSize", {builder});
    if (auto* p = get<MemoryPool>(b.pool)) {
        b.memory = pool_host(*p, b.offset);
        b.gpu = p->gpu + b.offset;
    }
    c.x[0] = 1;
}

void BufferFinalize(CPU& c) { destroy(c.x[0]); }

void BufferMap(CPU& c) {
    auto* b = get<Buffer>(c.x[0]);
    c.x[0] = b ? b->memory : 0;
}

void BufferGetAddress(CPU& c) {
    auto* b = get<Buffer>(c.x[0]);
    c.x[0] = b ? b->gpu : 0;
}

void BufferGetSize(CPU& c) {
    auto* b = get<Buffer>(c.x[0]);
    c.x[0] = b ? b->size : 0;
}

void BufferGetMemoryPool(CPU& c) {
    auto* b = get<Buffer>(c.x[0]);
    c.x[0] = b ? b->pool : 0;
}

void BufferGetMemoryOffset(CPU& c) {
    auto* b = get<Buffer>(c.x[0]);
    c.x[0] = b ? b->offset : 0;
}

void BufferFlushMappedRange(CPU&) {}
void BufferInvalidateMappedRange(CPU&) {}

// ---- textures ---------------------------------------------------------------------------------------------------

// NX_TRACE_TEXTURES=1 (or =<frame>: from that presented frame on) logs texture inits, finalizations and pool
// registrations.
bool trace_textures() {
    static const char* e = std::getenv("NX_TRACE_TEXTURES");
    static const uint64_t from = e ? std::strtoull(e, nullptr, 10) : 0;
    return e && g_frame >= from;
}

uint64_t keep_for_slots(uint64_t guest);  // below, with the pool registrations
void hand_image_to_snapshots(uint64_t guest);  // below

// NX_REINIT_WAITS_REPLAY=1 (experiment): texture objects are re-initialized and finalized only between replays, never
// while a submit is being replayed on another thread (the intermittent course band, DEBUG.md #54).
std::unique_lock<std::recursive_mutex> reinit_guard() {
    static const bool wait = std::getenv("NX_REINIT_WAITS_REPLAY") != nullptr;
    return wait ? std::unique_lock<std::recursive_mutex>(gpu_lock()) : std::unique_lock<std::recursive_mutex>();
}

void TextureInitialize(CPU& c) {  // NVNboolean (NVNtexture*, const NVNtextureBuilder*)
    auto guard = reinit_guard();
    uint64_t guest = c.x[0], builder = c.x[1];
    // Re-initializing a texture a pool slot still names (without finalizing it: Wonder reuses the structs of its
    // dynamic-resolution targets every frame) keeps the old one for those slots, as the pool's descriptor copy
    // would: replayed later, a frame's draws sampled another frame's AO and ambient-light buffers (dark course
    // actors, docs/nvn/DEBUG.md #39).
    // The new incarnation is built aside and published whole, in the section that retires the old one (#55): a replay
    // on another thread could see a texture half built (a size, memory 0) or a gap with none. NX_TEXTURE_RACE_OLD=1:
    // the old one goes first and the new one is filled in place.
    static const bool old = texture_race_old();
    std::unique_ptr<Texture> fresh = old ? nullptr : std::make_unique<Texture>();
    if (old) {
        std::lock_guard lk(objects_lock());
        keep_for_slots(guest);
        hand_image_to_snapshots(guest);
    }
    auto& t = old ? make<Texture>(guest) : *fresh;
    t.target = uint32_t(driver(c, "nvnTextureBuilderGetTarget", {builder}));
    t.format = uint32_t(driver(c, "nvnTextureBuilderGetFormat", {builder}));
    t.flags = uint32_t(driver(c, "nvnTextureBuilderGetFlags", {builder}));
    t.levels = uint32_t(driver(c, "nvnTextureBuilderGetLevels", {builder}));
    t.samples = uint32_t(driver(c, "nvnTextureBuilderGetSamples", {builder}));
    t.width = uint32_t(driver(c, "nvnTextureBuilderGetWidth", {builder}));
    t.height = uint32_t(driver(c, "nvnTextureBuilderGetHeight", {builder}));
    t.depth = uint32_t(driver(c, "nvnTextureBuilderGetDepth", {builder}));
    t.stride = uint32_t(driver(c, "nvnTextureBuilderGetStride", {builder}));
    t.depth_stencil_mode = uint32_t(driver(c, "nvnTextureBuilderGetDepthStencilMode", {builder}));
    t.pool = driver(c, "nvnTextureBuilderGetMemoryPool", {builder});
    t.offset = driver(c, "nvnTextureBuilderGetMemoryOffset", {builder});
    t.storage_size = texture_storage_size(c, builder);
    uw_widen_texture(t);  // NX_ULTRAWIDE: screen-sized render targets get the display's width
    if (old) {
        std::lock_guard lk(objects_lock());
        pool_textures()[t.pool].insert(guest);
    }
    // Packaged (BNTX) textures were tiled offline with a block height often one step below the default
    // (docs/nvn/TILING.md). The layout the builder returns does not carry it (it reads 1 for every texture in
    // Wonder), so it only says "packaged", and the block height is the fit that matches every BNTX texture in the
    // game: the smallest power of two >= rows * 3 / 32, at most 16 GOBs.
    uint64_t layout = driver(c, "nvnTextureBuilderGetPackagedTextureLayout", {builder});
    if (layout >= kModuleBase && layout < kKernelBase + kKernelSize) layout = ld<uint64_t>(layout);
    if (layout) {
        uint32_t bw = 1, bh = 1, bytes = 0;
        format_block(t.format, bw, bh, bytes);
        uint32_t rows = (t.height + bh - 1) / bh, h = 1;
        while (h * 32 < rows * 3 && h < 16) h <<= 1;
        t.block_height_gobs = h;
    }

    // Swizzle: single-channel textures are often read through it (a font atlas in R8 read as alpha = R); without
    // it Vulkan returns alpha 1 and the text shaders covered the screen.
    {
        uint64_t tmp = scratch(c, 16);
        for (int k = 0; k < 4; k++) st<uint32_t>(tmp + 4 * k, 2u + k);
        driver(c, "nvnTextureBuilderGetSwizzle", {builder, tmp, tmp + 4, tmp + 8, tmp + 12});
        for (int k = 0; k < 4; k++) t.swizzle[k] = uint8_t(std::min<uint32_t>(ld<uint32_t>(tmp + 4 * k), 5));
    }
    if (auto* p = get<MemoryPool>(t.pool)) {
        t.memory = pool_host(*p, t.offset);
        t.gpu = p->gpu + t.offset;
    }
    if (g_trace || trace_textures())
        log("nvn: texture %llx: memory %llx target %u format %#x %ux%ux%u levels %u samples %u flags %x size %llx block height %u "
            "swizzle %u%u%u%u", (unsigned long long)guest, (unsigned long long)t.memory, t.target, t.format, t.width, t.height, t.depth, t.levels,
            t.samples, t.flags, (unsigned long long)t.storage_size, t.block_height_gobs, t.swizzle[0], t.swizzle[1],
            t.swizzle[2], t.swizzle[3]);
    if (!old) {
        std::lock_guard lk(objects_lock());
        note_replaced(guest);
        keep_for_slots(guest);
        hand_image_to_snapshots(guest);
        pool_textures()[t.pool].insert(guest);
        publish_locked(guest, std::move(fresh));
    }
    c.x[0] = 1;
}

// Pool registrations by texture: {pool, id} slots that named it (a slot re-registered since is skipped when read).
std::unordered_map<uint64_t, std::vector<std::pair<uint64_t, uint32_t>>> g_registered;
uint64_t g_next_kept = 0, g_released_kept = 0;
std::deque<std::pair<uint64_t, uint64_t>> g_kept_order;  // (host key, kept at ns), oldest first
constexpr uint64_t kKeptTexture = 0xF000000000000000ull;  // host keys of finalized textures a pool still names

// NVN pools hold a copy of the texture's descriptor: a texture may be finalized while registered and stay sampleable
// through the pool (Wonder does it for most course objects: Goombas, trees, bushes drew black with the dummy, and
// every such draw ended the render pass; docs/nvn/DEBUG.md #38). The object moves to a host key the pool slots are
// pointed at, and is destroyed once no slot names it.
// Moves the texture at `guest` to a host key when a pool slot still names it, and points those slots there (under
// objects_lock). Returns the key, or 0 when no slot names it.
uint64_t keep_for_slots(uint64_t guest) {
    // Off unless NX_KEEP_TEXTURES=1: kept textures fed stale data to the course's sky (white, blocky) and did not fix
    // what they were for (docs/nvn/DEBUG.md #38); missing pool textures sample the dummy without a pass break (#34).
    static const bool on = std::getenv("NX_KEEP_TEXTURES") != nullptr;
    if (!on) return 0;
    auto reg = g_registered.find(guest);
    uint64_t kept = 0;
    if (reg == g_registered.end()) return 0;
    for (auto [pool, id] : reg->second) {
        auto po = objects().find(pool);
        auto* d = po == objects().end() ? nullptr : dynamic_cast<DescriptorPool*>(po->second.get());
        auto slot = d ? d->textures.find(id) : decltype(d->textures.end()){};
        if (!d || slot == d->textures.end() || slot->second != guest) continue;
        if (!kept) {
            auto o = objects().find(guest);
            if (o == objects().end() || !dynamic_cast<Texture*>(o->second.get())) break;
            kept = kKeptTexture | ++g_next_kept;
            if (g_next_kept % 2000 == 0)
                log("nvn: %llu textures kept for their pool slots, %llu released", (unsigned long long)g_next_kept,
                    (unsigned long long)g_released_kept);
            std::unique_ptr<Object> moved = std::move(o->second);
            objects().erase(o);  // before inserting: an insertion may rehash and invalidate `o`
            objects()[kept] = std::move(moved);
            g_registered[kept];
        }
        slot->second = kept;
        g_registered[kept].push_back({pool, id});
        if (g_registered[kept].size() == 1) g_kept_order.push_back({kept, plat::monotonic_ns()});
    }
    g_registered.erase(guest);
    for (auto& [pool, set] : pool_textures())
        if (set.erase(guest) && kept) set.insert(kept);
    return kept;
}

// The game rarely re-registers those slots: a kept texture nothing sampled for 2 s goes (its slots then sample the
// dummy, as before). Without this they piled up at ~450 per second (6 GB in two minutes). Under objects_lock.
void expire_kept() {
    uint64_t now = plat::monotonic_ns();
    for (int n = 0; n < 8 && !g_kept_order.empty(); n++) {
        auto [key, since] = g_kept_order.front();
        auto o = objects().find(key);
        auto* t = o == objects().end() ? nullptr : dynamic_cast<Texture*>(o->second.get());
        // Never sampled: 250 ms (a course load finalizes thousands); sampled: 2 s after the last use.
        uint64_t sampled = t ? t->last_sampled.load() : 0;
        if (t && (sampled ? std::max(since, sampled) + 2'000'000'000ull : since + 250'000'000ull) > now) break;
        g_kept_order.pop_front();
        if (!t) continue;
        g_registered.erase(key);
        for (auto& [pool, set] : pool_textures()) set.erase(key);
        defer_object(std::move(o->second));
        objects().erase(o);
        g_released_kept++;
    }
}

void TextureFinalize(CPU& c) {
    auto guard = reinit_guard();
    uint64_t guest = c.x[0];
    if (trace_textures()) log("nvn: texture %llx finalized", (unsigned long long)guest);
    static const bool old = texture_race_old();
    {
        std::lock_guard lk(objects_lock());
        note_replaced(guest);  // a replay in flight keeps resolving it (#55)
        hand_image_to_snapshots(guest);
        uint64_t kept = keep_for_slots(guest);
        for (auto& [pool, set] : pool_textures()) set.erase(guest);
        expire_kept();
        if (kept) return;
        if (!old) {  // in the same section (#55)
            if (auto o = objects().find(guest); o != objects().end()) defer_object(std::move(o->second)), objects().erase(o);
            return;
        }
    }
    destroy(guest);
}

// ---- pool snapshots ---------------------------------------------------------------------------------------------
// An NVN texture pool holds a copy of each registered texture's descriptor: a game may finalize the NVNtexture, or
// re-initialize it as something else, and still sample the slot. Wonder does both with its light-probe cubemaps at a
// course load (the struct becomes a 2D texture, a cube binding then fell back to the black dummy: dark Goombas and
// trees, docs/nvn/DEBUG.md #42). Each registration keeps a copy of the texture as registered under a host key; when
// the texture object dies or is re-initialized, the copies inherit its GPU image. texture_for (nvn_draw.cpp) uses the
// copy when the slot's texture is gone or no longer has the registered target and format.
constexpr uint64_t kSnapshot = 0xE000000000000000ull;
uint64_t g_next_snapshot = 0;
std::unordered_map<uint64_t, std::vector<uint64_t>> g_snapshots_of;  // guest texture -> its snapshots
std::deque<std::pair<uint64_t, uint64_t>> g_snapshot_images;         // (snapshot, since ns) that own an image

uint64_t make_snapshot(uint64_t guest) {  // under objects_lock
    auto o = objects().find(guest);
    auto* t = o == objects().end() ? nullptr : dynamic_cast<Texture*>(o->second.get());
    if (!t) return 0;
    auto copy = std::make_unique<Texture>();
    copy->target = t->target, copy->format = t->format, copy->flags = t->flags, copy->levels = t->levels;
    copy->samples = t->samples, copy->depth_stencil_mode = t->depth_stencil_mode;
    copy->width = t->width, copy->height = t->height, copy->depth = t->depth, copy->stride = t->stride;
    copy->pool = t->pool, copy->offset = t->offset, copy->storage_size = t->storage_size;
    copy->memory = t->memory, copy->gpu = t->gpu, copy->block_height_gobs = t->block_height_gobs;
    std::memcpy(copy->swizzle, t->swizzle, sizeof copy->swizzle);
    copy->pool_swizzle = t->pool_swizzle.load();
    uint64_t key = kSnapshot | ++g_next_snapshot;
    objects()[key] = std::move(copy);
    auto& keys = g_snapshots_of[guest];
    if (keys.size() >= 16)  // re-registered every frame: forget the copies already dropped
        keys.erase(std::remove_if(keys.begin(), keys.end(), [](uint64_t k) { return !objects().count(k); }), keys.end());
    keys.push_back(key);
    return key;
}

void drop_snapshot(uint64_t key) {  // under objects_lock
    auto o = objects().find(key);
    if (o == objects().end()) return;
    note_replaced(key);  // a replay in flight may resolve the slot to this copy (pool_entry_at, DEBUG.md #63)
    defer_object(std::move(o->second));
    objects().erase(o);
}

void hand_image_to_snapshots(uint64_t guest) {  // under objects_lock: the texture at `guest` is going away
    auto s = g_snapshots_of.find(guest);
    if (s == g_snapshots_of.end()) return;
    auto o = objects().find(guest);
    auto* t = o == objects().end() ? nullptr : dynamic_cast<Texture*>(o->second.get());
    uint64_t now = plat::monotonic_ns();
    for (uint64_t key : s->second) {
        auto so = objects().find(key);
        auto* snap = so == objects().end() ? nullptr : dynamic_cast<Texture*>(so->second.get());
        // Cubes and cube arrays only (the light probes): inheriting every image kept ~450 per second alive.
        if (!snap || !t || !t->image || snap->image || (t->target != 8 && t->target != 9)) continue;
        snap->image = t->image;
        g_snapshot_images.push_back({key, now});
    }
    g_snapshots_of.erase(s);
    // A copy holding an image nothing sampled for 2 s lets it go (their slots are rarely re-registered).
    for (int n = 0; n < 8 && !g_snapshot_images.empty(); n++) {
        auto [key, since] = g_snapshot_images.front();
        auto so = objects().find(key);
        auto* snap = so == objects().end() ? nullptr : dynamic_cast<Texture*>(so->second.get());
        if (snap && std::max<uint64_t>(since, snap->last_sampled) + 2'000'000'000ull > now) break;
        g_snapshot_images.pop_front();
        if (snap) snap->image.reset();
    }
}

// A kept texture no slot names any more is destroyed (under objects_lock).
void release_kept(uint64_t key) {
    if (!(key & kKeptTexture)) return;
    auto reg = g_registered.find(key);
    if (reg != g_registered.end())
        for (auto [pool, id] : reg->second) {
            auto po = objects().find(pool);
            auto* d = po == objects().end() ? nullptr : dynamic_cast<DescriptorPool*>(po->second.get());
            if (d && d->textures.count(id) && d->textures[id] == key) return;  // still named
        }
    g_registered.erase(key);
    for (auto& [pool, set] : pool_textures()) set.erase(key);
    if (auto o = objects().find(key); o != objects().end()) defer_object(std::move(o->second)), objects().erase(o);
    g_released_kept++;
}

// nvnTextureWriteTexels(const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*, const void* data) and the
// Strided form (+ ptrdiff_t rowStride, ptrdiff_t imageStride). NVNcopyRegion: {int x, y, z, width, height, depth}.
// The driver would swizzle the texels into the texture's memory; we keep them for the GPU image instead.
void write_texels(CPU& c, uint64_t row_stride, uint64_t image_stride) {
    auto* t = get<Texture>(c.x[0]);
    uint64_t region = c.x[2], data = c.x[3];
    if (!t || !region || !data) return;
    ViewInfo v = read_view(c, c.x[1]);
    Texture::TexelWrite w;
    w.level = v.base_level;
    w.x = ld<uint32_t>(region), w.y = ld<uint32_t>(region + 4), w.z = ld<uint32_t>(region + 8);
    w.w = ld<uint32_t>(region + 12), w.h = ld<uint32_t>(region + 16), w.d = std::max(1u, ld<uint32_t>(region + 20));
    uint32_t bw = 1, bh = 1, bytes = 4;
    format_block(t->format, bw, bh, bytes);
    uint64_t row = uint64_t((w.w + bw - 1) / bw) * bytes, rows = (w.h + bh - 1) / bh;
    if (!row_stride) row_stride = row;
    if (!image_stride) image_stride = row_stride * rows;
    if (row * rows * w.d > (256ull << 20)) return;
    w.data.resize(row * rows * w.d);
    for (uint32_t z = 0; z < w.d; z++)
        for (uint64_t r = 0; r < rows; r++)
            std::memcpy(w.data.data() + (z * rows + r) * row,
                        reinterpret_cast<const void*>(data + z * image_stride + r * row_stride), row);
    static std::map<uint64_t, uint64_t> counts;
    std::lock_guard lk(objects_lock());
    uint64_t n = ++counts[c.x[0]];
    if (n == 1 || n == 100 || n == 1000)
        log("nvn: WriteTexels #%llu into %llx (%ux%u fmt %#x): %ux%ux%u at %u,%u,%u level %u", (unsigned long long)n,
            (unsigned long long)c.x[0], t->width, t->height, t->format, w.w, w.h, w.d, w.x, w.y, w.z, w.level);
    t->writes.push_back(std::move(w));
    t->has_writes.store(true, std::memory_order_release);
}

void TextureWriteTexels(CPU& c) { write_texels(c, 0, 0); }
void TextureWriteTexelsStrided(CPU& c) { write_texels(c, c.x[4], c.x[5]); }
void TextureFlushTexels(CPU&) {}  // coherent: the writes above are what matters

#define NX_TEXTURE_GETTER(fn, field)          \
    void fn(CPU& c) {                         \
        auto* t = get<Texture>(c.x[0]);       \
        c.x[0] = t ? uint64_t(t->field) : 0;  \
    }
NX_TEXTURE_GETTER(TextureGetWidth, width)
NX_TEXTURE_GETTER(TextureGetHeight, height)
NX_TEXTURE_GETTER(TextureGetDepth, depth)
NX_TEXTURE_GETTER(TextureGetFormat, format)
NX_TEXTURE_GETTER(TextureGetTarget, target)
NX_TEXTURE_GETTER(TextureGetLevels, levels)
NX_TEXTURE_GETTER(TextureGetSamples, samples)
NX_TEXTURE_GETTER(TextureGetFlags, flags)
NX_TEXTURE_GETTER(TextureGetStride, stride)
NX_TEXTURE_GETTER(TextureGetMemoryPool, pool)
NX_TEXTURE_GETTER(TextureGetMemoryOffset, offset)
NX_TEXTURE_GETTER(TextureGetStorageSize, storage_size)
NX_TEXTURE_GETTER(TextureGetDepthStencilMode, depth_stencil_mode)
#undef NX_TEXTURE_GETTER

// ---- samplers and descriptor pools ----------------------------------------------------------------------------

// NVNboolean nvn{Texture,Sampler}PoolInitialize(pool, const NVNmemoryPool*, ptrdiff_t offset, int numDescriptors)
void DescriptorPoolInitialize(CPU& c) {
    auto& d = make<DescriptorPool>(c.x[0]);
    d.pool = c.x[1];
    d.offset = c.x[2];
    d.count = uint32_t(c.x[3]);
    c.x[0] = 1;
}

void DescriptorPoolFinalize(CPU& c) { destroy(c.x[0]); }

// nvnTexturePoolRegisterTexture / RegisterImage(pool, int id, const NVNtexture*, const NVNtextureView*)
void TexturePoolRegister(CPU& c) {
    // The view may pick levels or layers (a bloom chain samples one mip of its target) and override the swizzle
    // (a font atlas in R8 is registered with alpha = R).
    ViewInfo v = read_view(c, c.x[3]);
    if (trace_textures()) {
        auto* tt = get<Texture>(c.x[2]);
        log("nvn: pool %llx id %#x = texture %llx (memory %llx %ux%u) view %llx: levels %u+%u layers %u+%u swizzle %08x "
            "format %#x", (unsigned long long)c.x[0], uint32_t(c.x[1]), (unsigned long long)c.x[2],
            (unsigned long long)(tt ? tt->memory : 0), tt ? tt->width : 0, tt ? tt->height : 0, (unsigned long long)c.x[3],
            v.base_level, v.levels, v.min_layer, v.layers, v.swizzle, v.format);
    }
    if (v.swizzle)
        if (auto* t = get<Texture>(c.x[2])) t->pool_swizzle = v.swizzle;
    if (auto* d = get<DescriptorPool>(c.x[0])) {
        std::lock_guard lk(objects_lock());
        uint32_t id = uint32_t(c.x[1]);
        PoolEntry before;  // what a replay in flight still has to see in this slot (nvn_incarnation.cpp)
        if (auto it = d->textures.find(id); it != d->textures.end()) before.has = true, before.texture = it->second;
        if (auto it = d->snapshots.find(id); it != d->snapshots.end()) before.snapshot = it->second;
        if (auto it = d->views.find(id); it != d->views.end()) before.has_view = true, before.view = it->second;
        note_pool_change(c.x[0], id, before);
        uint64_t old = before.texture;
        d->textures[id] = c.x[2];
        if (auto s = d->snapshots.find(uint32_t(c.x[1])); s != d->snapshots.end()) drop_snapshot(s->second);
        if (uint64_t snap = make_snapshot(c.x[2])) d->snapshots[uint32_t(c.x[1])] = snap;
        auto& slots = g_registered[c.x[2]];  // re-registered every frame: keep each slot once
        std::pair<uint64_t, uint32_t> slot{c.x[0], uint32_t(c.x[1])};
        if (std::find(slots.begin(), slots.end(), slot) == slots.end()) slots.push_back(slot);
        if (old && old != c.x[2]) release_kept(old);
        // A view setting only a base level, first layer or format counts too (it was dropped: level 0 sampled).
        if (v.base_level || v.levels || v.min_layer || v.layers || v.swizzle || v.format)
            d->views[uint32_t(c.x[1])] = v;
        else
            d->views.erase(uint32_t(c.x[1]));
    }
}

void DescriptorPoolGetMemoryPool(CPU& c) {
    auto* d = get<DescriptorPool>(c.x[0]);
    c.x[0] = d ? d->pool : 0;
}

void DescriptorPoolGetMemoryOffset(CPU& c) {
    auto* d = get<DescriptorPool>(c.x[0]);
    c.x[0] = d ? d->offset : 0;
}

void DescriptorPoolGetSize(CPU& c) {
    auto* d = get<DescriptorPool>(c.x[0]);
    c.x[0] = d ? d->count : 0;
}

// nvnDeviceGetTextureHandle(device, int textureID, int samplerID): Maxwell's bindless handle, texture id in the
// low 20 bits and sampler id above (what the shaders' TEX instructions decode). Texel fetch: the texture id alone.
void DeviceGetTextureHandle(CPU& c) { c.x[0] = (c.x[1] & 0xFFFFF) | ((c.x[2] & 0xFFF) << 20); }
void DeviceGetTexelFetchHandle(CPU& c) { c.x[0] = c.x[1] & 0xFFFFF; }
void DeviceGetImageHandle(CPU& c) { c.x[0] = c.x[1] & 0xFFFFF; }

}  // namespace

NX_HLE("nvnMemoryPoolInitialize", MemoryPoolInitialize);
NX_HLE("nvnMemoryPoolFinalize", MemoryPoolFinalize);
NX_HLE("nvnMemoryPoolMap", MemoryPoolMap);
NX_HLE("nvnMemoryPoolMapVirtual", MemoryPoolMapVirtual);
NX_HLE("nvnTextureWriteTexels", TextureWriteTexels);
NX_HLE("nvnTextureWriteTexelsStrided", TextureWriteTexelsStrided);
NX_HLE("nvnTextureFlushTexels", TextureFlushTexels);
NX_HLE("nvnMemoryPoolGetBufferAddress", MemoryPoolGetBufferAddress);
NX_HLE("nvnMemoryPoolGetSize", MemoryPoolGetSize);
NX_HLE("nvnMemoryPoolGetFlags", MemoryPoolGetFlags);
NX_HLE("nvnMemoryPoolFlushMappedRange", MemoryPoolFlushMappedRange);
NX_HLE("nvnMemoryPoolInvalidateMappedRange", MemoryPoolInvalidateMappedRange);
NX_HLE("nvnBufferInitialize", BufferInitialize);
NX_HLE("nvnBufferFinalize", BufferFinalize);
NX_HLE("nvnBufferMap", BufferMap);
NX_HLE("nvnBufferGetAddress", BufferGetAddress);
NX_HLE("nvnBufferGetSize", BufferGetSize);
NX_HLE("nvnBufferGetMemoryPool", BufferGetMemoryPool);
NX_HLE("nvnBufferGetMemoryOffset", BufferGetMemoryOffset);
NX_HLE("nvnBufferFlushMappedRange", BufferFlushMappedRange);
NX_HLE("nvnBufferInvalidateMappedRange", BufferInvalidateMappedRange);
NX_HLE("nvnTextureInitialize", TextureInitialize);
NX_HLE("nvnTextureFinalize", TextureFinalize);
NX_HLE("nvnTextureGetWidth", TextureGetWidth);
NX_HLE("nvnTextureGetHeight", TextureGetHeight);
NX_HLE("nvnTextureGetDepth", TextureGetDepth);
NX_HLE("nvnTextureGetFormat", TextureGetFormat);
NX_HLE("nvnTextureGetTarget", TextureGetTarget);
NX_HLE("nvnTextureGetLevels", TextureGetLevels);
NX_HLE("nvnTextureGetSamples", TextureGetSamples);
NX_HLE("nvnTextureGetFlags", TextureGetFlags);
NX_HLE("nvnTextureGetStride", TextureGetStride);
NX_HLE("nvnTextureGetMemoryPool", TextureGetMemoryPool);
NX_HLE("nvnTextureGetMemoryOffset", TextureGetMemoryOffset);
NX_HLE("nvnTextureGetStorageSize", TextureGetStorageSize);
NX_HLE("nvnTextureGetDepthStencilMode", TextureGetDepthStencilMode);
NX_HLE("nvnSamplerPoolInitialize", DescriptorPoolInitialize);
NX_HLE("nvnTexturePoolInitialize", DescriptorPoolInitialize);
NX_HLE("nvnSamplerPoolFinalize", DescriptorPoolFinalize);
NX_HLE("nvnTexturePoolFinalize", DescriptorPoolFinalize);
NX_HLE("nvnTexturePoolRegisterTexture", TexturePoolRegister);
NX_HLE("nvnTexturePoolRegisterImage", TexturePoolRegister);
NX_HLE("nvnSamplerPoolGetMemoryPool", DescriptorPoolGetMemoryPool);
NX_HLE("nvnTexturePoolGetMemoryPool", DescriptorPoolGetMemoryPool);
NX_HLE("nvnSamplerPoolGetMemoryOffset", DescriptorPoolGetMemoryOffset);
NX_HLE("nvnTexturePoolGetMemoryOffset", DescriptorPoolGetMemoryOffset);
NX_HLE("nvnSamplerPoolGetSize", DescriptorPoolGetSize);
NX_HLE("nvnTexturePoolGetSize", DescriptorPoolGetSize);
NX_HLE("nvnDeviceGetTextureHandle", DeviceGetTextureHandle);
NX_HLE("nvnDeviceGetTexelFetchHandle", DeviceGetTexelFetchHandle);
NX_HLE("nvnDeviceGetImageHandle", DeviceGetImageHandle);

}  // namespace nx::nvn
