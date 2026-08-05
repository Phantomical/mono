## What this repo is

Unity Technologies' fork of Mono (branch `unity-main`). The **active work** here is a
new **LLVM-only JIT** under `mono/llvm/` (greenfield, ORCv2, a direct CIL→LLVM-IR
translator in `mono/llvm/method-to-llvm/`), developed on branch `llvm18-tiered-jit`.
It is the runtime's only JIT: every compile routes through it, the classic mini
back end and the older tiered backend under `mono/mini/llvm/` (now a parts bin) never
engage, and `--llvm`/`--nollvm` are deprecated no-ops. It builds against
**unmodified upstream LLVM 22** — a local
RelWithDebInfo+assertions build of `llvmorg-22.1.8` from
`~/projects/llvm-project`, installed at `~/projects/llvm-project/install` — not
a patched fork, and not the removed `external/llvm-project` submodule. (Check
`llvm-config --version` there says 22.x; the prefix once carried a stale 18
install.) Assertions being ON is deliberate: it catches API misuse the distro
LLVM silently tolerated.

Project scope constraints: amd64 + Linux first, JIT only (AOT/llvmonly deleted from this
scope), unmodified LLVM. The design doc is `.claude/plans/orc-direct-multitier.md`.

## Build

The build is **CMake + Ninja**. autotools is gone — there is no `autogen.sh`,
`configure`, or `Makefile.am` any more, and `mcs/` is the only make-driven part left
(its own hand-written system, which CMake invokes rather than reimplements). Full
from-scratch instructions are in `build.md` (Linux/WSL2, system toolchain); the short
version, **with the LLVM tier enabled**:

```bash
cd /home/swlynch/projects/mono

cmake -S . -B build -G Ninja \
  -DMONO_LLVM_PREFIX="$HOME/projects/llvm-project/install" \
  -DCMAKE_INSTALL_PREFIX="$PWD/tmp" \
  -DMONO_USE_SYSTEM_RUNTIME_FOR_TOOLS=ON \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

cmake --build build -j"$(nproc)"
```

**Always pass both of these.** The build does not turn either on by itself.

`MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS=ON` compiles all C# on the mono already installed
on the machine rather than the one being built. Hosting Roslyn on the in-tree runtime
costs roughly 4x on ordinary sources and is pathological on generated ones — one
CoreCLR CSE fixture took eight hours and then died, against 88 seconds on the system
mono — and it puts every C# compile downstream of the native link, so a one-line
change under `mono/mini` relinks the runtime before any C# can be compiled at all.
Leave it on unless you are deliberately investigating the in-tree runtime as a
compiler host.

`ccache` covers the native half: a rebuild after switching branches or toggling a
cache variable re-runs the compiler over ~1500 C/C++ sources, and most of those
translation units have not actually changed. `CMAKE_*_COMPILER_LAUNCHER` is the right
knob rather than `CC="ccache cc"`, which CMake's own compiler checks trip over.
Confirm it is working with `ccache -s` — the hit rate should climb across rebuilds.
When a launcher naming `ccache` is set, configure makes the cache shareable across
worktrees on its own: it wraps the launcher so `CCACHE_BASEDIR` points at the source
root (which relativises paths in the hash) and passes
`-ffile-prefix-map=<source root>=/mono` (which keeps `-g` from baking this tree's
path into the object, and into the hash with it). Nothing needs setting in
`ccache.conf`. The cost is that debug info names `/mono` rather than the tree it was
built in: gdb started from the build directory still finds the sources, from anywhere
else it wants `set substitute-path /mono <worktree>`.

Key facts:
- `-DMONO_LLVM_PREFIX=<prefix>` is what defines `ENABLE_LLVM`; empty (the default) means
  no LLVM tier. It replaces `--with-llvm=`. The defaults already match Unity's desktop
  Linux configuration, so the other old `--enable`/`--with` flags have no counterpart to
  pass. Configure ends with a summary naming the collector, the LLVM prefix and the
  class-library profiles — read it rather than guessing.
- Changing a cache variable just means re-running `cmake -S . -B build`. There is no
  `config.status` to delete, and no stale-config trap.
- Needs an **existing `mono`/`mcs` in `PATH`** to bootstrap the C# class libraries.
- Artifacts move into the build tree: `build/mono/mini/mono-sgen`,
  `build/mono/mini/mono-boehm`, and the class libs under
  `build/mcs/class/lib/net_4_x-linux/` and the other `lib/*` profiles.
- Rebuild after editing backend sources with `cmake --build build -j"$(nproc)"` — it is
  incremental, so there is no per-directory equivalent of `make -C mono/mini` to reach for.
- `MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS` moves only the *compiler's* host. Tests still run on
  the runtime being built — that never moves — as do resgen, ilasm, cil-stringreplacer,
  gensources and the AOT step, which load out of `build/mcs/class/lib/build` and would hit
  "Corlib not in sync with this runtime" on a foreign mono. The one caveat: `mono/tests`
  and `acceptance-tests` compile without `-nostdlib`, so they see the system mono's BCL
  rather than this tree's. Runtime behaviour is unaffected; but if a test fails in a way
  that looks like a missing or differently-shaped API rather than a codegen bug, rule that
  out by reconfiguring with the option off before chasing it. See `build.md`.
- The class-library bootstrap runs on **system mono by name** — `$(EXTERNAL_RUNTIME)` is
  literally `mono`, resolved through `PATH`. If it ever fails, delete
  `mcs/build/deps/use-monolite` before retrying: the failed check writes that flag, and it
  latches, silently redirecting later runs onto the in-tree runtime.
- Killing a build with Ctrl-C does not stop the class libraries. `gmake` under `mcs/` is
  spawned through a `cmake -E env` wrapper and outlives ninja; check for stray `gmake` and
  `VBCSCompiler` processes if the next build misbehaves.
## Worktrees

Submodules are **not** shared with the main worktree, so a fresh one needs its own checkout —
and a bare `git submodule update --init --recursive` re-clones every one of them over the
network (~790M, several minutes). Run this from the new worktree's root instead:

```bash
.claude/scripts/init-submodules.sh
```

It borrows the main worktree's object stores and lands at ~9M in ~25s. It also finds the main
tree on its own, so there is nothing to point it at; `--reference-tree <dir>` overrides that,
and `--dissociate` copies the objects in at full disk cost if the worktree must outlive the
tree it borrowed from.

Do not "simplify" it to one `git submodule update --reference <main worktree>`. Git hands that
value straight to `git clone` for every submodule, so a single superproject path makes each
submodule take the superproject's object store as its alternate — which shares no objects with
it. Git re-downloads the lot anyway and leaves a dead alternates entry behind, silently. Each
submodule has to name its own store, and nested ones their own again, which is why the script
walks the tree itself rather than passing `--recursive`. Its header documents the rest.

Two submodules fail in ways that name something else entirely. Without `external/bdwgc`'s own
`libatomic_ops` the native build stops at `atomic_ops.h: No such file or directory`; without
`external/api-doc-tools` (and its nested `Lucene.Net.Light`) the CLASS-LIBRARY build stops at
`No rule to make target '.../linux_net_4_x__monodoc.dll.sources'`, which reads like a broken
makefile. That second one is worth knowing about: `gensources` runs on the **in-tree** runtime,
so a missing checkout there is easy to mistake for a codegen bug in whatever you are working on.

An interrupted submodule update does not heal on a re-run. Git clones with `--no-checkout`, so
`HEAD` already sits at the remote's default branch; for any submodule whose recorded SHA *is*
that tip, a later update sees `HEAD == gitlink` and skips the checkout — forever. The symptom is
an empty directory that `git submodule status` calls clean and that `git submodule update --init
<path>` exits 0 on without doing anything. `git -C <path> reset --hard HEAD` is the fix.

A worktree's build tree is its own — nothing is inherited from the main tree, so configure it
with the **same flags as the main build**, `MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS=ON` included:

```bash
cmake -S . -B build -G Ninja \
  -DMONO_LLVM_PREFIX="$HOME/projects/llvm-project/install" \
  -DCMAKE_INSTALL_PREFIX="$PWD/tmp" \
  -DMONO_USE_SYSTEM_RUNTIME_FOR_TOOLS=ON \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
```

Leaving the system-runtime option off is worse in a worktree than in the main tree: the C#
compile has no warm class libs to fall back on, so the whole bootstrap runs on the runtime
you are actively changing, at ~4x on ordinary sources and pathologically on generated ones.
The bootstrap mono comes from `PATH` (`$(EXTERNAL_RUNTIME)` is literally `mono`) — make sure
that is the machine's installed mono and not something a worktree put on `PATH`.

A worktree also needs `MONO_CFG_DIR=$PWD/build/runtime/etc` when running `mono-sgen` directly,
or every corpus touching the filesystem dies with `DllNotFoundException: System.Native`.

## Test / verify

The mono/mini regression harness (`--regression`) runs `.exe` corpora built from the
in-tree `*.cs`, one pass per corpus, with every test body compiled through the LLVM
backend like any other method.

Everything runs through **CTest**, selected by label rather than by directory. A bare
`ctest` is serial and therefore much slower than the work needs; the two convenience
targets below pass `-j` for you.

```bash
cmake --build build --target check       # the fast set: unit tests, eglib, mini
                                         # regression, runtime one-offs
cmake --build build --target check-all   # everything except the slow/stress suites

# Or by label, which is how to run one suite:
ctest --test-dir build -L regression -j"$(nproc)"   # the mini corpora, one test each
ctest --test-dir build -R test-llvm  -j"$(nproc)"   # the LLVM backend unit tests
ctest --test-dir build -N                           # list without running
```

Labels: `regression`, `runtime`, `gshared`, `sgen`, `interp`, `slow`, `stress`,
`acceptance`. The heavy ones are excluded from `check`/`check-all` and are opt-in.
Corpora are built by the regular build (`cmake --build build`), not by ctest, so build
before running `ctest` directly.

Every program in `mono/tests` is its own CTest test, named `<suite>/<program>@<gc>`, so a
failure names the program and the collector configuration that broke. That is what makes
`ctest -R runtime/bug-18026` and `--rerun-failed` useful; the price is that `ctest -N`
lists a few thousand tests. `test-runner.exe` still exists for running a list by hand
without a configure step, but nothing in the build drives it any more.

Everything runs on **both collectors**, `@sgen` and `@boehm`, selected through
`MONO_EXECUTABLE`. Pick one with `ctest -R '@boehm$'` — it is a name suffix, not a label,
because `ctest -L` is a regex and a `gc-sgen` label would be swept up by `-L sgen`, which
already means the SGen collector matrix. That matrix is the one suite pinned to `GC sgen`:
`mono-boehm` accepts `--gc=sgen` and `--gc-params`, ignores them, and exits 0.

Boehm is not fully green, and the two halves fail differently. The JIT on Boehm is stable:
three programs are excluded from the boehm half only, listed in `MONO_TESTS_BOEHM_DISABLED`
in `tests.cmake` with a note on each. The **interpreter on Boehm is unstable**
and is therefore not run at all (`GC sgen` on the `runtime-interp` suite) — the failures
are not a fixed set, so there is nothing to list: two back-to-back runs of the same 481
tests failed 5 and 4 and agreed on only 2, and a full run failed 76. Each of those passes
under interp+SGen and under JIT+Boehm, so it is the combination that is broken. Making
Boehm green means chasing that instability, not ticking off names.

`acceptance` additionally needs `-DMONO_ENABLE_ACCEPTANCE_TESTS=ON` and the corpus
submodules; the `print-versions` target reports which are checked out. See `build.md`.

**The `mono/llvm/` backend is the only JIT.** Every method the runtime compiles goes
through `mono_llvm_jit_compile_method ()`; a method it cannot translate raises an
ExecutionEngineException rather than falling back anywhere. `--llvm` and `--nollvm` are
deprecated no-ops that warn, and the old tiered backend under `mono/mini/llvm/` never
engages (its `MONO_TIERED*` env vars with it). The AOT compiler is out of scope and
refuses immediately.

Backend debugging env vars (read in `mono/llvm/runtime.cpp`):
- `MONO_LLVM_JIT_TRACE=1` — print every method the backend translates; a method reached
  as a callee is compiled without the runtime ever being asked for it, so nothing else
  says it happened.
- `MONO_LLVM_JIT_DUMP=<substr>` — dump the IL and translated IR of methods whose full
  name contains the substring.
- `MONO_LLVM_JIT_VERIFY=<0|off|each|all>` — how much of the IR the verifier sees. On by
  default when LLVM was built with assertions (the configuration this project uses), and
  then it checks the translator's output, the module after each pass written here, and the
  module codegen is handed; `each` extends that to every stock pass in the pipeline, `0`
  turns it off. Costs roughly 6% of compile CPU on the default setting and 29% on `each`.
  A failure names the method, the pass and prints the module, then aborts.

Running a single corpus directly against the freshly built runtime:

```bash
MONO_PATH=build/mcs/class/lib/net_4_x \
  build/mono/mini/mono-sgen --regression build/mono/mini/basic.exe
```

## Architecture of the LLVM tier (`mono/mini/llvm/`)

Everything here is **C++ (`.cpp`) by default**. Use `.hpp` for C++-only headers; use a
`.h` with an `extern "C"` boundary **only** for the minimal surface the rest of mono's C
code includes (`backend.h`). Keep that boundary small.

- **`backend.h`** — the single `extern "C"` entry-point header mono's C sources include.
- **`translator*.cpp`** — the IL→LLVM-IR translator (ported from dotnet/runtime's
  opaque-pointer-clean `mini-llvm.c`, AOT/llvmonly/non-amd64 stripped)..
- **`engine.cpp` / `engine.hpp`** — ORCv2 in-process JIT + `MonoJitMemoryManager`.
  `compile()` is **non-destructive**: it JITs a private clone of the caller's module and
  leaves the original intact (mono keeps using it afterward).
- **`tiered.cpp`** — tiering policy: promotion thresholds, env-var config, counters.
- **`ehframe.cpp`**, **`lsda.cpp`/`mono_lsda.cpp`** — consuming/transcoding stock
  `.eh_frame` / `.gcc_except_table` (LSDA) so the backend runs against unmodified LLVM.
- **`passes/`** — the custom LLVM passes. `inliner.cpp` (drives LLVM's own inliner over
  bodies it materializes on demand), `devirt.cpp` (exact devirtualization), plus the
  lowering/cleanup passes: `elide-class-init`, `wbarrier`, `eh-gather`, `finally-range`,
  `replace-builtins`, `null-check-guard`, `inline-advisor`, `pass-dump`.

A pass that needs to ask the runtime something does **not** include mono's metadata
headers itself. It declares what it needs in a small `*-support.hpp` boundary
(`inliner-support.hpp`, `devirt-support.hpp`) and the implementation lives in
`translator.cpp`, which already has those headers in scope. Keep new passes to that shape.

The translator tags instructions so passes can find them by meaning rather than by matching
on IR shape — `mono.class-init`, `mono.wbarrier`, `mono.virtcall`, `mono.runtime-check`.
When a pass needs to know something only the front end knew, add a tag rather than
reverse-engineering it from the emitted arithmetic.

**Devirtualization is exact-only, by decision.** No guarded devirtualization, no type
profiling, no class-hierarchy analysis — every rewrite is a proof that the receiver is
exactly some class, so the failure mode is a site left alone rather than a site left wrong,
and no later assembly load can invalidate one. If you find yourself wanting a type check
plus a fallback arm, that is out of scope; check with the user first.

The legacy backend (`mono/mini/mini-llvm.c`, `mini-llvm-cpp.*`, `llvm-jit.*`) linked
patched LLVM 6 and is **excluded from the build** — do not edit those; the replacement
grows under `mono/mini/llvm/`.

Depatching notes (running off unmodified LLVM): `nest` attribute replaces
`CallingConv::Mono`; consume stock `.eh_frame`/`.gcc_except_table`. See the design docs.

Build/link specifics: LLVM is `-fno-rtti` — subclassing polymorphic LLVM classes
(memory managers, plugins) with RTTI on is a silent ABI break, so backend TUs compile
`-std=c++17 -fno-exceptions -funwind-tables -fno-rtti`.

## Commenting Guidelines
- Keep comments conversational. These are meant to be read by humans. Dense or cryptic
  comments that cannot be understood are not useful.
- Match comment length to what the thing actually needs. A subtle invariant can usually be
  argued in a few sentences; if IR/pseudocode conveys the shape of a transform faster than
  prose does, use that instead of describing it in words. A wall of text is not more
  rigorous than a short one - past a certain length it just hides the one or two sentences
  that matter, which is worse than being terse.
- Assume that those reading the comments have a decent working knowledge of the codebase
  and JIT compilation in general.
- Do not include archeology. Comments referencing deleted/legacy code are generally not
  useful since it is likely that the code will evolve more, at which point those comments
  are obsolete.
- Do not reference the current plan or current task list. For people reading the code later
  on without access to the plan documents these references are noise that hide what is
  actually going on.
- Doc comments on methods should just explain _what_ the method does and not _how_ it does it.
  Users needing to know how a method works can just read its implementation.
- Comments inside a method should be minimal. If present they should explain _why_ a specific
  thing is being done and only extend to _what_ or _how_ if these are very nonobvious.
- Do not explain why something is not happening in the code. Justify the code that is there;
  don't narrate the absence of some other mechanism (e.g. why a different pass doesn't do X,
  or why some hypothetical isn't a problem) unless that absence is itself the nonobvious thing
  a reader needs to know to trust the code.
