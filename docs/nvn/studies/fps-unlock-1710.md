# Wonder above 60 fps, and a light 30 fps, 2026-10-17

Study on branch `study/unlock`. All runs headless (`NX_HEADLESS=1`, nothing presented: they measure frame
production, not display), muted, Intel UHD (ADL-S GT0.5), `NX_HANDHELD=1` (720p), same scripted input
(`NX_INPUT_SCRIPT` A every 3 s, reaches the Flower Kingdom intro at ~50 s). Run folders: `scratchpad/study/runs/unlock-*`.

## 1. Summary

* **Wonder cannot simply be unlocked above 60.** Its logic advances by the present interval it asked for (1 or 2
  sixtieths per frame), not by delta time. Presenting at 120 Hz with interval 1 would run the game at 2x speed
  (inferred, see 3). The only fractional-step data in the binary feeds the pad hold timers.
* **A forced 30 fps works today and is clean** (`NX_FPS_CAP=30`): 30.0 fps in the title and in the course on the Intel,
  right game speed by construction (the game itself asks interval 2), and 18% less CPU than the 38 fps free-running course.
* Measured: the course on the Intel is not CPU-spin bound. No yield storm (6.3 k yields/s, 4.4 k ticks/s in total), nearly zero
  system time per thread: the CPU is real work (render thread replay, job workers). A yield nap does nothing.
* `NX_DISPLAY_HZ=120` (prototype, experimental) makes the host produce 120 fps where the GPU allows: **119.8 fps in the title**,
  65-90 in the intro, 38 in the course (GPU/replay bound as at 60).

## 2. How Wonder paces today (measured + code)

| Piece | What | Where |
|---|---|---|
| Frame loop | `sead::GameFramework::procFrame_` (main+e15ec) computes the interval every frame: `int(t*60 + 0.05)+1` clamped 1..2, `t` = main thread time (fw[0x118], ticks of 19.2 MHz); 2 at >= 15.83 ms, locked after 3 slow frames, released after 15 fast | DEBUG.md #43 |
| What the interval does | e16ac..e16bc: `fw[0x88] = interval`, `fw[0x260] = interval`, and the float `(float)interval` stored at main+0x3543b90 | disassembly |
| Readers of that float | exactly three: main+a5c90, 1cd2a4, 1ceada4, all pad hold-time accumulators (scan of every adrp/ldr/add in text) | static, `scan.py` |
| Our host pacing | `pace_present`: a present returns >= interval x 16.67 ms after the previous (CPU sleep, not display) | nvn_queue.cpp |
| vsync | `hle_vi.cpp` host thread signals the vsync SystemEvent every 16.67 ms (kernel event, auto clear) | hle_vi.cpp |
| Time sources | `GetSystemTick` = monotonic ns x 19.2 MHz; used for the governor's frame time, not for the simulation step | kernel.cpp |

Our own governor (`NX_GOVERNOR`, default on only on Android) clamps the game to interval 1; its code comment records that
below 60 fps at interval 1 "the game would run visibly slow". That is the author's evidence that the simulation is per
present: speed = fps / 60 at interval 1, and interval 2 is what keeps 30 fps at the right speed.

## 3. Is the simulation tied to frames?

Evidence (what is measured and what is inferred):

* Inferred, strong: the interval is stored as the frame step (fw[0x88], fw[0x260], float global); the game's 30 fps mode
  is correct on a Switch only if the step scales; our governor's comment (above).
* Measured: main-thread time at interval 2 is not double that at interval 1 (title: fw[0x118] 7.7 ms at 60, 8.1 ms at the
  forced 30; `NX_FW_PROBE`), so interval 2 is **one calc with step 2, not two substeps**. That makes a fractional step
  (0.5 at 120 fps) conceivable if every consumer of the integer step can be found: **not done**.
* Not conclusive: frame dumps at equal wall times for 30/60/120 (`NX_DUMP_EVERY_S`, runs `unlock-spd-*`, `unlock-idle-*`)
  show the title loop with sparkle noise and no attract mode within 100 s, and the scripted menus/character select
  landed on different characters in the three runs (Peach at 30, Daisy at 60, Luigi at 120 at the same wall time), which is
  consistent with a different input/animation rate but depends on input timing too. I did not get a clean speed number.
  A clean test needs a scripted run with a deterministic distance (Mario running in the course) at 30/60/120.

## 4. Options and cost

| Option | Verdict |
|---|---|
| (a) plain unlock (`NX_DISPLAY_HZ`) | Present rate goes up, **game speed goes up with it** (inferred). Only useful as a measurement tool |
| (a2) fractional step | Make the step 0.5 at 120 Hz. Needs every consumer of fw[0x88]/fw[0x260] found (not the float global, which only feeds pad timers), then a guest-side patch: the generated C++ has no patch hook; indirect calls go through the module function table (`loader.cpp call()`), so a virtual function such as the calc entries (vtable +0xe8/+0xf0/+0x70 called from main+e1890) could be replaced from the host, but a mid-function store (main+e16b4) cannot. Effort: days of reverse engineering, high risk (physics, animations tuned per 1/60) |
| (b) frame generation in the present pass | Works regardless of the game: blend/warp the last two presented frames. No motion vectors: needs optical flow compute (FSR3-FG-class) on a GPU that is already the limit (Intel: 38 fps in course). Costs GPU time on exactly the hardware that has none. Pointless on the Intel, plausible on a discrete GPU. Weeks |
| (c) re-present at a higher rate | no gain |
| (d) stable 60 / forced 30 | already exists (`NX_FPS_CAP=30`, `NX_GOVERNOR=1`); see 5 |

## 5. Low end (measured on the Intel, handheld 720p, same script)

| Run | Course fps | Process CPU (course) | Notes |
|---|---|---|---|
| free (interval 1/2 by the governor) | 37-39 | 1.77 cores | render thread 46%, main 26%, workers 11-26% each |
| `NX_FPS_CAP=30` | **30.0 flat** | 1.45-1.49 cores | render thread 36%, main 21% |
| `NX_FPS_CAP=30 NX_YIELD_NAP=100` (variant tried, removed) | 30.0 | same within 1% | |
| nap after 8 yields | 30.0 | same | |

* Per-second counts in the course: GetSystemTick 4.4 k, yield 6.3 k, SleepThread(ns>0) 152. System time per thread 0-4%.
  The job workers do not spin in syscalls here; any spin is in user space (atomics) and cannot be throttled from the HLE.
  So the "cheaper yields" idea has no measurable gain on Linux (the Hades-style naps stay off for Wonder).
* The CPU saved by 30 fps is what the frame costs; the remaining 1.45 cores are work. A thread (g6b, 14-18% constant, even
  in the title and even muted) is probably the audio renderer: worth a look for phones (not verified).
* The 30 fps lock does not lower the GPU cost per frame (fence wait 12.5-13 ms per frame in the course either way): a
  weak GPU still needs a lower resolution or `NX_PIPELINED` to hold 30 comfortably.

## 6. Switches added

| Switch | Effect |
|---|---|
| `NX_DISPLAY_HZ=<30..360>` | host vsync event and present unit at that rate. **Experimental: game logic runs fast above 60.** Below 60 it is wrong the same way |
| `NX_CPU_LOG=1` | every 300 frames: wall, process CPU, per-thread % (and system part), and per-second tick/yield/sleep counts |
| `NX_FW_PROBE=1` | which words of the framework object change per frame; wall time vs interval sum |
| `NX_DUMP_EVERY_S=<s>` | frame dumps by wall time (logs the present number and time), for comparing runs at different rates |

## 7. Measurements (unlock-spd-*, same script)

| Mode | Title | Intro | Course |
|---|---|---|---|
| default | 60.0 | 60 | 37-38 |
| `NX_FPS_CAP=30` | 30.0 | 30.0 | 30.0 |
| `NX_DISPLAY_HZ=120` | 119.8 | 91 down to 65 | 37-38 |

## 8. Not verified

* Game speed at 120 Hz (no clean number, see 3); physics and audio sync above 60 (audio runs on its own thread, music is
  real-time, so a 2x logic rate would desync it).
* Any run on a phone or on a display; headless measures production only.
* Whether the step consumers can be found and patched (a2).

## 9. Next steps

1. Clean speed test: scripted run in the course holding R, same wall time at `NX_FPS_CAP=30`, default and `NX_DISPLAY_HZ=120`,
   comparing Mario's screen position in the dumps (0.5 day).
2. Find the consumers of the step: write/read-watch fw[0x88], fw[0x260] (kernel_watch/write_watch infra) to list the code reading
   them, then decide on (a2) (2-4 days).
3. Offer `NX_FPS_CAP=30` in the launcher as "Light mode" together with `NX_PIPELINED=2` and handheld resolution (already exist).
