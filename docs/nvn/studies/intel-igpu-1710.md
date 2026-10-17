# Intel iGPU study (2026-10-17)

**Superseded in two points by `intel-60fps-1710.md`:** the black silhouettes are fixed (vertex outputs left unwritten, zero-initialized now), and the
fps ceiling seen here (30/38-41, replay 12 ms) was the CPU running at its power-saver clock, not the code: with the performance power profile the
same scene holds 60 fps. The measurements below are kept as they were taken.

Machine: i5-12450HX (4P+4E), Intel UHD Graphics (ADL-S GT0.5, Mesa 26.0.8 ANV), shared RAM. Everything below ran headless
(`NX_HEADLESS=1`, `NX_GPU=integrated`), scene = Flower Kingdom intro after the scripted input in
`private/.../study/script_a.txt` (A every 3 s). "Measured" = log lines `nvn/vk: per frame over 60`, median of the last 12 windows;
"inferred" is marked. The desktop was in use and other studies shared the lock: expect +-2 fps noise.

**Important: every fps number here was taken with the scene rendered wrongly** (near terrain and characters black, see
"Black silhouettes"). The black pixels still cost shading time, but the numbers must be repeated once the picture is right.

## Baseline (unmodified dev, same scene)

| Config | fps | replay ms (CPU) | fence wait ms (GPU) | upload MB/frame |
|---|---|---|---|---|
| docked 1080p, synchronous (default) | 26.2 | 11.8 | 23.2 | 10.3 |
| docked, NX_ASTC_DECODE=1 (CPU ASTC) | 26.1 | 11.6 | 23.9 | 10.2 |
| docked, NX_RENDER_RES=1280x720 | 26.6 | 10.9 | 24.0 | 10.1 |
| handheld 720p (NX_HANDHELD=1) | 37.8 | 11.2 | 12.5 | 10.2 |
| handheld + NX_FSR=1 (headless: FSR not drawn) | 38.0 | 11.0 | 12.5 | 10.2 |
| docked, NX_PIPELINED=1 / 2 | 31.4 / 31.3 | 12.0 | 7.4 | 10.3 |
| handheld, NX_PIPELINED=2 | 40.1 | 12.0 | 3.2 | 10.3 |
| handheld, pipelined 2 + NX_REPLAY_THREAD=1 | 41.4 | 12.1 | 3.1 | 10.2 |
| handheld, pipelined 2 + NX_SUBMIT_CHUNK_DRAWS=150 | 31.9 | 12.7 | 1.2 | 10.4 |
| handheld, pipelined 2 + NX_MAX_INTERVAL=1 | 54.4 | 11.8 | 3.3 | 4.6 |
| handheld, pipelined 2 + NX_FPS_CAP=30 | 30.0 flat | 11.0 | 4.1 | 4.9 |
| title / menus, handheld | 60 | ~2.7 | - | 3.6 |

Menus and title run 48-60 fps; the gameplay scene is the heavy one (about 220 draws and 10 passes per frame).

### Where the time goes (measured)

* **GPU (NX_GPU_PROFILE=1):** docked 22.2 ms/frame, handheld 11.2 ms: it scales with pixels (2.25x). Top passes (docked, ms):
  4.2 `d2610231` (uber, 1080p), 3.6 `505b565c`, 2.2 outside passes (clears, copies), 1.9 `de5d38a4`, 1.4 `77fc39b1`, 1.3 `255093ce`,
  1.3 `352e8b76`. Half-resolution passes total ~2 ms. No MSAA is used. Resolution is the only big lever.
* `NX_RENDER_RES` does nothing for the scene size: the game takes its render size from the operation mode (docked 1080p / handheld
  720p, log `window crop`), not from `GetDefaultDisplayResolution`. The game has its own dynamic resolution (passes of 1856x1044,
  1664x936 appear), driven by its governor, not by us.
* **CPU critical path (timeline, `NX_TRACE_TIMELINE`):** non-RT mode: submit-call -> replay 17.5 ms (docked and handheld alike)
  -> present; the game's main thread frame is then ~19 ms, which is above the game's own 15.83 ms rule, so it locks present
  interval 2 (30 fps) and flips back to 1 when frames are fast: that alternation is the 38-41 fps "average". With
  `NX_REPLAY_THREAD=1` the replay thread runs 15-17 ms per frame, right at the 16.7 ms limit. Holding 60 needs the replay under
  ~13 ms.
* **Replay thread profile (NX_SAMPLE, gameplay, handheld, 14k samples):** libvulkan_intel 42% inclusive (draw_call 45% of that,
  bind_sets 21%, frame_descriptor_set 14%, begin_rendering 9%, vkQueueSubmit 8%), libc 25% (submit ioctl 29%, dedup_upload
  memcpy/memset 25%, draw_call 15%), own code ~33% (dedup_upload 13%, build_sets 17%, bind_sets 14%). It is flat: no single fix
  saves more than ~1 ms.
* ASTC: native on ANV (`ASTC native`, emulated by the driver). The CPU decode (`NX_ASTC_DECODE=1`) decodes 76 textures in
  1060 ms at a scene load and does not change steady-state fps. Not worth it on this GPU.
* `NX_SUBMIT_CHUNK_DRAWS=150` is worse on Intel (31.9 vs 40). Affinity: `NX_AFFINITY` is Android-only (frame_stats.cpp); the CPU
  here is hybrid (4P+4E) so pinning the replay thread to P cores is a next step (not done).
* Pipeline cache: `pipelines_8086_468b.bin` persists (loaded 10 MB in the later runs), pipeline creation per 60 frames was 0.2-5 ms
  except one 31 ms window at the scene load. No stutter seen from it.

## What was implemented (commit 979d12f, 50a885a)

| Change | Switch | Result |
|---|---|---|
| Pipelined fences on by default for an integrated GPU (`gpu_is_integrated()`) | `NX_PIPELINED=0` turns off | docked 26.2 -> 31.4 fps, handheld 37.8 -> 40.1 (mixed 30/60 present intervals) |
| No 4 KB / 64 KB zero padding of uniform blocks and read-only storage where `robustBufferAccess` exists (`pad_min`, nvn_sets.cpp). The padding exists for Metal; on Vulkan out-of-range reads return 0. `dedup_upload` was memset-ing ~14 GB and copying 6.4 GB per 70 s | `NX_PAD_BUFFERS=1` restores | upload 10.3 -> 4.8 MB/frame; **no fps change** (replay 11.8 ms before and after: the memset was 0.6% of the replay thread) |
| `tools/wonder_linux.sh intel` / `lightest` presets | | see below |

Experimental, all **off** by default: `NX_INVARIANT=1` (Invariant on vertex Position), `NX_SUBGROUP32=1` (compute pipelines pin
subgroup size 32; Intel runs 8..32), `NX_FLOAT_CONTROLS=1` (SignedZeroInfNanPreserve on graphics shaders). None changed the black
silhouettes.

Diagnostics added: `NX_SAMPLE=<hz>` (+ `NX_SAMPLE_STACKS=<file>`) SIGPROF sampler for machines with `perf_event_paranoid=4`;
`NX_TRACE_TIMELINE=<present>`; `NX_TRACE_C8=1`; `NX_DUMP_DRAW_TEXTURES=<fs>@<draw>` (snapshots every texture a draw samples, all layers
of cubes, with `NX_SNAP_RAW=1` it counts NaN/Inf of RGBA16F ones); `NX_SPIRV_OPT=<flags>` / `NX_SPIRV_TOOL=<script>` (every
module through `spirv-opt` or a script with `in out hash`); `NX_EQUAL_ALWAYS=1`; `NX_NO_STENCIL=1`; GPU profile now prints 40 labels.
Scripts in `tools/intel/` (NaN visualizer, NaN sanitizers, clamp tests).

## Presets (`tools/wonder_linux.sh`)

* `intel`: `NX_GPU=integrated NX_HANDHELD=1 NX_PIPELINED=2 NX_FPS_CAP=30`. Measured: **30.0 fps flat** (handheld, cap 30, scene with
  black silhouettes). Without the cap the game alternates 30/60 (38-41 fps average). A clean 60 is not reachable yet.
* `lightest`: `intel` + `NX_FSR=1 NX_FSR_SHARP=1.0`. The FSR present pass is not drawn headless, so its cost is **not measured**.
* Docked on this iGPU: 31.4 fps pipelined; `NX_FPS_CAP=30` docked was not run (GPU 22 ms + CPU: marginal).

## Black silhouettes on Intel (the top correctness issue)

Symptom: near-layer terrain, Mario/Peach/Toad and the foreground ground render exactly (0,0,0). Same on the unmodified build.
Not the known #44/#45 glitches: `NX_EQUAL_SLACK` (0, 4, 64, 20000), `NX_KEEP_SAMPLED_DEPTH`, `NX_NO_ALPHA_TO_COVERAGE`,
`NX_R11G11B10_EMULATE`, `NX_NO_IMAGE_SHARE`, `NX_NO_PASS_BARRIER`, `NX_NO_DEDUP`, CPU ASTC decode, `INTEL_DEBUG=nohiz|noaux`,
stencil off, `spirv-opt -O`, Invariant position, subgroup 32, float controls, NMin/NMax/NClamp: **no change** (frames
`runs/intel-slack*`, `intel-bug-*`, `intel-dbg-*`, `intel-opt`, `intel-nmm`).

Root cause found to the level of a texture (measured):

1. The black pixels come from the lit "uber" fragment shaders (skipping groups with `NX_SKIP_FS` turns exactly those areas
   into the flat lavender base: `runs/intel-skipE`, `intel-skipI`). The material textures they sample are fine (snapshots with
   `NX_DUMP_DRAW_TEXTURES`, `runs/dt_sheet.png`); the probe cubes look plausible.
2. **The lit shaders sample NaN texels.** `tools/intel/nansan.py` with `NANSAN=Sample` (NaN texel -> 0 after every image sample)
   brings Peach, the bushes and the flower back in full colour (`runs/intel-ns-Sample/frames/03000.png`); `tools/intel/nanvis.py`
   (NaN in fragment output location 0 xyz -> 50.0) shows the ground white, i.e. NaN: `runs/nv_sheet.png`, `runs/intel-nanvis`.
   NaN in the output of the near-layer shaders is converted to 0 by the Intel ROP/blend, hence exactly black.
3. The NaN texture is the **192x108 RGBA16F low-res buffer** (`texture3` of the uber draws): 25920 of 82944 values NaN
   (`NX_DUMP_DRAW_TEXTURES=d26102316f876f15@300000 NX_SNAP_RAW=1`). It is a blur chain (fs `305f0dc7`/`977afcd9`/`38edabcd`: 384x216 ->
   192x108 -> 96x54 -> 48x27 -> 64x36) of the 384x216 buffer, which already holds 6831 NaN. All textures upstream of it
   (probe cubes 64x64 and 128x128, the 64x36 sky ambient) have 0 NaN/Inf.
4. The 384x216 buffer is written by the low-res additive pass (viewport 384x216, depth test with a low-res depth copy, fs
   `671fea36 465e881c 9a959f14 f708ec2c 004bbec8 40ece00f 148f1d23 c01bab55`, all "uber"). Sanitizing NaN after every
   `Fma` of those eight shaders gives 0 NaN downstream (`runs/intel-nh1`). Bisect: the three shaders
   **`465e881ccb17cc7d`, `9a959f14e6a1dc0e`, `f708ec2ce5a7ff55`** are the source (`runs/intel-nb-b1`: 0 NaN; the other five and
   `671fea36` alone leave the NaN: `intel-nb-a`, `intel-nb-b2`).
5. Not the source: NaN from `FDiv`, `InverseSqrt`, `Sqrt`, `Log2/Exp2` results (sanitizing each alone leaves the NaN), nor
   clamping rsqrt/rcp results to +-FLT_MAX (`intel-ic`), nor FP16 overflow (clamp of outputs to +-65504, `intel-fp16c`), nor NaN
   texture coordinates (`intel-ncoord`). So a plain `Fma`/`FMul`/`FAdd` chain yields NaN (0*inf or inf-inf) from inputs I did
   not isolate.

My reading (**inferred**): Maxwell (and NVIDIA's compiler on the RTX) tolerate or flush a case that ANV evaluates strictly
(candidates: the Maxwell "multiply by zero is zero" FMZ mode, 0*rsqrt(0) in `normalize` of skinned/zero vectors, or a denormal/NaN
handled by NVIDIA hardware); the three shaders are the character/skinned variants (skin compute `4d9c06a0` is the only compute
shader used). **Not fixed.** Next step (about half a day): in those three modules, find the first NaN Fma by bisecting
`NANRANGE` in `nansan.py` (`./nanbisect.sh`), look at its operands, and emulate Maxwell's rule in the SPIR-V patch pass (the
likely fix is a `select(a == 0 || b == 0, c, fma(a, b, c))` on that instruction, or the same for all `Fma` with an operand that
can be rsqrt(0)).

A runtime-side stop-gap that I did **not** implement: NaN -> 0 after every image sample of the uber family (proved on the
characters; the flat ground stays black until the source is found; cost: one OpIsNan+OpSelect per fetch).

## Risks to check before merging (NVIDIA, Mac, Android)

* `pad_min`: relies on `robustBufferAccess` returning 0 for out-of-range reads. Intel returned the same pictures, but NVIDIA,
  Adreno and Mali must be checked on the HUD, UI text and skinned characters (a shader reading `c3[10]` of a 0x90-byte block was
  the original reason, DEBUG.md #21). Mac is unchanged (`__APPLE__` keeps the padding).
* Pipelined default on integrated GPUs: the README notes exploded meshes with pipelined fences on Windows/NVIDIA (DEBUG-2 #76);
  the Intel frames at 1500/2500/3000 showed none, but only one scene was checked. Android integrated GPUs (Adreno is "integrated") will
  also take the new default: check `NX_PIPELINED` expectations in the launcher config.
* The experimental switches are off, so nothing else changes.

## Not verified

No windowed run (FSR/present-pass cost, vsync behaviour), no second scene (course 1-1 not reached), no NVIDIA/Mac/Android runs,
no memory-footprint measurement, no thread-affinity test, no render-scale below 720p (the game does not offer one), and every
fps figure is from the NaN-black picture. Disk and desktop load varied between runs.
