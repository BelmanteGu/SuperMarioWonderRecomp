// Pipelines and sampled resources (nvn_pipeline.h). Everything is cached for the life of the process: layouts by
// their bindings, pipelines by their full state, samplers by their state, views by (image, type).
#include <algorithm>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

#include "nvn_pipeline.h"
#include "nvn_ultrawide.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

// NVN views carry a format: the game renders into RGBA8 textures through sRGB views (the GPU encodes on write and
// blends in linear) and samples them the same way. Ignoring it lost one conversion per draw: far too much contrast
// and saturation (docs/nvn/DEBUG.md #29).
VkFormat view_format(const GpuImage& img, uint32_t nvn) {
    if (VkFormat f; bc_view_format(img, nvn, f)) return f;  // decoded BC (nvn_bc.cpp)
    if (!nvn || nvn == img.nvn_format || img.alpha_from_r || img.aspect != VK_IMAGE_ASPECT_COLOR_BIT) return img.format;
    VkFormat f = vk_format(nvn);
    uint32_t bw = 1, bh = 1, bytes = 0, iw = 1, ih = 1, ibytes = 0;
    format_block(nvn, bw, bh, bytes);
    format_block(img.nvn_format, iw, ih, ibytes);
    // The sizes of the host formats: R11G11B10F is 4 bytes in NVN but RGBA16F here, and a 4-byte view (RGBA8, R32F)
    // of an 8-byte image is no valid view (G4, docs/nvn/DEBUG.md #50). NX_VIEW_NVN_SIZES=1: the NVN sizes (bisecting).
    static const bool nvn_sizes = std::getenv("NX_VIEW_NVN_SIZES") != nullptr;
    if (!nvn_sizes && vk_block_bytes(f) && vk_block_bytes(img.format))
        bytes = vk_block_bytes(f), ibytes = vk_block_bytes(img.format);
    bool ok = f != VK_FORMAT_UNDEFINED && format_aspect(nvn) == 0 && bw == iw && bh == ih && bytes == ibytes;
    if (ok && f != img.format && !img.mutable_format) {  // ubwc_flags: an image made without MUTABLE_FORMAT
        static std::set<std::pair<uint32_t, uint32_t>> not_mutable;
        if (not_mutable.insert({img.nvn_format, nvn}).second)
            log("nvn/vk: texture format %#x viewed as %#x, but the image is not mutable: its own format", img.nvn_format,
                nvn);
        return img.format;
    }
    static std::set<std::pair<uint32_t, uint32_t>> logged;
    if (logged.insert({img.nvn_format, nvn}).second && logged.size() <= 40)
        log("nvn/vk: texture format %#x viewed as %#x%s", img.nvn_format, nvn, ok ? "" : " (not compatible: ignored)");
    return ok ? f : img.format;
}

VkImageViewType view_type(const ImageShape& s) {  // SPIR-V Dim: 0 1D, 1 2D, 2 3D, 3 Cube
    switch (s.dim) {
        case 0: return s.arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        case 2: return VK_IMAGE_VIEW_TYPE_3D;
        case 3: return s.arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        default: return s.arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}

namespace {

int process_id() {
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

VkShaderStageFlags stage_bit(ShaderStage s) {
    return s == ShaderStage::Vertex     ? VK_SHADER_STAGE_VERTEX_BIT
           : s == ShaderStage::Fragment ? VK_SHADER_STAGE_FRAGMENT_BIT
                                        : VK_SHADER_STAGE_COMPUTE_BIT;
}

// `kinds`: per vertex input location, 0 float or the integer kind of its attribute format (vertex_int_kind).
// `unwritten`: fragment input locations the vertex stage does not write (default_unwritten_inputs).
VkShaderModule module_of(const ShaderInfo* s, const uint8_t* kinds = nullptr, uint32_t unwritten = 0) {
    static std::map<std::tuple<const ShaderInfo*, std::string, uint32_t>, VkShaderModule> cache;
    // The integer-vertex-input patch (skinned models' bone indices; without it Metal refuses every character's
    // pipeline, DEBUG.md #25/#26). NX_INT_ATTRIBS=0 turns it off.
    static const char* int_env = std::getenv("NX_INT_ATTRIBS");
    static const bool off = int_env && int_env[0] == '0';
    std::string k = kinds && !off ? std::string(reinterpret_cast<const char*>(kinds), 16) : std::string(16, '\0');
    auto& m = cache[{s, k, unwritten}];
    if (!m) {
        bool is_patched = false;
        const uint8_t* int_kinds = k != std::string(16, '\0') ? reinterpret_cast<const uint8_t*>(k.data()) : nullptr;
        std::vector<uint32_t> code = patched_spirv(s, int_kinds, unwritten, &is_patched);
        if (s->stage == ShaderStage::Vertex && uw_on()) {  // NX_ULTRAWIDE: gl_Position.x * spec constant 0
            std::vector<uint32_t> scaled;
            if (patch_x_scale(code, scaled)) code.swap(scaled);
            else log("nvn/vk: shader %s: ultrawide x scale not patched", s->hash.c_str());
        }
        if (s->stage == ShaderStage::Vertex) clip_z_module(s, code);  // NX_CLIP_Z_PATCH (nvn_spirv_clip.cpp)
        if (const char* opt = std::getenv("NX_SPIRV_OPT"); opt || std::getenv("NX_SPIRV_TOOL")) {  // diagnostic: each module through `spirv-opt <flags>` (NX_SPIRV_OPT) or `<tool> in out` (NX_SPIRV_TOOL) first
            static int n = 0;
            std::string in = "/tmp/nx_spv_" + std::to_string(process_id()) + "_" + std::to_string(n++) + ".spv", out = in + ".out";
            if (FILE* f = std::fopen(in.c_str(), "wb")) {
                std::fwrite(code.data(), 4, code.size(), f);
                std::fclose(f);
                std::string cmd = std::getenv("NX_SPIRV_TOOL") ? std::string(std::getenv("NX_SPIRV_TOOL")) + " " + in + " " + out + " " + s->hash
                                                                : std::string("spirv-opt ") + opt + " " + in + " -o " + out + " 2>/dev/null";
                if (std::system(cmd.c_str()) == 0)
                    if (FILE* g = std::fopen(out.c_str(), "rb")) {
                        std::vector<uint32_t> o(1 << 20);
                        size_t words = std::fread(o.data(), 4, o.size(), g);
                        std::fclose(g);
                        if (words > 5) o.resize(words), code.swap(o);
                    }
                std::remove(in.c_str()), std::remove(out.c_str());
            }
        }
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size() * 4;
        ci.pCode = code.data();
        if (p_vkCreateShaderModule(vk_device(), &ci, nullptr, &m) != VK_SUCCESS) log("nvn/vk: shader %s rejected", s->hash.c_str());
        static int logged = 0;
        if (is_patched && logged++ < 20) log("nvn/vk: shader %s: integer vertex inputs", s->hash.c_str());
    }
    return m;
}

VkDescriptorSetLayout set_layout(const std::vector<VkDescriptorSetLayoutBinding>& bindings) {
    static std::map<std::string, VkDescriptorSetLayout> cache;
    std::string key(reinterpret_cast<const char*>(bindings.data()), bindings.size() * sizeof(bindings[0]));
    auto& l = cache[key];
    if (!l) {
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = uint32_t(bindings.size());
        ci.pBindings = bindings.data();
        p_vkCreateDescriptorSetLayout(vk_device(), &ci, nullptr, &l);
    }
    return l;
}

// ---- images ---------------------------------------------------------------------------------------------------------

// Views made for sampling and for rendering, by image; forget_image_views() drops an image's before it is freed.
using SampledKey = std::tuple<VkImage, VkImageViewType, uint32_t, uint64_t, uint64_t>;
std::map<SampledKey, VkImageView> g_sampled_views;
std::map<std::tuple<VkImage, uint32_t, uint32_t, VkFormat>, VkImageView> g_attachment_views;
// In front of g_sampled_views (a tree walk per sampled texture of every draw): copies of entries it holds, by a hash
// of the key (view null: an empty slot). forget_image_views, the only place entries go, empties the image's slots.
struct ViewHit {
    SampledKey key{};
    VkImageView view = VK_NULL_HANDLE;
};
ViewHit g_view_hits[256];

bool identity(const VkComponentMapping& m) {
    auto id = [](VkComponentSwizzle s, VkComponentSwizzle own) { return s == VK_COMPONENT_SWIZZLE_IDENTITY || s == own; };
    return id(m.r, VK_COMPONENT_SWIZZLE_R) && id(m.g, VK_COMPONENT_SWIZZLE_G) && id(m.b, VK_COMPONENT_SWIZZLE_B) &&
           id(m.a, VK_COMPONENT_SWIZZLE_A);
}


// Views Metal accepts of an image: cubes only of cube images with 6 layers per cube, 3D only of 3D images (a 2D
// view of a 3D image is for rendering a slice), 1D of 1D (only with NX_1D_IMAGES: NVN 1D textures are 2D images of
// height 1, since the translator samples them as 2D, G8). A shader declaring another shape than the texture it gets
// (a samplerCube given a 2D texture) reads the dummy instead: Metal aborts on the view.
bool view_compatible(const GpuImage& img, VkImageViewType type, uint32_t layers) {
    bool cube_img = img.view_type == VK_IMAGE_VIEW_TYPE_CUBE || img.view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    bool img3d = img.view_type == VK_IMAGE_VIEW_TYPE_3D;
    bool img1d = img.view_type == VK_IMAGE_VIEW_TYPE_1D || img.view_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    switch (type) {
        case VK_IMAGE_VIEW_TYPE_CUBE:
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: return cube_img && layers >= 6;
        case VK_IMAGE_VIEW_TYPE_3D: return img3d;
        case VK_IMAGE_VIEW_TYPE_1D:
        case VK_IMAGE_VIEW_TYPE_1D_ARRAY: return img1d;
        default: return !img3d && !img1d;
    }
}

// A view for sampling: the image's own when it fits, else one of the view type with the texture's swizzle.
VkImageView view_of(GpuImage& img, VkImageViewType type, const ViewInfo& view) {
    uint32_t swizzle = img.alpha_from_r ? 0 : view.swizzle;  // materialized at upload
    VkComponentMapping m = img.swizzle;
    if (swizzle) {
        constexpr VkComponentSwizzle kSwizzle[] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE,
                                                   VK_COMPONENT_SWIZZLE_R,    VK_COMPONENT_SWIZZLE_G,
                                                   VK_COMPONENT_SWIZZLE_B,    VK_COMPONENT_SWIZZLE_A};
        m = {kSwizzle[swizzle & 0xFF], kSwizzle[(swizzle >> 8) & 0xFF], kSwizzle[(swizzle >> 16) & 0xFF],
             kSwizzle[swizzle >> 24]};
    }
    uint32_t base = std::min(view.base_level, img.levels - 1);
    uint32_t levels = view.levels ? std::min(view.levels, img.levels - base) : img.levels - base;
    uint32_t min_layer = std::min(view.min_layer, img.layers - 1);
    bool whole = base == 0 && levels == img.levels && min_layer == 0 && (!view.layers || view.layers == img.layers);
    VkFormat format = view_format(img, view.format);
    if (type == img.view_type && identity(m) && whole && format == img.format) return img.view;
    auto& cache = g_sampled_views;
    uint64_t range = uint64_t(base) | uint64_t(levels) << 16 | uint64_t(min_layer) << 32 | uint64_t(view.layers) << 48;
    uint32_t mapping = uint32_t(m.r) | uint32_t(m.g) << 8 | uint32_t(m.b) << 16 | uint32_t(m.a) << 24;
    SampledKey key{img.image, type, mapping, range, uint64_t(format)};
    uint64_t h = uintptr_t(img.image) >> 4 ^ range * 31 ^ mapping ^ uint64_t(type) << 5 ^ uint64_t(format);
    ViewHit& hit = g_view_hits[(h ^ h >> 8) & 255];
    if (hit.view && hit.key == key) return hit.view;
    auto& v = cache[key];
    if (!v) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = img.image;
        ci.viewType = type;
        ci.format = format;
        ci.components = m;
        uint32_t layers = std::min(view.layers ? view.layers : img.layers - min_layer, img.layers - min_layer);
        if (type == VK_IMAGE_VIEW_TYPE_2D || type == VK_IMAGE_VIEW_TYPE_1D) layers = 1;
        if (type == VK_IMAGE_VIEW_TYPE_CUBE) layers = 6;
        if (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) layers -= layers % 6;
        if (!view_compatible(img, type, layers)) return VK_NULL_HANDLE;
        ci.subresourceRange = {img.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                   ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                   : img.aspect,
                               base, levels, min_layer, layers};
        if (p_vkCreateImageView(vk_device(), &ci, nullptr, &v) != VK_SUCCESS) v = img.view;
    }
    if (v) hit = {key, v};  // never a null one: those are tried again
    return v;
}

// R11G11B10F (0x3f) has no alpha, stored here as RGBA16F: sampling reads alpha 1 and blending takes destination
// alpha as 1, as Maxwell does (G4, docs/nvn/DEBUG.md #50). NX_R11G11B10_RAW=1: the stored alpha (bisecting).
bool ufloat_raw() {
    static const bool raw = std::getenv("NX_R11G11B10_RAW") != nullptr;
    return raw;
}

}  // namespace

// ---- layouts ----------------------------------------------------------------------------------------------------------

const ProgramLayout& program_layout(const std::vector<const ShaderInfo*>& stages) {
    static std::map<std::vector<const ShaderInfo*>, ProgramLayout> cache;
    static const std::pair<const std::vector<const ShaderInfo*>, ProgramLayout>* last = nullptr;  // entries stay
    if (last && last->first == stages) return last->second;  // runs of draws use one program
    auto it = cache.find(stages);
    if (it != cache.end()) return (last = &*it)->second;
    std::map<uint32_t, VkDescriptorSetLayoutBinding> sets[4];
    for (const ShaderInfo* s : stages)
        for (const DescriptorBinding& b : s->refl.bindings) {
            if (b.set >= 4) continue;
            auto& vb = sets[b.set][b.binding];
            vb.binding = b.binding;
            vb.descriptorType = VkDescriptorType(b.type);
            vb.descriptorCount = std::max(1u, b.count);
            vb.stageFlags |= stage_bit(s->stage);
        }
    ProgramLayout pl;
    // On by default on Android only (NX_DYNAMIC_UBO=0/1 overrides): on the Mac (MoltenVK) every frame came out black
    // and the GPU hung a few thousand frames in (2026-10-01), cause not found yet.
    static const uint32_t max_dynamic = [] {
        const char* e = std::getenv("NX_DYNAMIC_UBO");
#if defined(__ANDROID__)
        if (e && e[0] == '0') return 0u;
#else
        if (!e || e[0] != '1') return 0u;
#endif
        VkPhysicalDeviceProperties p{};
        p_vkGetPhysicalDeviceProperties(vk_physical(), &p);
        log("nvn/vk: uniform buffers bound with dynamic offsets (up to %u per layout)",
            p.limits.maxDescriptorSetUniformBuffersDynamic);
        return p.limits.maxDescriptorSetUniformBuffersDynamic;
    }();
    uint64_t dynamic = 0;
    for (auto& [n, b] : sets[0])
        if (b.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b.descriptorCount == 1 && n < 64) dynamic |= 1ull << n;
    if (dynamic && uint32_t(__builtin_popcountll(dynamic)) <= max_dynamic) {
        for (auto& [n, b] : sets[0])
            if (n < 64 && dynamic >> n & 1) b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        pl.dynamic = dynamic;
    }
    for (int k = 0; k < 4; k++) {
        std::vector<VkDescriptorSetLayoutBinding> v;
        for (auto& [n, b] : sets[k]) v.push_back(b);
        pl.sets[k] = set_layout(v);
    }
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 4;
    ci.pSetLayouts = pl.sets;
    p_vkCreatePipelineLayout(vk_device(), &ci, nullptr, &pl.layout);
    return cache[stages] = pl;
}

// ---- graphics pipelines ---------------------------------------------------------------------------------------------

VkPipeline graphics_pipeline(const PipelineKey& key, const ProgramLayout& layout) {
    // By the key's bytes, as before, but hashed (nvn_pipeline.h) instead of a ~700-byte std::string built and
    // compared down a tree per draw; and the last entry first: consecutive draws mostly share their state.
    using Key = ByteKey<sizeof(PipelineKey)>;
    static std::unordered_map<Key, VkPipeline, ByteKeyHash> cache;
    static const std::pair<const Key, VkPipeline>* last = nullptr;  // entries are never erased
    if (last && std::memcmp(&key, last->first.b, sizeof key) == 0) return last->second;
    Key k;
    std::memcpy(k.b, &key, sizeof key);
    if (auto it = cache.find(k); it != cache.end()) return (last = &*it)->second;

    VkPipelineShaderStageCreateInfo stages[2]{};
    VkSpecializationInfo spec_info{};
    float uw_x_value = 1.0f;
    uint32_t nstages = 0;
    uint8_t int_kinds[16] = {};
    for (uint32_t a = 0; a < key.attrib_count && a < 16; a++) int_kinds[a] = uint8_t(vertex_int_kind(key.attribs[a].format));
    // NX_TRACE_PIPELINES=1: every new pipeline's shaders, and each vertex shader's inputs once.
    static std::set<const ShaderInfo*> logged_vs;
    static const bool trace = std::getenv("NX_TRACE_PIPELINES") != nullptr;
    if (trace) log("nvn/vk: new pipeline vs %s fs %s", key.vs ? key.vs->hash.c_str() : "-", key.fs ? key.fs->hash.c_str() : "-");
    if (trace && key.vs && logged_vs.insert(key.vs).second) {
        std::string line;
        for (const ShaderIo& in : key.vs->refl.inputs) {
            char b[64];
            const VertexAttrib* va = in.location < key.attrib_count ? &key.attribs[in.location] : nullptr;
            std::snprintf(b, sizeof b, " %u:%x/s%u+%u", in.location, va ? va->format : 0, va ? va->stream : 0,
                          va ? va->offset : 0);
            line += b;
        }
        log("nvn/vk: vs %s inputs (location:format/stream+offset)%s", key.vs->hash.c_str(), line.c_str());
    }
    uint32_t unwritten = unwritten_inputs(key.vs, key.fs);
    for (const ShaderInfo* s : {key.vs, key.fs}) {
        if (!s) continue;
        auto& st = stages[nstages++];
        st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st.stage = VkShaderStageFlagBits(stage_bit(s->stage));
        st.module = module_of(s, s == key.vs ? int_kinds : nullptr, s == key.fs ? unwritten : 0);
        st.pName = "main";
        if (s == key.vs && key.uw_x > 0) {  // NX_ULTRAWIDE: constant 0 of the patched vertex module
            static VkSpecializationMapEntry entry{0, 0, sizeof(float)};
            spec_info = {1, &entry, sizeof(float), &uw_x_value};
            uw_x_value = key.uw_x;
            st.pSpecializationInfo = &spec_info;
        }
    }

    // Vertex input: each shader input location reads the attribute of the same index. Locations with no attribute
    // (or an unknown format) read a zero vec4 from binding 15.
    std::vector<VkVertexInputAttributeDescription> attrs;
    std::vector<VkVertexInputBindingDescription> binds;
    bool streams_used[16] = {}, zero_used = false;
    if (key.vs)
        for (const ShaderIo& in : key.vs->refl.inputs) {
            VkVertexInputAttributeDescription a{in.location, 15, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
            if (in.location < key.attrib_count) {
                const VertexAttrib& va = key.attribs[in.location];
                VkFormat f = vk_vertex_format(va.format);
                if (f != VK_FORMAT_UNDEFINED && va.stream < 15) {
                    a.binding = va.stream, a.format = f, a.offset = va.offset;
                    streams_used[va.stream] = true;
                }
            }
            if (a.binding == 15) zero_used = true;
            attrs.push_back(a);
        }
    for (uint32_t s = 0; s < 15; s++)
        if (streams_used[s]) {
            const VertexStream& vs = key.streams[s];
            binds.push_back({s, vs.stride, vs.divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
        }
    if (zero_used) binds.push_back({15, 0, VK_VERTEX_INPUT_RATE_INSTANCE});
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = uint32_t(binds.size());
    vi.pVertexBindingDescriptions = binds.data();
    vi.vertexAttributeDescriptionCount = uint32_t(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = vk_topology(key.topology);

    VkPipelineViewportDepthClipControlCreateInfoEXT clip{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    clip.negativeOneToOne = VK_TRUE;  // NVN depth mode 0 (Wonder): clip-space z in -1..1
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    if (vk_depth_clip_control() && !depth_remap()) vp.pNext = &clip;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = key.polygon.mode == 0 ? VK_POLYGON_MODE_POINT : key.polygon.mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = vk_cull(key.polygon.cull);
    // NVNfrontFace CW 0 / CCW 1, taken as is: the draws use a negative-height viewport (nvn_draw.cpp) that gives NVN's
    // upper-left-origin mapping, so windings match NVN's.
    rs.frontFace = key.polygon.front == 0 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    // SetDepthClamp: clamps instead of clipping at the near and far planes, like depthClampEnable (a device feature;
    // nvn_vk.cpp enables every core feature the GPU has, and MoltenVK has it through Metal's depth clip mode).
    rs.depthClampEnable = key.depth_clamp && vk_features().depthClamp ? VK_TRUE : VK_FALSE;
    // Polygon offset: NVNpolygonOffsetEnable has a bit per polygon mode (POINT 1, LINE 2, FILL 4; modes are POINT 0,
    // LINE 1, FILL 2), and the bit of the mode drawn with decides. Factor, units and clamp are dynamic (SetDepthBias
    // at each draw, nvn_raster.cpp): the game sets them on the command buffer, not in the state object.
    uint32_t offset_bit = key.polygon.mode <= 2 ? 1u << key.polygon.mode : 4u;
    rs.depthBiasEnable = key.polygon.offset_enables & offset_bit || equal_slack_pass(key.depth) ? VK_TRUE : VK_FALSE;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // BindMultisampleState's alpha to coverage (G7; single-sampled targets: alpha under 0.5 drops the fragment),
    // logged once per pipeline that has it. NX_NO_ALPHA_TO_COVERAGE=1: off, as when it was a stub.
    static const bool no_a2c = std::getenv("NX_NO_ALPHA_TO_COVERAGE") != nullptr;
    ms.alphaToCoverageEnable = key.alpha_to_coverage && !no_a2c ? VK_TRUE : VK_FALSE;
    if (key.alpha_to_coverage)
        log("nvn/vk: alpha to coverage%s: fs %s", no_a2c ? " (off)" : "", key.fs ? key.fs->hash.c_str() : "-");

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    bool has_depth = key.depth_format != VK_FORMAT_UNDEFINED;
    ds.depthTestEnable = has_depth && key.depth.depth_test;
    ds.depthWriteEnable = has_depth && key.depth.depth_write;
    ds.depthCompareOp = vk_compare(key.depth.depth_func);
    // EQUAL after a depth pre-pass (the colour pass does not write depth) needs both passes to compute bit-identical
    // depth; two translated shaders compiled separately by Metal do not, and Wonder's whole course colour pass was
    // discarded (only the sky showed). LESS_OR_EQUAL keeps what EQUAL keeps for opaque geometry after a pre-pass.
    if (key.depth.depth_func == 3 && !key.depth.depth_write) {
        ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        static const bool always = std::getenv("NX_EQUAL_ALWAYS") != nullptr;  // diagnostic: EQUAL passes test nothing
        if (always) ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    }
    ds.stencilTestEnable = has_depth && key.depth.stencil_test && !std::getenv("NX_NO_STENCIL");
    VkStencilOpState* faces[2] = {&ds.front, &ds.back};
    for (int f = 0; f < 2; f++) {
        faces[f]->failOp = vk_stencil_op(key.depth.stencil_fail[f]);
        faces[f]->depthFailOp = vk_stencil_op(key.depth.depth_fail[f]);
        faces[f]->passOp = vk_stencil_op(key.depth.depth_pass[f]);
        faces[f]->compareOp = vk_compare(key.depth.stencil_func[f]);
        faces[f]->compareMask = faces[f]->writeMask = 0xFF;  // dynamic: SetStencilValueMask / SetStencilMask
    }

    VkPipelineColorBlendAttachmentState att[8]{};
    for (uint32_t k = 0; k < key.color_count; k++) {
        const BlendTarget& b = key.blend[k];
        // An R11G11B10F target's destination alpha is 1 on Maxwell; the RGBA16F image holds what the shader wrote.
        bool ufloat = (key.ufloat_colors >> k & 1) && !ufloat_raw();
        auto factor = [ufloat](uint32_t f) {
            VkBlendFactor v = vk_blend_factor(f);
            if (!ufloat) return v;
            return v == VK_BLEND_FACTOR_DST_ALPHA             ? VK_BLEND_FACTOR_ONE
                   : v == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA ? VK_BLEND_FACTOR_ZERO
                   : v == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE  ? VK_BLEND_FACTOR_ZERO  // min(As, 1 - 1)
                                                              : v;
        };
        att[k].blendEnable = key.color.blend_enable[k] ? VK_TRUE : VK_FALSE;
        att[k].srcColorBlendFactor = factor(b.src_rgb);
        att[k].dstColorBlendFactor = factor(b.dst_rgb);
        att[k].colorBlendOp = vk_blend_op(b.eq_rgb);
        att[k].srcAlphaBlendFactor = factor(b.src_a);
        att[k].dstAlphaBlendFactor = factor(b.dst_a);
        att[k].alphaBlendOp = vk_blend_op(b.eq_a);
        att[k].colorWriteMask = key.mask.mask[k] & 0xF;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = key.color_count;
    cb.pAttachments = att;

    // Stencil reference and masks and depth bias values come from command-buffer calls (nvn_raster.cpp), so they are
    // dynamic: one pipeline for every value the game uses.
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = uint32_t(sizeof dyn / sizeof dyn[0]);
    dy.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = key.color_count;
    ri.pColorAttachmentFormats = key.color_formats;
    bool stencil = key.depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT || key.depth_format == VK_FORMAT_S8_UINT;
    ri.depthAttachmentFormat = key.depth_format == VK_FORMAT_S8_UINT ? VK_FORMAT_UNDEFINED : key.depth_format;
    ri.stencilAttachmentFormat = stencil ? key.depth_format : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &ri;
    if (render_pass_objects())  // nvn_renderpass.cpp: compatible with every pass of these formats
        ci.pNext = nullptr, ci.renderPass = compatible_render_pass(key.color_count, key.color_formats, key.depth_format);
    ci.stageCount = nstages;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = layout.layout;
    VkPipeline p = VK_NULL_HANDLE;
    const uint64_t t0 = plat::monotonic_ns();
    VkResult made = p_vkCreateGraphicsPipelines(vk_device(), pipeline_cache(), 1, &ci, nullptr, &p);
    note_pipeline_created(plat::monotonic_ns() - t0);
    if (made != VK_SUCCESS)
        log("nvn/vk: pipeline for %s + %s failed", key.vs ? key.vs->hash.c_str() : "-", key.fs ? key.fs->hash.c_str() : "-");
    else if (g_trace)
        log("nvn/vk: pipeline %zu: %s + %s", cache.size() + 1, key.vs ? key.vs->hash.c_str() : "-", key.fs ? key.fs->hash.c_str() : "-");
    return (last = &*cache.emplace(k, p).first)->second;
}

// ---- sampled textures -------------------------------------------------------------------------------------------------

// Pending nvnTextureWriteTexels regions go to the image (a transfer: outside render passes).
void apply_texel_writes(VkCommandBuffer cb, Texture& t, GpuImage& img) {
    // Nothing queued (nearly always): no lock. Every sampled texture of every draw comes here, and taking the
    // registry lock alone also invalidated every thread's get<T> cache each time (docs/nvn/PERF-PLAN.md).
    if (!t.has_writes.load(std::memory_order_acquire)) return;
    std::vector<Texture::TexelWrite> writes;
    {
        std::lock_guard lk(objects_lock());
        writes.swap(t.writes);
        t.has_writes.store(false, std::memory_order_release);
    }
    if (writes.empty()) return;
    img.texels = true;
    VkImageLayout back = img.layout;
    transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    bool is3d = img.view_type == VK_IMAGE_VIEW_TYPE_3D;
    for (const auto& w : writes) {
        if (w.level >= img.levels) continue;
        Upload u = upload(w.data.data(), w.data.size(), 16);
        VkBufferImageCopy r{};
        r.bufferOffset = u.offset;
        r.imageSubresource = {img.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                  ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                  : img.aspect,
                              w.level, is3d ? 0 : std::min(w.z, img.layers - 1), is3d ? 1 : std::min(w.d, img.layers)};
        r.imageOffset = {int32_t(w.x), int32_t(w.y), is3d ? int32_t(w.z) : 0};
        r.imageExtent = {w.w, w.h, is3d ? w.d : 1};
        p_vkCmdCopyBufferToImage(cb, u.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }
    if (back != VK_IMAGE_LAYOUT_UNDEFINED) transition(cb, img, back);
}

uint64_t g_submit_serial = 0;

void pipeline_begin_submit() { g_submit_serial++; }

uint64_t g_not_ready[6];  // why sampled_ready said no: no image, dirty, texel writes, rendered, not uploaded, layout

bool sampled_ready(uint64_t texture) {
    Texture* t = get<Texture>(texture);
    if (!t || !t->image) return g_not_ready[0]++, false;  // no image yet (missing textures are the caller's)
    check_linear_content(*t);
    if (t->dirty && !t->image->rendered) return g_not_ready[1]++, false;
    if (t->has_writes.load(std::memory_order_acquire)) return g_not_ready[2]++, false;  // no lock (nvn.h)
    const GpuImage& img = *t->image;
    bool ok = img.rendered ? img.layout == VK_IMAGE_LAYOUT_GENERAL
                           : img.uploaded && img.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!ok) g_not_ready[img.rendered ? 3 : !img.uploaded ? 4 : 5]++;
    return ok;
}

void sampled_ready_log() {
    log("nvn passes: textures not ready: %llu no image, %llu dirty, %llu texel writes, %llu rendered not GENERAL, "
        "%llu not uploaded, %llu uploaded in another layout",
        (unsigned long long)g_not_ready[0], (unsigned long long)g_not_ready[1], (unsigned long long)g_not_ready[2],
        (unsigned long long)g_not_ready[3], (unsigned long long)g_not_ready[4], (unsigned long long)g_not_ready[5]);
}

VkImageView attachment_view(GpuImage& img, uint32_t level, uint32_t layer, VkFormat format) {
    if (format == VK_FORMAT_UNDEFINED) format = img.format;
    level = std::min(level, img.levels - 1);
    // A 3D image's slice is a layer of its 2D-array-compatible views, counted at this level.
    layer = std::min(layer, img.view_type == VK_IMAGE_VIEW_TYPE_3D ? std::max(1u, img.depth >> level) - 1 : img.layers - 1);
    if (level == 0 && layer == 0 && img.levels == 1 && img.layers == 1 && img.view_type == VK_IMAGE_VIEW_TYPE_2D &&
        format == img.format)
        return img.view;
    auto& cache = g_attachment_views;
    auto& v = cache[{img.image, level, layer, format}];
    if (!v) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = img.image;
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = format;
        ci.subresourceRange = {img.aspect, level, 1, layer, 1};
        if (p_vkCreateImageView(vk_device(), &ci, nullptr, &v) != VK_SUCCESS) v = img.view;
    }
    return v;
}

GpuImage* ensure_contents(VkCommandBuffer cb, uint64_t texture) {
    Texture* t = get<Texture>(texture);
    GpuImage* img = t ? image_of(texture) : nullptr;
    if (!img) return nullptr;
    bool dirty = t->dirty.exchange(false);
    uint64_t dirty_lo = t->dirty_lo.exchange(UINT64_MAX), dirty_hi = t->dirty_hi.exchange(0);
    if (dirty_hi <= dirty_lo) dirty_lo = 0, dirty_hi = UINT64_MAX;  // marked without a range: all of it
    static const bool reupload = std::getenv("NX_TEXELS_REUPLOAD") != nullptr;  // bisecting: the old way
    bool upload = !img->rendered && (!img->uploaded || (dirty && (!img->texels || reupload)));
    // A render target (COMPRESSIBLE, flag 0x8) the GPU never wrote through this image: its guest memory never holds
    // GPU output here (another target over the same memory left only stale data), so it starts cleared, not uploaded
    // (G3 cause 2, docs/nvn/DEBUG.md #50). NX_RT_UPLOAD=1: uploaded as before; NX_ALIAS_MAGENTA=1: cleared magenta.
    static const bool rt_upload = std::getenv("NX_RT_UPLOAD") != nullptr;
    static const bool magenta = std::getenv("NX_ALIAS_MAGENTA") != nullptr;
    if (upload && !rt_upload && (t->flags & 0x8) && !img->uploaded && !img->texels && !dirty) {
        if (!t->has_writes.load(std::memory_order_acquire)) {
            clear_image(cb, *img, magenta);
            img->uploaded = true;
            upload = false;
        }
    }
    // A texture already on the GPU that the CPU wrote part of: only those bytes go again (the glyph cache).
    if (upload) upload_texture(cb, *t, *img, img->uploaded && dirty ? dirty_lo : 0, img->uploaded && dirty ? dirty_hi : UINT64_MAX);
    apply_texel_writes(cb, *t, *img);
    if (img->layout == VK_IMAGE_LAYOUT_UNDEFINED) {  // never written: black, not garbage
        transition(cb, *img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange r{img->aspect, 0, img->levels, 0, img->layers};
        if (img->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
            VkClearColorValue v{};
            p_vkCmdClearColorImage(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
        }
    }
    return img;
}

VkImageView sampled_view(VkCommandBuffer cb, uint64_t texture, const ImageShape& shape, const ViewInfo* view,
                         VkImageLayout* layout) {
    *layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    GpuImage* img = ensure_contents(cb, texture);
    static uint64_t no_image = 0, incompatible = 0;
    if (!img) {
        if (++no_image % 100000 == 1) log("nvn/vk: dummy sampled: %llu with no image, %llu with an incompatible view",
                                          (unsigned long long)no_image, (unsigned long long)incompatible);
        return dummy_view(cb, shape);
    }
    *layout = img->rendered ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    transition(cb, *img, *layout);
    // The swizzle belongs to the texture (its builder, or its pool view), not to the image: an image may be shared
    // with or inherited from another texture over the same memory (#36, #42), whose swizzle differs. With the
    // image's, the UI's single-channel masks lost their alpha and drew as rectangles (docs/nvn/DEBUG.md #44).
    // The slot's own view decides: pool_swizzle is whichever slot registered the texture last (the world map's player
    // mask read as G G G G: a flat silhouette, DEBUG-2.md #67). NX_POOL_SWIZZLE_ANY=1: as before.
    static const bool pool_any = std::getenv("NX_POOL_SWIZZLE_ANY") != nullptr;
    ViewInfo vi = view ? *view : ViewInfo{};
    if (!vi.swizzle && !img->alpha_from_r)
        if (Texture* t = get<Texture>(texture)) {
            uint32_t s = pool_any || !view ? t->pool_swizzle.load() : 0u;
            if (!s)
                for (int k = 0; k < 4; k++) s |= uint32_t(std::min<uint8_t>(t->swizzle[k], 5)) << (8 * k);
            vi.swizzle = s;
        }
    // R11G11B10F: a channel reading A (NVNtextureSwizzle 5) reads ONE (1), after the texture's swizzle (G4).
    if (!ufloat_raw() && !img->alpha_from_r && (img->nvn_format == 0x3f || vi.format == 0x3f)) {
        uint32_t s = vi.swizzle ? vi.swizzle : 0x05040302u;  // 0: identity (R G B A)
        for (int k = 0; k < 4; k++)
            if ((s >> (8 * k) & 0xFF) == 5) s = (s & ~(0xFFu << (8 * k))) | (1u << (8 * k));
        vi.swizzle = s;
    }
    VkImageView v = view_of(*img, view_type(shape), vi);
    if (!v) {
        *layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (++incompatible % 1000 == 1)
            log("nvn/vk: texture %llx (%ux%u x%u, view type %d) sampled as view type %d: dummy", (unsigned long long)texture,
                img->width, img->height, img->layers, int(img->view_type), int(view_type(shape)));
        return dummy_view(cb, shape);
    }
    return v;
}

VkPipeline compute_pipeline(const ShaderInfo* cs, const ProgramLayout& layout) {
    static std::map<std::pair<const ShaderInfo*, VkPipelineLayout>, VkPipeline> cache;
    auto& p = cache[{cs, layout.layout}];
    if (!p) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module_of(cs);
        ci.stage.pName = "main";
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sg{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        sg.requiredSubgroupSize = 32;
        if (gpu_require_subgroup32()) ci.stage.pNext = &sg;
        ci.layout = layout.layout;
        if (p_vkCreateComputePipelines(vk_device(), pipeline_cache(), 1, &ci, nullptr, &p) != VK_SUCCESS) {
            log("nvn/vk: compute pipeline for %s failed", cs->hash.c_str());
            p = VK_NULL_HANDLE;
        }
    }
    return p;
}

VkImageView storage_view(VkCommandBuffer cb, uint64_t texture, const ImageShape& shape, const ViewInfo* view) {
    GpuImage* img = texture ? ensure_contents(cb, texture) : nullptr;
    if (img && img->storage) {
        transition(cb, *img, VK_IMAGE_LAYOUT_GENERAL);
        img->rendered = true;  // the shader may write it
        ViewInfo v = view ? *view : ViewInfo{};
        v.levels = 1, v.swizzle = 0;  // storage views: one level, identity
        if (VkImageView iv = view_of(*img, view_type(shape), v)) return iv;
    }
    static std::map<VkImageViewType, GpuImage> dummies;
    VkImageViewType type = view_type(shape);
    auto& d = dummies[type];
    if (!d.image) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        ci.format = VK_FORMAT_R32_UINT;
        ci.extent = {1, 1, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        p_vkCreateImage(vk_device(), &ci, nullptr, &d.image);
        VkMemoryRequirements req{};
        p_vkGetImageMemoryRequirements(vk_device(), d.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        p_vkAllocateMemory(vk_device(), &ai, nullptr, &d.memory);
        p_vkBindImageMemory(vk_device(), d.image, d.memory, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = d.image;
        vi.viewType = type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                      : (type == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D);
        vi.format = ci.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        p_vkCreateImageView(vk_device(), &vi, nullptr, &d.view);
        d.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        d.format = ci.format;
    }
    transition(cb, d, VK_IMAGE_LAYOUT_GENERAL);
    return d.view;
}

void forget_image_views(const GpuImage& img) {
    // The caches are ordered by image first: its views are one run from the smallest key with that image (this was
    // a scan of every cached view per destroyed texture, a visible share of the replay time).
    auto drop = [&](auto& cache, auto lowest) {
        for (auto it = cache.lower_bound(lowest); it != cache.end() && std::get<0>(it->first) == img.image;) {
            forget_framebuffers(it->second);
            if (it->second && it->second != img.view) p_vkDestroyImageView(vk_device(), it->second, nullptr);
            it = cache.erase(it);
        }
    };
    forget_framebuffers(img.view);  // nvn_renderpass.cpp (attachments may use the whole-image view)
    drop(g_sampled_views, std::make_tuple(img.image, VkImageViewType(0), 0u, uint64_t(0), uint64_t(0)));
    for (ViewHit& h : g_view_hits)
        if (std::get<0>(h.key) == img.image) h = {};
    drop(g_attachment_views, std::make_tuple(img.image, 0u, 0u, VkFormat(0)));
}

}  // namespace nx::nvn
