# `mono/mini/llvm/` — new LLVM 14+ tiered JIT backend

New LLVM-related code lives here, kept apart from the legacy backend so the two can coexist during
the port. Design + scope: `.claude/plans/mono-llvm-handoff/` (esp. `06-scope-and-roadmap.md`).

## Target
- Builds and runs against **unmodified system LLVM 18** (`/usr/lib/llvm-18`) — no patched fork.
- amd64 only, JIT only, Linux first (Windows is a later port).
- Tiered: classic mini JIT = tier 0, LLVM = tier 1 (synchronous for the first milestone; background
  thread + full concurrency deferred).

## Relationship to the legacy backend (`mono/mini/*.c`, staying put)
The legacy backend links **patched LLVM 6** (`external/llvm-project`, `LLVM_API_VERSION=610`) and still
builds/runs today. We do not edit those files yet; we grow the replacement here and switch the build
over once it stands on its own.

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
- `translator.cpp` — the ported IL→IR translator (donor: dotnet/runtime `mini-llvm.c`, which is
  opaque-pointer-clean and `LLVM_API_VERSION >= 1400`), with AOT/llvmonly/non-amd64 removed.
- `engine.cpp` (+ `engine.hpp`) — ORCv2 in-process JIT + `MonoJitMemoryManager`.
- `backend.h` — the single `extern "C"` entry-point header mono's C code includes.
- `depatch.md` — running notes on removing the fork's dependence: `nest` attribute in place of
  `CallingConv::Mono`, consuming stock `.eh_frame`/`.gcc_except_table`.

## Port milestone 1
`mono --llvm hello.exe` executes against unmodified LLVM 18. Tiering (W6/W7) comes after.
