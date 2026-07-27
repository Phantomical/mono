# `mono/mini/llvm/` — new LLVM 14+ tiered JIT backend

New LLVM-related code lives here, kept apart from the legacy backend so the two can coexist during
the port. Design + scope: `.claude/plans/mono-llvm-handoff/` (esp. `06-scope-and-roadmap.md`).

## Target
- Builds and runs against **unmodified system LLVM 18** (`/usr/lib/llvm-18`) — no patched fork.
- amd64 only, JIT only, Linux first (Windows is a later port).
- Tiered: classic mini JIT = tier 0, LLVM = tier 1. Tier-1 compiles run on a pool of background
  worker threads (`MONO_TIERED_COMPILE_THREADS`, default a quarter of the machine capped at 4) and
  concurrently with each other: each compile owns its `LLVMContext`, so no two ever share one.

## Relationship to the legacy backend (`mono/mini/*.c`, staying put)
The legacy backend linked **patched LLVM 6** (`LLVM_API_VERSION=610`) from the vendored
`external/llvm-project` submodule. That submodule has been removed and the legacy sources are
excluded from the build (see `mono/mini/Makefile.am.in`); LLVM is now supplied externally with
`configure --with-llvm=<prefix>` (unmodified upstream 14+). We do not edit those files; we grow the
replacement here.

Legacy files being superseded:
- `mini-llvm.c` / `mini-llvm.h` — the IL→LLVM-IR translator.
- `mini-llvm-cpp.{cpp,h}` — the C++ shim over the LLVM C++ API.
- `llvm-jit.{cpp,h}` — the in-process execution engine (MCJIT/RuntimeDyld → to be rewritten on ORCv2).
- `llvmonly-runtime.{c,h}`, `llvm-runtime.cpp` — AOT/llvmonly only; **deleted** in this scope.

## Language / file conventions
- **Everything here is C++ (`.cpp`) by default.** Only drop to C linkage where code *outside* this
  directory (mono's C sources, e.g. `mini.c`) must consume the symbol.
- **`.hpp`** for headers used only within this directory (C++-only).
- **`.h` with an `extern "C"` boundary** *only* for the small header(s) the rest of mono includes.
  Keep that boundary surface minimal.

## Planned layout (fills in as the port lands)
- `translator*.cpp` — the ported IL→IR translator (donor: dotnet/runtime `mini-llvm.c`, which is
  opaque-pointer-clean and `LLVM_API_VERSION >= 1400`), with AOT/llvmonly/non-amd64 removed. It
  arrived as one 9,496-line file and is now six translation units — `translator.cpp` (entry points,
  `emit_method_inner`, JIT module), `-types`, `-emit`, `-call`, `-bb` (`process_bb` alone),
  `-intrinsics` — sharing `translator-internal.hpp`, which documents the split. `-bb` is still
  ~4,400 lines because `process_bb` is; carving *that* up is a code change, not a move.
- `engine.cpp` (+ `engine.hpp`) — ORCv2 in-process JIT + `MonoJitMemoryManager`. `compile()`
  is **non-destructive**: it JITs a private clone of the caller's module and leaves the
  original intact (mono keeps using it after the call, e.g. `remove_gc_safepoint_poll`), and
  `mono_llvm_create_ee` returns `NULL` (there is no per-EE handle; the engine is a singleton).
- `passes/` — every custom pass, one pair of files each, so neither `engine.cpp` nor the
  translator carries them inline: `inliner.{hpp,cpp}` (the tier-1 inliner that occupies the
  stock inliner's slot in the -O2 pipeline, plus `inliner-support.hpp`, its mono-side
  boundary), `eh-gather.{hpp,cpp}` and `finally-range.{hpp,cpp}` (the two machine-level
  passes `engine.cpp` schedules after `addMachinePasses()` to recover EH clauses and finally
  body ranges), and `pass-dump.{hpp,cpp}` (the opt-in per-pass IR dumper).
- `backend.h` — the single `extern "C"` entry-point header mono's C code includes.
- `depatch.md` — running notes on removing the fork's dependence: `nest` attribute in place of
  `CallingConv::Mono`, consuming stock `.eh_frame`/`.gcc_except_table`.

## Verified LLVM 18 build facts (from the step-1 spike, since removed)
- System LLVM **18.1.3** at `/usr/lib/llvm-18`; use `llvm-config-18`. Dev headers present under
  `/usr/lib/llvm-18/include` (full ORCv2 stack).
- `--shared-mode` is `shared`: a single `libLLVM-18.so`, so `--libs orcjit native` → just `-lLLVM-18`.
  `--system-libs` is empty.
- Compile flags that link + JIT cleanly (ORCv2 `LLJIT`, process symbol resolution working):
  `-std=c++17 -fno-exceptions -funwind-tables -fno-rtti` + `llvm-config-18 --cxxflags` include dir.
  **`-fno-rtti` is required** and NOT emitted by `--cxxflags`: `libLLVM-18` is itself built `-fno-rtti`,
  so subclassing polymorphic LLVM classes (memory managers, `ObjectLinkingLayer::Plugin`) RTTI-on is a
  silent ABI break.
- LLJIT defaults to `RTDyldObjectLinkingLayer` on ELF/amd64 → our custom-allocator/`.eh_frame` hook is
  RTDyld's `MemoryManager` + `registerEHFrames()` (not JITLink's `JITLinkMemoryManager`/plugin).
- `-rdynamic` + `DynamicLibrarySearchGenerator` resolves host symbols for a spike; the real backend
  should register runtime helpers explicitly (`absoluteSymbols`/custom generator) instead.

## Port milestone 1
`mono --llvm hello.exe` executes against unmodified LLVM 18. Tiering (W6/W7) comes after.
