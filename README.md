# Super Mario Bros. Wonder Recomp

<p align="center">
  <a href="https://www.youtube.com/@belmante"><img src="docs/media/projeto-belmante.jpg" alt="Projeto Belmante" width="120"></a><br>
  A <b>Projeto Belmante</b> project &middot; <a href="https://www.youtube.com/@belmante">youtube.com/@belmante</a>
</p>

Static recompilation of **Super Mario Bros. Wonder** (Nintendo Switch, 1.0.0, title ID `010015100B514000`) to native
code. No emulator runs: the game's AArch64 machine code becomes C++ compiled for the host, and its system and graphics
calls (NVN, Nintendo's own API) are implemented natively on **Vulkan** (MoltenVK on the Mac).

Part of the [SwitchGamesRecomp](https://github.com/BelmanteGu/SwitchGamesRecomp) work (same runtime, see
[docs/FRAMEWORK.md](docs/FRAMEWORK.md)); this repo follows Wonder alone. Living log: [PROGRESS.md](PROGRESS.md).

> [!NOTE]
> **No game files are included.** This repository holds no game code, assets, keys or firmware, and no build
> made from them. Everything derived from the game stays in `private/` (gitignored). You need your own legally
> dumped copy of Wonder 1.0.0, plus your own keys and firmware. Instructions on how to obtain them are **not**
> provided.

> [!IMPORTANT]
> **This project was made with a lot of AI and a lot of study.** Most of the code, tooling and documentation was
> written with AI assistance (Anthropic's Claude), directed, tested and reviewed by a human who spent a great deal of
> time studying the Switch's CPU, OS and graphics stack (AArch64, NVN, Maxwell shaders, Vulkan) to guide it. If you
> don't agree with the use of AI or don't trust AI-written code, that is your right; you run the code at your own
> discretion.

> [!IMPORTANT]
> **A note on the history of this repo.** The project was developed across several computers, and files were lost
> when moving between them. Part of the code had to be rewritten from scratch, and some early notes, test
> scripts and intermediate results no longer exist. If something in [PROGRESS.md](PROGRESS.md) or the docs refers
> to a file you cannot find, that is probably why. Bug reports about gaps like this are welcome.

## Platforms

| Platform | Status | Graphics |
|---|---|---|
| macOS (Apple Silicon) | works, the reference platform | Vulkan on Metal through MoltenVK |
| Windows (x86-64) | works, tested on NVIDIA (RTX 3050) and Intel GPUs | native Vulkan 1.3 |
| Android (arm64) | works, tested on a Galaxy S23 | native Vulkan 1.3 |
| Linux | **not ported yet, but close** | native Vulkan 1.3 |

Linux is not a rewrite. The recompiled game code is portable C++ and the NVN layer talks only generic Vulkan, with
no Metal code. What is missing is the same small platform layer macOS, Windows and Android already have: a
`platform_linux.cpp` (window, input, audio) and a `memguard_linux.cpp` next to the existing `platform_mac.cpp` /
`platform_win.cpp` / `memguard_*.cpp` in `runtime/src/`, plus a Linux branch in `CMakeLists.txt` (today its
non-Apple, non-Android branch is the Windows one). Nobody has built or run it on Linux yet. Pull requests are
welcome.

## State

| Title | Character select |
|---|---|
| ![title](docs/media/wonder/37-title.png) | ![select](docs/media/wonder/35-character-select-samplers.png) |
| **World map** | **Course 1-1, 60 fps** |
| ![map](docs/media/wonder/39-world-map-player.png) | ![course](docs/media/wonder/38-course-60fps-pipelined.png) |

### What works

- **CPU:** `main` and the SDK are recompiled: 17.8M AArch64 instructions, 250k functions, about 1.4 GB of generated C++.
  The game runs its whole frame loop natively on macOS.
- **Graphics:** Wonder draws through **NVN**, Nintendo's own graphics API. The `nvn*` functions are implemented on
  the host: recorded command buffers are replayed into Vulkan, running on Metal through MoltenVK.
  - All 12,717 Maxwell shader blocks were translated to SPIR-V ahead of time, with the Ryujinx translator (MIT).
  - Textures are untiled from the block-linear layout. ASTC and BCn are sampled natively.
  - Render targets, samplers, views, clears, compute and write-backs are replayed per submit.
- **Screens:** title, file and player select, character select, the opening, the world map and course 1-1 render
  correctly.
- **Speed:** 60 fps in the menus and in course 1-1 (headless test runs). Submits are pipelined: the frame's GPU
  work is not waited for.
- **Button icons:** our own glyph font for the NintendoExtended codepoints (`tools/fonts/make_button_font.py`).
- **Audio:** the sdk's own AudioRenderer (server and DSP code) runs in process.
- **Memory:** 3.5 to 4.2 GB in play.
- **Dump used:** Super Mario Bros. Wonder **1.0.0**, base game, no update, from an XCI of 3,769,099,776 bytes (SHA-256 and the rest in
  [docs/WONDER-DUMP.md](docs/WONDER-DUMP.md)). Other versions are not supported.

### Open

The details are in the GitHub milestones.

- **Windowed stretches at 35-40 fps (#51):** the GPU goes past the frame budget only when a window is visible.
- **Windowed confirmation of the W7 glitch fixes (#43-#45, #47):** horizon band, silhouettes, present mode.
- **Character-select blur:** needs a reference from a real console.
- **Android rendering bugs and performance:** hand-off notes in [docs/nvn/android-perf/](docs/nvn/android-perf/).
- **Linux:** see [Platforms](#platforms).

### Windows

Wonder runs on Windows 10/11 with a Vulkan 1.3 GPU (tested on an NVIDIA RTX 3050 and on Intel). The port is
described in [docs/nvn/WINDOWS-PORT.md](docs/nvn/WINDOWS-PORT.md) and [WINDOWS-PLAN.md](docs/nvn/WINDOWS-PLAN.md),
the PC quality work in [docs/nvn/pc-quality/](docs/nvn/pc-quality/). `dev.ps1` sets up the build environment
(Visual Studio Build Tools, clang, CMake, Ninja, Vulkan SDK) and `tools/pack_pc_wonder.py` packs your own data for
the PC. The recompile, shader and build steps are the same as in the macOS section below.

### Android

The same recompiled code runs on arm64 phones with a native Vulkan 1.3 driver. On a Galaxy S23 (Snapdragon 8 Gen 2)
the game runs with drawn touch controls, a present pass rotated to the display (16:9 with bars), a pipeline cache on
disk and ADPF performance hints. Progress, the test list and the performance phases are in
[docs/nvn/ANDROID-PORT.md](docs/nvn/ANDROID-PORT.md).

Build and install (macOS or Linux, Android SDK with NDK 28.2):

```sh
private/venv/bin/python tools/pack_phone_wonder.py        # your data -> private/wonder/Wonder-data.zip (~4 GB)
CMAKE_BUILD_PARALLEL_LEVEL=4 bash android/build_apk.sh --game wonder   # -> private/wonder/apk/wonder.apk
bash tools/wonder_push_android.sh <adb serial>             # install + data to Internal storage + all-files access
bash tools/wonder_run_android.sh <adb serial> 90 run1      # start, wait, save the log, pull frame dumps
```

Without a computer, copy the APK and the zip to the phone:
1. Extract the zip into Internal storage. You get `Super Mario Bros. Wonder/`.
2. Install the APK.
3. Allow "all files access" when the app asks.

The APK contains recompiled game code built from your own dump. It is for you only: never share or publish it.

The APK has its own launcher (`android/wonder/`, see [docs/WONDER-ANDROID-OPTIONS.md](docs/WONDER-ANDROID-OPTIONS.md)): it imports
the game on the phone from a folder with your `prod.keys` and your XCI/NSP, has the options screen and the touch layout editor.
`LAUNCHER_ONLY=1 bash android/build_apk.sh --game wonder` builds it without the game library (the UI and the import work, Play does not).

#### Launcher, fixes and optimizations (Galaxy S23)

- **Launcher in the Wonder style:** animated background, Lucide icons, and an options screen with a reader behind every row
  (no dead switches). Import the game on the phone from your own `prod.keys` + **XCI or NSP**; the APK carries no game data, keys or firmware.
- **Fixes:**
  - **Button icons showed as `??`.** The launcher now installs a stand-in button font (drawn by `tools/fonts/make_button_font.py`
    on Fredoka, OFL, 56 KB). The title shows "Press (L) + (R) to Start" with the icons.
  - **Closing at once on a POCO F6 (Android 15):** an address range the runtime reserved was already taken on that phone and the
    reservation was fatal; it is now a warning.
  - **Docked mode** (`NX_HANDHELD=0`) really renders 1080p; **mute** and the touch layout keys now work.
- **FPS limit:** 60 keeps the game's own rule (60 fps when the phone keeps up, its own 30 fps mode when heavy); 30 holds the 30 fps
  cadence (steady 33 ms frame, cooler). Console mode: Handheld (720p) or Docked (1080p).
- **Optimization that worked (S23, one scene, one run each):** the "Flower Kingdom" fountain scene ran at **24 fps** with the
  default synchronous submits and at **60 fps** with `NX_PIPELINED=2` ("GPU submits in flight = 2" in Settings > Performance):
  replay 20.6 to 9.7 ms and GPU wait 12.7 to 1.3 ms per frame, because the CPU replay and the GPU no longer run in series.
  It is still **off by default** (it once drew exploded meshes in some courses): turn it on, and go back if the geometry breaks.
  The 30 fps limit and the docked mode have not been timed on a phone yet.
- Measurements, recommended settings, what has and has not run on a device: [docs/WONDER-ANDROID-OPTIONS.md](docs/WONDER-ANDROID-OPTIONS.md).

Touch controls:
- **Right side:** A (jump, big), B, Y (dash), X.
- **Top:** ZR, R, L, + and -.
- **Left half:** a stick that appears where your thumb lands; pushed all the way, it also dashes.

A Bluetooth or USB controller works too, and the touch controls hide while it is in use.

### Build and run (macOS, Apple Silicon)

Requirements: Xcode command line tools, CMake, Ninja, Homebrew `sdl3` + `vulkan-headers` + `vulkan-loader` + `molten-vk`, Python 3 venv
in `private/venv`.

1. **Dump.** Dump your own cartridge (nxdumptool) and keys (Lockpick_RCM). Extract to `private/wonder/exefs` and
   `private/wonder/romfs` with `nstool`.
2. **Recompile the CPU code.** This takes about 6 minutes and 2.4 GB of memory for `main`:
   ```sh
   private/venv/bin/python -m recomp private/wonder/exefs/main private/wonder/gen/main --name main --base 0x4000000000
   private/venv/bin/python -m recomp private/wonder/exefs/sdk  private/wonder/gen/sdk  --name sdk  --base 0x4010000000
   ```
3. **Translate the shaders.** Extract the shader containers with `tools/maxwell/containers.py`, then translate them
   with the harness in `tools/maxwell/spirv_harness`. This writes `private/wonder/spirv/<hash>.spv`. See
   [docs/nvn/TRANSLATOR.md](docs/nvn/TRANSLATOR.md).
4. **Build the button font.** It is local only and never committed:
   ```sh
   private/venv/bin/python tools/fonts/make_button_font.py
   ```
5. **Build.** The first build takes about 15 minutes:
   ```sh
   cmake -S . -B private/wonder/build -G Ninja -DNX_GAME=wonder -DCMAKE_BUILD_TYPE=Release
   ninja -C private/wonder/build -j6 wonder
   ```
6. **Run:**
   ```sh
   NX_LOG=private/wonder/nx.log private/wonder/build/wonder
   ```
   It plays with a gamepad (SDL) or the keyboard.

Useful switches:

| Switch | What it does |
|---|---|
| `NX_MEM_LIMIT_GB=<n>` | memory guard, ends the process past this footprint (default 8) |
| `NX_LANGUAGE=<code>` | the game's language, as the console spells it: `es`, `es-419`, `en-US`, `en-GB`, `fr`, `fr-CA`, `de`, `it`, `nl`, `pt`, `pt-BR`, `ru`, `ja`, `ko`, `zh-Hans`, `zh-Hant` (`es-ES` and other ISO names are accepted as aliases). Falls back to `<data root>/language.txt`, else `en-US` |
| `NX_HEADLESS=1` | no window, for unattended test runs |
| `NX_MUTE=1` | no sound |
| `NX_HANDHELD=1` | handheld mode, 720p |
| `NX_PIPELINED=0` | waits for every submit, as before 60 fps (diagnosis) |

Every debug switch (draw traces, snapshots, bisecting toggles) is listed in [docs/nvn/SWITCHES.md](docs/nvn/SWITCHES.md).

### How it works, and where it is written down

| Document | What |
|---|---|
| [PROGRESS.md](PROGRESS.md) | living log, newest first, with screenshots |
| [docs/NVN.md](docs/NVN.md) | the NVN → Vulkan plan: phases N0-N7, where to cut, ground rules |
| [docs/nvn/WONDER.md](docs/nvn/WONDER.md) | facts about the game: modules, RomFS, recompilation numbers, the host GPU |
| [docs/nvn/SHADERS.md](docs/nvn/SHADERS.md), [TRANSLATOR.md](docs/nvn/TRANSLATOR.md) | shader containers, and Maxwell → SPIR-V |
| [docs/nvn/TILING.md](docs/nvn/TILING.md) | block-linear texture layouts |
| [docs/nvn/DEBUG.md](docs/nvn/DEBUG.md), [DEBUG-2.md](docs/nvn/DEBUG-2.md) | every rendering bug found so far (67 entries): symptom, cause, fix, switch |
| [docs/nvn/GLITCH-PLAN.md](docs/nvn/GLITCH-PLAN.md), [PERF-PLAN.md](docs/nvn/PERF-PLAN.md) | glitch and performance studies and plans |
| [docs/nvn/ANDROID-PORT.md](docs/nvn/ANDROID-PORT.md) | study for the Android port: gaps, risks, milestones A0-A7 |

Ground rules for this track:
- only our own dumps, keys and firmware;
- no leaked NVN headers: the API is reverse engineered from binaries;
- no GPL code (Ryujinx, MIT, is used as a reference and for the offline shader translator);
- Hades must not regress.


## Repository layout

| Path | What |
|---|---|
| `recomp/` | the AArch64 → C++ recompiler (Python) |
| `runtime/` | shared runtime: kernel, IPC, `nn::*` HLE, memory, platform layers (Windows, macOS, Android) |
| `runtime/src/nvn/` | NVN over Vulkan (Wonder) |
| `games/<game>/` | per-game glue (entry, patches, mods) |
| `tools/` | extraction, shader and font tools, packaging |
| `android/` | Android project and build script |
| `docs/` | findings, plans, debug history, screenshots (`docs/media/`) |

## Credits

- **Ryujinx** shader translator (MIT) - used offline to translate the Maxwell shaders to SPIR-V; also a reference for
  the Switch GPU. Ryujinx and yuzu were studied as references only; no GPL code is used.
- **Hephaistos** by Nicolas Busseneau (MIT, copy of the license in
  `games/hades/mods/hephaistos/LICENSE`) - the Lua widescreen mod served to Hades by the shared runtime. It is not
  used by Wonder.
- [SDL 3](https://libsdl.org) (zlib), [MoltenVK](https://github.com/KhronosGroup/MoltenVK) (Apache 2.0) and the
  Vulkan headers and loader (Apache 2.0 / MIT).
- [envytools](https://github.com/envytools/envytools), [Switchbrew](https://switchbrew.org) and the Switch homebrew
  community's documentation.

## Legal

This is a fan project. It is not affiliated with, endorsed by or sponsored by Nintendo. *Super Mario Bros. Wonder* and
*Nintendo Switch* are trademarks of Nintendo. No game files, game code, assets, keys, firmware or fonts are included,
in this repository or in any release. You must provide your own legally obtained copy of the game. The APK or data zip
you build from your own dump contains Nintendo's code and data: it is for you only, never share or publish it.
The API (NVN) is reverse engineered from binaries; no leaked NVN headers are used.

## License

The code in this repository is open source under the [MIT License](LICENSE). Third-party components keep their own
licenses (see Credits). The license covers this project's code only and grants nothing for Nintendo's game.
See [CONTRIBUTING.md](CONTRIBUTING.md) to report bugs or contribute.
