// Debug readback of any NVN texture (docs/NVN.md): NX_DUMP_TEXTURES=<hex NVNtexture*>[,...] writes, at every frame
// NX_DUMP_FRAMES dumps, each listed texture's level 0 / layer 0 to <data root>/frames/NNNNN_<texture>.ppm. The image
// is blitted to RGBA8 first, so float and packed formats come out as the GPU would display them (clamped to 0..1).
// Runs under the GPU lock, after the frame's work completed, on its own command buffer.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

using namespace vk;

VkQueue vk_queue();
uint32_t vk_queue_family();

namespace {

std::vector<uint64_t> dump_list() {
    std::vector<uint64_t> v;
    const char* e = std::getenv("NX_DUMP_TEXTURES");
    while (e && *e) {
        char* end = nullptr;
        uint64_t t = std::strtoull(e, &end, 16);
        if (end == e) break;
        v.push_back(t);
        e = *end == ',' ? end + 1 : end;
    }
    return v;
}

struct Scratch {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
    void destroy(VkDevice d) {
        if (image) p_vkDestroyImage(d, image, nullptr);
        if (image_memory) p_vkFreeMemory(d, image_memory, nullptr);
        if (buffer) p_vkDestroyBuffer(d, buffer, nullptr);
        if (buffer_memory) p_vkFreeMemory(d, buffer_memory, nullptr);
    }
};

bool make_scratch(Scratch& s, uint32_t w, uint32_t h) {
    VkDevice d = vk_device();
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {w, h, 1};
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (p_vkCreateImage(d, &ii, nullptr, &s.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(d, s.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (p_vkAllocateMemory(d, &ai, nullptr, &s.image_memory) != VK_SUCCESS) return false;
    p_vkBindImageMemory(d, s.image, s.image_memory, 0);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = uint64_t(w) * h * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (p_vkCreateBuffer(d, &bi, nullptr, &s.buffer) != VK_SUCCESS) return false;
    p_vkGetBufferMemoryRequirements(d, s.buffer, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (p_vkAllocateMemory(d, &ai, nullptr, &s.buffer_memory) != VK_SUCCESS) return false;
    p_vkBindBufferMemory(d, s.buffer, s.buffer_memory, 0);
    return true;
}

void write_ppm(const Scratch& s, uint32_t w, uint32_t h, uint64_t frame, uint64_t texture) {
    void* p = nullptr;
    if (p_vkMapMemory(vk_device(), s.buffer_memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return;
    std::string dir = plat::data_root() + "/frames";
    plat::make_dirs(dir);
    char name[64];
    std::snprintf(name, sizeof name, "/%05llu_%llx.ppm", (unsigned long long)frame, (unsigned long long)texture);
    if (FILE* f = plat::fopen_utf8(dir + name, "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        const uint8_t* px = static_cast<const uint8_t*>(p);
        for (size_t k = 0; k < size_t(w) * h; k++) std::fwrite(px + k * 4, 1, 3, f);
        std::fclose(f);
    }
    p_vkUnmapMemory(vk_device(), s.buffer_memory);
}

}  // namespace

std::vector<uint64_t> g_dump_extra;  // this submit's (NX_DUMP_ON_FS)
void debug_dump_add(uint64_t texture) {
    if (texture && std::find(g_dump_extra.begin(), g_dump_extra.end(), texture) == g_dump_extra.end())
        g_dump_extra.push_back(texture);
}

void debug_dump_textures(uint64_t frame) {
    static const std::vector<uint64_t> fixed = dump_list();
    std::vector<uint64_t> list = fixed;
    list.insert(list.end(), g_dump_extra.begin(), g_dump_extra.end());
    g_dump_extra.clear();
    if (list.empty()) return;
    VkDevice d = vk_device();
    static VkCommandPool pool = VK_NULL_HANDLE;
    static VkCommandBuffer cb = VK_NULL_HANDLE;
    static VkFence fence = VK_NULL_HANDLE;
    if (!pool) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = vk_queue_family();
        p_vkCreateCommandPool(d, &pi, nullptr, &pool);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        p_vkAllocateCommandBuffers(d, &ai, &cb);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        p_vkCreateFence(d, &fi, nullptr, &fence);
    }
    for (uint64_t texture : list) {
        GpuImage* src = image_of(texture);
        log("nvn: dump %llx at %llu: %s", (unsigned long long)texture, (unsigned long long)frame,
            !src ? "no image" : src->layout == VK_IMAGE_LAYOUT_UNDEFINED ? "undefined" : "ok");
        if (!src || !src->image || src->layout == VK_IMAGE_LAYOUT_UNDEFINED || !(src->aspect & VK_IMAGE_ASPECT_COLOR_BIT))
            continue;
        uint32_t w = src->width, h = src->height;
        Scratch s;
        if (!make_scratch(s, w, h)) {
            s.destroy(d);
            continue;
        }
        p_vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        p_vkBeginCommandBuffer(cb, &bi);
        VkImageLayout back = src->layout;
        transition(cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        GpuImage dst;
        dst.image = s.image;
        dst.format = VK_FORMAT_R8G8B8A8_UNORM;
        transition(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (std::getenv("NX_DUMP_RAW")) {  // 4-byte texels as stored, to <frame>_<texture>.raw (w, h, vk format, data)
            transition(cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy r{};
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.imageExtent = {w, h, 1};
            p_vkCmdCopyImageToBuffer(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buffer, 1, &r);
            transition(cb, *src, back);
            p_vkEndCommandBuffer(cb);
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cb;
            p_vkResetFences(d, 1, &fence);
            void* p = nullptr;
            if (p_vkQueueSubmit(vk_queue(), 1, &si, fence) == VK_SUCCESS &&
                p_vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS &&
                p_vkMapMemory(d, s.buffer_memory, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) {
                char name[64];
                std::snprintf(name, sizeof name, "/frames/%05llu_%llx.raw", (unsigned long long)frame,
                              (unsigned long long)texture);
                if (FILE* f = plat::fopen_utf8(plat::data_root() + name, "wb")) {
                    uint32_t head[3] = {w, h, uint32_t(src->format)};
                    std::fwrite(head, 4, 3, f);
                    std::fwrite(p, 4, size_t(w) * h, f);
                    std::fclose(f);
                }
                p_vkUnmapMemory(d, s.buffer_memory);
            }
            s.destroy(d);
            continue;
        }
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(w), int32_t(h), 1};
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[1] = blit.srcOffsets[1];
        p_vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        transition(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {w, h, 1};
        p_vkCmdCopyImageToBuffer(cb, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buffer, 1, &r);
        transition(cb, *src, back);
        p_vkEndCommandBuffer(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        p_vkResetFences(d, 1, &fence);
        if (p_vkQueueSubmit(vk_queue(), 1, &si, fence) == VK_SUCCESS) {
            p_vkWaitForFences(d, 1, &fence, VK_TRUE, UINT64_MAX);
            write_ppm(s, w, h, frame, texture);
        }
        s.destroy(d);
    }
}

// NX_DUMP_ON_TARGET / NX_DUMP_ON_FS (nvn_draw.cpp): the n-th requesting draw of a submit, every NX_DUMP_TARGET_EVERY-th
// (default NX_DUMP_FRAMES) is read back when the submit completes.
void debug_dump_requested(uint64_t n) {
    static const uint64_t every = [] {
        const char* e = std::getenv("NX_DUMP_TARGET_EVERY");
        return e ? std::strtoull(e, nullptr, 10) : uint64_t(frame_dump_every());
    }();
    if (n && every && n % every == 0) debug_dump_textures(n);
}

// ---- presented frames (NX_DUMP_FRAMES=N) ----------------------------------------------------------------------------
// Every Nth presented texture is read back and written to <data root>/frames/NNNNN.ppm (what the game drew,
// independent of the window). The copy is recorded into the present's command buffer, written after its fence.

uint32_t frame_dump_every() {
    static uint32_t n = [] {
        const char* e = std::getenv("NX_DUMP_FRAMES");
        return e ? uint32_t(std::strtoul(e, nullptr, 10)) : 0u;
    }();
    return n;
}

// NX_DUMP_FRAMES, or on demand while playing: a file named dump_frames in the data folder (optionally holding
// "<count> [<every>]", default 40 frames, every 2nd) dumps the next presented frames, then is deleted. The game runs
// at full speed until then, for glitches that only show in a long session (DEBUG-2 #77). Checked once a second.
bool frame_dump_due(uint64_t presents) {
    static uint64_t left = 0, every = 2, next_check = 0;
    if (presents >= next_check) {
        next_check = presents + 60;
        std::string path = plat::data_root() + "/dump_frames";
        if (FILE* f = plat::fopen_utf8(path, "rb")) {
            unsigned long long n = 40, e = 2;
            int got = std::fscanf(f, "%llu %llu", &n, &e);
            std::fclose(f);
            std::remove(path.c_str());
            left = got >= 1 && n ? n : 40, every = got >= 2 && e ? e : 2;
            log("dump_frames: the next %llu frames, every %llu (from present %llu)", (unsigned long long)left,
                (unsigned long long)every, (unsigned long long)presents);
        }
    }
    if (left && presents % every == 0) return left--, true;
    // NX_DUMP_EVERY_S=<seconds> (study fps-unlock-1710): a dump every <seconds> of wall time since the first present, so
    // runs at different frame rates are compared at equal wall times. The wall time is logged with the present number.
    static const double every_s = std::getenv("NX_DUMP_EVERY_S") ? std::atof(std::getenv("NX_DUMP_EVERY_S")) : 0.0;
    if (every_s > 0) {
        static uint64_t t0 = 0, next_ns = 0, last_presents = ~0ull;
        static bool last_due = false;
        if (presents == last_presents) return last_due;
        last_presents = presents;
        uint64_t now = plat::monotonic_ns();
        if (!t0) t0 = now, next_ns = now + uint64_t(every_s * 1e9);
        last_due = now >= next_ns;
        if (last_due) {
            next_ns += uint64_t(every_s * 1e9);
            log("dump_frames: present %llu at %.2f s wall", (unsigned long long)presents, double(now - t0) / 1e9);
        }
        return last_due;
    }
    return frame_dump_every() && presents % frame_dump_every() == 0;
}

namespace {

struct Readback {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint64_t size = 0;
};

Readback g_readback;

}  // namespace

bool frame_dump_record(VkCommandBuffer cb, GpuImage& img) {
    VkDevice d = vk_device();
    uint64_t size = uint64_t(img.width) * img.height * 4;
    if (img.format != VK_FORMAT_R8G8B8A8_UNORM && img.format != VK_FORMAT_R8G8B8A8_SRGB &&
        img.format != VK_FORMAT_B8G8R8A8_UNORM)
        return false;
    if (g_readback.size < size) {
        if (g_readback.buffer) p_vkDestroyBuffer(d, g_readback.buffer, nullptr);
        if (g_readback.memory) p_vkFreeMemory(d, g_readback.memory, nullptr);
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        p_vkCreateBuffer(d, &bi, nullptr, &g_readback.buffer);
        VkMemoryRequirements req{};
        p_vkGetBufferMemoryRequirements(d, g_readback.buffer, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex =
            memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        p_vkAllocateMemory(d, &ai, nullptr, &g_readback.memory);
        p_vkBindBufferMemory(d, g_readback.buffer, g_readback.memory, 0);
        g_readback.size = size;
    }
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {img.width, img.height, 1};
    p_vkCmdCopyImageToBuffer(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_readback.buffer, 1, &r);
    return true;
}

void frame_dump_write(const GpuImage& img, uint64_t frame) {
    void* p = nullptr;
    if (p_vkMapMemory(vk_device(), g_readback.memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return;
    std::string dir = plat::data_root() + "/frames";
    plat::make_dirs(dir);
    char name[64];
    std::snprintf(name, sizeof name, "/%05llu.ppm", (unsigned long long)frame);
    if (FILE* f = plat::fopen_utf8(dir + name, "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", img.width, img.height);
        const uint8_t* px = static_cast<const uint8_t*>(p);
        bool bgra = img.format == VK_FORMAT_B8G8R8A8_UNORM;
        std::vector<uint8_t> row(size_t(img.width) * 3);
        for (uint32_t y = 0; y < img.height; y++) {
            for (uint32_t x = 0; x < img.width; x++) {
                const uint8_t* s = px + (size_t(y) * img.width + x) * 4;
                row[x * 3 + 0] = bgra ? s[2] : s[0];
                row[x * 3 + 1] = s[1];
                row[x * 3 + 2] = bgra ? s[0] : s[2];
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
    }
    p_vkUnmapMemory(vk_device(), g_readback.memory);
}

// ---- GPU profile (NX_GPU_PROFILE=1) --------------------------------------------------------------------------------
// A timestamp at the start of every render pass and at the end of each submit; the time between two marks goes to
// the first one's label ("<target WxH> fs <hash>"). Totals per label are logged every 60 presents, largest first.
// Apple GPUs sample timestamps at encoder boundaries, which is what render passes are here.

namespace {

constexpr uint32_t kMarks = 1024;
VkQueryPool g_queries = VK_NULL_HANDLE;
std::vector<std::string> g_labels;
std::map<std::string, std::pair<double, uint64_t>> g_totals;  // label -> (ms, passes)
double g_period_ns = 1.0;

}  // namespace

bool gpu_profile_on() {
    static const bool on = std::getenv("NX_GPU_PROFILE") && std::atoi(std::getenv("NX_GPU_PROFILE")) == 1;
    return on;
}

void gpu_profile_begin(VkCommandBuffer cb) {
    if (!gpu_profile_on()) return;
    if (!g_queries) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kMarks;
        p_vkCreateQueryPool(vk_device(), &qi, nullptr, &g_queries);
        VkPhysicalDeviceProperties props{};
        p_vkGetPhysicalDeviceProperties(vk_physical(), &props);
        g_period_ns = props.limits.timestampPeriod;
    }
    p_vkCmdResetQueryPool(cb, g_queries, 0, kMarks);
    g_labels.clear();
}

void gpu_profile_mark(VkCommandBuffer cb, const std::string& label) {
    if (!gpu_profile_on() || !g_queries || g_labels.size() >= kMarks) return;
    p_vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_queries, uint32_t(g_labels.size()));
    g_labels.push_back(label);
}

void gpu_profile_collect() {  // after the submit's fence
    if (!gpu_profile_on() || g_labels.size() < 2) return;
    std::vector<uint64_t> t(g_labels.size());
    if (p_vkGetQueryPoolResults(vk_device(), g_queries, 0, uint32_t(t.size()), t.size() * 8, t.data(), 8,
                                VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    for (size_t k = 0; k + 1 < t.size(); k++) {
        auto& e = g_totals[g_labels[k]];
        e.first += double(t[k + 1] - t[k]) * g_period_ns / 1e6;
        e.second++;
    }
}

void gpu_profile_log(uint64_t frames) {
    if (!gpu_profile_on()) return;
    std::vector<std::pair<double, std::string>> v;
    for (auto& [label, e] : g_totals) v.push_back({e.first, label});
    std::sort(v.rbegin(), v.rend());
    double total = 0;
    for (auto& e : v) total += e.first;
    log("gpu profile: %8.2f ms/frame in all, %zu labels", total / double(frames), v.size());
    for (size_t k = 0; k < v.size() && k < 40; k++)
        log("gpu profile: %8.2f ms/frame  %s (%llu passes)", v[k].first / double(frames), v[k].second.c_str(),
            (unsigned long long)g_totals[v[k].second].second);
    g_totals.clear();
}

// ---- mid-frame snapshots (NX_SNAP_DRAWS=<draw>,<draw>,...) -------------------------------------------------------
// Right after each listed draw (numbers as NX_TRACE_DRAWS prints them), colour target 0 is blitted to RGBA8 in the
// same command buffer; after the submit each copy is written to frames/snap_<draw>.ppm. Shows a frame being built.

namespace {

struct Snap {
    Scratch s;
    uint32_t w = 0, h = 0;
    uint64_t draw = 0;
    uint32_t raw_format = 0;  // NX_SNAP_RAW: the texels as stored (4-byte formats), written as .raw
};
std::vector<Snap> g_snaps;

}  // namespace

bool debug_snap_wanted(uint64_t draw) {
    static const std::vector<uint64_t> list = [] {
        std::vector<uint64_t> v;
        const char* e = std::getenv("NX_SNAP_DRAWS");
        while (e && *e) {
            char* end = nullptr;
            uint64_t n = std::strtoull(e, &end, 10);
            if (end == e) break;
            v.push_back(n);
            e = *end == ',' ? end + 1 : end;
        }
        return v;
    }();
    return std::find(list.begin(), list.end(), draw) != list.end();
}

uint32_t g_snap_layer = 0, g_snap_level = 0;  // which layer and level the next debug_snapshot reads (nvn_draw.cpp)

void debug_snapshot(VkCommandBuffer cb, GpuImage& img, uint64_t draw) {
    if (!img.image || !(img.aspect & VK_IMAGE_ASPECT_COLOR_BIT)) return;
    Snap snap;
    snap.w = img.width, snap.h = img.height, snap.draw = draw;
    // NX_SNAP_CROP=x,y,w,h (fractions of the image): only that part (8 MB per whole 1080p snapshot otherwise).
    static float crop[4] = {0, 0, 1, 1};
    static const bool cropped = [] {
        const char* e = std::getenv("NX_SNAP_CROP");
        return e && std::sscanf(e, "%f,%f,%f,%f", &crop[0], &crop[1], &crop[2], &crop[3]) == 4;
    }();
    int32_t x0 = 0, y0 = 0;
    if (cropped) {
        x0 = int32_t(crop[0] * img.width), y0 = int32_t(crop[1] * img.height);
        snap.w = std::max(1u, std::min(uint32_t(crop[2] * img.width), img.width - uint32_t(x0)));
        snap.h = std::max(1u, std::min(uint32_t(crop[3] * img.height), img.height - uint32_t(y0)));
    }
    static const bool raw_early = std::getenv("NX_SNAP_RAW") != nullptr;
    const bool wide = raw_early && img.format == VK_FORMAT_R16G16B16A16_SFLOAT;  // 8-byte texels: a buffer twice as wide
    if (!make_scratch(snap.s, snap.w * (wide ? 2 : 1), snap.h)) {
        snap.s.destroy(vk_device());
        return;
    }
    VkImageLayout back = img.layout;
    transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    // NX_SNAP_RAW=1: 4-byte formats (R32F depth copies, RG16F, ...) as stored, to frames/00000_<draw>.raw (u32 width,
    // height, VkFormat, then the texels): an RGBA8 blit rounds small floats to 0..2.
    static const bool raw = std::getenv("NX_SNAP_RAW") != nullptr;
    if (raw && (img.format == VK_FORMAT_R32_SFLOAT || img.format == VK_FORMAT_R16G16_SFLOAT ||
                img.format == VK_FORMAT_R32_UINT || img.format == VK_FORMAT_R8G8B8A8_UNORM || wide)) {
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageOffset = {x0, y0, 0};
        r.imageExtent = {snap.w, snap.h, 1};
        p_vkCmdCopyImageToBuffer(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snap.s.buffer, 1, &r);
        if (back != VK_IMAGE_LAYOUT_UNDEFINED) transition(cb, img, back);
        snap.raw_format = uint32_t(img.format);
        g_snaps.push_back(snap);
        return;
    }
    GpuImage dst;
    dst.image = snap.s.image;
    dst.format = VK_FORMAT_R8G8B8A8_UNORM;
    transition(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, std::min(g_snap_level, img.levels - 1), std::min(g_snap_layer, img.layers - 1), 1};
    blit.srcOffsets[0] = {x0, y0, 0};
    blit.srcOffsets[1] = {x0 + int32_t(snap.w), y0 + int32_t(snap.h), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {int32_t(snap.w), int32_t(snap.h), 1};
    p_vkCmdBlitImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snap.s.image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    transition(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {snap.w, snap.h, 1};
    p_vkCmdCopyImageToBuffer(cb, snap.s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snap.s.buffer, 1, &r);
    if (back != VK_IMAGE_LAYOUT_UNDEFINED) transition(cb, img, back);
    g_snaps.push_back(snap);
}

bool debug_snapshots_pending() { return !g_snaps.empty(); }

void debug_snapshots_write() {  // after the submit's fence
    for (Snap& snap : g_snaps) {
        void* p = nullptr;
        if (snap.raw_format == VK_FORMAT_R16G16B16A16_SFLOAT &&
            p_vkMapMemory(vk_device(), snap.s.buffer_memory, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) {
            // RGBA16F: how many texels hold NaN or Inf (NX_SNAP_RAW + NX_DUMP_DRAW_TEXTURES), and the largest finite value
            const uint16_t* h = static_cast<const uint16_t*>(p);
            uint64_t nan = 0, inf = 0, n = uint64_t(snap.w) * snap.h * 4;
            float big = 0;
            for (uint64_t k = 0; k < n; k++) {
                uint32_t e = h[k] >> 10 & 31, m = h[k] & 1023;
                if (e == 31) (m ? nan : inf)++;
                else big = std::max(big, std::ldexp(float(1024 + m), int(e) - 25) * (e ? 1.0f : 0.0f));
            }
            log("snapshot %llx: %ux%u rgba16f, %llu NaN, %llu Inf of %llu values, largest finite %g", (unsigned long long)snap.draw, snap.w,
                snap.h, (unsigned long long)nan, (unsigned long long)inf, (unsigned long long)n, big);
            p_vkUnmapMemory(vk_device(), snap.s.buffer_memory);
        } else if (snap.raw_format && p_vkMapMemory(vk_device(), snap.s.buffer_memory, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) {
            char name[64];
            std::snprintf(name, sizeof name, "/frames/00000_%llx.raw", (unsigned long long)snap.draw);
            if (FILE* f = plat::fopen_utf8(plat::data_root() + name, "wb")) {
                uint32_t head[3] = {snap.w, snap.h, snap.raw_format};
                std::fwrite(head, 4, 3, f);
                std::fwrite(p, 4, size_t(snap.w) * snap.h, f);
                std::fclose(f);
            }
            p_vkUnmapMemory(vk_device(), snap.s.buffer_memory);
        } else if (!snap.raw_format) {
            write_ppm(snap.s, snap.w, snap.h, 0, snap.draw);  // frames/00000_<draw in hex>.ppm
        }
        snap.s.destroy(vk_device());
    }
    g_snaps.clear();
}

// ---- draws that produce no pixels (NX_ZERO_DRAWS=1) ------------------------------------------------------------------
// Every draw runs inside an occlusion query; after each submit the samples passed are added up per (vertex, fragment)
// shader pair. Every 300 presents the pairs whose draws never produced a pixel are logged, most drawn first: what the
// scene tries to show and we lose (culled, depth-failed, off screen, or broken vertex data).

namespace {

constexpr uint32_t kZeroQueries = 8192;
VkQueryPool g_zero_pool = VK_NULL_HANDLE;
std::vector<std::string> g_zero_labels;  // this submit's queries
struct ZeroStat {
    uint64_t draws = 0, samples = 0;
};
std::map<std::string, ZeroStat> g_zero_stats;

}  // namespace

bool zero_draws_on() {
    static const bool on = std::getenv("NX_ZERO_DRAWS") != nullptr;
    return on;
}

void zero_draws_begin_submit(VkCommandBuffer cb) {
    if (!zero_draws_on()) return;
    if (!g_zero_pool) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_OCCLUSION;
        qi.queryCount = kZeroQueries;
        p_vkCreateQueryPool(vk_device(), &qi, nullptr, &g_zero_pool);
    }
    p_vkCmdResetQueryPool(cb, g_zero_pool, 0, kZeroQueries);
    g_zero_labels.clear();
}

// Around one draw call (inside the render pass). Returns false when this submit's queries are used up.
bool zero_draws_begin(VkCommandBuffer cb, const std::string& label) {
    if (!zero_draws_on() || !g_zero_pool || g_zero_labels.size() >= kZeroQueries) return false;
    p_vkCmdBeginQuery(cb, g_zero_pool, uint32_t(g_zero_labels.size()), 0);
    g_zero_labels.push_back(label);
    return true;
}

void zero_draws_end(VkCommandBuffer cb) { p_vkCmdEndQuery(cb, g_zero_pool, uint32_t(g_zero_labels.size() - 1)); }

void zero_draws_collect() {  // after the submit's fence
    if (!zero_draws_on() || g_zero_labels.empty()) return;
    std::vector<uint64_t> r(g_zero_labels.size());
    if (p_vkGetQueryPoolResults(vk_device(), g_zero_pool, 0, uint32_t(r.size()), r.size() * 8, r.data(), 8,
                                VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    for (size_t k = 0; k < r.size(); k++) {
        ZeroStat& z = g_zero_stats[g_zero_labels[k]];
        z.draws++, z.samples += r[k];
    }
}

void zero_draws_log() {
    if (!zero_draws_on()) return;
    std::vector<std::pair<uint64_t, std::string>> zero;
    for (auto& [label, z] : g_zero_stats)
        if (z.samples == 0) zero.push_back({z.draws, label});
    std::sort(zero.rbegin(), zero.rend());
    log("zero draws: %zu of %zu shader pairs produced no pixel", zero.size(), g_zero_stats.size());
    for (size_t k = 0; k < zero.size() && k < 25; k++)
        log("zero draws: %6llu x %s", (unsigned long long)zero[k].first, zero[k].second.c_str());
    g_zero_stats.clear();
}

}  // namespace nx::nvn
