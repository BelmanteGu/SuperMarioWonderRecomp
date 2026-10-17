// Ultrawide (NX_ULTRAWIDE, docs/nvn/studies/ultrawide-1710.md): the game renders a fixed 16:9 picture (its render
// targets are built from 1920x1080 immediates and its cameras assume 16:9). With the switch on, the runtime widens
// every 16:9 colour/depth render target by k = (display aspect) / (16:9), widens viewports, scissors, clears and the
// window crop to match, and scales gl_Position.x of the vertex shaders by 1/k in passes into widened targets, so the
// world keeps its height and shows more to the sides (Hor+). Off (the default): none of this runs.
#pragma once

#include <cstdint>
#include <vector>

namespace nx::nvn {

struct ShaderInfo;
struct Texture;

float uw_factor();                        // k > 1 when on, 1 when off (decided once)
inline bool uw_on() { return uw_factor() > 1.0f; }
uint32_t uw_widen(uint32_t w);            // the widened width of a 16:9-chain size
bool uw_widen_texture(Texture& t);        // a screen-sized render target: width widened, t.uw set
bool uw_crop(int32_t crop[4]);            // nvnWindowSetCrop's rect widened when it is the whole 16:9 picture
// Set by the draw replay before it builds descriptor sets and state: the draw's colour/depth target is a widened one.
extern bool g_uw_wide;
// A guest viewport/scissor span on a widened target: x and width scaled by k, kept inside `extent_w`.
void uw_span(int32_t& x, int32_t& w, uint32_t extent_w);
void uw_cameras(uint64_t present);     // camera aspect patch (called at each present)
void uw_scan(uint64_t present);        // NX_UW_SCAN debug
void uw_ui_vertex_shader(const ShaderInfo* vs);  // learned: drew into a UI layer target
bool uw_is_ui_vertex_shader(const ShaderInfo* vs);
bool uw_vs_scaled(const ShaderInfo* vs);  // does this vertex shader's x get scaled by 1/k (world and UI geometry)
// A copy of a vertex module whose gl_Position.x is multiplied by specialization constant 0 (default 1.0).
bool patch_x_scale(const std::vector<uint32_t>& in, std::vector<uint32_t>& out);

}  // namespace nx::nvn
