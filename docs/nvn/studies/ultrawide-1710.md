# Ultrawide (21:9, 32:9), study of 2026-10-17

Question: can the Linux build show Wonder wider than 16:9, and how. Everything below was run on the Intel UHD (ADL-S GT0.5),
headless (`NX_HEADLESS=1`), 2560x1080 window size via `NX_RESOLUTION`, gameplay reached with `NX_INPUT_SCRIPT` (title, user
select, character select, Flower Kingdom intro, then Mario walking right). "Measured" = seen in a log or a frame dump;
"inferred" = read from code or deduced. Frame dumps are in the study's scratch area (`runs/ultrawide-*/frames`), listed per claim.

Verdict: feasible, and a first version works with runtime-only code (no regenerated game code): `NX_ULTRAWIDE=1`
(prototype, off by default, nothing runs when off). The world is Hor+ (same height, wider view), the HUD sits in a centred 16:9
safe area, menus and the title are centred 16:9. Cost on the Intel: about 15% fps at 2560x1080.

## 1. How the picture reaches the window at a non-16:9 size (inferred from code; headless cannot show it)

* The game's window textures are 1920x1080 (docked) and `nvnWindowSetCrop` crops them (handheld: 1280x720 inside them).
* Default on Linux/Windows: `nvn_vk.cpp` blits the crop to the whole swapchain image (`vkCmdBlitImage`, destination = the
  full extent): at a 21:9 window the 16:9 picture is **stretched**. With `NX_FSR=1` or `NX_PRESENT_PASS=1` the present pass
  (`nvn_present.cpp`, `scale = min(tw/cw, th/ch)`, box centred) **pillarboxes** it keeping the aspect.
* Headless dumps (`NX_DUMP_FRAMES`) are of the game's window texture, not the swapchain, so neither is visible in a dump.

## 2. Where the 16:9 comes from

Measured:
* `NX_RENDER_RES=2560x1080` (the runtime answers `GetDefaultDisplayResolution` with it, `svc_am.cpp`): **ignored**. The window
  crop stays `0,0 1920x1080` and the textures stay 1920x1080 (`runs/ultrawide-a1`, `nx.log` and its three dumps are 1920x1080).
  Same finding as `MAC-PERF.md`. So option (a) "tell the game a wider display" does nothing: the game picks docked 1920x1080 or
  handheld 1280x720 by operation mode and ignores the display size.
* Render targets (`NX_TRACE_TEXTURES`): the window textures (flags 1, fmt 0x25, 1920x1080), the scene colour (fmt 0x3f,
  flags 0 and 8), depth (0x35), LDR targets (0x25, 0xa) and the whole bloom/blur chain are all 16:9 and are **re-initialised
  every frame at the same struct addresses** with dynamic-resolution sizes: 1920x1080, 1856x1044, 1664x936, 1408x792, half
  and quarter chains 960x540, 480x272, 240x136, ... 30x17. The dynamic resolution keeps the target and shrinks the viewport
  (viewport 0,0,1664,936 inside a 1920x1080 target happens in the draw trace).
* Camera: the world draws' uniform blocks carry `c3[15] = (.., .., 1.77778, 0.5625)` (aspect, 1/aspect) and
  `c3[18] = (1920, 1080, 1/1920, 1/1080)`, `c3[19] = (1280, 720, ...)` (draw trace, `NX_TRACE_VS_BLOCKS=1`).
* Guest memory scan (`NX_UW_SCAN=2600`, gameplay): the camera objects hold `{.., 1000.0 (far), 0.785398 (fovy, 45 deg), 1.77778
  (aspect), ..}`; the heap has **51 of them** (a pool, ~0x430 bytes apart). Also 40+ screen-info blocks `1920.0 1080.0
  1/1920 1/1080 | 1920 1080 1 99999 ...` (uniform data) and the texture-builder structs with the immediates 0x780, 0x438.
* Guest code (`private/wonder/gen/main`, grep for the immediates 0x780/0x438; inferred, not run): the window/target sizes are
  immediates in a handful of functions, e.g. `main+0x56A4F0` (calls with w3=0x780, w4=0x438, format 0x1a, builds the target
  group), `main+0x5A48E0` (a descriptor with 1920x1080 stored). No `1.7777778` literal exists in the binary (the aspect is
  computed), so there is no data constant to patch. Patching the guest properly needs `--hook-wrap` regeneration.
* The 2D UI is not drawn full-screen directly: the HUD is drawn into a **430x180 BGRA8 layer target** (draw trace, targets
  `430x180 fl 8 fmt 25`, quads of vertex shaders `42ea6138`, `2fc962d8`, `a106c763`, `e66384ab`) and composited into the scene
  target by one blended quad (`42ea6138`, count 6). Menus draw sprites straight into the scene target (no depth test).
  The final pass is one quad `d0c055be` (+ fs `38edabcd`) from the scene target to the window texture.

## 3. Options

| | what | cost | risk | result |
|---|---|---|---|---|
| (a) | report a wider display | none | none | **does nothing** (measured above) |
| (c) | widen targets and viewports, keep the projection | small | picture is a horizontal stretch | not run as a clean case. `runs/ultrawide-c3` (cameras patched, no UI squeeze) shows what is left of it for the UI: HUD, title art and menus stretched 1.33x |
| (b1) | widen targets + squeeze every geometry draw by 1/k (`NX_UW_CAMERA=0`) | small | the world only shrinks into the centre | measured, `runs/ultrawide-u6`: a 16:9 picture centred in 2560, sides empty. The game submits nothing past its own 16:9 frustum, so clip-space tricks cannot add scenery |
| (b2) | (b1) targets + **camera aspect patched to k*16/9** + squeeze only the UI | medium | heap scan, 51 camera objects | **works** (`runs/ultrawide-m1`, `-c7`): see 4 |
| (d) | regenerate with `--hook-wrap` on the camera/target constructors | large | needs reversing the camera, target and layout code | the clean version; not tried |

How much of the level is visible past the old edges: with the aspect patched the game's own culling widens too. Measured
over the intro course walk (`runs/ultrawide-m1/frames/02400..04600`, 2560x1080): scenery, coins, enemies and a tree at the
right edge appear all the way to both edges, no popping line seen. (Mario and the terrain draw as black silhouettes in the
baseline too, a separate known glitch, so characters could not be checked for edge cut-off beyond their silhouettes.)

## 4. The prototype (commit on branch `study/ultrawide`, `runtime/src/nvn/nvn_ultrawide.cpp` + hooks)

`NX_ULTRAWIDE=1` (display aspect from the window) or `=<w>x<h>` / `=<a>:<b>`; below 16:9*1.02 it stays off. k = aspect / (16/9).

1. **Targets.** In `TextureInitialize` every 2D texture that is not a packaged asset (flags 4) and is 16:9 within 3%
   (the chain: 1920x1080 ... 30x17, window textures) gets `width = even(round(w * k))`. Logged once per shape
   (`nvn: ultrawide: widening 1920x1080 f0 fmt3f`, 70+ shapes). The scene colour has **flags 0**, not 8: the first version
   missed it and the final composite sampled a 1920 texture (`runs/ultrawide-c2`, fixed in `c3`).
2. **Viewport / scissor / clears.** For a draw whose first colour (or depth) target was widened: x and width scaled by k and
   clamped (`uw_span`); same for the support buffer's viewport size (`nvn_sets.cpp`) and scissored clears (`nvn_clears.cpp`).
   `nvnWindowSetCrop(0,0,1920,1080)` becomes `0,0,2560,1080`.
3. **Cameras.** `uw_cameras()` (called at each present) scans the heap for the three floats `1000.0, 0.785398, 1.7777778` in a
   row on a worker thread every 2 s (220 ms per scan, 51 hits, a core at about 11%), and rewrites each aspect to k*16/9 at
   every present. The game turns it into the projection and culls with it. (Replace by a guest hook in a real version.)
4. **UI squeeze.** The vertex modules get `gl_Position.x *= spec_constant(0)` (`patch_x_scale`, modelled on `patch_clip_z`;
   specialization value in `PipelineKey.uw_x`, 0 = none). It is set to 1/k for draws into widened targets that have **no depth
   test** (screen-space UI: HUD composite, menus, title, text) and, if short (count <= 6), do not sample a widened texture
   (post chain and composites sample widened targets and must fill them). `NX_UW_UI=learn` instead squeezes only the vertex
   shaders learned from draws into BGRA8 layer targets (HUD only: menus then came out half stretched, `runs/ultrawide-c6`).
5. Everything is off without `NX_ULTRAWIDE`: no thread, no pipeline-key change (field 0), no patched modules; per draw one
   cached `uw_on()` test. Off vs baseline on the Intel: title 49.4 fps, gameplay 26.6 fps (same as the unmodified build).

## 5. Result (frames, all 2560x1080 unless noted)

* `runs/ultrawide-m1/frames/03800.png`: gameplay, Hor+: sky, hills, hill and a character at the right edge, HUD in the
  centred safe area, undistorted (`runs/hud_cmp.png`: coin 50 px wide at 1920, 65 px stretched in `c3`; squeezed back in `c7`).
* `runs/ultrawide-c7/contact.png`: title, user/player select, intro: centred 16:9 UI, sky gradient bands on the sides.
* `runs/ultrawide-w32/frames/02000.png`: `NX_ULTRAWIDE=3840x1080` (32:9, window texture 3840x1080), same scene, works.

Intel iGPU, 2560x1080 target vs 1920x1080 (60-frame windows, means of the title screen and of the last 12 windows in the
intro course):

| | title fps | gameplay fps | gameplay fence wait |
|---|---|---|---|
| off (1920x1080) | 49.4 | 26.6 | 23.7 ms |
| on, 21:9 (2560x1080, 1.33x pixels) | 42.0 | 22.3 | 30.6 ms |
| on, 32:9 (3840x1080, 2x pixels) | 28.5 | 16.9 | n/m |

About -15% fps for 21:9 (1.33x the pixels), -36% for 32:9: GPU bound, close to the pixel ratio. Replay time on the host is
unchanged (2.5 vs 1.9 ms title, 11.0 vs 11.2 gameplay).

## 6. Open problems

* **HUD and menus are a centred 16:9 area**, not anchored to the screen edges; full-screen UI backdrops (title art, fades,
  menu dimming quads) are squeezed too, so the sides show the (clamped) scene behind them. The HUD layout (nn::ui2d-style)
  would need its screen size patched in the guest to anchor, or per-element offsets.
* **Aspect is decided once** at the first texture; a window resize or F11 to another aspect needs a restart. A 16:9 window with the switch on
  squeezes the picture.
* Textures read their size back through `nvnTextureGetWidth` (the widened one); not seen to matter, not proven.
* Screen-space shaders that use the constants `1920 1080 1/1920 1/1080` (blur taps, `c3[18]`) still think 16:9: bloom/blur
  taps are 1.33x too far apart in x. Not visible in the frames, not measured.
* **Camera scan hack**: 11% of a core, 2 s latency after a scene load (the new cameras show 16:9 for up to 2 s, seen as a
  brief pillar at scene starts; not measured), and it patches all 51 pool cameras, including any non-world ones.
* Full-screen transitions (wipes, the "Wonder effect"), the world map, boss rooms, vertical/auto-scroll courses, cutscenes with
  letterboxed cameras, 2-4 players and the 640x360 `fmt 0x6` targets (widened by the rule, purpose unknown) were **not tested**.
  Only the title, menus and the first minute of the intro course were run.
* Intel only, headless only; the swapchain/present path (stretch vs pillar) was not run.

## 7. Next steps

1. Run the whole of course 1-1, the world map, a wipe and a 2-player screen with `NX_ULTRAWIDE=1`; collect what breaks (1 day).
2. Replace the heap scan by a guest hook: `--hook-wrap` on the camera setup function that writes `1.77778` (find the writer
   by the pool of 0x430-byte objects) so the aspect is set at creation and the thread goes away (1-2 days of reversing).
3. HUD anchoring: find the layout system's screen-size input (the 430x180 layer target sizes come from somewhere) and give
   it k*1920 so layers anchor to the edges, then drop the UI squeeze for the HUD (2-4 days).
4. Make the switch dynamic with the window (recreate targets on resize) and a menu/config entry; fullscreen on the real
   display, and document the cost (0.5 day).
