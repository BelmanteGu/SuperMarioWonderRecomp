// Pipelines and the resources draws read (docs/NVN.md, N4-N6): shader modules, descriptor-set and pipeline
// layouts, graphics pipelines keyed by their state, samplers, and sampled textures (untiled and uploaded on first
// use). Used by nvn_draw.cpp, under the GPU lock.
#pragma once

#include <cstddef>
#include <cstring>
#include <vector>

#include "nvn_shaders.h"
#include "nvn_vk.h"

namespace nx::nvn {

bool vk_depth_clip_control();

// Cache keys that are compared byte for byte (a PipelineKey with its padding, a SamplerInfo and a flag): the bytes,
// a 64-bit hash of them for an unordered_map, and the exact comparison that confirms a hit. Four lanes: one multiply
// chain over a 704-byte PipelineKey would cost what the lookup saves.
inline uint64_t hash_bytes(const unsigned char* p, size_t size) {
    constexpr uint64_t kMul = 0x9E3779B97F4A7C15ull;
    uint64_t h[4] = {size, 1, 2, 3}, w[4];
    for (; size >= 32; p += 32, size -= 32) {
        std::memcpy(w, p, 32);
        for (int l = 0; l < 4; l++) h[l] = (h[l] ^ w[l]) * kMul, h[l] ^= h[l] >> 32;
    }
    uint64_t r = h[0];
    for (int l = 1; l < 4; l++) r = (r ^ h[l]) * kMul, r ^= r >> 32;
    while (size) {
        size_t n = size < 8 ? size : 8;
        uint64_t t = 0;
        std::memcpy(&t, p, n);
        r = (r ^ t) * kMul, r ^= r >> 32;
        p += n, size -= n;
    }
    return r;
}
template <size_t N>
struct ByteKey {
    unsigned char b[N];
    bool operator==(const ByteKey& o) const { return std::memcmp(b, o.b, N) == 0; }
};
struct ByteKeyHash {
    template <size_t N>
    size_t operator()(const ByteKey<N>& k) const { return size_t(hash_bytes(k.b, N)); }
};

// The four descriptor sets of the translator's scheme (0 uniform buffers, 1 storage buffers, 2 textures, 3 images)
// for a set of stages, and the pipeline layout over them.
struct ProgramLayout {
    VkDescriptorSetLayout sets[4] = {};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    // Set 0 bindings (< 64) that are UNIFORM_BUFFER_DYNAMIC: their ring offset goes with vkCmdBindDescriptorSets, so
    // the set depends only on which buffers and sizes it names (the set cache, nvn_sets.cpp). NX_DYNAMIC_UBO=0: none.
    uint64_t dynamic = 0;
};
const ProgramLayout& program_layout(const std::vector<const ShaderInfo*>& stages);

// Everything a graphics pipeline is built from (NVN enums, decoded state).
struct PipelineKey {
    const ShaderInfo* vs = nullptr;
    const ShaderInfo* fs = nullptr;
    uint32_t topology = 4;
    VertexAttrib attribs[16] = {};
    VertexStream streams[16] = {};
    uint32_t attrib_count = 0, stream_count = 0;
    BlendTarget blend[8] = {};
    ColorStateInfo color;
    ChannelMaskInfo mask;
    DepthStencilInfo depth;
    PolygonInfo polygon;
    VkFormat color_formats[8] = {};
    uint32_t color_count = 0;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    uint32_t depth_clamp = 0;  // SetDepthClamp (applied only where the device has the depthClamp feature)
    uint32_t ufloat_colors = 0;  // bit k: colour target k is R11G11B10F (no alpha: destination alpha reads 1)
    uint32_t alpha_to_coverage = 0;  // BindMultisampleState (G7)
    float uw_x = 0;  // NX_ULTRAWIDE: gl_Position.x scale of the vertex stage (0: none, the module has no constant)
};
VkPipeline graphics_pipeline(const PipelineKey& key, const ProgramLayout& layout);
bool depth_remap();  // NX_CLIP_Z_PATCH: clip z -w..w mapped in the vertex shaders, not by VK_EXT_depth_clip_control
bool patch_clip_z(const std::vector<uint32_t>& in, std::vector<uint32_t>& out);  // nvn_spirv_clip.cpp
void clip_z_module(const ShaderInfo* s, std::vector<uint32_t>& code);             // patch_clip_z if depth_remap

// Integer vertex inputs (nvn_spirv_patch.cpp): the kind of an NVN attribute format (0 float, 1 unsigned bits,
// 2 signed bits), and a copy of a module with those locations declared as integers and bitcast to float on load.
uint32_t vertex_int_kind(uint32_t nvn_format);
bool patch_int_inputs(const std::vector<uint32_t>& in, const uint8_t kinds[16], std::vector<uint32_t>& out);
// Constant buffer reads with a computed index (c[k][index]) are clamped to the 64 KB a bank can hold, and the
// set 0 bindings read that way are returned: those are bound zero-padded to 64 KB (nvn_spirv_patch.cpp).
bool clamp_uniform_indices(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, uint64_t* bindings);
// Storage buffer bindings the shader writes (nvn_spirv_patch.cpp).
uint64_t written_storage_bindings(const ShaderInfo* s);
uint64_t dynamic_uniform_bindings(const ShaderInfo* s);
// Compute: the shared memory array redeclared with the control section's size (the translator declares 48 KB, past
// Metal's 32 KB). Fragment: the input locations in `locations` (bit mask) that the vertex stage does not write become
// private variables holding (0, 0, 0, 1), Maxwell's value for them (nvn_spirv_patch.cpp).
bool resize_shared_memory(const std::vector<uint32_t>& in, uint32_t bytes, std::vector<uint32_t>& out);
bool default_unwritten_inputs(const std::vector<uint32_t>& in, uint32_t locations, std::vector<uint32_t>& out);
// The fragment input locations `vs` does not write (0 without both stages).
uint32_t unwritten_inputs(const ShaderInfo* vs, const ShaderInfo* fs);
// The code of a stage's module: its SPIR-V with every patch above that applies (`kinds` as patch_int_inputs, or
// null; `unwritten` as default_unwritten_inputs). `int_inputs` gets whether patch_int_inputs changed it.
std::vector<uint32_t> patched_spirv(const ShaderInfo* s, const uint8_t* kinds, uint32_t unwritten, bool* int_inputs);

VkPipeline compute_pipeline(const ShaderInfo* cs, const ProgramLayout& layout);

VkSampler sampler_for(const SamplerInfo& info, bool shadow);
// A storage-image view of the texture (single level of the pool view), or the dummy storage image. Layout GENERAL.
VkImageView storage_view(VkCommandBuffer cb, uint64_t texture, const ImageShape& shape, const ViewInfo* view);

// A sampled texture ready for the shaders (uploaded from guest memory the first time, unless the GPU renders it),
// and a view of it matching what the shader declares. Transitions the image to SHADER_READ_ONLY on cb.
// `view`: the texture pool entry's view (levels, layers, swizzle), if any. `layout` gets the layout to sample in:
// images the GPU renders stay GENERAL (a bloom chain samples one level while rendering another).
VkImageView sampled_view(VkCommandBuffer cb, uint64_t texture, const ImageShape& shape, const ViewInfo* view,
                         VkImageLayout* layout);
// The image behind a texture with its contents in place (uploaded, CPU writes applied, never-written ones cleared);
// null for an unknown texture. Records transfers: outside render passes.
GpuImage* ensure_contents(VkCommandBuffer cb, uint64_t texture);
// nvn_upload.cpp: the texture's pool memory untiled (or taken row by row, pitch-linear) into the image, every level
// and layer; cleared instead when the copies would read past what was staged. Marks it uploaded.
// [lo, hi): the bytes of a texture already uploaded that changed (pitch-linear ones send only their rows).
void upload_texture(VkCommandBuffer cb, const Texture& t, GpuImage& img, uint64_t lo = 0, uint64_t hi = UINT64_MAX);
// Every level and layer to zero (colour; magenta with `magenta`), depth and stencil to 0. Leaves TRANSFER_DST.
void clear_image(VkCommandBuffer cb, GpuImage& img, bool magenta = false);
// A single level and layer of an image, for rendering to.
void sampled_ready_log();
void check_linear_content(Texture& t);  // nvn_sampler.cpp
VkImageView attachment_view(GpuImage& img, uint32_t level, uint32_t layer, VkFormat format = VK_FORMAT_UNDEFINED);
// The Vulkan format of a view with NVN format `nvn` (0: the image's own) of `img`: the image's when the view's is not
// a same-size colour format it can be viewed as.
VkFormat view_format(const GpuImage& img, uint32_t nvn);
// Destroys the cached views of an image about to be freed.
void forget_image_views(const GpuImage& img);
// True when sampled_view() would need no transfer or layout change (it can then be called inside a render pass).
bool sampled_ready(uint64_t texture);
void pipeline_begin_submit();  // per-submit state (the content checks of pitch-linear textures)
VkImageViewType view_type(const ImageShape& shape);  // the view type a shader's image declaration samples
bool dummy_ready(const ImageShape& shape);
// A 1x1 black texture of the shape, for bindings with nothing behind them.
VkImageView dummy_view(VkCommandBuffer cb, const ImageShape& shape);

}  // namespace nx::nvn
