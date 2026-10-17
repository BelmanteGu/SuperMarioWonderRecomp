# Debugging the NVN → Vulkan path (continued)

Continues [DEBUG.md](DEBUG.md) (entries 1-58) from entry 59. The environment variables are in
[SWITCHES.md](SWITCHES.md). Milestone W7 on
GitHub (issues #43-#48): the glitches in the user's recording of course 1-1 (`docs/media/wonder/glitches/`) and
60 fps.

## Bugs found this way (2026-09-30, W7), in order

59. **The game's CPU waits did not cover a submit still being replayed (issue #43, `nvn_queue.cpp`).**
    - The game records frame N+1 on one thread while another thread is inside `nvnQueueSubmitCommands` for frame N.
      Our replay runs inside that call.
    - `nvnSyncWait`, `nvnQueueFinish` and pool invalidate/finalize returned at once there: `gpu_serial()` is 0, and
      the replay-thread job is inline. So the game could rewrite vertex and uniform data that the replay had not read
      yet. This is the leading cause of the horizon band and the exploded meshes (`glitches/play-012`, `play-044`).
    - Now there are two counters, submits called and submits done (a mutex and condition variable, never the GPU
      lock):
      - a fence records the submits made before it;
      - a sync wait blocks until those are done;
      - `QueueFinish`, `MemoryPoolInvalidateMappedRange` and `MemoryPoolFinalize` wait for every submit made so far.
    - Each wait gives up after 2 s, and the timeout is logged. The log also prints how many waits blocked (`CPU waits
      blocked on a submit still being replayed`). The course run had one.
    - `NX_SYNC_LOOSE=1`: the old behaviour.

60. **Depth EQUAL colour pass after a pre-pass: a small depth bias toward the camera (issue #44, `nvn_raster.cpp`).**
    - The colour pass tests EQUAL (taken as LESS_OR_EQUAL since #13) against depth written by a pre-pass that uses a
      different vertex shader. Metal compiles the two separately. Where the colour pass lands a few ulps behind, the
      pixel keeps the pre-pass's (0,0,0,1): black silhouettes and black/white wedges on slopes (`play-019`).
    - Pipelines with EQUAL, depth tested and not written enable depth bias. At each draw they get the game's bias
      minus `NX_EQUAL_SLACK` units (default 4), minus a quarter of that per unit of slope, with no clamp.
    - It only lets through what lies within that distance of the surface the pre-pass kept. `NX_EQUAL_SLACK=0`: off.

61. **Pipelined fences on by default (issue #46, `nvn_frames.cpp`; PERF-PLAN step 3): course 50 → 60 fps.**
    - A submit with dispatches is split after its last `DispatchCompute`. The first part is queued at the split,
      while the rest is still being replayed. Only its fence is waited for, and the write-backs are copied before
      `gpu_submit` returns, so guest memory holds the compute results exactly when it does synchronously.
    - The fence of the rest, and of the frame, is never waited for. At most 2 submits are in flight.
    - Review fixes applied before turning it on:
      1. The upload ring wrapping over its own submit waits for that submit's first part (the GPU may still read it).
      2. A first part that failed to queue drops the submit's write-backs (they would copy unwritten buffers over
         guest memory). A rest that failed to begin is not queued.
      3. A draw that binds storage over a written region (it may write the copy) counts like a compute binding.
         The write-backs then wait for the whole submit.
      4. The throttle (oldest submit waited for) runs after the rest is queued. Before, the rest waited behind it.
    - Log: `pipelined: 60 submits, 60 split (first part: 75% of the replay), write-backs after the first part 60`.
    - Course 1-1 headless:

      | Mode | fps |
      |---|---|
      | synchronous | 48-58 |
      | pipelined | 57-60 |
      | with #62-#64 | 60.0 steady |

    - Images match the synchronous run (animation phase aside).
    - `NX_PIPELINED=0`: the synchronous path; `NX_PIPELINED=3|4`: more submits in flight.

62. **Windowed present mode: MAILBOX, else IMMEDIATE, FIFO last (issue #47, `nvn_vk.cpp`).**
    - FIFO waited for the display's refresh while the game paced itself on its own 60 Hz vsync event: two clocks
      drifting against each other.
    - IMMEDIATE is now preferred over FIFO when MAILBOX is missing. On macOS, IMMEDIATE only removes the Metal
      layer's display sync; the compositor still shows whole frames.
    - The chosen mode is logged (`present mode ...`).
    - With pipelined fences, the blit's fence is not waited for either (`vkQueuePresentKHR` waits on the semaphore).
    - `NX_FIFO=1`: FIFO.

63. **Replay marks: dropped snapshots and sampler slots (issue #48).**
    - `drop_snapshot` erased the snapshot copy without `note_replaced`. A replay whose mark predates the new
      registration resolved the slot to that snapshot key and found nothing. Now the copy is noted as retired: it
      stays alive in the deferred list until the replay floor passes it.
    - Sampler pool slots had no history. A sampler re-registered with other state mid-replay was seen early. Now
      changes are recorded (only when the state differs: re-registering the same sampler every frame records
      nothing), and `sampler_entry_at` resolves the slot at the mark.
    - Regression caught in the course run: a slot **empty** at the mark fell back to `SamplerInfo{}` (nearest,
      clamp, `maxLod` 0.25, the #50 bug). The ground lost its grass and turned black past the brick stair. NVN reads
      descriptors when the GPU runs, so the game may register a slot after recording the lists and before
      submitting. An empty slot at the mark now uses the current entry.

64. **A texture slot or object empty at the mark resolves to the current one (issue #45, flat green silhouettes).**
    - The same rule as #63, for texture pool slots (`pool_entry_at` returned the "no texture" entry) and for objects
      (`find_object` returned null for an object created after the mark but before the replay). Both fell to the
      zero dummy for a few frames: the leading hypothesis for the flat green layers (`play-017`, `play-069`).
    - `NX_MARK_EMPTY_OLD=1`: the empty entry, as before, for bisecting. Confirmation needs a windowed run (the
      silhouettes were never seen headless).

    ![Course at 60 fps](../media/wonder/38-course-60fps-pipelined.png)

## W8 (2026-09-30, night)

65. **Button icons showed as "?" (issue #49): our own NintendoExtended font.**
    - The game draws button icons as Private Use Area characters of shared font 5 (NintendoExtended). `hle_pl.cpp`
      gave it Arial Unicode, which has none of them. The console's fonts (system archives 0100000000000810-814 of
      firmware 23) are key generation 23, and our keys stop at `master_key_15`.
    - The codepoints come from the game's own text (`Mals/*/Key.msbt` labels):
      - E0E0-E0E9: A, B, X, Y, L, R, ZL, ZR, SL, SR;
      - E0EF / E0F0: Plus / Minus;
      - E0AB-E0AE, E0EA: the D-pad;
      - E100 / E101: the sticks;
      - E090-E097: the directions;
      - also the older E0A0-E0A9 set and E145/E146 (seen in layouts only).
    - The title is "Press <10.14:08cd> + <10.14:09cd> to Start": L + R.
    - `tools/fonts/make_button_font.py` draws those glyphs (our own vector shapes; only the letters inside come from
      the host font). It writes `private/wonder/fonts/shared_5.ttf` (git-ignored: it embeds the host font), which
      `hle_pl.cpp` already loads first.
    - Check: the title reads "Press [L] + [R] to Start", and menus show "(B) Back (A) Select".
    - To build it: `private/venv/bin/python tools/fonts/make_button_font.py`.

66. **Windowed stretches at 35-40 fps (issue #51): the GPU goes past the frame, headless does not.**
    - In the play session log, the slow stretches in the course show:
      - replay 8-10 ms;
      - `fence wait` 13-15 ms: all of it the wait for the first part of the split submit, with no throttle waits;
      - 60 of 60 submits split.
      So the GPU takes ~25 ms per frame there.
    - The same course headless: replay ~6 ms, fence ~4 ms, 60 fps. Audio is not the difference: the renderer runs
      headless too, only the output is dropped.
    - A windowed test run cannot measure it unattended. When the window is covered, SDL reports it hidden and
      `gpu_present` stops (`window_minimized`).
    - Next, with the window visible: `NX_GPU_PROFILE=1` in the slow stretch; `NX_PIPELINED=3`; and a check of whether
      the compositor holds the 3 swapchain images (IMMEDIATE, 2560x1440).

67. **World map: the player was a flat silhouette (issue #50). A pool swizzle leaked from another slot.**
    - The map draws the player into a target of its own. It then composites the player over the scene
      (fs `0bebd775`) from three textures:
      - A (RG16F): the player's distance and silhouette strength, written by fs `0030c911`;
      - C (R32F): the scene depth made linear by fs `0aa56d1b`;
      - P: the player colour.
    - Where `A.x >= C.x + 1e-5`, the player is behind the scenery: dark overlay. Otherwise P is shown.
    - Measured in one frame (new `NX_SNAP_RAW`): A.x and C match within 4e-6 at every player pixel. The shader
      still took the "behind" branch.
    - Cause: the game also registers A in another pool slot with a G G G G view. `TexturePoolRegister` stored that
      swizzle on the Texture (`pool_swizzle`). `sampled_view` applied it to every slot whose own view has no swizzle,
      the composite's included. A.x then read the strength (1.0), not the distance.
    - Fix (`nvn_pipeline.cpp` `sampled_view`): a slot's own view decides. `pool_swizzle` stands in only when no slot
      view is known. `NX_POOL_SWIZZLE_ANY=1`: as before.
    - Ruled out on the way, each kept as an opt-in switch:
      - depth invariance of GREATER (`NX_GREATER_SLACK=1`);
      - clip-space z convention: `NX_CLIP_Z_PATCH=1` maps z in the vertex shaders (`nvn_spirv_clip.cpp`) instead of
        using VK_EXT_depth_clip_control. It gives the same image, so MoltenVK's extension works.
    - Kept on: a pass that samples its own depth image while neither testing nor writing depth now runs without it
      attached (undefined on Metal). `NX_KEEP_SAMPLED_DEPTH=1`: as before.
    - Debug tools added:
      - `NX_SNAP_RAW=1`: 4-byte formats snapshotted as stored, to `.raw`;
      - `NX_SNAP_FS_MAX`;
      - `NX_TRACE_FS_HASH` takes a list;
      - `NX_TRACE_VERTS=1`: the first vertices of stream 0.
    - In the draw trace, the indented state lines come *before* the `draw N:` line they belong to.
    - Check: the map shows the chosen character. The course is still 60 fps, and menus and "Here we go!" are
      unchanged.

## Windows (2026-09-30, the PC: RTX 3050 laptop + Intel UHD 770; WINDOWS-PLAN.md)

68. **The data packs had no `spirv/results.tsv` (all storage buffers of translated shaders unbound).**
    - `tools/pack_phone_wonder.py` packs only `*.spv`, and `pack_pc_wonder.py` reuses it. On the phone and on the
      first PC boot the log said `no private/wonder/spirv/results.tsv: storage buffers of translated shaders cannot
      be bound`: every compute and vertex shader that reads a storage buffer read nothing.
    - Fix: both packers require and pack it. On the PC it was regenerated with the harness (TRANSLATOR.md, "Setup
      (Windows)"); `shader_cache_test` checks it against every `.spv`.
    - The Android build has the same hole: rebuild its data zip.

69. **NVIDIA: the process died in the driver at the title (`c0000094`, divide by zero in `nvoglv64.dll`).**
    - The Windows crash handler now names the module and unwinds the host stack (`host #n <module>+<offset>`;
      `tools/symbolize.py` for the exe's own frames). The stack was `image_of` → `vkCreateImage`.
    - Cause: an ASTC texture (NVN 0x87, ASTC 4x4 sRGB). Desktop NVIDIA GPUs have no `textureCompressionASTC_LDR`;
      creating an image in an unsupported format is undefined behaviour, and this driver divides by zero.
    - Wonder ships ASTC widely: the title's key art and logo (4x4) and the models' albedo maps (6x5).
    - Fix 1 (`nvn_image.cpp` `image_supported`): every image is checked with
      `vkGetPhysicalDeviceImageFormatProperties` first. Unsupported storage/attachment usages are dropped; an
      unsupported format is logged once and the texture keeps no image.
    - Fix 2: without the feature, ASTC textures are RGBA8 (sRGB or not) images, decoded on the CPU at upload by our
      own decoder (`nvn_astc.cpp`, from the Khronos specification). `nvn/vk: ASTC decoded on the CPU: n textures,
      MB, ms` is logged. `NX_ASTC_DECODE=1` forces it on a GPU that has ASTC (the Intel UHD 770 does: CPU and
      hardware decoding can be compared on the same GPU).
    - Tests: `astc_test --selftest` (void extent, reserved modes, 280,000 random blocks at all 14 footprints) and
      `tests/astc_check.py`: 29 of the game's textures (4x4 and 6x5, 4x4 to 1920x1080) match an independent decoder
      within 1 per channel on 100% of texels.
    - Check: the title on the RTX 3050 has its key art and logo; against the Mac's `37-title.png` only the animated
      parts differ (logo shine, scrolling checkers).

70. **The first Vulkan device was the integrated GPU.** On the laptop the loader lists the Intel UHD 770 first.
    `NX_GPU=discrete|integrated|<index>|<name part>` chooses (`nvn_gpu_pick.cpp`); the default is the first discrete
    GPU. Every device and the choice are logged. `gpu_pick_test` covers the rule (it caught `NX_GPU=3050` being read
    as an index).

71. **No barrier between render passes: the course HUD in blocks on NVIDIA, a black sky on Intel.**
    - Symptoms: on the RTX 3050 the HUD (coins, lives) had rectangular holes that changed every frame; on the UHD 770
      with `NX_ASTC_DECODE=1` the sky was black with green edges and a stretch of ground was garbage. Neither showed
      synchronously on the Mac, nor on the UHD 770 with its own ASTC (timing).
    - Cause: rendered images stay in `GENERAL` (they are sampled while rendered), so `transition()` records no
      barrier, and `pass_end_only` ended a pass with none either. The next pass sampled, copied or loaded what the
      last one wrote with no dependency. Metal (MoltenVK) tracks those hazards itself; NVIDIA overlaps passes and
      read half-written tiles.
    - Fix (`nvn_draw.cpp` `pass_end_only`): a memory barrier after every pass, from the graphics stages' attachment
      and shader writes to all later reads and writes. `NX_NO_PASS_BARRIER=1`: none, as before.
    - Cost: none measurable (course 58 fps headless on the RTX 3050, as before).
    - Check: the HUD is whole in every frame of the course on both GPUs, and the sky is right with the CPU decoder.

72. **Scene loads wrapped the upload ring over themselves: black and green characters in the opening's cast scene.**
    - Symptoms (RTX 3050, and the UHD 770 with `NX_ASTC_DECODE=1`; never with native ASTC): at the Flower Castle the
      Toads' bodies black with sparkles, Mario's and Luigi's faces black with green eyes, Daisy green; once a GPU
      hang. Gameplay was fine.
    - Bisecting: decoding only the linear or only the sRGB ASTC formats was right; an artificial delay on native
      uploads was right (not a race); the log had `upload buffer full (312 MB in this submit)` (the per-frame upload
      averages hid it).
    - Cause: the cast loads every character at once. Decoded to RGBA8 (4x to 9x the ASTC size) its textures staged
      312 MB in one submit through the 256 MB ring, which then wrapped over its own data before the GPU copied it.
    - Fix (`nvn_frames.cpp` `staging_alloc`, `nvn_upload.cpp`): texture uploads of 1 MB and more get a host-visible
      buffer of their own, freed when the submit being recorded retires (as images in the grave).
      `NX_STAGING_RING=1`: through the ring, as before.
    - Bisecting switches kept: `NX_ASTC_DECODE=linear|srgb|<hex format>,...`.
    - Tests: `tests/astc_check.py` now compares every mip level and the linear ASTC formats too (32 textures of the
      cast's models: all within 1 per channel). Check: the cast scene is whole on both GPUs (`tools/wonder_course.ps1`).

73. **Pipelines compiled in the replay: stutters when new objects come in; an on-disk pipeline cache.**
    - The course's slow windows had 10-42 ms replays where new objects appeared: `vkCreateGraphicsPipelines` runs
      the first time a draw needs a pipeline, inside the replay.
    - `nvn_pipeline_cache.cpp`: a `VkPipelineCache` loaded from `<data root>/pipelines_<vendor>_<device>.bin` and
      saved every 600 presents when it grew (the driver validates the header). `NX_PIPELINE_CACHE=0`: none.
    - A new line per 60 frames: `created over 60 frames: n pipelines (ms), n ASTC textures decoded (ms)`.
    - Measured (RTX 3050, course run with a warm cache): 853 pipelines in 1.2 s in all; 383 ASTC textures decoded
      in 3.8 s in all, about 10 ms each on the replay thread: the next cost to cut (#74).
    - `nvn_pipeline.cpp` went over 700 lines: the stand-in images moved to `nvn_dummy.cpp`.

74. **Audio: the limiter crash at boot again on Windows (issue #16, DEBUG.md #28).**
    - `CRASH ... sdk+228220` (the limiter's apply) under `CommandListProcessor::Process` 3 s into one run of ~25.
    - An update waits for a render since the previous one, but gave up after 50 ms; with every thread loading at
      boot a render can take longer, and a second update then overwrites the effect's initialize status.
    - Now it waits up to 1 s and logs `audio: no render for 1 s since the last update` if it gives up. 12 boots in a
      row: no crash, the log line never printed. To watch: any further crash with that line before it.

75. **ASTC decoding over a few threads.** Images of 8,192 blocks and more are decoded by up to 6 threads, by rows of
    blocks (`nvn_astc.cpp`). Course run on the RTX 3050: 379 textures decoded in 2.4 s in all (3.8 s on one thread);
    `astc_test` and `astc_check.py` still pass. With the warm pipeline cache, pipelines cost 0.36 s in all (1.2 s
    before the cache had filled). Course median 58 fps headless.

76. **Exploded meshes in course 1-1 on Windows: the pipelined path. Synchronous by default off macOS.**
    - The owner's play session (RTX 3050, windowed, pipelined): long black lines across the screen, stretched
      triangles around the ground and bushes near Elephant Mario, a goal house turned into a sheaf of streaks
      (`private/wonder/play2`, a frame every 5 s; `tools/find_glitches.py` flags them by near-black pixels in bright
      scenes).
    - Scripted runs did not show them: with Parsec taking ~70% of the CPU the game ran at 30 fps and the timing
      changed. So the A/B ran with the owner playing: the same course with `NX_PIPELINED=0` (and 1-2 after it) had
      none (`private/wonder/play3`, a frame every 2 s), at a 58 fps median against 40 for the pipelined session.
    - So it is the overlap of frame N's GPU work with what the game does next (the Mac's #43 "exploded meshes" is
      likely the same). The pipelined path was made for MoltenVK, which encodes Metal inside `vkQueueSubmit`; native
      drivers do not need it.
    - Fix (`nvn_frames.cpp` `pipelined()`): on by default on macOS only. `NX_PIPELINED=1..4` turns it on elsewhere
      (to find the race itself, which is still open for the Mac).
    - Tools: `tools/wonder_course.ps1 -Course11` goes on through the opening and the map into course 1-1;
      `NX_GPU_PROFILE` logs the frame's GPU total.

77. **World map: every other frame broken while Mario walks (fixed by #91), and the limiter crash at boot once more
    (#74).**
    - The owner's recording (map, after playing courses): while Mario moves, about half the frames show a flat
      lavender scene, huge black triangles and Mario as a purple silhouette; standing still is fine.
    - Not reproduced from a boot straight into the map (headless at 14 fps, windowed at 59 fps with
      `tools/capture_burst.ps1` taking 116 window captures while a script walked Mario back and forth): it needs the
      state of a longer session.
    - Tool for that: a file named `dump_frames` in the data folder (`"<count> [<every>]"`) dumps the next presented
      frames at full speed (`nvn_debug.cpp` `frame_dump_due`).
    - Cause found in #91: the command-list safety net dropped the map's lists after 3+ minutes in a course.
    - #74 again: a render that began before an update and ended after it counted as a render that used it. The update
      now records the render count after it ran, under the render lock, and renders count themselves under it.
      20 boots in a row: no crash.

## Android bring-up (2026-09-30, evening; branch `feat/android-wonder`)

78. **First run on a phone: the alias region collided with Android's zygote.**
    - Device: Galaxy S23 (SM-S911B, Snapdragon 8 Gen 2, Adreno 740, 8 GB, Android 16).
    - The app died at start with `FATAL: cannot reserve alias at 5800000000 (+1000000000): the range is in use`.
    - `/proc/<pid>/maps` showed `app_process64` (the zygote's executable, position independent) at
      0x5f_0972_b000. Android places it at random, from about 0x55_5555_0000 to 0x65_5555_0000, at every boot. So
      the old layout worked for Hades only by luck of the boot.
    - Fix: on Android the alias region (64 GiB) sits at 0x20_0000_0000, below the modules. Android keeps to the low
      4 GiB there (ART heap). The guest learns the region from svcGetInfo; `memory.cpp` marks the gap between the
      alias and the stack region inaccessible.
    - The second failure was the data folder. Files pushed by `adb` into `Android/data/<pkg>` belong to the shell
      user, and the app cannot read them. The data now goes to `Internal storage/Super Mario Bros. Wonder`, with
      all-files access granted by `appops set --uid <pkg> MANAGE_EXTERNAL_STORAGE allow`
      (`tools/wonder_push_android.sh`).
    - Result: the game runs on the S23. Adreno 740, Vulkan 1.3, MAILBOX, 2340x1080 swapchain, 40-60 fps at the
      title, tens of thousands of draws with none failed, and still running after a minute. The screen stays black
      (#79).

79. **Black screen on the phone: the NVN present path was Mac-only.** The picture renders (no failed draw), and
    `gpu_present` blitted it to the swapchain. Three things are wrong there on Android:
    - The swapchain format was hard-coded to `B8G8R8A8_UNORM`. Android surfaces offer RGBA, not BGRA; the format
      now comes from `vkGetPhysicalDeviceSurfaceFormatsKHR`.
    - `compositeAlpha` was hard-coded to OPAQUE, which an Android surface may not offer (INHERIT then takes the
      image's alpha as transparency). It is now chosen from the capabilities, and the picture's alpha is forced to 1.
    - The blit does not rotate. With the surface's `currentTransform` at 90° the compositor has to turn the image.
    - Fix: `nvn_present.cpp`, a render pass that samples the window texture. It rotates by `preTransform` (the
      swapchain made in the panel's own orientation), fits the picture with bars (16:9 on 19.5:9) and writes alpha
      1. It is on by default on Android; `NX_PRESENT_PASS=1` enables it elsewhere, `=0` restores the blit.
    - A new Android window (home, lock screen) now re-makes the surface and the swapchain (`window_generation`).
    - Check without a phone: `NX_PRESENT_TEST=1080x2340:90` renders the pass into an offscreen "display" in headless
      runs and dumps it. The title comes out turned 90° with bars, the same mapping as the Hades pass that is proven
      on the S23.
    - The on-device check is pending (the phone was disconnected).

80. **Adreno reads BC natively.** On the S23 no texture image failed to be made, and the game uploads BC1/BC4/BC5/
    BC7 from its first frames. Decision D1 (BC → ASTC on the Mac, ANDROID-PORT.md) is therefore for Mali phones
    only, not a blocker for Adreno.

81. **Touch controls for Wonder, drawn (A4).**
    - `touch_layout.cpp` holds a fixed layout: round buttons with A big at the bottom right, B, Y, X around it, and
      ZR, R, L, +, - along the top. It is hit-tested by `platform_android_input.cpp`.
    - The left half is a stick that appears where the thumb lands. Pushed past 85% it also holds Y (dash), so
      running and jumping need one thumb each.
    - The present pass draws the circles over the picture: translucent, brighter while held, with the stick under
      the thumb. They hide while a controller is in use.
    - Hades keeps its own zones.
    - `NX_OVERLAY_DEMO=1` draws the layout on the Mac for the self-test
      (`docs/media/wonder/41-android-touch-overlay-selftest.png`).
    - Also checked on the Mac: menus take short taps. Seven 0.15 s A presses go from the title to course 1-1. The
      "A held ~1 s" of 2026-09-29 is gone.

82. **The Android emulator is no use for rendering on the M1.** The emulator 34.2.16 selects SwiftShader (a CPU
    Vulkan) whatever the settings: `-no-window` forces it, and with a window, `ANDROID_EMU_VK_ICD` and the bundled
    MoltenVK ICD were ignored. Wonder boots on it, but no frame came in 11 minutes: every pipeline compiles on the
    CPU. It is kept for install and boot checks only (AVD `WonderTest`: 5 GB RAM, 16 GB disk). Rendering checks use
    `NX_PRESENT_TEST` on the Mac and the phone.

83. **Pipeline cache on disk (A3, also for the Mac).** (Superseded at the merge by the PC's cache, #73: see #89.)
    - Every session compiled every pipeline again: 672 over the scripted course on the Mac. That is a hitch at each
      new scene, and slower on Adreno.
    - `nvn_pipeline_cache.cpp` keeps a `VkPipelineCache` in `<data root>/cache/pipelines_<vendor>_<device>.bin`
      (10.5 MB on the M1). The header must match the device, or the cache starts over.
    - It is saved on a thread of its own every 32 new pipelines, and retried when the driver answers INCOMPLETE
      (MoltenVK did, twice, as the cache grew between the two calls).
    - Course 1-1 headless, two identical runs: 11 intervals under 55 fps without the cache, 5 with it.
    - `NX_PIPELINE_CACHE=0`: no cache.

84. **Android performance hints (ADPF, A5).**
    - `perf_hint.cpp`: the thread that replays NVN (`QueueSubmitCommands`) and the presenting one join an
      `APerformanceHint` session with a 16.7 ms target.
    - Each frame's work, from the end of the last present's pacing to the next present, is reported. The phone then
      raises clocks when a frame runs late instead of guessing from load.
    - Loaded with `dlsym` (Android 13+; older versions go without). `NX_ADPF=0`: off. It does nothing on the Mac and
      Windows.
    - The Mac regression is unchanged (course 1-1 at 60 fps). The effect on the S23 waits for the phone.

85. **BC decoded on the CPU for GPUs without BC (Mali; D1's fallback).**
    - `nvn_bc.cpp` decodes BC1-BC5 and BC7 at upload, after untiling. The image is created as RGBA8, R8 (BC4) or RG8
      (BC5), in UNORM, SNORM or sRGB as the BC format was. Views follow (`bc_view_format`). BC6H is not decoded.
    - It is on when the device has no `textureCompressionBC`. `NX_FORCE_BC_DECODE=1` forces it, to check it on the
      Mac.
    - The BC7 partition and anchor tables (2 and 3 subsets, 64 shapes each) were derived from Pillow's decoder,
      with blocks whose subsets decode to distinct colours (`tools/bc/derive.py`).
    - Against Pillow (`tools/bc/compare.py`), on 5,120 random blocks per format with all BC7 modes: a maximum
      difference of 0 on every format.
    - Course 1-1 with `NX_FORCE_BC_DECODE=1`: the same screens as native BC, 60 fps, a 3.95 GB peak.
    - The same comparison caught the exploded-mesh glitch (issue #43) in the *native* reference run. Huge brown
      triangles appeared near the pipe near the end of the course, so #43 is still open (DEBUG-2 #86).

86. **Exploded meshes after loading a pipeline cache (#83's first version).**
    - Of about twenty course runs today, one (w9r) drew the tree trunk as huge brown triangles. It stayed that way
      from frame 13200 to the end: a lasting state, not a one-frame race.
    - That run was the first to load a cache that had been saved while pipelines were being made: the first version
      read the cache on a thread of its own. MoltenVK could then serialise a half-updated cache, and a vertex shader
      loaded from it came out wrong.
    - Fix: the cache's contents are read on the replaying thread, under the GPU lock, where no pipeline is being made.
      Only the file is written on another thread. The file name changed (`pipelines2_*`), so a cache saved the old
      way is never loaded.
    - Check: three course runs in a row (a new cache, then loaded twice) are clean, 60 fps.
    - `tools/detect_explode.py` flags frames with large brown areas in the sky. It has known false positives (title,
      character select, the tree), so look at what it flags.
    - The exploded meshes of the user's recording (#43) predate the cache, and that issue stays open.

87. **Windowed 35-40 fps stretches (#51) no longer reproduce.**
    - Windowed course 1-1 runs on the scripted input, current build:
      - muted: 283 intervals, 2 under 50 fps;
      - with sound: 285 intervals, a 59.4 fps mean, 3 under 50 (scene changes).
    - The user's second play session (play5) was also at 60 nearly throughout.
    - The first session's stretches had a 13-15 ms fence wait, the GPU past its frame. Likely causes, not isolated:
      other load on the Mac then, and the pipelines compiled on the fly (the cache, #83, came later).
    - Closed; reopen with a log if it shows again.

88. **Independent review of the Android present path, and its fixes (before the phone test).** The review hunted for
    what MoltenVK tolerates and Adreno or Mali would not. Fixed:
    - **Descriptor set and overlay data per swapchain image.** They were written after acquire, but with MAILBOX an
      image can come back while the frame that last drew into it is still in flight (more so with 3+ submits in
      flight). They now come from the submit's slot (the descriptor pool and the upload ring, reset when the slot
      retires).
    - **SUBOPTIMAL compared the turned swapchain extent with the unturned currentExtent.** On a phone held sideways
      it re-made the swapchain at every present. It now compares with the currentExtent and transform the swapchain
      was made from.
    - **A failed present pass fell back to the blit on a pre-rotated, portrait swapchain:** a sideways picture. The
      swapchain is now made again upright for the blit.
    - **Swapchain destroyed while the pass's views and framebuffers still referenced its images.** They are released
      first.
    - Smaller fixes:
      - the swapchain extent is bounded by the surface's maximum (either orientation);
      - more than 8 swapchain images is logged (the present semaphores are 8);
      - the presenting thread joins the ADPF session.
    - Checked again on the Mac: the portrait self-test (`NX_PRESENT_TEST=1080x2340:90` with the touch layout) and a
      windowed run through the pass.
    - Left as is: a failed `begin()` right after acquire leaves the image unpresented (rare), and after
      `APP_CMD_TERM_WINDOW` the surface lives until the next window (the window is kept acquired).

89. **The Windows branch merged into the Android one (2026-10-01).** The two had been worked in parallel, the PC on
    Windows and the Mac on Android. Decisions:
    - **Entry numbers:** the PC's #68-#77 kept theirs, and Android's became #78-#88. References in code and docs
      were renumbered.
    - **Pipeline cache:** the PC's (#73) is kept, the Android one (#83) dropped. It lives in
      `<data root>/pipelines_<vendor>_<device>.bin` and is saved every 600 presents from `gpu_present`, under the
      GPU lock: the same safety #86 asked for. It also logs pipeline creation times.
    - **Texture decoders:** both are kept. The BC decoder (#85, for GPUs without BC: Mali) and the ASTC decoder (PC,
      for GPUs without ASTC: NVIDIA and Intel desktops) are complementary in `nvn_upload.cpp`.
    - **Present and frame dumps:** the PC's on-demand dump trigger is kept, plus the layout transition the present
      pass needs.
    - **Pipelined fences:** the PC's #76 makes submits synchronous by default off macOS, after exploded meshes in
      course 1-1 on Windows. **That includes Android.** The S23 test of 2026-09-30 ran pipelined, and the user saw
      graphics bugs and a low frame rate. `NX_PIPELINED=1` in `config.txt` turns pipelined fences back on, to compare.
    - Also from the PC, now on Android:
      - a barrier after every render pass (#71);
      - staging buffers of their own for big uploads (#72);
      - the integrated-GPU choice fix (#70);
      - the audio limiter pacing (#74);
      - on-demand frame dumps (#77);
      - the recompiler split into files under the line cap, same output.

## PC quality (2026-10-02; pc-quality/PLAN.md)

90. **Exploded geometry at course 1-1's start and after each respawn, for a whole session (#76's rest): command lists
    of one memory pool dropped by lists of another.**
    - Symptom (RTX 3050, synchronous submits, windowed; `pc-quality/E1-baseline.md`): for 1-4 s at the course start
      and after every respawn, black columns from the sky down to the ground, a black band along the ground, thin
      lines from the top of the screen; or a flat lavender band and the Talking Flower drawn black. 2 of 6 course
      sessions; a session that had it kept it. The same processes also drew a brown rock blown up over the world map,
      with black holes, in the first frame back on the map (the kind of #77).
    - Repro: an input script enters 1-1 from the owner's map save, pauses, exits and enters again, 8 times in 7 min;
      the window is recorded (`tools/record_run.ps1`) and the frames at 2 per second go through `find_glitches.py`
      and `find_lavender.py`, sorted by the script's phase (course start, map return). Glitchy processes showed it
      in 1-2 of 8 entries; some processes never did, so a variant needs several processes.
    - Ruled out on the way: the five Android caches off together (`NX_SET_CACHE`, `NX_RESOLVE_CACHE`,
      `NX_CALL_TABLE`, `NX_JANITOR`, `NX_STAGING_MIN` all 0): glitch in 2 of 8 entries. `NX_IMAGE_REUSE=0` with
      `NX_NO_IMAGE_SHARE=1` cannot run (the 8 GB memory guard stopped it 36 s in).
    - Found with `NX_CHECK_VERTS=1` (new: draws that fetch vertices past their bound vertex buffer range). In the
      glitchy runs, draws of vs `21d4f4c0`/`82906934` and others indexed vertices 0..1211 of 12 bytes (14.5 KB) with
      684, 2,928 or 8,652 bytes bound: earlier draws' buffers. Clean runs had none. Every glitchy run's log also had
      `nvn: called handle ... is unknown` (a CallCommands of a list we no longer had), and no clean one; the same split
      holds for E1's seven logs (rec-c11-1 and rec-long: 5 lines each; the five clean runs: none).
    - Cause (`nvn_cmd.cpp`): a list lives as long as its command memory, so a list recorded over the bytes of older
      ones drops them (DEBUG.md #26). Those bytes were keyed by the `NVNmemoryPool*`, the guest struct's address, plus
      the offset. Pool structs sit a few KB apart while the offsets reach megabytes, so a list in one pool
      "overlapped" lists of another and dropped them: `list c463 (pool 5014c7d060) dropped by list 14246 recorded in
      pool 5014c7dbf0` (0xb90 apart). Wonder records objects' draw lists when a course loads and calls them each
      frame. A call to a dropped list skipped its vertex buffer binds, and the draws after it read the buffers bound
      before them through their own indices. Where a load puts the lists decides which get dropped: hence per
      session, and only on the objects drawn through them (the Talking Flower near the start).
    - Fix: the key is the pool's GPU address plus the offset (pools have disjoint GPU ranges), so only lists over
      the same memory drop each other. `NX_CMD_REGION_OLD=1`: the old key, with the first 5 cross-pool drops logged.
    - Validation, before: unknown handles in 5 of 7 repro processes (glitch frames in 3). After: 2 repro processes
      (16 course entries, 14 map returns, 15 min) with no glitch frame, no unknown handle and no out-of-range fetch;
      `record_run.ps1 -Course11` (330 s: opening, map, course 1-1 with two respawns) clean, no unknown handle.
      `tools/regress.ps1 -NoIntegrated -Repeat 2`, interleaved before/after: no loss (course median 47.8 / 48.6 fps,
      p10 30.6 / 30.3, replay 9.16 / 9.15 ms, 0 glitch frames, unit tests pass).

91. **World map lavender with black wedges after a course (#77): live command lists dropped by the safety net.**
    - The owner's session: back from a course, the map stayed a flat lavender with one huge black wedge, only the HUD
      and the course balloon right; the log had `called handle ... is unknown` for low (early) handles just before.
    - Cause (nvn_cmd.cpp): the safety net that bounds the number of command lists counted `g_handle_order`, which
      keeps an entry for every list ever recorded, dropped ones included. Past 2^18 recordings (2-3 minutes of play)
      it ran at every EndRecording and dropped any live list not called in the last 2^18 recordings. The map's lists,
      recorded at its first visit and not called during 3+ minutes in a course, were gone on the way back.
    - Also: lists recorded in a memory pool stayed live after the pool was finalized (GPU addresses are never
      reused, so nothing recorded over them): +3,000-7,000 live lists per course entry, which made the net run.
    - Fix: the net counts live lists (`g_handles`) and compacts stale queue entries; a finalized pool drops the lists
      recorded in its memory (`command_memory_released`). `NX_HANDLES_OLD=1`, `NX_CMD_POOL_DROP=0`: as before;
      `NX_KEEP_HANDLES=<n>`: the limit (reproduces it quickly with a small n). `NX_CMD_DROPS=1` tells, on each
      `called handle ... is unknown` line, what dropped it.
    - Check: the owner played map ↔ course cycles after the fix: map clean. (The study was stopped before its own
      validation runs; a formal repro run is still to do.)

92. **Course 1-1's background without its haze for single frames.**
    - The distant towers lost their aerial-perspective haze for one frame now and then (6.5% of course frames).
    - Cause (nvn_sets.cpp): Wonder registers its 64x36 haze texture in a new pool slot every frame and re-initializes
      the same NVNtexture struct as a dynamic-resolution target of the same format, over other memory, before the
      replay. The slot resolved through the struct to the new texture, so the haze pass sampled that target.
    - Fix: a pool slot whose texture struct was re-initialized over other memory keeps sampling the registered copy;
      a target re-initialized in place (same memory, new size) is still followed (#45). `NX_SNAPSHOT_MEMORY=0`: as
      before. Diagnosis tool: `NX_FRAME_SIG` (per-frame draw signature, `nvn_draw_trace.cpp`) and
      `tools/find_haze_flicker.py`.
    - Check: the owner's play after the fix: no haze flicker. Formal recordings still to do.

93. **Windows: crash after the character select (#5, null read at 0x150, `main+91f464`): not reproduced; what is known.**
    - Symptom (issue #5, Windows 11, clang-cl 23, RTX 5070 Ti): a guest thread crashes with a read of 0x150 about a
      second after a character is chosen, right after four small `WriteTexels` and `ICommonStateGetter cmd 66`.
    - What the crash is (read from the recompiled code, `private/wonder/gen`, and checked on Linux): `main+91f40c` is
      the "load parameters" method of a *rail* object (it reads `IsEntity`, `StartEdgeType`, `EndEdgeType` from the
      rail's BYML node, `main+452530` taking the default from `[object+8]->+0x150` when the key is missing). The
      caller is the stage loader's area function (`main+1abf28`: `Actors`, `Rails`, `ActorToRailLinks` of an area's
      `BancMapUnit/*.bcett.byml`); it creates the rail from a 15-entry class table (`main+347d748`, index 6 here) and
      stores in `[rail+8]` the resource it finds by path (`main+39d70`, `Gyml/Rail/RailParam/<type>.game__rail__RailParam.bgyml`).
      The crash is that `[rail+8]` is null: the rail's parameter resource was not found/loaded.
    - Not reproduced on Linux. The same point (four `WriteTexels`, then `cmd 66`, then the area loader on the same
      thread, stack `68bdc3...` as in the report) runs without a crash for new and for continued saves, for several
      characters (Blue Toad, Red Yoshi), with one CPU, with the world map and course 1-1 loaded after it (their rails
      load `WorldMapRoute`, `NextGoTo`, `Default`, `SectionFrame` fine). In the opening stage (Course900) the area
      loader finds no `Rails` key at all (checked in its four `bcett` files), so the report's thread reached a
      rail that this run never meets: it loaded a stage that has rails, or read a different `Rails` list.
    - Not the cause (checked): `ICommonStateGetter` cmd 66 is `SetCpuBoostMode` (input only, no output; the stub
      answered it correctly), huge or short file reads (no read over 1 TB in a run), directory order (the game
      opens no directory), RomFS names (no Windows-reserved or case-colliding names), the thread core numbers,
      saves (new, second run and progressed saves).
    - Changes: cmd 66 is answered explicitly (no more log line). Every crash report (Windows, Linux, macOS, Android)
      now ends with `last file opens`: the last 24 `OpenFile`/`OpenDirectory` of any thread, with the host thread id
      and the result (ok, not found, the host could not open it). That names the stage and the resource that was
      loading. `NX_TRACE_FS=1` also logs `OpenDirectory` and host open failures (with errno).
    - To close it: the reporter's `CRASH` block now shows which stage was loading; with `NX_TRACE_FS=1` the lines
      `not found` / `failed to open on the host` before it tell which rail resource is missing on that machine.
