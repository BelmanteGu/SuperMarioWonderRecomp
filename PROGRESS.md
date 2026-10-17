# Progress: Super Mario Bros. Wonder, NVN → Vulkan on macOS

Living log, newest first. The plan is [docs/NVN.md](docs/NVN.md); what we learn about the game is in
[docs/nvn/WONDER.md](docs/nvn/WONDER.md). Hades is out of scope for this track (it is done on PC and focused on
Android elsewhere).

**Target:** Wonder 1.0.0 (010015100B514000) running natively on the Mac (Apple M1, 16 GB, macOS 14), CPU recompiled,
NVN implemented over Vulkan (MoltenVK).

## Status by phase

| Phase | Issue | State |
|---|---|---|
| N0 shared core, per-game layout, probe | #7 | done for this track (Windows/Hades items left, not needed here) |
| N1 CPU side: recompile, boot to `nvnBootstrapLoader` | #8 | **done**: the game runs its frame loop on macOS (2,000+ frames, no crash) |
| N2 NVN reconnaissance (tracer, API.md) | #9 | **mostly done**: tracer, driver addresses of all 534 procs, device info, signatures of the procs in use; API.md to write |
| N3 device, pools, sync, window (first NVN pixel) | #10 | **done**: MoltenVK device, swapchain, window textures presented, clears replayed (dark grey = never drawn) |
| N4 textures and samplers | #11 | **working**: untiling + upload (BNTX block heights), ASTC/BCn, samplers, swizzles, views (levels/layers) |
| N5 Maxwell shaders → SPIR-V | #12 | **done**: 12,717/12,717 blocks offline + the 34 embedded in main; runtime cache by hash |
| N6 pipeline state and draws | #13 | **working**: pipelines, descriptor sets, dynamic rendering, texture views; **the title screen renders correctly** |
| N7 whole game, performance | #14 | **in progress**: title, menus, world map and the first course render correctly; 60 fps in menus and in the course (headless, pipelined fences) |
| NA AudioRenderer (`audren:u`) | — (new) | **renders in process** (the sdk's own server + DSP code, DFC); output to the Mac next |

## Log

### 2026-10-17: Linux port, languages, ultrawide, 60 fps on an Intel iGPU, build guide

- **Linux port** (BelmanteGu): `platform_linux.cpp` (SDL3 window, pad, keyboard, audio, Vulkan surface, crash
  handler), `memguard_linux.cpp`, a CMake branch, `tools/wonder_linux.sh`. On Ubuntu 26.04 the generated C++ and the
  12,751 SPIR-V modules came out byte for byte the Mac's; the importer gained `--exefs` so no nstool is needed.
- **Languages** (#4, LokiCode404): `NX_LANGUAGE` / `language.txt` and the `set` service; zh-Hans / zh-Hant fixed
  after review (all 16 Mals archives checked with `NX_TRACE_FS`).
- **Ultrawide** (`NX_ULTRAWIDE=1`): render targets widened to the window's shape and the cameras' aspect patched:
  real Hor+ at 21:9 and 32:9; HUD and menus a centred 16:9 area. [docs/nvn/studies/ultrawide-1710.md](docs/nvn/studies/ultrawide-1710.md).
- **Above 60 fps:** not by a switch; the simulation steps once per present interval
  ([fps-unlock-1710.md](docs/nvn/studies/fps-unlock-1710.md)). `NX_FPS_CAP=30` is the clean low-end mode.
- **Intel UHD (ADL-S GT0.5), the weakest GPU here:** from 26-36 fps with black characters to **60.0 fps at 720p,
  uncapped, correct picture (14 of 14 windows at 58+, three runs)**. Black characters: unwritten vertex outputs now
  start at zero. Speed: the CPU replay of a frame went from 12 ms to 5 ms once the performance power profile is held
  while playing (power-saver throttled the bursty threads); pipelined submits and unpadded buffers on integrated GPUs.
  Ultrawide 2560x1080 at 720p: 59.0. Docked 1080p stays GPU-bound (~39). [intel-60fps-1710.md](docs/nvn/studies/intel-60fps-1710.md),
  [summary-1710.md](docs/nvn/studies/summary-1710.md).
- **Android launcher:** Widescreen, Screen rate (experimental), automatic pipelining, Buffer padding.
- **Build guide** (#8, from ouni666's community guide): [docs/BUILD.md](docs/BUILD.md) and `tools/extract_shaders.sh`
  (all 141 shader groups; checked identical to the extraction used for the build).
- **#5 (Windows crash after character select):** not reproduced on Linux; analysis in DEBUG-2 #93 (a rail whose
  parameter resource is not found); crash reports now list the last files opened.
- README pictures and GIF re-shot on Linux with the RTX 3050 in ultrawide.

### 2026-10-14: Mac performance study; glyph-cache upload fixed; FSR 1

Full write-up and method: [docs/nvn/MAC-PERF.md](docs/nvn/MAC-PERF.md).

- **Black screen on start** was a stale pipeline cache file (`pipelines_106b_*.bin`): MoltenVK's `readData` never
  returned. The file was moved aside; it is rebuilt on the first run.
- **Why the course was slow:** the 2048x8192 R8 glyph cache was untiled, expanded to RGBA8 (64 MB) and uploaded whole
  almost every frame. Now the write watch reports the written pages and only those rows go up
  (`5f01c9e`). Course 1-1 headless: 50.4 -> 59.1 fps mean, replay 6.3 -> 2.5 ms, windows under 35 fps 37 -> 0,
  main-thread frames over 15.83 ms (the game's 30-fps governor trigger) 17.6% -> 1.7%.
- **GPU is now the limit** at 1080p: 12.8 ms median, 16.3 p90. Handheld mode (`NX_HANDHELD=1`, 720p) needs 5.3 ms.
- **FSR 1** (EASU + RCAS) in the present pass, `NX_FSR=1`, about 0.13 ms at 2560x1440. `tools/wonder_mac.sh
  [quality|balanced|performance]` picks the switches.
- Condvar fix from the Castlevania study: the waiters word is written before the mutex is released (`e579e08`).
- Not done: arbitrary resolution scale (the game ignores the display resolution answer), FSR 3 frame generation
  (no motion vectors; MetalFX interpolation needs macOS 26).

### 2026-09-30 (night): Wonder boots on Android; W8 fixes; Windows plan

![World map with the player](docs/media/wonder/39-world-map-player.png)
![Touch controls, present-pass self-test](docs/media/wonder/41-android-touch-overlay-selftest.png)

- **Mac (W8):**
  - Button icons: our own NintendoExtended glyph font. The title reads "Press [L] + [R] to Start".
  - World map: the player is drawn. A pool swizzle leaked from another slot, and the composite read the silhouette
    strength as the distance.
  - A pipeline cache on disk halves the course's hitches.
  - A CPU BC decoder for GPUs without BC, bit-exact against Pillow.
- **Android (branch `feat/android-wonder`):**
  - A0 (build) and A1 (runs on a Galaxy S23 at 40-60 fps at the title) are met.
  - Two phone-only faults are fixed: the zygote's executable sat in the alias region, and the data folder was not
    readable by the app.
  - Screen: the present pass (rotation, fit, alpha 1, the surface's own format) is checked on the Mac and waits for
    the phone.
  - Drawn touch controls for Wonder, ADPF performance hints, and the phone's data zip and install scripts.
- **Windows:** a plan and a data zip for the PC (branch `feat/windows-wonder`, docs/nvn/WINDOWS-PORT.md, milestone
  WIN).
- **Open:**
  - The exploded-mesh glitch (#43) is intermittent. One run of today's showed it, persistently, after loading a
    pipeline cache saved while pipelines were being made. The save is now done under the GPU lock and being
    re-checked (DEBUG-2 #86).
  - Windowed fps (#51).
- Details: [docs/nvn/DEBUG-2.md](docs/nvn/DEBUG-2.md), entries 65-67 and 78-88.

### 2026-09-30 (evening): 60 fps in the course (headless), W7 fixes

![Course at 60 fps](docs/media/wonder/38-course-60fps-pipelined.png)

- **60.0 fps steady** in course 1-1 headless (it was 48-58).
  - Cause: pipelined fences, now on by default. The frame's GPU work is no longer waited for, and compute results
    still reach guest memory before the submit returns.
  - `NX_PIPELINED=0` restores the synchronous path.
- Glitches from the gameplay recording (milestone W7):
  - the game's waits now cover submits being replayed (horizon band, exploded meshes);
  - EQUAL colour passes get a small depth bias (black silhouettes, wedges);
  - a slot or object empty at the replay mark uses the current one (green silhouettes);
  - replay-mark gaps closed (snapshots, sampler slots);
  - windowed present mode: MAILBOX, else IMMEDIATE.
- A regression was caught and fixed in the same series: an empty sampler slot at the mark lost the ground's grass.
- Details: [docs/nvn/DEBUG-2.md](docs/nvn/DEBUG-2.md), entries 59-64.
- Next: windowed validation with the user (glitches and 60 fps on screen).

### 2026-09-30 (day): the course renders correctly (samplers restored), glitch plan W6

![Course](docs/media/wonder/33-course-samplers-restored-goombas-lit.png)
![World map](docs/media/wonder/34-world-map-samplers-restored.png)

Five read-only studies of the remaining glitches gave one plan, [docs/nvn/GLITCH-PLAN.md](docs/nvn/GLITCH-PLAN.md)
(milestone W6, issues #33-#42). What landed (docs/nvn/DEBUG.md #46-#55):

- **Sampler objects restored (#50).** A file split in c41b534 had dropped every sampler proc, so all textures sampled
  nearest, clamped, mip 0 only, with shadow compare ALWAYS.
  - Goombas, the tree and the characters are lit: their shaders read the AO target at LOD 1, the only level the game
    renders.
  - The sky is smooth, and the map's repeating textures are back.
- **Images shared across swizzles (#46).** The UI's text strips and the shadow map are no longer sampled as stale
  uploads.
- **Deferred clears (#47).** Clears become the next pass's loadOp CLEAR.
- **Handheld mode (#48).** `NX_HANDHELD=1` gives 720p and 52-57 fps in the course; `nvnWindowSetCrop` is implemented.
- **Uploads and fresh targets (#51).**
  - R11G11B10F uploads are widened to the RGBA16F host format.
  - Fresh render targets and unwritten attachments are cleared, not uploaded or loaded.
- **Smaller state fixes (#52).**
  - 1D textures are 2D images of height 1.
  - Channel-masked clears.
  - Alpha to coverage.
  - Copies within one image.
- **Compute results shared within a submit (#53).**
- **Texture incarnations per submit (#55).** 640-750 thousand replay lookups per run had hit a texture re-initialized
  mid-replay.
- **Translator.** FMZ honoured on FMUL/FFMA (#49, 21 compute blocks).

- **The game records frame N+1 while frame N is replayed (#56, #57).** 98.5% of the texture-pool registrations
  happen during a replay.
  - The replay now resolves textures and pool slots as they were when its command lists ended recording.
  - Compute write-backs copy back only the words the GPU changed.
  - This removed the intermittent glitches: the band at the horizon, exploded meshes, a white foreground layer.
    4 runs, 28 course frames, all clean (before: about 1 run in 3).

![Course](docs/media/wonder/36-course-tree-goombas.png)

Course (docked 1080p, headless): 42-47 fps.

Open:

- **Character-select blur (#42).** The chain runs once, and the game's own command stream clears its capture
  (DEBUG #58). The mask and halftone dots are drawn. It needs a capture from a real console to know what is right.
- **"Too bad!"** is an outline by design (its font is an outline face, DEBUG #56).
- **Button glyph font.** Needs `master_key_16` from the user's console.
- **60 fps docked.** The plan is in [docs/nvn/PERF-PLAN.md](docs/nvn/PERF-PLAN.md).

### 2026-09-30 (night): 60 fps in menus, the course at ~30 fps

![Title](docs/media/wonder/27-title-60fps.png)
![Character select](docs/media/wonder/28-character-select-60fps.png)
![World map](docs/media/wonder/29-world-map.png)
![Course 1-1 (actors still dark)](docs/media/wonder/30-course-sky-fixed-actors-dark.png)

- **Speed.**
  - Title, menus, character select and the world map run at 55-60 fps; they were at ~15 fps yesterday.
  - The first course runs at 30-44 fps.
  - Changes (docs/nvn/DEBUG.md #30-#37):
    - uploads reused within a submit (60 → 12 MB per frame);
    - clears inside render passes;
    - missing pool textures no longer end the render pass (1.4 M breaks per run);
    - the kernel's spin naps limited to Hades (the job system's yields cost 1 ms each);
    - game threads at interactive QoS;
    - a frame limiter at the window's present interval;
    - mailbox present;
    - one texture resolution per draw.
- **Rendering fixes.**
  - Characters: the 2D menu art and the 3D models on the map render with the right colours.
  - Live textures over the same memory share one image (#36).
  - Stencil reference and masks, depth bias and depth clamp are applied (#38).
  - Review fixes: host scratch data is not shared between draws, and clears honour their masks and sizes.
- **Open.**
  - The course's actors render dark (#39, #42). Mario is fixed by reusing the images of destroyed render targets.
    Goombas and trees still sample dark light-probe cubemaps.
  - The course is at 30-40 fps. The game's governor asks for 30 fps when its main thread's frame passes 15.8 ms
    (#43). That thread waits in the job system for the render thread, which replays our Vulkan work and waits for
    the GPU. Next: fix the replay thread (#40) so the replay leaves the game's critical path, and make the replay
    cheaper.
  - Tried and kept off by default, documented: the replay thread (#40), Deferred binds (#41), kept pool textures (#38).

### 2026-09-29 (night): real colours, two crashes fixed

![Before / after](docs/media/wonder/26-colours-before-after.png)
![Course](docs/media/wonder/23-colours-fixed-course.png)
![Bowser's castle](docs/media/wonder/25-bowser-castle-transformation.png)

- Milestones W1 (stability), W2 (colour), W3 (60 fps) and W4 (glitches) on GitHub, issues #15-#25, from the study
  docs/nvn/STUDY-colour-speed-glitches.md.
- **Colours match the real game** (DEBUG.md #29): NVN sRGB texture views are now honoured. Mean luma 0.756 against
  0.748 on the same shot.
- **Crashes**: texture mip levels clamped (#27, a Metal abort after Bowser arrives), and audio updates paced to renders
  (#28, a LightLimiter never initialized). Runs now reach Bowser's castle transformation.

### 2026-09-29 (evening): Mario and every character appear

![Mario in the course](docs/media/wonder/19-mario-appears-course.png)
![The cast at the Flower Castle](docs/media/wonder/20-cast-at-flower-castle.png)
![Bowser arrives](docs/media/wonder/21-bowser-arrives.png)

- **Characters render.** Two bugs hid them (docs/nvn/DEBUG.md #26): Metal refused every skinned pipeline (integer
  bone-index inputs declared as float; the integer input patch is now on by default), and the per-mesh draw lists the
  characters use, recorded when the course loads, were dropped by our command-handle limit before the game first
  called them. Handles now live as long as their command memory.
- New debug tools: `NX_ZERO_DRAWS` (occlusion query per draw), `NX_TRACE_SKIN`, `NX_TRACE_SKINLISTS`.
- Studied in parallel by four read-only agents (shader classification, emulator research, CPU-side draw issue, GPU
  sync/state audit). Found for later: stencil reference and masks are stubs, a stencil-only clear also clears depth,
  polygon offset and depth bounds are ignored.

### 2026-09-29 (afternoon): sound on the Mac, right-side-up course, 793 shaders fixed

- **Audio plays on the Mac's speakers.** The AudioRenderer's final mix is a DeviceSink command that the SDK's DSP
  command processor skips in CPU mode (the ADSP does it on the console); an HLE around
  `CommandListProcessor::Process` now reads the sink's mix buffers after each 5 ms frame, folds 6 channels to stereo
  and queues them to SDL3 (40-55 ms latency). A race between the game's sound thread and our render thread
  (`AudioRenderSystem::Update` vs command generation) is serialized. `NX_AUDIO_DUMP=file.wav` records 10 s.
- **The course was rendered upside down** since the beginning: NVN's upper-left origin maps NDC +Y to the top like D3D.
  Negative-height viewports now, NVN's front faces as is (DEBUG.md #17).
- **793 shaders were silently miscompiled** by the offline translator's SPIR-V back end (dropped `else if` chains):
  the dialog text among them. Patched in the harness and regenerated (DEBUG.md #18, TRANSLATOR.md).
- Two investigations ran in parallel with the main line (the translator and the audio output) while the orientation
  bug was being found; the approach works and will continue.

### 2026-09-29 (late morning): the opening cutscene plays in 3D

![Opening: the Flower Kingdom](docs/media/wonder/12-opening-flower-kingdom-3d.png)
![Opening: hills and the airship](docs/media/wonder/13-opening-hills-airship.png)
![Opening: the logo](docs/media/wonder/14-opening-logo.png)

- The opening now runs past its dialogs: Peach's voice, the party timeline music, Kamek's airship flying over the
  Flower Kingdom, the logo. The recompiler fix that got it there: a jump table whose base register comes from another
  control-flow path (DEBUG.md #16). Memory stays under 4.6 GB.
- Some frames alternate with the sky/mirrored view (to look at), dialog text still invisible.

### 2026-09-29 (morning): character select, the opening, audio, a memory leak fixed

![Character select](docs/media/wonder/07-character-select.png)
![Opening, sky](docs/media/wonder/10-opening-sky.png)
![HUD after the opening dialogs](docs/media/wonder/11-hud-after-opening.png)

- **Title → file select → player count → character select → confirmation → opening cutscene → HUD.** Menus need
  A held ~1 s (short presses were missed); the opening dialogs advance with repeated/long presses.
- **NVN coverage**: plural and deferred binds, viewports/scissors/depth ranges, rasterizer discard and render
  enable, `CopyTextureToTexture` (copy or blit), `DispatchCompute[Indirect]` with storage images, `c[0]` driver
  constants (the buffer descriptor table compute shaders search; a zero table made them write garbage), texture
  writes (`WriteTexels`), CPU-written textures re-uploaded (flushes, pitch-linear content hashes), long-lived command
  lists kept (LRU), uniform/storage padding (Metal has no bounds checks), 256 MB upload buffer.
- **Audio renders**: the in-process AudioRenderer only renders in the manual/CPU mode; switched after opening, frames
  take ~250 us and streamed music is consumed (the opening waited for it).
- **Memory leak that froze the Mac (85 GB)**: images of destroyed textures were never freed. Fixed (freed at the next
  submit); the same run now stays at ~4.5 GB. A **memory guard** ends the process past `NX_MEM_LIMIT_GB` (default 8).
- **Headless test runs** (`NX_HEADLESS=1`): no window, nothing presented, frames still dumped.
- Open: dialog text is invisible (the glyph vertex shader's corner-indexed texture-coordinate load was only partly
  resolved by the offline translator); the dialog box shows a green-cyan gradient; a second recompiler jump-table case
  (table base defined on another control-flow path) crashed after the HUD, fixed in the recompiler.

### 2026-09-29 (early morning): 30 fps, past the title to the file select

![File select](docs/media/wonder/06-file-select.png)

- **Performance: from 2-3 fps to the game's 30 fps.** 8-bit index buffers were converted by MoltenVK on the GPU at
  every draw (~10 ms each, ~550 ms per frame); they are widened on the host now. GPU ~10 ms per frame on the title.
  Found with a process sample (all threads waiting on the GPU fence), a per-pass GPU profiler (`NX_GPU_PROFILE`) and
  elimination (no draw calls: fast; no rasterization: still slow). docs/nvn/DEBUG.md #7.
- **Recompiler bug fixed**: a jump table bounded by an unrelated `cmp` lost a case (crash after the title, "call into
  the middle of a function"). Regenerating now rewrites only the files that change: rebuild in 25 s. DEBUG.md #8.
- **A works: the file select opens** (save card, new-file card, cursor). Text rendering is still broken (green
  specks where text should be). Scripted input for unattended runs: `NX_INPUT_SCRIPT="25:A:0.3;30:A:0.3"`.
- Metal-incompatible texture views (a cube view of a 2D texture) fall back to the dummy instead of aborting.

### 2026-09-28 (night, latest): the title screen, correct colours

![Super Mario Bros. Wonder title screen rendered natively through NVN → Vulkan on an M1](docs/media/wonder/05-title-screen.png)

- **The title screen renders correctly on the Mac**: recompiled CPU code + NVN replayed on Vulkan/MoltenVK, colours
  exact (a texel of the key art: 60,196,130 in the texture, 60,195,128 on screen after the post chain).
- Last fix: texture views. The bloom chain renders each mip of one texture while sampling the previous one; NVN picks
  the level with the `NVNtextureView` of `SetRenderTargets` and of the texture pool. Without it everything went to
  level 0, uninitialised levels were sampled, R and B became NaN and the screen green. Now attachments are per
  level/layer, rendered images stay `GENERAL`, 3D images are 2D-array compatible (the LUT is rendered by slice).
- Still slow (~2-3 fps on the title): every submit waits for the GPU, descriptor sets and uploads are redone per draw.

### 2026-09-28 (night, later): the title screen through Vulkan

![Title screen, tiling and swizzles fixed, colours still wrong](docs/media/wonder/04-title-swizzle-nan-green.png)

- **The title art renders natively** (M1, MoltenVK): the Super Mario Bros. Wonder key art, logo and checkered
  background come out of the game's own NVN command lists, replayed as Vulkan draws with the translated shaders. The
  remaining defect is colour: R and B are NaN from the bloom chain (render-target views not implemented yet).
- Six bugs found on the way, each with its screenshot and how it was found: [docs/nvn/DEBUG.md](docs/nvn/DEBUG.md).
  In short: command-memory usage must survive `EndRecording` (the engine dropped every scene list), the front face is
  inverted under the upper-left origin (the composite was culled: magenta), packaged textures use the survey's
  block-height fit (the builder's layout is always 1), channel-mask outputs are 4-byte words, texture swizzles
  (builder and pool view) are applied.
- Recorded vs replayed draws now match (6,368 recorded, 6,492 replayed including re-submitted lists).
- `ReportCounter` writes {counter, timestamp} at replay; timestamps are host nanoseconds (the conversions are
  identities).
- New debug switches (texture dumps at any point, raw texels, per-target draw traces with state, texture traces):
  table in docs/nvn/DEBUG.md.
- Slow: ~3-4 fps once the title scene runs (synchronous replay, every submit waits for the GPU). N7.

### 2026-09-28 (late night): Vulkan on the Mac, shaders translated

- **Shader translation settled (decision 2):** Ryujinx's MIT translator run offline (tools/maxwell/spirv_harness):
  all 12,717 distinct Maxwell blocks (47,797 stages) become SPIR-V that passes spirv-val and compiles to Metal on
  the M1. docs/nvn/TRANSLATOR.md has NVN's constant-bank layout (c[0] driver, c[1] constants, c[2] texture
  handles, c[3+n] uniform blocks) and the descriptor-set scheme.
- **First NVN frame path on Vulkan** (17c4c9c): MoltenVK device (portability), Metal surface, swapchain 2560x1440,
  a VkImage per NVN texture, recorded command lists replayed at submit, window textures blitted and presented.
  `NX_DUMP_FRAMES=N` writes every Nth presented texture to private/wonder/frames/ (the window can be behind).
- **Formats and enums from the driver:** the format table (0xa1 entries: color families, depth/stencil, sRGB, BCn,
  ASTC 4x4..12x12, BGR orders) mapped to VkFormat; NVN enums follow OpenGL's numbering from 1 (checked on Wonder's
  decoded states: blend 5/6, depth LEQUAL 4, sampler trilinear 5/1, wrap 1 repeat / 7 clamp to edge).
- **Draw recording** (31fc899, 53b66ef): programs, buffers, textures, vertex layouts, blend/color/mask/depth/polygon
  states decoded through the driver's getters while recording; samplers decoded the same way.
- In progress, in parallel: block-linear deswizzle (validated on the game's BNTX textures) and the runtime shader
  cache (hash -> SPIR-V, reflection, the translator's support buffer).

### 2026-09-28 (night): the frame loop runs (7c9aa0c)

Wonder boots on the Mac through every system and presents frames (`nvnQueuePresentTexture` #1..#32+, nothing on
screen yet: the Vulkan side is next). What it took, in boot order:
- **nn::init::Start** has an extra rtld callback in SDK 16; **VAMM** (nn::os virtual address memory) needs
  `svcGetInfo(SystemResourceSizeTotal)` = main.npdm's 16 MiB; `nn::vi::GetDisplayVsyncEvent` (60 Hz event).
- **NVN device info**: the 99 `nvnDeviceGetInteger` values read out of the SDK's own driver (jump tables at
  sdk+451090 / sdk+440c80); the game requires API 55.15.
- **Builders and state objects run in the SDK's own NVN driver** (recompiled), so their struct layout is exact;
  our objects read them back through its getters. Its texture storage size needs the GPU driver, so block-linear
  sizes are ours, from the driver's format table (sdk+d34198: block size and bytes per block per NVNformat).
- **nn::hid** SDK 16 calls, **nn::pl** shared fonts (the firmware's fonts need master_key_16, not in our keys: any
  TrueType font works because the game accepts already-decoded BFTTF data; the Mac's Arial Unicode for now).
- **Programs**: `NVNshaderDataExt` = {code GPU address, control section pointer, 0} (0x18 bytes).
- **Audio without writing a renderer**: the sdk carries the AudioRenderer server; `CreateAudioRendererManagerByHipc`
  is redirected to `...ByDfc` (in process), the renderer maps its work buffer with svcMapTransferMemory (added),
  a guest thread of ours calls `AudioRendererImpl::ExecuteAudioRendererRendering` every 5 ms (the sdk's DSP command
  processor mixes on the CPU) and signals the game's renderer event (its handle was 0 in process). Output to the
  Mac's speakers: next.
- **TLS**: main has 16 bytes of `.tdata` ({-1, 0}) that must start every thread's static TLS (an allocator's
  owner id); found through the module layout DT_INIT passes to `__nnmusl_init_dso` (slots 13/14).
- **The frame deadlock**: `svcSetThreadCoreMask` was ignored, so every worker reported core 0 and the per-core job
  queues of cores 1 and 2 were never drained. Found with `NX_TRACE_ADDR` (logs waits/signals on one address).
- Then (4041ae5): `acc:u0` ListQualifiedUsers (the null pointer was an empty user list); **svcExitThread kept the
  kernel lock** (a lock_guard across pthread_exit, which runs no destructors on macOS: everything froze at frame
  ~50); **16 KiB host pages** (decommitting one 4 KiB guest page protected its neighbours: SIGBUS).
- **Result: 90 s, 2,048+ frames presented, no crash.** The CPU side of the game runs: boot, save mount, audio frames,
  the whole frame loop with clears, depth/stencil, conditional rendering, storage buffers. Next: pixels (N3-N6).

### 2026-09-28 (evening): runtime on macOS, TIPC, NVN tracer

- **Recompiler: 0 unhandled instructions** in both modules after the last additions (LSE atomics, `rshrn`,
  `sadalp`, `ld3r`, `xpaclri`, fused `frsqrts`/`frecps`). Committed (b50bfe8). The SDK 16 handlers moved to
  `recomp/emit_ext.py`, structured loads/stores to `recomp/emit_vldst.py` (output byte-identical).
- **`runtime/src/platform_mac.cpp`**: mmap reservations (no MAP_FIXED_NOREPLACE on Darwin: hint + check), pthreads,
  SDL3 window with a Metal view (Cocoa needs the main thread: the game runs on its own 64 MiB-stack thread, window
  work is queued to the main loop), `VK_EXT_metal_surface` from the CAMetalLayer, SDL3 gamepad + keyboard sampled
  on the main thread, SDL3 audio stream fed from the submitted buffers, signal crash handler.
- **CMake macOS branch** (`-DNX_GAME=wonder`, SDL3 + Homebrew Vulkan headers/loader). The whole shared runtime
  compiled on macOS with no source changes. `sdk` (140 MB of C++) builds in 69 s with `-j6`.
- **`games/wonder/game.cpp`**; module images via `tools/nso2bin.py`; `private/wonder/extract/romfs` links to the
  RomFS.
- **TIPC** in `ipc.cpp`: SDK 12+ talks to `sm:` with TIPC (command = message type - 16, no SFCI/SFCO, result
  first in the response). CMIF unchanged.
- **Boot**: DT_INIT's init-status load is found by following `adrp` targets (SDK 16 interleaves another `adrp`;
  the old fixed-offset decode would have read garbage).
- **N2 started**: `runtime/src/nvn/nvn_trace.cpp` HLEs `nvnBootstrapLoader` and `nvnDeviceGetProcAddress`; every
  proc we do not implement gets a tracing thunk (arguments, caller) writing `private/wonder/nvn_trace.txt`.
  `hle_address_or()` makes thunks at run time (the thunk table no longer reallocates while other threads call).
- **Shaders (N5 recon, done, ac772e3):** `tools/maxwell/containers.py` parses SARC / FSHA / BNSH / VFXB / agl BAHS.
  Wonder has **47,797 stages** (23,752 vertex, 23,752 pixel, 293 compute, **no geometry or tessellation**, good
  news for MoltenVK) and **12,717 distinct Maxwell code blocks (55 MB)**. The NVN control section layout (program
  offset/size, constants, sampler slots, compute work-group size) is in `docs/nvn/SHADERS.md`. Ui2d layouts even
  ship 66 `.glsl` sources, useful as a reference.
- **Next shader step (in progress, parallel):** evaluating Ryujinx's MIT shader translator offline on those blocks
  (a .NET harness, SPIR-V validated with `spirv-val`), to decide plan decision 2.
- **First boot attempt:** `cannot reserve modules at 0x40_0000_0000`. macOS on Apple silicon refuses fixed
  mappings in `0x10_0000_0000..0x6F_FFFF_FFFF`, and the dyld shared cache (system frameworks, pulled in by SDL3)
  sits at `0x1_8000_0000..~0x3_0000_0000`. The macOS guest layout now starts at `0x3_0000_0000` (still inside
  Horizon's 39 bits); `NX_BASE` is per host and generated code uses `NX_BASE + offset`, so one generated tree
  serves every platform (docs/RUNTIME.md).
- **nn::fs:** 5 SDK 16 functions added (MountRom without cache, ReadFile with ReadOption, SetFileSize,
  IsSaveDataExisting, DeleteDirectoryRecursively).
- **Audio finding:** the SDK 16 `sdk` carries the AudioRenderer *server* too, reachable in process through
  `CreateAudioRendererManagerByDfc`: phase NA may mostly come for free.

### 2026-09-28

**Extraction (Mac).** nstool v1.9.2 (macOS arm64 release) with our own `prod.keys`:
`nstool -x /secure` on the XCI, then `-x /0` (ExeFS) and `-x /1` (RomFS) on the program NCA. The cartridge has no
update partition: this is 1.0.0. Output in `private/wonder/{exefs,romfs}` (3.5 GB). Steps in WONDER.md.

**Probe.** NVN (`NintendoSDK_NVN-16_2_0`), SDK 16.2.0 (Hades: 9.3). Modules: `rtld`, `main` (53.6 MB), `sdk`
(13.3 MB), no `subsdk`. Engine: sead + NintendoWare G3d2/Ui2d/Vfx2/Font, Pia (online). 1,107 imports in `main`,
791 of them not in Hades. The loader table asks for all **536** NVN procs by name (the whole C API; what is used
comes out of the N2 trace).

**New finding: AudioRenderer.** Hades only used AudioOut (FMOD mixes inside the game). Wonder imports 197
`nn::audio` renderer functions: voices, ADPCM/Opus, reverb, delay, aux, sub/final mixes. The service side
(`audren:u`, the audio DSP) must be implemented. Added as phase NA.

**MoltenVK on the M1** (1.4.2 via Homebrew, Vulkan 1.4):
has `external_memory_host` (pools imported, no shadow copies), extended dynamic state 1-3, dynamic rendering,
timeline semaphores, descriptor indexing, sync2, robustness2, ASTC and BC natively. Missing: geometry shaders,
transform feedback, logicOp, `vertex_input_dynamic_state`, float64 in shaders.

**Shaders in the RomFS** (all plain zstd, no dictionary): `Shader/SecredUber...bfsha` (42 MB, G3d2 uber shader),
`Shader/Secred...sarc` (agl shaders), Vfx2 effects (`Effect/static...esetb.byml`, 102 MB, plus 33 per-effect),
layouts (`Layout/*.blarc`).

**Recompiler (issue #8).**
- `main`: 209,629 functions, 16.3M instructions (10x Hades), 1,742 jump tables.
- `sdk`: 40,395 functions, 1.5M instructions.
- Crashes fixed: a register Capstone cannot name (literal `ldr` read the address as a register), a scalar by-element
  `fmul` without an arrangement.
- Silent miscompile fixed: vector `clz`/`cls` were emitted as general-purpose register ops (caught by difftest).
- New instructions: AdvSIMD scalar forms (one lane), `fcvtzs/fcvtzu` to a SIMD register, `fmaxp/fminp/fmaxnmp/
  fminnmp`, `smaxp/sminp/umaxp/uminp`, `sminv/uminv`, `facge/facgt`, `sqrdmulh`, `shsub/uhsub`, `srshl`, `rshrn`,
  `uabdl/sabdl`, `umlsl/smlsl`, `sadalp/uadalp`, `ld2r/ld3r/ld4r`, `scvtf/ucvtf` vector fixed point, `xpaclri`,
  exclusive pairs `ldxp/ldaxp/stxp/stlxp`, **all LSE atomics** (`cas*`, `casp*`, `ld{add,clr,eor,set,smax,smin,umax,
  umin}*`, `st*`, `swp*`), crypto extension (AES, SHA-1, SHA-256, PMULL.1Q; host instructions on arm64, a software
  version elsewhere, both tested).
- `frsqrts/frecps` are now fused like hardware (one rounding, no intermediate overflow).
- `tests/difftest.py` runs on macOS/Linux now (clang++, mmap + signals, Unicorn CPU model MAX for ARMv8.1+).
  Results: `main` 2,414 tests / 28,968 runs, `sdk` 3,472 tests / 55,552 runs, **0 mismatches**.

## Next

1. Past the opening into World 1-1 "Welcome to the Flower Kingdom!" (Pipe-Rock Plateau), then play it.
2. Text: the glyph vertex shader's indexed texture-coordinate load (translator), the dialog box colours.
3. Performance beyond 30 fps headroom: stop waiting for the GPU at every submit, cache descriptor sets.
4. Audio: longer runs for a rare limiter crash; pace rendering from the device clock; `docs/nvn/API.md`.
5. Split `nvn_shaders.cpp` (815 lines, over the 700-line cap).

Branch: `feat/nvn-n1-wonder-cpu`, stacked on `feat/nvn-n0-core-split`.
