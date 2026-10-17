# Runtime switches (environment variables)

Every switch below works on the normal build, no rebuild needed. Moved here from [DEBUG.md](DEBUG.md) (700-line
rule); the bugs each one helped find are in DEBUG.md and [DEBUG-2.md](DEBUG-2.md). Player-facing ones are also in the
README.

| Variable | What it does | Code |
|---|---|---|
| `NX_LOG=<file>` | runtime log (use `private/wonder/nx.log`) | runtime |
| `NX_DUMP_FRAMES=N` | every Nth presented window texture → `private/wonder/frames/NNNNN.ppm` | nvn_vk.cpp |
| `NX_DUMP_TEXTURES=<hex>,<hex>` | at the same frames, those `NVNtexture*` are read back too → `NNNNN_<texture>.ppm` (blitted to RGBA8) | nvn_debug.cpp |
| `NX_DUMP_RAW=1` | with the above: texels as stored → `.raw` (u32 width, height, VkFormat, then 4-byte texels) | nvn_debug.cpp |
| `NX_DUMP_ON_TARGET=<hex>` | dump `NX_DUMP_TEXTURES` right after the submit that draws into that texture (every `NX_DUMP_FRAMES`-th such draw), before later frames overwrite them | nvn_draw.cpp, nvn_vk.cpp |
| `NX_TRACE_DRAWS=<first>` | logs 80 replayed draws from draw number `<first>`: shaders, targets, textures (handle:texture, size, format, `rt` = GPU-written), fragment `c3` contents, blend/mask/logic op, viewport/scissor/cull, depth | nvn_draw.cpp |
| `NX_TRACE_TARGET=<hex>` | with `NX_TRACE_DRAWS`: only draws into that color target | nvn_draw.cpp |
| `NX_TRACE_TEXTURES=1` | every `nvnTextureInitialize`: target, format, size, levels, flags, storage size, block height, swizzle | nvn_objects.cpp |
| `NX_NVN_LISTS=1` | per ended command list (a window of 200): commands, draws, calls, the caller; per submit (5 frames): handles | nvn_cmd.cpp, nvn_queue.cpp |
| `NX_NVN_STATES=1` | decoded state objects and samplers as they are bound | nvn_state.cpp, nvn_sampler.cpp |
| `NX_DUMP_SHADER_MISSES=1` | writes shader blocks that have no SPIR-V yet (to translate offline) | nvn_shaders.cpp |
| `NX_NVN_REAL=1` | records every proc's real driver address in `nvn_trace.txt` | nvn_trace.cpp |
| `NX_TRACE_SKIN=1` | the first draws of every vertex shader with an integer vertex input (skinned models): input formats and streams, vertex uniform blocks, the first vertices' bone indices and the bone rows of c8 they select | nvn_draw_trace.cpp |
| `NX_TRACE_SKINLISTS=1` | follows command lists that bind skinned programs: recorded (handle, command buffer), called, replayed; dumps the first ones' commands (`P<vs>` binds, `*` skinned, `D` draws, `C<handle>(cmds/draws/programs)` calls, `GONE` for a handle already dropped) | nvn_cmd.cpp |
| `NX_INT_ATTRIBS=0` | turns off the integer vertex input patch (on by default since #26) | nvn_pipeline.cpp |
| `NX_ZERO_DRAWS=1` | every draw inside an occlusion query; every 300 frames lists the `vs fs` shader pairs whose draws never passed a single sample (what the scene draws but we lose) | nvn_debug.cpp |
| `NX_GPU_PROFILE=1` | GPU time per render pass (timestamps at pass starts), top 10 labels every 60 frames: `ms/frame WxH L<level> fs <hash>` | nvn_debug.cpp |
| `NX_SKIP_FS=<hash>,<hash>` | drops draws with those fragment shaders (measure what they cost, or hide a broken one) | nvn_draw.cpp |
| `NX_INPUT_SCRIPT="<s>:<button>:<hold s>;..."` | presses buttons at times after the first pad read (A B X Y L R ZL ZR PLUS MINUS LEFT RIGHT UP DOWN; directions also push the stick) | input_script.cpp |
| `NX_INPUT="<frame>:<button>,..."` | older, per presented frame, 8-frame presses | hle_hid.cpp |
| `NX_TRACE_FS=1` | every file the game opens or looks for, every directory it opens, host open failures (errno); also counts reads of streamed audio (`.bwav`). Crash reports always end with the last 24 opens (DEBUG-2 #93) | hle_fs.cpp |
| `NX_HEADLESS=1` | no visible window, nothing presented; frames still render and dump (unattended runs) | platform_mac.cpp, nvn_vk.cpp |
| `NX_MEM_LIMIT_GB=<n>` | the memory guard ends the process past this footprint (default 8); `memory:` lines every 30 s | memguard_mac.cpp |
| `NX_THREAD_DUMP_AT=<s>` | once, every guest thread's call chain (frame-pointer walk) | kernel.cpp |
| `NX_TRACE_FS_HASH=<hash>` | with `NX_TRACE_DRAWS`: only draws with that fragment shader | nvn_draw.cpp |
| `NX_TRACE_COUNT=<n>` | how many draws `NX_TRACE_DRAWS` logs (default 80) | nvn_draw.cpp |
| `NX_TRACE_GLYPHS=1` | with the draw trace: the first glyph records of text draws, the atlas memory at their UVs, the sampler | nvn_draw.cpp |
| `NX_SWAP_FS=<from>:<to>` | draws with fragment shader `<from>` use `<to>` (already loaded): shows what a draw covers | nvn_draw.cpp |
| `NX_SNAP_DRAWS=<n>,<n>` | mid-frame snapshots: colour target 0 right after those draws → `frames/00000_<draw hex>.ppm` (use `seq -f %.0f`: macOS `seq` prints big numbers in scientific notation) | nvn_debug.cpp |
| `NX_SNAP_FS=<hash>,...[@<draw>]` | snapshots after draws with those fragment shaders (8 at most), from draw `<draw>` on | nvn_draw.cpp |
| `NX_SNAP_FS_BEFORE=<hash>[@<draw>]` | snapshot of colour target 0 *before* such a draw (what a feedback pass like a tonemap reads) | nvn_draw.cpp |
| `NX_TRACE_VS_BLOCKS=1` | with the draw trace: the vertex stage's uniform blocks (camera block rows, c4..c6) | nvn_draw.cpp |
| `NX_TRACE_UPLOADS=1` | uploads of big textures, with how much of their memory is non-zero | nvn_pipeline.cpp |
| `NX_ULTRAWIDE=1\|<w>x<h>\|<a>:<b>` | ultrawide: widens the 16:9 render targets, viewports, scissors, clears and the window crop to the display's aspect (`1`: the window's, else the given one; 16:9 and narrower: off), patches the cameras' aspect (heap scan) and squeezes the UI back to 16:9. Off (unset) costs nothing. docs/nvn/studies/ultrawide-1710.md | nvn_ultrawide.cpp, nvn_draw.cpp |
| `NX_UW_CAMERA=0` | with `NX_ULTRAWIDE`: no camera patch, every geometry draw squeezed (the world shrinks to the centre: for comparison) | nvn_ultrawide.cpp, nvn_draw.cpp |
| `NX_UW_UI=learn` | with `NX_ULTRAWIDE`: squeeze only the vertex shaders seen drawing into BGRA8 UI layer targets, not every draw without depth test | nvn_draw.cpp |
| `NX_UW_NOSCALE=<hash>,..` / `NX_UW_SCALE=<hash>,..` | with `NX_ULTRAWIDE`: vertex shaders never / only these are squeezed | nvn_ultrawide.cpp |
| `NX_UW_TRACE=<first>:<count>` | with `NX_ULTRAWIDE`: one `uwdraw` line per draw (shaders, count, target, depth/blend, viewport, whether squeezed) | nvn_draw.cpp |
| `NX_UW_SCAN=<present>` | once: where guest memory holds 16/9, 9/16, 1920x1080, 1280x720 (heap and main's data), with context (`uwscan` lines) | nvn_ultrawide.cpp |

Always on: every 60 presents `nvn/vk: per frame over 60: submits, replay ms (host), GPU wait ms, present wait ms,
upload MB`.

Clear colors are logged under `NX_TRACE_DRAWS` too (`clear <texture> (WxH) to r g b a`, each pair once).

Reading dumps: `.ppm` opens in Preview. `.raw` of `VK_FORMAT_B10G11R11_UFLOAT_PACK32` (122): bits 0-10 R and 11-21 G
(5-bit exponent, 6-bit mantissa), 22-31 B (5/5); exponent 31 is Inf/NaN. A 20-line numpy script decodes it.

Shaders: `spirv-cross private/wonder/spirv/<hash>.spv --version 450` shows the translated GLSL; the hash is what
`NX_TRACE_DRAWS` prints. `private/wonder/spirv/results.tsv` has each shader's resources.

## Windows (docs/nvn/WINDOWS-PLAN.md)

| Variable | What it does | Code |
|---|---|---|
| `NX_GPU=discrete\|integrated\|<index>\|<name part>` | the Vulkan device to run on; default the first discrete GPU. Every device is logged (`nvn/vk: GPU n: ...`), then the choice | nvn_gpu_pick.cpp |
| `NX_MUTE=1` | no sound device (also implied by `NX_HEADLESS`) | platform_win.cpp, platform_mac.cpp |
| `NX_FULLSCREEN=1` | start fullscreen (F11 / Alt+Enter toggle) | platform_win.cpp |
| `NX_HANDHELD=1` | handheld mode: the game renders for 1280x720 | svc_am.cpp |
| `NX_ASTC_DECODE=1|linear|srgb|<hex>,...` | ASTC decoded on the CPU even where the GPU has it (all, one half, or the listed NVN formats) (compare with the hardware decoder; DEBUG-2 #69) | nvn_vk.cpp, nvn_upload.cpp |
| `NX_NO_PASS_BARRIER=1` | no barrier after render passes, as before DEBUG-2 #71 (bisecting) | nvn_draw.cpp |
| `NX_STAGING_RING=1` | big texture uploads through the upload ring, as before DEBUG-2 #72 (bisecting) | nvn_upload.cpp |
| `NX_PIPELINE_CACHE=0` | no on-disk pipeline cache (DEBUG-2 #73) | nvn_pipeline_cache.cpp |
| `NX_PIPELINED=0..4` | Windows/Android: synchronous by default; 1..4 turns pipelined fences on (macOS default; DEBUG-2 #76) | nvn_frames.cpp |
| `NX_CMD_REGION_OLD=1` | command memory keyed by the `NVNmemoryPool*` plus offset again, as before DEBUG-2 #90 (lists of one pool dropped by another's: exploded geometry); logs the first 5 such drops | nvn_cmd.cpp |
| `NX_HANDLES_OLD=1` | the command-list safety net counts every list ever recorded again, as before DEBUG-2 #91 (live lists not called for ~2^18 recordings dropped: the world map lavender with black wedges after 3+ minutes in a course) | nvn_cmd.cpp |
| `NX_KEEP_HANDLES=<n>` | the safety net's limit of live command lists (default 262144) | nvn_cmd.cpp |
| `NX_CMD_POOL_DROP=0` | command lists recorded in a memory pool stay when the pool is finalized, as before DEBUG-2 #91 (live lists then grow by thousands per course entry) | nvn_cmd.cpp |
| `NX_SNAPSHOT_MEMORY=0` | a pool slot whose texture struct was re-initialized over other memory (same target and format) samples that new texture again instead of the registered copy, as before DEBUG-2 #92 (course 1-1's background without its haze for single frames) | nvn_sets.cpp |
| `NX_FRAME_SIG=1` or `=<fs prefix>,...[+]` | one `fsig <present>` line per frame: frame time, draws, a hash of their shader order, the per-shader draw count changes, uniform blocks all zero or rewritten before the present; with prefixes, the first draw of those fragment shaders: its textures (image, size, format, last render serial) and fragment blocks; `+` also how each rendered texture's pool slot resolved (DEBUG-2 #92). Slow | nvn_draw_trace.cpp, nvn_sets.cpp |
| `NX_CMD_DROPS=1` | remembers what dropped each command list (the list recorded over it, its pool, the safety net) and tells it on each `called handle ... is unknown` line; `nvn: handles: live, queued, dropped by ...` is logged every 4096 lists either way | nvn_cmd.cpp |
| `NX_CHECK_VERTS=1` | logs draws that fetch vertices or instances past their bound vertex buffer range (vs/fs, indices, bound size; DEBUG-2 #90) | nvn_written.cpp |
