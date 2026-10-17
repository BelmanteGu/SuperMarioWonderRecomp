# Intel iGPU: correct picture and a steady 60 fps (2026-10-17)

Follows `intel-igpu-1710.md`. Machine: i5-12450HX (4P+4E, 12 threads), Intel UHD Graphics (ADL-S GT0.5, Mesa 26.0.8 ANV), 14 GB
shared RAM, Linux 7.0, power-profiles-daemon. Everything ran headless (`NX_HEADLESS=1`, `NX_GPU=integrated`, nothing presented),
through `private/estudos/bench.sh` or its copy with a power-profile switch: the scripted Flower Kingdom intro, **median fps of the last
14 windows of 60 frames, and how many of those 14 are >= 58 fps**. "Measured" is a log line or a file; "inferred" is marked.

## 1. Result

| Item | Result |
|---|---|
| Picture on the Intel | **Fixed.** Characters and near terrain were black (0,0,0): NaN lighting. Root cause: **vertex shader outputs that a shader leaves unwritten on some paths hold garbage on Mesa ANV** (Maxwell reads zero). A zero initializer on every vertex `Location` output removes it (section 2). Frames: `runs/bench-z1/frames/05400.png`, `runs/bench-perf3/frames/05400.png` (Toad, flowers, ground in colour, 1280x720 in the 1080p dump) |
| 60 fps | **Reached: 60.0 fps median, 14/14 windows >= 58**, uncapped (no `NX_FPS_CAP`, no interval clamp: the game's own 60 fps mode, 0 of 600 presents at interval 2), handheld 720p, with the desktop's **performance power profile** (held by the runtime, section 3) |
| What limits it | **The CPU's power state, not the GPU and not the code.** Under power-saver the same replay of a frame takes 12 ms, under performance 4.9 ms |
| Not reached | Docked 1080p: 38.5-38.9 fps (GPU bound, ~26 ms per frame); capacity measured at 38.7 unpaced. 60 at 1080p would need a render-resolution scale this game does not offer |

Reading the table below needs one fact: **the machine's power profile changed during the session** (power-saver for most of the session,
`balanced` from late morning on; each later run directory has a `profile_before.txt`). The baselines given with this task (31.2 docked,
34.7 handheld) were power-saver numbers. Runs below say which profile they ran under; `powerprofilesctl launch -p <profile>` holds one
for a single run without touching the user's setting, and one set of runs temporarily set the user profile to power-saver (restored
afterwards, the run lock held so nothing else ran).

## 2. The black silhouettes: root cause and fix

Symptom (from the earlier study): near terrain, Mario, Peach and Toad exactly (0,0,0); `NX_SKIP_FS` showed the black areas came from
the lit "uber" fragment shaders; their 192x108 RGBA16F low-res lighting buffer held NaN (25920 of 82944 values); the NaN was born
in three character-lighting fragment shaders. Measured here, in this order (`tools/intel/nansan.py`, count of NaN in the 192x108
snapshot, `NX_DUMP_DRAW_TEXTURES=305f0dc79835b13c@300000 NX_SNAP_RAW=1`, baseline **2046 NaN** in the draw watched):

| Test | NaN left | Meaning |
|---|---|---|
| NaN -> 0 after every `Fma` of `465e881c` alone / `f708ec2c` alone | 2046 / 2046 | not these |
| the same for `9a959f14e6a1dc0e` alone | **0** | the source is this one |
| `Fma` index range 0-24 / 0-12 / 12-24 / 12-18 of that shader | 0 / 2046 / 0 / 0 | in Fma 12-17: the tangent-frame normalization |
| NaN -> 0 after every image sample of that shader | 2046 | not texels |
| NaN -> 0 on every `Input` float load of that shader | **0** | the NaN arrives in the varyings |
| the same on locations 7, 5, 4 only | 2046 | not these |
| NaN -> 0 on every float `Output` store of **all vertex shaders** | 2046 | the vertex shaders do not store a NaN... |
| the same with Inf too (`OutStoreInf`) | 2046 | ...nor an Inf |
| every vertex `Output` variable initialized to zero (`zinit.py`, then the built-in patch) | **0** | so the garbage is in outputs that were never stored |

Inferred (the conclusion that fits all rows): the translated vertex shaders store some output components only on some paths. SPIR-V
leaves the rest undefined; a Maxwell register reads back zero there, Mesa ANV leaves whatever the previous vertex computed (NaN
included), and the fragment shader's `x * inverseSqrt(dot(v, v))` normalization turns it into NaN lighting. It also explains why the
RTX never showed it.

Fix (`df2f48b`, `runtime/src/nvn/nvn_spirv_patch.cpp`): `zero_vertex_outputs()` gives every `Location` output `OpVariable` of a vertex
module an `OpConstantNull` initializer. Components the shader does store are unchanged; only never-stored ones read 0 instead of
an undefined value. 119 vertex modules of the run were patched (116 had outputs), all pass `spirv-val`
(dumped through `NX_SPIRV_TOOL`, `runs/bench-dump2`). `NX_ZERO_VS_OUTPUTS=0` turns it off; **default off on Apple** (MoltenVK untried),
on elsewhere. Measured after: 0 NaN in the 192x108 buffer (`runs/bench-z1/nx.log`), colour frames as above. On the RTX 3050 of the
same machine (`NX_GPU=discrete`, handheld, 60.0 fps both ways) the frames with and without the patch look the same (they differ in
which character the script picked, so no pixel diff is meaningful): `runs/bench-rtx-zero`, `runs/bench-rtx-nozero`.

Tools kept in `tools/intel/`: `nansan.py` gained `Input`, `OutStore`, `OutStoreInf` (and `NANSTAGE=Vertex`, `NANVARS=%44,...`),
`zinit.py` is the zero initializer as an `NX_SPIRV_TOOL` script.

## 3. What limits the frame rate

### 3.1 Measured, in the order it was found

* **The GPU is not the limit at 720p.** Dropping the five heaviest passes (`NX_SKIP_FS=77fc39b1,505b565c,de5d38a4,255093ce,352e8b76`,
  ~8 ms of the 11 ms) left handheld at 52.8 fps against 54 (`bench-skipheavy`). The GPU clock sat at 1200-1300 MHz in those runs
  (`gt_cur_freq_mhz` sampled every 0.25 s).
* **The replay thread was waiting for the GPU's first part** (4 ms per frame: a submit is split after its last compute dispatch and the
  replay waits for that part's fence to copy the results back). Making that write-back lazy removed the wait (0.00 ms) and the replay
  thread idled 4 ms per frame, **and the frame rate did not change** (54.5 vs 54.5): the timeline (`NX_TRACE_TIMELINE`) showed the cycle
  is set by the game's own threads (main thread 13.6 ms of work, then 3.5 ms to the next submit; 17.1 ms in all).
* **The main thread is ~40% busy and blocked 5-7 ms per frame** (`NX_SVC_PROFILE`, `NX_CPU_LOG`) in one condition-variable wait (the job system's
  joins, 9 waits a frame); the presenting thread sleeps the whole frame. No run-queue wait anywhere (`NX_CPU_LOG`: runq wait 0%).
  The thread profile is flat: ~25% NVN emulation, the rest recompiled game code.
* **The CPU clock.** Same binary, same flags, only the profile differs: replay of a gameplay frame **12.0-12.3 ms under power-saver, 8.4 under
  balanced, 4.8-5.1 under performance** (`replay` of the `per frame over 60` lines). A busy loop of 8 ms every 17 ms ran at a third of
  the speed of a continuous one while the machine was in power-saver (energy preference `power`, platform profile `low-power`).
  Inferred: HWP with that preference and the package power limit keep a core that wakes every few milliseconds at a low clock.
  (`scaling_cur_freq` is useless here: it read 0.9-1.1 GHz while a spin loop ran at full speed.)

### 3.2 The lever: ask for the performance profile

`plat::request_performance_profile()` (`runtime/src/frame_stats.cpp`, commit `6f885a4`) starts
`powerprofilesctl launch -p performance -- tail --pid=<game> -f /dev/null`, the way GameMode and Steam hold it: the profile is held until the
process ends, the daemon puts the user's profile back, nothing happens without `powerprofilesctl`. Automatic for an integrated GPU
(`gpu_is_integrated()`), `NX_POWER_PROFILE=1` for any GPU, `=0` never. Linux desktop only.

## 4. Before / after (bench.sh, handheld = `NX_HANDHELD=1`, uncapped, game's own governor)

"dev" is the main checkout's `private/wonder/build/wonder` (the binary of the baselines); "new" is this branch. **The picture of every dev row is
the black one.** Under "sys" the system profile was `balanced`; "user power-saver" is the user profile set to power-saver for those runs.

| Profile | dev docked | dev handheld | new handheld, no request | new handheld, automatic request |
|---|---|---|---|---|
| power-saver (user profile) | - | 35.9, 0/14 | 45.0, 0/14 | **60.0, 14/14** (`bench-new-hh-psuser-req`) |
| power-saver (a `launch -p power-saver` hold) | 31.1, 0/14 | 38.2, 0/14 | 40.5, 0/14 | 40.6, 0/14 (a power-saver *hold* outranks ours; not a real case) |
| `balanced` (sys) | 40.5, 0/14 | 59.0, 12/14 | - | **60.0, 14/14** (`bench-new-hh-sys`) |
| performance (hold) | - | 60.0, 14/14 | - | 60.0, 14/14 |

Replay per frame behind those rows: dev 12.2 ms (power-saver), 8.4 (balanced), 4.8 (performance); new 12.0 (no request), 5.1 (request), 4.9
(sys). Docked 1080p, new: 38.9 (sys), capacity unpaced (`NX_NO_FRAME_LIMIT=1`) 38.7: the GPU, not the CPU. Handheld unpaced capacity 70.6 fps.
The 10 fps from 35.9 to 45.0 under the same power-saver profile is the main thread's lock work (`real_proc` per-thread cache: 7.0 -> 5.1 ms
blocked per frame) plus the colour pictures; it does not reach 60 by itself.

`lightest` (handheld + FSR 1 to a 1920x1080 offscreen target every frame, `NX_PRESENT_TEST=1920x1080 NX_PRESENT_TEST_ALL=1`): 60.0, 14/14
(`bench-lightest-test`; the pass and its wait are ~0.26 ms a frame on this log line). The plain blit of the preset costs less.

## 5. Tried and not kept (all in the power-saver period, handheld, median fps; baseline of that period 54-54.5)

| Experiment | Result | Why dropped |
|---|---|---|
| `NX_PIPELINED=3` / `4` | 54.3 / 54.1 | the wait is the game's chain, not depth |
| Lazy write-back of compute results (no wait for the first part) | 54.5, waits 0.00 ms | no fps gain once the CPU clock is the limit |
| Recording thread (all `vkCmd*` and submits on a second thread; closures, 4.5 ms busy) | 52.5-53 | replay thread was not the limit; adds a thread |
| Replay / main / presenting thread pinned to P cores | 54.0 (`0:2:4`), 50.9 (workers kept off them), 51.3, 50.8 | no gain or worse |
| Spinners on the SMT siblings | 53.6 | siblings share the core's resources |
| Spin-then-sleep in kernel waits (`NX_SPIN_WAIT=100 / 500 us`), under `balanced` | 59.5 / 56.1 against 60.0 | no gain |
| State-object decode cache (blend/color/depth/polygon) | 52.2 | within noise, risk of aliasing |
| Skip the five heaviest GPU passes | 52.8 | GPU not the limit |

The patch of the lazy write-back and the recording thread is not in the branch; the findings above are what it showed.

## 6. Switches

| Switch | Effect |
|---|---|
| `NX_ZERO_VS_OUTPUTS=0` | no zero initializer on vertex outputs (default on; off on Apple, `=1` there to try) |
| `NX_POWER_PROFILE=0 / 1` | never / always ask for the performance profile (default: only with an integrated GPU) |
| `tools/wonder_linux.sh intel` | `NX_GPU=integrated NX_HANDHELD=1 NX_PIPELINED=2 NX_POWER_PROFILE=1`, no fps cap (the old preset held 30) |
| `tools/wonder_linux.sh lightest` | `intel` plus `NX_FSR=1 NX_FSR_SHARP=1.0` |
| `NX_SVC_PROFILE=1` | wall time of the kernel calls over 30 us of the main and presenting threads, per call and caller, every 600 frames |
| `NX_CPU_LOG=1` | now also the run-queue wait of each thread |

## 7. Quality reduced

Only what the `intel` preset already did: handheld mode, so the game renders 1280x720 instead of 1920x1080 (the window shows it
scaled; `lightest` with FSR 1). The game's own dynamic resolution still applies. Nothing else was lowered; no frames are skipped, the
game runs at its own 60 fps step (interval 1 at 60 presents a second in the 14 windows).

## 8. Risks

* **NVIDIA / AMD / Windows:** `zero_vertex_outputs` is on there. It only changes outputs that were never stored. The RTX 3050 here shows no
  difference in picture or fps. Not run on Windows or on AMD. The performance-profile request is off for a discrete GPU.
* **Mac:** the patch is off by default (MoltenVK and `OpVariable` initializers on stage-out variables were not tried); the profile code is empty.
* **Android:** the patch is on (core SPIR-V, Adreno/Mali untried); the profile code is empty.
* **Power:** the request raises power use while the game runs on a laptop, and a user on battery saver gets the performance profile for
  as long as the game lives (`NX_POWER_PROFILE=0` stops it). Changing the profile by hand while the game runs drops our hold (daemon rule).

## 9. Not verified

* **No windowed run**: swapchain present, vsync behaviour and a real FSR pass were not measured; the present-pass cost is the offscreen test above.
* **One scene** (Flower Kingdom intro); course 1-1 was not reached. Heavier scenes may need more than the 70.6 fps of headroom.
* **No 1080p at 60**; docked stays at ~39 fps.
* Power-saver without `powerprofilesctl` (other distributions) stays at ~45 fps: nothing in the runtime can raise a locked-down clock.
* The idle-to-load clock behaviour is inferred from a synthetic loop and the replay times; no `turbostat` (no root).
* The profile changed under the session; the early experiments of section 5 were all taken before it did, which the replay times (12 ms) show.
* Disk and desktop load varied; two other agents shared the lock.
