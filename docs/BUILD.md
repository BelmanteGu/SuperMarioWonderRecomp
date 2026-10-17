# Build guide

How to turn your own dump of Super Mario Bros. Wonder 1.0.0 into a running native build, step by step: Linux (the
complete walk-through), macOS, Windows and Android.

Based on the community guide by [ouni666](https://github.com/ouni666) ([#8](https://github.com/BelmanteGu/SuperMarioWonderRecomp/issues/8)),
which was the first complete Linux recipe. Thank you.

> [!NOTE]
> **No game files are included and none are described here.** You need your own legally dumped copy of Wonder
> **1.0.0** (base game, no update; checks in [WONDER-DUMP.md](WONDER-DUMP.md)) and your own `prod.keys` and
> `title.keys`. How to get them is not covered. Everything built from them stays in `private/` (gitignored). Never
> share the generated code, the shaders, the data zip or an APK.

## The pipeline

Every platform runs the same five stages. Only the tools differ.

| # | Stage | Input | Output | Time (reference) |
|---|---|---|---|---|
| 1 | Import | XCI + keys | `private/wonder/{analysis,exefs,extract}` | 5 min |
| 2 | Recompile the CPU code | `exefs/main`, `exefs/sdk` | `private/wonder/gen/` (C++, about 1.4 GB) | 7 min |
| 3 | Translate the shaders | `extract/romfs`, `analysis/main.bin` | `private/wonder/spirv/*.spv` (12,751 files) | 10 to 15 min |
| 4 | Button font | `android/wonder/assets/fonts/shared_5.ttf` | `private/wonder/fonts/` | seconds |
| 5 | Build and run | all of the above | `private/wonder/build/wonder` | 40 min (Linux, `-j6`) |

All paths below are relative to the repository root, and every command is run from there.

---

## Linux (x86-64)

Tested on Ubuntu 26.04 with an NVIDIA RTX 3050 laptop GPU. **Nothing here needs root** except installing system
packages; every tool that is not in your distribution can live under `private/third_party/`.

### 0. What you need

| Tool | Version | Notes |
|---|---|---|
| clang, clang++ | 21 used | The recompiled code and the runtime are built with clang. `CMakeLists.txt` has no GCC-specific flags (only a `Clang` branch for warnings) and GCC has not been tried on the 1.4 GB of generated code, so use clang. ouni666's guide says "gcc 16+"; that is not a requirement of this repository. |
| CMake | 3.25+ | |
| Ninja | any | |
| `patch` | any | Used by the shader translator's csproj on non-Windows ([TRANSLATOR.md](nvn/TRANSLATOR.md)). Ubuntu: package `patch`. |
| Vulkan loader | any | `libvulkan1` plus a driver with Vulkan 1.3 |
| **Vulkan-Headers 1.4.357** | exactly this or newer | `runtime/src/vk_gen.h` was generated from it. Ubuntu 26.04 ships 1.4.341, which misses types and fails (see Troubleshooting). Use `-DNX_VULKAN_INCLUDE`. |
| SDL3 | 3.2.x | The distribution's if it has it, else build it as below. |
| .NET SDK | 10.0.401 | Tarball, no root. |
| Python | 3.11+ with `venv` | pip packages: `capstone lz4 zstandard fonttools` |
| `spirv-tools` | optional | Only for the `spirv-val` check of step 6. |
| curl, tar, git | | |

Ubuntu/Debian packages (needs root once; skip what you have):

```sh
sudo apt install build-essential clang cmake ninja-build patch git curl python3-venv spirv-tools \
     libvulkan1 libvulkan-dev \
     libx11-dev libxext-dev libxcursor-dev libxi-dev libxrandr-dev libxfixes-dev libxss-dev libxkbcommon-dev \
     libpulse-dev libasound2-dev
```

The `lib*-dev` ones are only for building SDL3 yourself. Without root, ask for them, or use a container.

You do not need to put the keys or the XCI in the repository root. The importer takes paths, so keep them anywhere
(the examples use `~/wonder/`). Do not copy them into the repository by accident: only `private/` is gitignored.

### 1. Clone and fetch the tools

```sh
git clone https://github.com/BelmanteGu/SuperMarioWonderRecomp
cd SuperMarioWonderRecomp
mkdir -p private/wonder private/build private/third_party
T=$PWD/private/third_party
```

**Vulkan headers 1.4.357:**

```sh
curl -L https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.357.tar.gz | tar -xz -C $T
ls $T/Vulkan-Headers-1.4.357/include/vulkan/vulkan.h
```

**SDL3 3.2.24** (skip if `pkg-config --modversion sdl3` already says 3.2 or newer; then drop `-DCMAKE_PREFIX_PATH` below).
Built here with X11 and PulseAudio/ALSA; Wayland was off because its headers were missing, so on a Wayland desktop
the window goes through XWayland. Install `libwayland-dev wayland-protocols libxkbcommon-dev libegl-dev` first if you
want native Wayland:

```sh
curl -L https://github.com/libsdl-org/SDL/releases/download/release-3.2.24/SDL3-3.2.24.tar.gz | tar -xz -C $T
cmake -S $T/SDL3-3.2.24 -B $T/SDL3-3.2.24/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$T/sdl3 \
      -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake --build $T/SDL3-3.2.24/build && cmake --install $T/SDL3-3.2.24/build
```

**.NET SDK 10.0.401** (tarball; SHA-512 on Microsoft's release page):

```sh
mkdir -p $T/dotnet
curl -L https://builds.dotnet.microsoft.com/dotnet/Sdk/10.0.401/dotnet-sdk-10.0.401-linux-x64.tar.gz | tar -xz -C $T/dotnet
export DOTNET_ROOT=$T/dotnet PATH=$T/dotnet:$PATH DOTNET_CLI_TELEMETRY_OPTOUT=1
dotnet --version          # 10.0.401
```

**Ryujinx shader translator source** (Ryubing fork, MIT; commit `a82350bb77`, from Codeberg):

```sh
mkdir -p $T/ryubing
curl -L https://codeberg.org/Software_arch/Ryubing/archive/a82350bb77.tar.gz | tar -xz -C $T/ryubing --strip-components=1
```

**Python venv:**

```sh
python3 -m venv private/venv
private/venv/bin/pip install --upgrade pip
private/venv/bin/pip install capstone lz4 zstandard fonttools
```

If `venv` fails with "ensurepip is not available" (Ubuntu without `python3-venv`, and no root): create it without
pip and bootstrap pip, or use `uv`:

```sh
python3 -m venv --without-pip private/venv
curl -L https://bootstrap.pypa.io/get-pip.py -o private/get-pip.py && private/venv/bin/python private/get-pip.py
# or: uv venv private/venv && uv pip install --python private/venv/bin/python capstone lz4 zstandard fonttools
```

### 2. Import the game

The importer is the same code that imports on the phone, built for the host. With `--exefs` it also writes the raw
NSOs the recompiler reads, so **`nstool` is not needed** (the old macOS instructions used it).

```sh
cmake -S android/wonder/importer -B private/build/importer -G Ninja && ninja -C private/build/importer
sha256sum ~/wonder/MarioWonder.xci      # must be 21395a2c0707df3d1fe4f62718b3be3c3a4abafbb53006ab964571b2c90fd426
private/build/importer/nximport --exefs --out private/wonder ~/wonder/prod.keys ~/wonder/title.keys ~/wonder/MarioWonder.xci
```

Check: it ends with `import complete: 19012 RomFS files, 3.73 GB` (about 5 minutes) and you have
`private/wonder/analysis/{main.bin,sdk.bin,main.npdm}`, `private/wonder/exefs/{main,sdk}` and
`private/wonder/extract/romfs/`. Put only the 1.0.0 dump in the folder: an update is applied and then `main` no longer
matches ([WONDER-DUMP.md](WONDER-DUMP.md)).

### 3. Recompile the CPU code

```sh
private/venv/bin/python -m recomp private/wonder/exefs/main private/wonder/gen/main --name main --base 0x4000000000
private/venv/bin/python -m recomp private/wonder/exefs/sdk  private/wonder/gen/sdk  --name sdk  --base 0x4010000000
```

Checks: `main` ends with `emitted ... 209629/209629`-style progress and `done: 1198 files, handled 16304252, unhandled 0`
(about 6 minutes, 2.4 GB of memory); `sdk` ends with `40395` functions, `127 files`, `unhandled 0` (about 30 s). Any
`unhandled` above 0 means a wrong dump or a wrong checkout.

### 4. Build the shader translator

```sh
export DOTNET_ROOT=$PWD/private/third_party/dotnet PATH=$PWD/private/third_party/dotnet:$PATH DOTNET_CLI_TELEMETRY_OPTOUT=1
dotnet build tools/maxwell/spirv_harness/SpirvHarness.csproj -c Release -m:2
```

About 2 to 8 minutes. Warnings `NU1900` (cannot reach the Ryujinx NuGet feed) are harmless; the harness builds
`Ryujinx.Graphics.Shader` from the checkout in `private/third_party/ryubing`. It needs `patch`. The result is
`private/build/spirv_harness/bin/Release/net10.0/spirv_harness.dll`.

### 5. Extract the shader containers (all of them)

> [!IMPORTANT]
> **Every group is needed.** If a group is missing, the shaders it holds are not translated and the game draws
> black (or nothing) wherever they are used. The community guide missed `secred` and the 103 `layout_*` groups,
> which hold 760 and 13,530 shader stages (see [SHADERS.md](nvn/SHADERS.md), "Where the shaders are").

The groups: `uber`, `secred`, `agl`, `gsys`, one `effect_<name>` for each of the 33 `Effect/*.esetb.byml.zs` (skip
`EffectFileInfo.*.byml.zs`: no shaders), one `layout_<name>` for each of the 234 `Layout/*.blarc.zs` (only 103 contain
shaders; for the others `containers.py` prints "no shader container recognised", which is harmless and their empty
folder is removed), and finally `exefs`: 34 blocks that live inside `main` and in no container. Expect **141 groups**.

The script does all of it, in parallel (`JOBS=4 tools/extract_shaders.sh` to choose):

```sh
tools/extract_shaders.sh
```

The same with plain commands:

```sh
P=private/venv/bin/python
X=tools/maxwell/containers.py
R=private/wonder/extract/romfs
O=private/wonder/shaders_extracted
mkdir -p $O

$P $X $R/Shader/SecredUber.Product.100.product.Nin_NX_NVN.bfsha.zs --extract $O/uber   --unique
$P $X $R/Shader/Secred.Nin_NX_NVN.release.sarc.zs                  --extract $O/secred --unique
$P $X $R/Lib/agl/agl_resource.Nin_NX_NVN.release.sarc.zs           --extract $O/agl    --unique
$P $X $R/Lib/gsys/gsys_resource.Nin_NX_NVN.release.sarc.zs         --extract $O/gsys   --unique

for f in $R/Effect/*.Nin_NX_NVN.esetb.byml.zs; do
    n=$(basename "$f" .Nin_NX_NVN.esetb.byml.zs)
    $P $X "$f" --extract "$O/effect_$n" --unique
done
for f in $R/Layout/*.Nin_NX_NVN.blarc.zs; do
    n=$(basename "$f" .Nin_NX_NVN.blarc.zs)
    $P $X "$f" --extract "$O/layout_$n" --unique
done
# layouts without shaders leave an empty folder; drop them
for d in $O/layout_*/; do [ -n "$(ls -A $d | grep -v manifest.tsv)" ] || rm -rf "$d"; done

# last: the blocks embedded in main (skips what the other manifests already have)
$P tools/maxwell/exefs_shaders.py private/wonder/analysis/main.bin --extract $O/exefs
```

Check: `ls -d private/wonder/shaders_extracted/*/ | wc -l` gives 141. About 2 minutes and 94 MB of extracted code.

### 6. Translate the shaders to SPIR-V

```sh
H="dotnet private/build/spirv_harness/bin/Release/net10.0/spirv_harness.dll"
$H --batch private/wonder/shaders_extracted --out private/wonder/spirv --jobs 3 --spirv-val /usr/bin/spirv-val
```

`--spirv-val` is the validator (package `spirv-tools`); the default path is the Homebrew one, so pass yours, or pass
`--no-val` to skip validation (faster, same output). The batch takes 2 to 5 minutes. Checks:

- the first line of `private/wonder/spirv/summary.txt` reads `blocks 12751 (stages using them: 47831)`;
- `ls private/wonder/spirv/*.spv | wc -l` gives **12751**;
- with the validator on, no `invalid` and no `exception` rows in the per-group table of `summary.txt`.

If you extracted fewer groups you get fewer files. 12,751 is the right number for 1.0.0.

### 7. Button font

```sh
mkdir -p private/wonder/fonts
cp android/wonder/assets/fonts/shared_5.ttf private/wonder/fonts/
```

That is the stand-in font for the Switch button glyphs. No Arial and nothing from the host is needed.
(`tools/fonts/make_button_font.py`, used in the macOS section, draws the same kind of font from a system font.)

### 8. Configure and build

```sh
T=$PWD/private/third_party
CC=clang CXX=clang++ cmake -S . -B private/wonder/build -G Ninja -DNX_GAME=wonder -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH=$T/sdl3 -DNX_VULKAN_INCLUDE=$T/Vulkan-Headers-1.4.357/include
ninja -C private/wonder/build -j6 wonder
```

About 40 minutes and 1,408 targets. The generated code is about 1.4 GB of C++: keep `-j` at or below
(RAM in GB) / 2; 14 GB is enough at `-j6`. The result is `private/wonder/build/wonder`. Ninja resumes where it stopped.

### 9. Run

```sh
tools/wonder_linux.sh                 # quality: docked 1080p
tools/wonder_linux.sh balanced        # renders 720p (handheld) and upscales with FSR 1
tools/wonder_linux.sh performance     # balanced with softer sharpening
tools/wonder_linux.sh intel           # integrated GPU: 720p, pipelined submits, 30 fps cap
tools/wonder_linux.sh lightest        # intel plus FSR 1
```

F11 or Alt+Enter toggles full screen. The log is `private/wonder/nx_play.log`. Useful environment variables:
`NX_LANGUAGE=zh-Hans` (the Switch's own language codes, or a name such as `spanish`; or put it in a
`language.txt` in the data folder), `NX_ULTRAWIDE=1`, `NX_FPS_CAP=30`, `NX_RESOLUTION=WxH`, `NX_MUTE=1`,
`NX_GPU=integrated|discrete`, `NX_MEM_LIMIT_GB`. The README and the notes in [nvn/studies/](nvn/studies/) explain
them. Known graphics problems (black silhouettes on Intel iGPUs, some flicker) are listed under "Open" in the README.

---

## macOS (Apple Silicon)

The reference platform. The steps are the ones in the README's "Build and run (macOS, Apple Silicon)" section; this
page only adds what changed since. The stages are the same as on Linux:

1. **Tools:** Xcode command line tools, CMake, Ninja, Homebrew `sdl3 vulkan-headers vulkan-loader molten-vk
   spirv-tools`, a Python venv (`capstone lz4 zstandard fonttools`), .NET SDK 10.0.401 and the Ryubing source
   exactly as in [nvn/TRANSLATOR.md](nvn/TRANSLATOR.md) ("Setup (macOS 14, arm64)"; the tarball is the `osx-arm64` one).
2. **Import:** the README says to extract with `nstool`; the importer above does the same job and also unpacks the
   RomFS. Build it with the same two lines (`cmake -S android/wonder/importer ...`) and run `nximport --exefs`.
3. **Recompile:** the two `python -m recomp` commands (Linux step 3).
4. **Shaders:** build the harness and extract and translate as in Linux steps 4 to 6 (`tools/extract_shaders.sh`
   works on macOS too; the harness' default `spirv-val` path is the Homebrew one, so `--spirv-val` can be left out).
5. **Font:** `private/venv/bin/python tools/fonts/make_button_font.py`, or the `cp` of Linux step 7.
6. **Build:** `cmake -S . -B private/wonder/build -G Ninja -DNX_GAME=wonder -DCMAKE_BUILD_TYPE=Release`, then
   `ninja -C private/wonder/build -j6 wonder`.
7. **Run:** `tools/wonder_mac.sh [quality|balanced|performance]`.

## Windows 10/11 (x86-64)

Needs a Vulkan 1.3 GPU. The full description is in [nvn/WINDOWS-PORT.md](nvn/WINDOWS-PORT.md) ("W0 in detail"); in
short:

1. **Tools:** Git, Visual Studio 2022 Build Tools ("Desktop development with C++" and "C++ Clang tools for Windows"),
   CMake 3.28+, Ninja, Python 3.11+, the LunarG Vulkan SDK 1.4.x (it sets `VULKAN_SDK`; use 1.4.357 or newer) and the
   .NET SDK 10.0.401. `. .\dev.ps1` sets up the compiler environment.
2. **Data:** either do the import and the shader steps on the PC (the pieces are portable: the importer builds with
   CMake, the Python tools run from `private\venv\Scripts\python`, and [TRANSLATOR.md](nvn/TRANSLATOR.md) has the
   "Setup (Windows)" commands for the harness, with `tools/maxwell/apply_patch.py` standing in for `patch`), or
   build the data on a Mac or Linux machine and pack it with `private/venv/bin/python tools/pack_pc_wonder.py`
   (a 4 GB zip, extracted in the repository root on the PC).
3. **Recompile** with the two `python -m recomp` commands (Linux step 3), using `private\venv\Scripts\python`.
4. **Shaders:** all groups of Linux step 5, then the batch (Linux step 6) with
   `--spirv-val C:\VulkanSDK\<version>\Bin\spirv-val.exe`. `tools/extract_shaders.sh` is bash; on Windows use the
   plain commands of that step (they are listed in PowerShell form in TRANSLATOR.md).
5. **Build**, from an "x64 Native Tools Command Prompt for VS 2022":
   ```powershell
   cmake -S . -B private\wonder\build -G Ninja -DNX_GAME=wonder -DCMAKE_BUILD_TYPE=Release `
         -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
   ninja -C private\wonder\build -j6 wonder
   ```
6. **Run:** `tools\wonder_run.ps1`, or `private\wonder\build\wonder.exe` directly (`NX_LOG`, `NX_MEM_LIMIT_GB`).

The Windows steps were written from the repository's own docs and were not re-run for this guide.

## Android (arm64)

The same recompiled code runs on arm64 phones with a Vulkan 1.3 driver. The APK is **for your private use only**: it
contains code and shaders derived from your dump. Details and the phone-side steps are in the README's "Android"
section and [ANDROID.md](ANDROID.md) / [nvn/ANDROID-PORT.md](nvn/ANDROID-PORT.md); the APK has its own launcher that
imports the game on the phone from a folder with `prod.keys` and the XCI/NSP ([WONDER-ANDROID-OPTIONS.md](WONDER-ANDROID-OPTIONS.md)).

Build on Linux (verified) or macOS. You need steps 2 to 6 of the Linux part first (the APK bundles the translated
`private/wonder/spirv`) and:

1. **JDK 21** (a tarball is fine) and the **Android command-line tools**; then, with `ANDROID_HOME` pointing at the SDK:
   ```sh
   export JAVA_HOME=$PWD/private/third_party/jdk ANDROID_HOME=$PWD/private/third_party/android-sdk
   $ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager --licenses
   $ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager "ndk;28.2.13676358" "build-tools;35.0.0" "platforms;android-35"
   ```
   (put the unzipped command-line tools in `.../cmdline-tools/latest/`).
2. **Vulkan headers:** `VULKAN_SDK` must name a folder that has **`Include/`** (capital I) with the 1.4.357 headers:
   ```sh
   mkdir -p private/third_party/vk-android && cp -r private/third_party/Vulkan-Headers-1.4.357/include private/third_party/vk-android/Include
   export VULKAN_SDK=$PWD/private/third_party/vk-android
   ```
3. **Build** (about 1 hour; the script needs `cmake` and `ninja` on the PATH):
   ```sh
   CMAKE_BUILD_PARALLEL_LEVEL=4 bash android/build_apk.sh --game wonder     # -> private/wonder/apk/wonder.apk
   ```
4. **Data and install:** `private/venv/bin/python tools/pack_phone_wonder.py` makes `private/wonder/Wonder-data.zip`
   (about 4 GB), and `bash tools/wonder_push_android.sh <adb serial>` installs everything over adb. Or copy the zip and
   the APK to the phone by hand (README, "Android").

`LAUNCHER_ONLY=1 bash android/build_apk.sh --game wonder` builds the launcher without the game library (to try the UI).

---

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| **Black screen** (the window opens, the game runs, nothing or only black blocks are drawn) | Shaders are missing. Check that `private/wonder/spirv` has 12,751 files and that step 5 covered every group (`secred`, the 103 `layout_*`, `exefs`). Re-run `tools/extract_shaders.sh`, then the batch, then relaunch (no rebuild needed: the runtime reads the `.spv` at start). |
| Compile error `unknown type name 'PFN_vkClearShaderInstrumentationMetricsARM'` or other missing `Vk...` types | Vulkan headers older than 1.4.357 (Ubuntu 26.04 has 1.4.341). Download 1.4.357 (step 1) and configure with `-DNX_VULKAN_INCLUDE=<...>/Vulkan-Headers-1.4.357/include`; delete `private/wonder/build/CMakeCache.txt` if the old path is cached. |
| `ensurepip is not available` | See the end of Linux step 1: `python3 -m venv --without-pip` plus `get-pip.py`, or `uv`. |
| `Could NOT find SDL3` | Pass `-DCMAKE_PREFIX_PATH=<sdl3 install prefix>`. |
| `patch: command not found` while building the harness | Install `patch` (Windows uses `tools/maxwell/apply_patch.py` automatically). |
| `dotnet: command not found` or the harness picks a different SDK | `export DOTNET_ROOT=...` and `PATH=$DOTNET_ROOT:$PATH` in the shell you build and run in (they do not persist). |
| `spirv-val: No such file` in the batch | Pass your `--spirv-val <path>` (the default is Homebrew's) or `--no-val`. |
| `ExeFS block N fails its check` in the importer | A damaged dump or copy. Check the XCI size and SHA-256 ([WONDER-DUMP.md](WONDER-DUMP.md)). |
| `unhandled` above 0 after recompiling | Not a 1.0.0 dump (an update was applied). |
| Out of memory or the machine freezes while building | Lower `-j` (RAM in GB / 2). `ninja` continues where it stopped. |
| `/tmp` fills up when dumping frames (`NX_DUMP_FRAMES` and the other `NX_DUMP_*` debug switches) | `/tmp` is a RAM-backed tmpfs on many distributions and the dumps are large. Check where the dump is written (`runtime/src/nvn/nvn_debug.cpp`), keep it on a disk path, and delete old dumps. |
| Window on a Wayland desktop is blurry or in XWayland | SDL3 was built without Wayland (headers were missing). Install the Wayland dev packages and rebuild SDL3 to change it. |
| Black silhouettes on an Intel integrated GPU | Known and not fixed: [nvn/studies/summary-1710.md](nvn/studies/summary-1710.md). Use `tools/wonder_linux.sh intel`. |
| Some flicker or black blocks in places | Known rendering bugs; see "Open" in the README and [nvn/GLITCH-PLAN.md](nvn/GLITCH-PLAN.md). Report with the log. |
| Android build: `VULKAN_SDK` or `vulkan.h` not found | `VULKAN_SDK/Include` must exist with a capital I (Linux file systems are case sensitive). |
| Android build: NDK or build-tools not found | They must be exactly `ndk/28.2.13676358` and `build-tools/35.0.0` under `ANDROID_HOME`. |

## Checks at a glance

| Step | Expected |
|---|---|
| XCI | 3,769,099,776 bytes, SHA-256 `21395a2c...fd426` |
| Import | 19,012 RomFS files, 3.73 GB |
| Recompile `main` | 209,629 functions, 1,198 files, `unhandled 0` |
| Recompile `sdk` | 40,395 functions, 127 files, `unhandled 0` |
| Shader groups | 141 |
| SPIR-V | 12,751 files (47,831 stages) |
| Build | 1,408 targets, `private/wonder/build/wonder` |

## Credits

Based on the community guide by **ouni666** ([#8](https://github.com/BelmanteGu/SuperMarioWonderRecomp/issues/8)).
Corrections and additions are welcome as issues or pull requests.
