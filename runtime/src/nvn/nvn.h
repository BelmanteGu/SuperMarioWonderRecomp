// NVN on the host (docs/NVN.md). Shared pieces of runtime/src/nvn/*.
//
// The game allocates every NVN object itself (NVNtexture, NVNmemoryPool, ...) at the real driver's sizes. Builders
// and state objects (NVNtextureBuilder, NVNblendState, ...) are plain structs the SDK's own driver fills and reads:
// their procs go straight to that driver (recompiled in sdk), so their layout and the sizes it computes (texture
// storage size and alignment for block-linear data) are exactly the real ones. Objects that do work (memory pools,
// textures, queues, command buffers, windows, ...) are ours: a host object keyed by the guest object's address,
// built from the builder through the driver's getters.
#pragma once

#include <cstdint>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <string>
#include <vector>

#include <nx/runtime.h>

#include "vk_bridge.h"  // Vulkan headers without prototypes; the dispatch table is vk_gen.h

namespace nx::nvn {

// The SDK driver's implementation of an NVN proc (0 if it has none). Resolved through its own nvnBootstrapLoader.
uint64_t real_proc(CPU& c, const char* name);

// Calls guest function `fn` with integer arguments, returns x0. Registers are restored afterwards.
uint64_t guest_call(CPU& c, uint64_t fn, std::initializer_list<uint64_t> args);

// Guest memory for out-parameters of driver calls: up to 0x1000 bytes private to the calling thread.
uint64_t scratch(CPU& c, size_t size);

// Same, for a function returning a float (in s0).
float guest_call_f(CPU& c, uint64_t fn, std::initializer_list<uint64_t> args);

// Calls the SDK driver's `name` (a getter of a builder or state object).
inline uint64_t driver(CPU& c, const char* name, std::initializer_list<uint64_t> args) {
    return guest_call(c, real_proc(c, name), args);
}

// ---- host objects ---------------------------------------------------------------------------------------------

struct Object {
    virtual ~Object() = default;
};

// Looked up for every command recorded and every draw replayed, from several threads: a hash map under a
// reader/writer lock (lookups share it; make/destroy take it alone). Every exclusive lock bumps `generation`.
struct ObjectsLock {
    std::shared_mutex m;
    std::atomic<uint64_t> generation{1};
    void lock() { m.lock(), generation.fetch_add(1, std::memory_order_acq_rel); }
    bool try_lock() { return m.try_lock() ? (generation.fetch_add(1, std::memory_order_acq_rel), true) : false; }
    void unlock() { m.unlock(); }
    void lock_shared() { m.lock_shared(); }
    bool try_lock_shared() { return m.try_lock_shared(); }
    void unlock_shared() { m.unlock_shared(); }
};
ObjectsLock& objects_lock();
std::unordered_map<uint64_t, std::unique_ptr<Object>>& objects();

// Texture incarnations (nvn_incarnation.cpp, docs/nvn/DEBUG.md #55). Game threads re-initialize and finalize texture
// structs while another thread replays a submit (Wonder's dynamic-resolution targets, every frame). A replay resolves
// every texture as it was when the submit began: each re-initialization or finalization records the incarnation it
// retires, with an epoch, and a replay's lookups (t_replay_mark != 0) return the one current at its mark. Retired
// objects live on as deferred objects until drain_objects, which also forgets the records. Game threads (mark 0)
// always see the current one. NX_TEXTURE_RACE_OLD=1: no records, no marks (the old behaviour).
inline thread_local uint64_t t_replay_mark = 0;  // the replaying thread's mark; 0 outside a replay
inline thread_local uint64_t t_replay_serial = 0;  // which replay (ReplayScope counts them): keys get<T>'s replay cache
bool resolve_cache_on();  // NX_RESOLVE_CACHE (nvn_incarnation.cpp)
bool texture_race_old();
Object* find_object(uint64_t guest, uint64_t mark);  // under objects_lock (shared)
void note_replaced(uint64_t guest);                  // under objects_lock (alone): guest's object is about to change
void purge_incarnations(uint64_t upto);              // under objects_lock (alone), from drain_objects: records <= upto
uint64_t current_epoch();                            // the epoch of the last recorded change
uint64_t replay_floor();                             // the last replay's mark: objects retired up to it can go
// A submit's replay (gpu_submit, under the GPU lock): takes the mark, then drains the deferred objects no replay
// needs any more. `lists_epoch`: the newest epoch at which the submit's command lists ended recording (0: unknown).
struct ReplayScope {
    explicit ReplayScope(uint64_t lists_epoch);
    ~ReplayScope();
};

// A per-thread cache of the last lookups of each type, valid until anything takes the lock alone: a recorded command
// looks up its command buffer, a replayed draw the same texture 5-10 times, and libc++'s shared lock is a mutex and a
// condition variable that the game threads and the replay contend on (~20% of the main thread's own CPU in the
// course, docs/nvn/DEBUG.md #44). The generation is read before the lookup: a concurrent change invalidates it. The
// replay mark is part of the key: a thread that replays (the render thread) also calls the game-side procs.
template <typename T>
T* get(uint64_t guest) {
    // During a replay every lookup answers as of its mark, and nothing a lookup returns is freed before the replay
    // ends (drain_objects runs between replays): an answer holds for the whole replay, so it is kept per replay
    // (docs/nvn/android-perf/PHASE4.md C4). The game threads bump the lock's generation ~180 times a frame, which
    // emptied the cache below during a replay (get<Texture> was 8.5% of the replay thread). Not for nulls: an object
    // created after the mark resolves to the current one (DEBUG.md #64). NX_RESOLVE_CACHE=0: off.
    if (uint64_t serial = t_replay_serial; serial && t_replay_mark && resolve_cache_on()) {
        struct Slot {
            uint64_t guest = 0, serial = 0;
            T* object = nullptr;
        };
        thread_local Slot table[1024];
        Slot& s = table[((guest >> 4) ^ (guest >> 14)) & 1023];
        if (s.serial == serial && s.guest == guest) return s.object;
        T* p;
        {
            std::shared_lock lk(objects_lock());
            p = dynamic_cast<T*>(find_object(guest, t_replay_mark));
        }
        if (p) s = {guest, serial, p};
        return p;
    }
    struct Entry {
        uint64_t guest = 0, generation = 0, mark = 0;
        T* object = nullptr;
    };
    thread_local Entry cache[8];
    ObjectsLock& ol = objects_lock();
    uint64_t gen = ol.generation.load(std::memory_order_acquire), mark = t_replay_mark;
    Entry& e = cache[((guest >> 3) ^ (guest >> 11)) & 7];
    if (e.generation == gen && e.guest == guest && e.mark == mark) return e.object;
    T* p;
    {
        std::shared_lock lk(ol);
        p = dynamic_cast<T*>(find_object(guest, mark));
    }
    e = {guest, gen, mark, p};
    return p;
}

// Objects the game destroys or re-creates are freed on the replay thread between two jobs (drain_objects), never
// while a replay may still hold a pointer from get<T>() (nvn_trace.cpp). Under objects_lock.
void defer_object(std::unique_ptr<Object> o);
void drain_objects();
enum { kWaitSync, kWaitFinish, kWaitInvalidate, kWaitFinalize, kWaitSites };  // nvn_queue.cpp's wait sites
void wait_submits_so_far(int site);  // nvn_queue.cpp: until the submits made so far were replayed (DEBUG.md #59)

template <typename T>
T& make(uint64_t guest) {
    std::lock_guard lk(objects_lock());
    auto& slot = objects()[guest];
    if (slot) defer_object(std::move(slot));
    slot = std::make_unique<T>();
    return static_cast<T&>(*slot);
}

// Installs a fully built object at `guest` (the one there goes to defer_object). Under objects_lock (alone).
inline void publish_locked(uint64_t guest, std::unique_ptr<Object> o) {
    auto& slot = objects()[guest];
    if (slot) defer_object(std::move(slot));
    slot = std::move(o);
}

inline void destroy(uint64_t guest) {
    std::lock_guard lk(objects_lock());
    auto it = objects().find(guest);
    if (it == objects().end()) return;
    defer_object(std::move(it->second));
    objects().erase(it);
}

// ---- memory ---------------------------------------------------------------------------------------------------

struct MemoryPool : Object {
    uint64_t memory = 0;  // guest (= host) address of the storage the game gave; 0 for a virtual pool
    uint64_t size = 0;
    uint32_t flags = 0;
    uint64_t gpu = 0;  // our GPU virtual address of byte 0
    // Virtual pools (nvnMemoryPoolMapVirtual): virtual offset -> (physical pool, its offset, size).
    struct Mapping {
        uint64_t physical = 0, physical_offset = 0, size = 0;
    };
    std::map<uint64_t, Mapping> mappings;
};

// Host address of `offset` in a pool, through its virtual mappings if it has no storage of its own; 0 if unmapped.
uint64_t pool_host(const MemoryPool& pool, uint64_t offset);

// GPU virtual addresses: every pool gets a range; address -> (pool, offset) for command translation.
uint64_t gpu_alloc(uint64_t size);
MemoryPool* pool_at_gpu(uint64_t gpu, uint64_t* offset);
void pool_register(MemoryPool* pool);
void pool_unregister(MemoryPool* pool);

// ---- textures and windows ---------------------------------------------------------------------------------------

struct GpuImage;  // the Vulkan side of a texture (nvn_vk.cpp)

struct Texture : Object {
    uint32_t target = 0, format = 0, flags = 0, levels = 0, samples = 0, depth_stencil_mode = 0;
    uint32_t width = 0, height = 0, depth = 0, stride = 0;
    uint64_t pool = 0, offset = 0, storage_size = 0;
    uint64_t memory = 0, gpu = 0;
    bool uw = false;  // NX_ULTRAWIDE: a screen-sized render target whose width was widened (nvn_ultrawide.cpp)
    uint32_t block_height_gobs = 0;  // packaged (BNTX) textures carry their own; 0: the layout's default
    uint8_t swizzle[4] = {2, 3, 4, 5};  // NVNtextureSwizzle per channel (0 zero, 1 one, 2 R, 3 G, 4 B, 5 A)
    // nvnTextureWriteTexels data not yet on the GPU (a glyph cache is filled this way): applied before the next use.
    struct TexelWrite {
        uint32_t level = 0, x = 0, y = 0, z = 0, w = 0, h = 0, d = 0;
        std::vector<uint8_t> data;  // rows of whole blocks, tightly packed
    };
    std::vector<TexelWrite> writes;  // under objects_lock()
    // Whether `writes` holds anything: set and cleared with it, under objects_lock() alone, and read without the lock
    // (every sampled texture of every replayed draw asks; docs/nvn/PERF-PLAN.md).
    std::atomic<bool> has_writes{false};
    // The CPU wrote the texture's memory (nvnMemoryPoolFlushMappedRange over it, e.g. a glyph cache): upload again.
    std::atomic<bool> dirty{false};
    // What of the memory the CPU wrote, as bytes from `memory` ([lo, hi); UINT64_MAX / 0: nothing known yet). Merged by
    // mark_dirty, taken with `dirty`; a pitch-linear texture then uploads only the rows it touches (glyph cache).
    std::atomic<uint64_t> dirty_lo{UINT64_MAX}, dirty_hi{0};
    void mark_dirty(uint64_t lo, uint64_t hi) {  // [lo, hi) of the texture's bytes; (0, UINT64_MAX): all of it
        for (uint64_t c = dirty_lo; lo < c && !dirty_lo.compare_exchange_weak(c, lo);) {}
        for (uint64_t c = dirty_hi; hi > c && !dirty_hi.compare_exchange_weak(c, hi);) {}
        dirty = true;
    }
    // The swizzle of the texture-pool view it is registered with (4 NVNtextureSwizzle bytes, 0: none).
    std::atomic<uint32_t> pool_swizzle{0};
    // Pitch-linear textures the CPU keeps writing without a flush (Wonder's glyph cache: 2048x8192 R8): their memory
    // is hashed at most once per submit that samples them, and a change means an upload.
    uint64_t content_hash = 0, hashed_submit = 0;
    bool watched = false;  // large ones: a write watch instead of the hash (write_watch.cpp)
    std::shared_ptr<GpuImage> image;  // made on first use by the GPU
    std::atomic<uint64_t> last_sampled{0};  // host ns of the last draw that sampled it (kept textures, nvn_objects.cpp)
    ~Texture();  // nvn_vk.cpp: the image is freed once the GPU is done with it
};

struct Window : Object {
    std::vector<uint64_t> textures;  // guest NVNtexture*
    int active = 0, next = 0, interval = 1;
    int32_t crop[4] = {};  // nvnWindowSetCrop: x, y, width, height of the texture shown (0 wide: all of it)
};

// ---- samplers and descriptor pools --------------------------------------------------------------------------------

struct SamplerInfo {  // NVN enums (nvn_sampler.cpp)
    uint32_t min_filter = 0, mag_filter = 0, wrap[3] = {}, compare_mode = 0, compare_func = 0, reduction = 0;
    float lod_min = 0, lod_max = 1000, lod_bias = 0, max_anisotropy = 1, border[4] = {};
};

// What an NVNtextureView selects (nvnTextureViewGet*): 0 levels/layers means "all of the texture's", swizzle 0 the
// texture's own (else 4 NVNtextureSwizzle bytes, R in the low byte).
struct ViewInfo {
    uint32_t base_level = 0, levels = 0, min_layer = 0, layers = 0, swizzle = 0;
    uint32_t format = 0;  // NVN format the view reads/writes the texture as (an sRGB view of RGBA8); 0: the texture's
};
ViewInfo read_view(CPU& c, uint64_t view);  // nvn_objects.cpp; a null view gives the defaults

struct DescriptorPool : Object {  // an NVNtexturePool or NVNsamplerPool
    uint64_t pool = 0, offset = 0;
    uint32_t count = 0;
    std::map<uint32_t, uint64_t> textures;     // id -> guest NVNtexture*
    std::map<uint32_t, uint64_t> snapshots;    // id -> host key of a copy of the texture as registered (nvn_objects.cpp)
    std::map<uint32_t, ViewInfo> views;        // id -> the registering view, when it selects anything
    std::map<uint32_t, SamplerInfo> samplers;  // id -> sampler state
};

// A texture pool slot as a replay must see it (nvn_incarnation.cpp, DEBUG.md #56): the game registers the next
// frame's slots from its recording thread while a submit is replayed, so each change records the entry it replaces.
struct PoolEntry {
    bool has = false, has_view = false;
    uint64_t texture = 0, snapshot = 0;
    ViewInfo view;
};
void note_pool_change(uint64_t pool, uint32_t id, const PoolEntry& before);  // objects_lock held exclusively
// objects_lock held: the slot's entry at `mark` when it changed since (false: the current entry is the one).
bool pool_entry_at(uint64_t pool, uint32_t id, uint64_t mark, PoolEntry& out);
bool mark_empty_old();  // NX_MARK_EMPTY_OLD=1: a slot or object empty at the mark stays empty (bisecting, #64)
// The same for sampler pool slots (DEBUG.md #63): recorded only when the sampler changes.
struct SamplerSlot {
    bool has = false;
    SamplerInfo info;
};
void note_sampler_change(uint64_t pool, uint32_t id, const SamplerSlot& before);  // objects_lock held exclusively
bool sampler_entry_at(uint64_t pool, uint32_t id, uint64_t mark, SamplerSlot& out);

// NVNformat -> VkFormat, and whether it is color (0), depth (1), stencil (2) or both (3) (nvn_format.cpp).
VkFormat vk_format(uint32_t format);
uint32_t format_aspect(uint32_t format);
// ASTC on the host (nvn_format.cpp): native, or RGBA8 images filled by the CPU decoder at upload (nvn_upload.cpp).
void set_astc_native(bool native);
bool astc_native();
bool is_astc(uint32_t format);
bool astc_decoded(uint32_t format);  // an ASTC format this device gets as RGBA8
// Bytes per texel of a Vulkan format as buffer-image copies count them (nvn_format.cpp); 0 for block-compressed and
// combined depth/stencil formats, whose NVN size (format_block) is the host's. Differs from the NVN size where a
// format is stored wider than the game's: 0x3f R11G11B10F is 4 bytes in NVN, RGBA16F (8) here.
uint32_t vk_block_bytes(VkFormat format);

// NVN enums -> Vulkan (nvn_enums.cpp).
VkBlendFactor vk_blend_factor(uint32_t f);
VkBlendOp vk_blend_op(uint32_t e);
VkCompareOp vk_compare(uint32_t f);
VkStencilOp vk_stencil_op(uint32_t op);
VkCullModeFlags vk_cull(uint32_t face);
VkPrimitiveTopology vk_topology(uint32_t mode);
VkIndexType vk_index_type(uint32_t type);
uint32_t index_size(uint32_t type);
VkFormat vk_vertex_format(uint32_t format);

// ---- recorded commands ----------------------------------------------------------------------------------------
// A command buffer records into a list of these; nvnQueueSubmitCommands replays the lists into Vulkan. State
// objects (blend, depth/stencil, vertex layouts, ...) are decoded when recorded, through the SDK driver's getters,
// and kept in the list's blobs: the game may reuse its structs right after the call.

struct Cmd {
    enum Op : uint8_t {
        SetRenderTargets, ClearColor, ClearDepthStencil, SetViewport, SetScissor, CallList,
        SetDepthRange, BindProgram, BindUniformBuffer, BindStorageBuffer, BindTexture, BindVertexBuffer,
        BindVertexAttribs, BindVertexStreams, BindBlendState, BindColorState, BindChannelMaskState,
        BindDepthStencilState, BindPolygonState, SetBlendColor, UpdateUniformBuffer, SetTexturePool, SetSamplerPool,
        DrawArrays, DrawElements, ReportCounter, SetRasterizerDiscard, SetRenderEnable, CopyTextureToTexture,
        BindImage, DispatchCompute,
        SetStencilRef, SetStencilMask, SetStencilValueMask, SetPolygonOffsetClamp, SetDepthBounds, SetDepthClamp,
        BindDeferred,  // mask: 0 textures, 1 images, 2 uniform buffers, 3 storage buffers; a[0] the guest array
        BindMultisampleState,  // n: alpha to coverage enabled (nvn_state.cpp)
    } op;
    uint32_t n = 0, mask = 0;
    uint64_t a[9] = {};  // textures (render targets: colors then depth), handles, GPU addresses, sizes, ...
    float f[4] = {};
    int32_t i[4] = {};
    uint32_t blob = 0;  // index into CmdList::blobs (decoded state, inline data)
};

struct CmdList {
    std::vector<Cmd> cmds;
    std::vector<std::vector<uint8_t>> blobs;
    uint64_t handle = 0;     // its NVNcommandHandle
    uint64_t epoch = 0;      // the texture epoch when its recording ended (nvn_incarnation.cpp): what its draws name
    uint32_t skinned = 0;    // NX_TRACE_SKINLISTS: binds of skinned programs (a vertex input at location 6) in it
    // count_dispatches' scan of cmds (nvn_frames.cpp, replay thread only): its own DispatchCompute commands and the
    // handles it calls, valid while cmds has scanned_size entries.
    size_t scanned_size = SIZE_MAX;
    uint32_t own_dispatches = 0;
    std::vector<uint64_t> calls;
};
// NX_TRACE_SKINLISTS (nvn_cmd.cpp): follows the lists that bind skinned programs, from recording to replay.
bool skin_lists_on();
void skin_list_replayed(const CmdList& list, int depth);

// Decoded state objects (values are NVN enums, see nvn_state.cpp).
struct VertexAttrib {
    uint32_t format = 0, offset = 0, stream = 0;
};
struct VertexStream {
    uint32_t stride = 0, divisor = 0;
};
struct BlendTarget {
    uint32_t target = 0, src_rgb = 0, dst_rgb = 0, src_a = 0, dst_a = 0, eq_rgb = 0, eq_a = 0;
};
struct ColorStateInfo {
    uint8_t blend_enable[8] = {};
    uint32_t alpha_test = 0, logic_op = 0;
};
struct ChannelMaskInfo {
    uint8_t mask[8] = {};  // bit 0 r, 1 g, 2 b, 3 a
};
struct DepthStencilInfo {
    uint8_t depth_test = 0, depth_write = 0, stencil_test = 0;
    uint32_t depth_func = 0;
    uint32_t stencil_func[2] = {}, stencil_fail[2] = {}, depth_fail[2] = {}, depth_pass[2] = {};
};
struct PolygonInfo {
    uint32_t cull = 0, front = 0, mode = 0, offset_enables = 0;
};
// nvn_raster.cpp: an EQUAL colour pass after a depth pre-pass (tested, not written), drawn with a small depth bias
// toward the camera (DEBUG.md #60). NX_EQUAL_SLACK=<units> (default 4, 0: off); the slope term is 1 per unit of it.
bool equal_slack_pass(const DepthStencilInfo& d);

struct TargetViews {  // SetRenderTargets' blob: level and layer per color target (0..7) and depth (8)
    uint8_t level[9] = {};
    uint16_t layer[9] = {};
    uint16_t format[9] = {};  // the views' NVN formats (0: the texture's own)
};

std::shared_ptr<CmdList> command_list(uint64_t handle);  // a recorded handle's commands (nvn_cmd.cpp)
std::string explain_handle(uint64_t handle);             // why a handle has no list (NX_CMD_DROPS=1: what dropped it)
void command_memory_released(uint64_t gpu, uint64_t size);  // a pool finalized: the lists recorded in it go
// Appends to the command buffer (guest NVNcommandBuffer*) being recorded; `blob` is copied into the list.
void record(uint64_t cmdbuf, Cmd cmd, const void* blob = nullptr, size_t size = 0);

// ---- the GPU (nvn_vk.cpp) -------------------------------------------------------------------------------------

// *Deferred binds read their array when the work is submitted (nvn_binds.cpp): the values of each array a submit
// uses, taken on the game's thread at nvnQueueSubmitCommands, keyed by array address.
using DeferredValues = std::unordered_map<uint64_t, std::vector<uint64_t>>;
DeferredValues deferred_snapshot(const std::vector<std::shared_ptr<CmdList>>& lists);
void gpu_submit(const std::vector<std::shared_ptr<CmdList>>& lists, std::shared_ptr<const DeferredValues> deferred = {});
const DeferredValues* current_deferred();  // the submit being replayed
// nvn_worker.cpp: the replay thread. worker_run queues a job (runs it at once when the thread is off) and returns its
// serial (0: done); worker_wait waits for a serial; worker_finish for everything queued so far.
uint64_t worker_run(std::function<void()> job);
void worker_wait(uint64_t serial);
uint64_t worker_last();
void worker_finish();
// guest NVNtexture* of a window texture; crop: the part shown (x, y, w, h; w 0: all), scaled to the window.
void gpu_present(uint64_t texture, const int32_t* crop = nullptr);
// With NX_ASYNC_GPU submits run while the game goes on (nvn_frames.cpp): the serial of the last one, and a wait until
// it completed with its results in guest memory (true if it had to wait). Synchronous: 0, and nothing to wait for.
uint64_t gpu_serial();
bool gpu_wait(uint64_t serial);
uint64_t gpu_write_back_serial();  // the last submit that writes guest memory (compute results)

// Programs: the translated stages bound by nvnProgramSetShaders (nvn_program.cpp; ShaderInfo in nvn_shaders.h).
struct ShaderInfo;
const std::vector<const ShaderInfo*>* program_shaders(uint64_t program);

// A format's block (width, height in texels; bytes), from the SDK driver's format table (nvn_texture_layout.cpp).
void format_block(uint32_t format, uint32_t& block_w, uint32_t& block_h, uint32_t& bytes);

// Bytes a texture built from `builder` occupies in its pool (nvn_texture_layout.cpp).
uint64_t texture_storage_size(CPU& c, uint64_t builder);

}  // namespace nx::nvn
