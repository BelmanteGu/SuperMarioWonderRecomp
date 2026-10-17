# The AArch64 → C++ recompiler

`python -m recomp <module.nso> <out dir>` turns a Switch NSO into C++ that builds with clang-cl.
On Hades' `main` (1.61M instructions) it takes about a minute, and the output builds into one static library.

## Pipeline

| Step | File | What |
|---|---|---|
| Load | `recomp/nso.py` | NSO segments (LZ4), MOD0, `.dynamic`, `.dynsym`, RELA/JMPREL relocations, init/fini arrays |
| Function bounds | `recomp/ehframe.py` | `.eh_frame_hdr` → every FDE's start and size (10,824 of main's functions, 96.3% of text, no overlaps) |
| Analysis | `recomp/analysis.py` | seeds (FDEs, BL targets, RELATIVE relocs that point into text, init_array, exports, ADR code pointers), PLT stubs → import names, jump tables, a fixpoint that promotes out-of-range branch targets to entries |
| Emission | `recomp/emit.py` | one C++ function per guest function |
| Driver | `recomp/__main__.py` | `funcs.h`, `code_NNN.cpp` (about 12k instructions each), `table.cpp` (guest offset → function), `imports.cpp` (GOT slot → import name), `report.txt` |
| Runtime header | `runtime/include/nx/cpu.h` | CPU context, flags, memory, exclusive monitor, FP/NEON helpers |

## Design

- **Guest memory is host memory.** The module is mapped at `NX_BASE` (default `0x40_0000_0000`; selected and reserved
  at startup on macOS to avoid the system's shared cache and heap).
  ADRP/ADR/literals are emitted as `NX_BASE + offset`, so every guest pointer is a host pointer. There is no translation and no byte swap (both sides are little endian).
- **Context.** `nx::CPU` holds x0–x30 and SP (`x[31]`), v0–v31 as `V128` unions, NZCV as four bytes, FPCR/FPSR, TPIDR/TPIDRRO and the exclusive monitor.
- **Control flow.** Branches inside a function become `goto`. A branch out of the function is a tail call. `BL` is a direct call.
  `BLR` and unknown `BR` go through `nx::call`, which looks up the runtime's function table. A jump table becomes a `switch` over the computed target, with `nx::call` as the fallback.
- **Jump tables.** Two shapes are resolved:
  - clang's compressed tables: `adr anchor; ldrb/ldrh [table, idx]; add anchor, entry, lsl #2; br`
  - table-relative `ldrsw` + `add base` tables, from another compiler

  The bound comes from `cmp idx, #N` plus `b.hi`/`b.hs`. On main this finds 801 tables. One table is left, with its base in another block. It falls back to `nx::call`.
- **Exclusives.** `LDXR`/`LDAXR` record (address, value). `STXR`/`STLXR` do a compare-exchange against that value. `LDAR`/`STLR` are acquire/release atomics. `DMB`/`DSB` are full fences.
- **Scalar writes to SIMD registers clear the upper bits**, as on hardware.
- **FP.** Host SSE in its default mode matches FPCR's default (round to nearest, no flush). Conversions to integer saturate like AArch64 (NaN → 0).
  `FMAXNM`/`FMINNM` follow IEEE maxNum: a sNaN propagates, a qNaN is a missing value, and +0 > −0. `FMADD` and family use `std::fma`.

## Validation: `tests/difftest.py`

Samples every instruction *shape* in the module: mnemonic × operand kinds × arrangement × index/shift/extend × addressing mode.
Each sample runs natively on random states, and on **Unicorn** as the reference. The test then compares x0–x30, SP, v0–v31, NZCV and a memory window.
Base registers of memory instructions point into a buffer mapped at the same address on both sides.

On main: **950 shapes, 1,800 tests, 43,200 runs, 0 mismatches**. NaN payloads are compared as "both NaN", and `FRECPE`/`FRSQRTE` are allowed to be more precise than the hardware estimate.

Bugs it caught:
- post-index structured loads and stores (`ld1r/ld2/st2/ld4/st4`): Capstone drops the immediate, and the recompiler now derives it from the transfer size
- `FMAXNM`/`FMINNM` with a sNaN
- `BIC`/`ORR` vector immediate on 64-bit arrangements (the upper half was not cleared)

### Running it on macOS / Linux

`python -m tests.difftest <module.nso> <work dir>` builds the runner with `clang++` (extra flags from
`NX_DIFFTEST_CFLAGS`, e.g. `-DNX_SOFT_CRYPTO` to test the software crypto path) and uses `mmap` + signals instead
of `VirtualAlloc` + SEH. The scratch buffer sits at 0x3_0000_0000 there (macOS arm64 reserves the low 4 GiB).
Unicorn runs as CPU model MAX, so ARMv8.1 atomics and the crypto extension are emulated.

On Super Mario Bros. Wonder (SDK 16): `main` 2,414 tests / 28,968 runs, `sdk` 3,472 tests / 55,552 runs,
0 mismatches. Bugs it caught there: vector `clz`/`cls` emitted as general-purpose ops; `frsqrts`/`frecps` not fused
(hardware rounds `(3 - a*b) / 2` once, without overflowing in between); `scvtf` vector with fractional bits.

## Instruction coverage added for SDK 16 (Wonder)

AdvSIMD scalar forms (one lane of b/h/s/d), `fcvt*` into a SIMD register, pairwise min/max (`fmaxp`, `sminp`, ...),
`sminv/uminv`, `facge/facgt`, `sqrdmulh`, halving subtract, `srshl`, `rshrn`, `uabdl/sabdl`, `umlsl/smlsl`,
`sadalp/uadalp`, `ld2r/ld3r/ld4r`, exclusive pairs (`ldxp/stxp`, 128-bit compare-exchange), all ARMv8.1 LSE
atomics (`cas`, `casp`, `ldadd`/`ldclr`/`ldeor`/`ldset`/`ld[su]{max,min}`, `st*`, `swp`, emitted as seq_cst
`__atomic` builtins) and the crypto extension (AES, SHA-1, SHA-256, PMULL.1Q in `nx/cpu.h`: host instructions on
arm64 with +aes/+sha2, a software version from the ARM pseudocode elsewhere).

## Known gaps

- Branches, calls and exclusives are not covered by the differential test (they need a whole-function harness).
- One jump table is unresolved (0x1ac780, its base is in another block).
- FPCR is not honoured: no rounding-mode or flush-to-zero changes. Hades never writes FPCR in main.
- Performance: registers live in the context, so every access is a memory access. Caching registers in locals comes later.
