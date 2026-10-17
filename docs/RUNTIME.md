# Runtime (phase 2)

The runtime turns the recompiled modules into a Windows program. Nothing is emulated at the CPU level: it plays
the part of rtld (loading and linking) and of a small Horizon kernel, and replaces system services.

## Games

The runtime is shared; what only one game needs lives in `games/<game>/` and reaches the runtime through
`runtime/include/nx/game.h`: the game's names and program id, and hooks (after the static constructors, when the
renderer starts, on the first frame). `-DNX_GAME=<game>` picks the game (default `hades`); CMake builds one target
of that name. Generated code and data live in `private/<game>/`, except Hades, which keeps `private/` as it was.
`android/build_apk.sh --game <game>` / `android\build_apk.ps1 -Game <game>` build that game's APK
(`games/<game>/AndroidManifest.xml`). `tools/probe.py` shows what a game's ExeFS is made of.

## Modules

Two modules are recompiled and linked into one executable:

| Module | Base | Why |
|---|---|---|
| `main` | `0x40_0000_0000` | the game (Supergiant engine, The Forge, FMOD, Bink, Lua) |
| `sdk` | `0x40_1000_0000` | Nintendo's SDK: musl libc, libc++/libc++abi, nn::os, nn::fs, nn::sf, ... |

The `sdk` has to be recompiled, because `main` inlines libc++'s object layouts (std::string, locale, ...) and
imports them by name. The host C/C++ runtime could not be substituted. `subsdk0` (Nintendo's Vulkan driver) and
`subsdk1` (OpenGL) are not loaded. Their exports are replaced by host Vulkan (phase 3).

`python -m recomp <nso> private/gen/<name> --name <name> --base <addr>` emits the code plus `table.cpp`:
functions, relocations, symbols and init arrays as `nx::ModuleInfo`. The module image (text/ro/data) is read
at startup from `private/analysis/<name>.bin`, produced by `tools/nso2bin.py`. The generated code carries no game
bytes.

## Boot (`runtime/src/main.cpp`)

What rtld's `_start` does on Horizon, reverse engineered from `rtld`:

1. Map each module at its base and zero its bss (`loader.cpp`).
2. Apply relocations. `RELATIVE` → base + addend. `GLOB_DAT`/`JUMP_SLOT`/`ABS64` → by name, looked up in this order:
   **HLE registry** first, then exports of the loaded modules, then a logging stub.
   The `sdk` calls its own exports through its PLT (7,986 `JUMP_SLOT`s), so any SDK function can be replaced by name.
3. Create the main thread (TLS page, 16 MiB stack).
4. Call `nn::init::Start(main thread handle, main base, rtld init, rtld fini)`. Everything after that is
   recompiled SDK code: libc init, static constructors, `nninitStartup`, `nnMain`.

## Address space (`memory.cpp`)

Guest pointers are host pointers. Fixed ranges are reserved at startup and committed on demand:

| Range (Windows, Android) | Preferred range (macOS) | Use |
|---|---|---|
| `0x40_0000_0000` | `0x3_0000_0000` | modules (`NX_BASE`; `sdk` at +0x1000_0000) |
| `0x50_0000_0000` | `0x4_0000_0000` | heap (svcSetHeapSize), up to 6 GiB |
| `0x58_0000_0000` | `0x6_0000_0000` | alias region (svcMapMemory destinations): 64 GiB / 16 GiB |
| `0x68_0000_0000` | `0xA_0000_0000` | stack region |
| `0x6A_0000_0000` | `0xB_0000_0000` | kernel allocations: TLS pages, main stack, shared memory |
| `0x6F_0000_0000` | `0xE_0000_0000` | HLE thunk addresses (never mapped; `nx::call` dispatches them) |

Those macOS addresses were measured on macOS 14.3. On an M4 with macOS 27.0.1, the shared-cache reservation extends
past the entire preferred layout, so reserving the modules at `0x3_0000_0000` fails before boot. Host malloc also
fragments the upper address window differently between launches.

`mac_address_space.cpp` first tries the preferred layout, then reserves free gaps without overwriting any host
mapping (`mach_vm_map`, `VM_FLAGS_FIXED` without `VM_FLAGS_OVERWRITE`). The fallback places the modules in a free
low gap and the heap, alias, stacks and kernel in free gaps between `0x70_0000_0000` and `0x80_0000_0000`. It tries
different heap placements and an 8 GiB alias region if 16 GiB cannot fit. The chosen size is advertised to the game
through the existing `svcGetInfo` path. Every range stays inside Horizon's 39-bit address space.

The layout is initialized on first use, before generated module tables capture `NX_BASE`; recompiled functions use
the same immutable layout. The recompiler still emits `NXB` as `NX_BASE + offset` (`--base` is measured in
the reference layout). Jump tables dispatch on the target minus the module base, leaving case labels constant.
Regenerate both modules when adopting this change: older generated jump tables put `NXB` in their case labels and
cannot compile with a runtime-selected base. `mac_address_space_test` loads SDL and checks initialization order,
non-overlap, 39-bit bounds and writable commits without game data. Apple silicon pages are 16 KiB: commits round
out to them.

`svcQueryMemory` is answered from a Horizon-style memory map (`vm_set`/`vm_query`).
`svcMapMemory` copies instead of aliasing: the source is inaccessible while mapped on Horizon, so a copy on
map and a copy back on unmap behave the same.

## Kernel (`kernel.cpp`)

- **Threads:** one Win32 thread per guest thread, each with its own `nx::CPU` and a TLS page (`TPIDRRO_EL0`).
- **Synchronization:** Horizon semantics, under one kernel lock and condition variable:
  - `ArbitrateLock`/`ArbitrateUnlock` (mutex word = owner tag | waiters bit)
  - `WaitProcessWideKeyAtomic`/`SignalProcessWideKey` (condvars hand the mutex over)
  - `WaitForAddress`/`SignalToAddress`, including SignalAndModifyByWaitingCountIfEqual
- **Events and waiting:** `CreateEvent`/`SignalEvent`/`ClearEvent`/`ResetSignal`, and `WaitSynchronization` on events and threads.
- **Memory and info:** `SetHeapSize`, `QueryMemory`, `MapMemory`, `MapPhysicalMemory`, `GetInfo` (regions, program id, entropy), `GetSystemTick` (19.2 MHz from QPC).
- **System registers:** CNTVCT/CNTFRQ/DCZID. `TPIDR_EL0` and `TPIDRRO_EL0` live in the context.

## Services (`ipc.cpp`)

A HIPC/CMIF framework parses the request in TLS: special header, copy/move handles, PID, X/A/B/W/C descriptors,
and domains. It dispatches to a `Service` object and builds the response, returning interfaces as domain objects
or as new sessions. `sm:` is implemented. Unknown services and commands log and succeed, so boot shows
which ones matter.

## Indirect calls

`nx::call(c, target)` finds the module containing `target` and calls through a dense table (text offset / 4 →
function). Otherwise the target is an HLE thunk. Calling into the middle of a function is fatal and logged with
module+offset and the nearest symbol.
