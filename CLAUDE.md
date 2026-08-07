## What this repo is

Unity Technologies' fork of Mono (branch `unity-main`). The **active work** here is a
new **LLVM-only JIT** under `mono/llvm/` (greenfield, ORCv2, a direct CIL→LLVM-IR
translator in `mono/llvm/method-to-llvm/`), developed on branch `llvm18-tiered-jit`.
It is the runtime's only JIT: every compile routes through it, the classic mini
back end never engages, and there is no command-line switch to select a
backend. It builds against **unmodified upstream LLVM 22** — a local
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

Labels: `regression`, `llvm`, `runtime`, `gshared`, `sgen`, `interp`, `bcl`,
`bcl-xunit`, `compiler`, `tools`, `benchmark`, `slow`, `stress`, `acceptance`
(`ctest --print-labels` is authoritative for the configuration you built).
`check` keeps `regression`, `llvm` and the unlabelled one-offs and drops
everything else — a few hundred tests, seconds. `check-all` drops only `slow`,
`stress` and `acceptance`, which leaves a few thousand and takes minutes, so it
is not the target to reach for while iterating. Corpora are built by the regular
build (`cmake --build build`), not by ctest, so build before running `ctest`
directly.

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

The class-library suites are named per assembly — `bcl-corlib`, `bcl-System.Xml`, and
`bcl-xunit-<assembly>` for the xunit half — and the largest of them are cut into one test
per namespace, `bcl-corlib/MonoTests.System.IO` and so on. A whole assembly under one
console prints nothing between its banner and its summary, so a suite that wedges reports
a timeout and nothing else; a group names where it stopped, keeps one bad fixture from
taking the assembly's other results with it, and lets a suite use more than one core. The
groups keep the `bcl`/`bcl-xunit` label and the assembly's name, so `ctest -R bcl-corlib`
still selects the lot. Which suites are cut is `MONO_BCL_TESTS_SPLIT` in
`cmake/MonoManagedTests.cmake`, and it only pays where a suite is large: every group is
another console process, and a console costs ~6s (nunit-lite) or ~45s (xunit) of JIT
before it runs a single case. That startup is why the xunit half is capped at eight groups
— the heaviest namespaces get one each and the rest share `other` — rather than being cut
per namespace like the nunit half. `bcl-Mono.Debugger.Soft` cannot be split at all: all
124 of its cases live in one fixture.

The listing behind that runs at **build** time (`cmake/MonoBclDiscover.cmake`, from a
custom command on the group file), so it re-runs when the test assembly or the runtime is
rebuilt rather than on every `ctest`. A listing that fails writes an empty group list and
the suite registers whole — the build stays green, which matters on a branch where the
runtime being built is routinely the reason the lister could not get that far. The xunit
groups also carry an `unlisted` complement, which runs anything the lister missed and
reports as a skip when it missed nothing. Timeouts follow the split: a whole suite gets
`MONO_BCL_TEST_TIMEOUT` (1800s), or `MONO_BCL_TEST_LONG_TIMEOUT` (3600s) if it is named in
`MONO_BCL_TESTS_LONG`, while a group gets `MONO_BCL_GROUP_TIMEOUT` (900s) whichever of
those its assembly would have had.

`acceptance` additionally needs `-DMONO_ENABLE_ACCEPTANCE_TESTS=ON` and the corpus
submodules; the `print-versions` target reports which are checked out. See `build.md`.

**The `mono/llvm/` backend is the only JIT.** Every method the runtime compiles goes
through `mono_llvm_jit_compile_method ()`; a method it cannot translate raises an
ExecutionEngineException rather than falling back anywhere. There is no `--llvm` or
`--nollvm` any more — both are gone, and mono rejects them like any other unknown
option. `--llvm-opt=OPT` is the one LLVM-facing flag left, and it just forwards
`OPT` to LLVM's own command-line parser (`--llvm-opt=-print-after-all`); repeat it
to pass more than one. The AOT compiler is out of scope and refuses immediately.

Backend debugging env vars:
- `MONO_LLVM_JIT_TRACE=1` (`runtime.cpp`) — print every method the backend translates;
  a method reached as a callee is compiled without the runtime ever being asked for
  it, so nothing else says it happened.
- `MONO_LLVM_JIT_DUMP=<substr>` (`runtime.cpp`) — dump the IL and translated IR of
  methods whose full name contains the substring.
- `MONO_LLVM_JIT_ASM=<substr>` (`compiler.cpp`) — print the assembly methods whose
  full name contains the substring compile to, side-table sections included, which
  is the half no offline `llc` run can reproduce. The `.mono_unwind` writer wants
  labels only an object streamer plants, so the frame description appears as the
  `.cfi_*` directives instead. Costs a second codegen over a clone of the module,
  which leaves the code that actually gets published untouched.
- `MONO_LLVM_JIT_VERIFY=<0|off|each|all>` (`jit.cpp`) — how much of the IR the verifier
  sees. On by default when LLVM was built with assertions (the configuration this
  project uses), and then it checks the translator's output, the module after each pass
  written here, and the module codegen is handed; `each` (or `all`) extends that to
  every stock pass in the pipeline, `0`/`off` turns it off, and anything else means the
  default. The default setting costs about **9%** of compile CPU — measured with
  `MONO_LLVM_JIT_TIMING`, paired alternating runs over `test-async-20`; the 29%
  figure for `each` is inherited and has not been re-measured. A failure names the
  method, the pass and prints the module, then aborts.
- `MONO_LLVM_JIT_TIMING=1` (`timing.cpp`) — at exit, print how long each phase of a
  compile took, summed over every method: metadata, translation, resolution, the IR
  pipeline, codegen setup and codegen proper, JITLink, and the pieces around them.
  Phases nest and the self column is a share of the whole, so it sums to 100. Worth
  reaching for before theorising about compile latency — `perf` does not work on this
  kernel and `--llvm-opt=-time-passes` aborts under concurrent compiles.
  The value is a comma-separated set of words rather than a flag. `cpu` charges
  thread CPU time instead of wall clock, which is what makes a run on a loaded box
  comparable to a quiet one, at ten times the cost per reading (~6 µs a compile).
  `fine` splits the four expensive phases into the pieces a per-compile floor is
  made of — the machine passes and their construction, the object write and the
  read back, the LLVMContext, the analysis managers — and costs ~21 µs a compile
  with `cpu` and ~2 µs without.
- `MONO_LLVM_JIT_HOIST=<word>[,<word>]` (`jit.cpp`) — measurement arms that take one
  piece of per-compile work away so it can be priced; none is a candidate
  implementation and `sharedjd` is not even safe under concurrent compiles.
  `sharedjd` puts every module in one JITDylib.
- `MONO_LLVM_SLAB_SIZE=<n>[kKmMgG]` (`codemem.cpp`) — the size of the reservations code
  is bump-allocated out of. Capped at 2GB whatever you ask for, because a slab's code
  and its mutable data reference each other with a PCRel32 and nothing stubs that.

Running a single corpus directly against the freshly built runtime:

```bash
MONO_PATH=build/mcs/class/lib/net_4_x \
  build/mono/mini/mono-sgen --regression build/mono/mini/basic.exe
```

## Architecture of the backend (`mono/llvm/`)

Everything here is **C++**, and every header is a `.hpp` — there are no `.h` files.
The one header mono's C sources include is `runtime.hpp`, whose declarations sit
inside `MONO_BEGIN_DECLS`; its whole audience is `mini-runtime.c`,
`mini-trampolines.c` and `driver.c`, and its whole surface is six functions. Keep
it that small.

- **`runtime.cpp` / `runtime.hpp`** — the boundary. `mono_llvm_jit_compile_method ()`
  compiles a method into a domain's linker and hands back the address to call;
  the rest is freeing a domain or a method, finding a compiled body, and the unbox
  entry. This is also where the runtime helpers and libcalls generated code may name
  are registered.
- **`method-to-llvm.cpp` + `method-to-llvm/`** — the CIL→IR front end. One class,
  `MethodLLVMEmitter`, split across `method-to-llvm/` by opcode family: `call.cpp`,
  `casts.cpp`, `exceptions.cpp`, `fields.cpp`, `newobj.cpp`, `signature.cpp` and so on.
- **`jit.cpp` / `jit.hpp`** — `MonoJit`, the ORCv2 stack: the JITLink object layer, the
  pipeline, symbol resolution, a JITDylib per compile. It deliberately knows nothing
  about mono — no metadata, no `MonoMethod` — so the unit tests can drive it directly.
- **`compiler.cpp`** — `TargetMachine::addPassesToEmitMC` restated, so the EH passes get
  a seat between the machine passes and the AsmPrinter and the side tables can be
  written while the streamer is still open, with code offsets as label differences.
- **`stubs.cpp`, `codemem.cpp`** — the redirectable stub every method is published as,
  and the slabs both it and the code are carved out of.
- **`jinfo.cpp`** — turns a compiled object's side tables back into the `MonoJitInfo`
  the runtime's unwinder and stack walks read.
- **`arch/`** — everything that names a register, encodes an instruction or restates the
  runtime's calling convention, behind `arch/arch.hpp`. A port is a new sibling of
  `arch/amd64/`, not a hunt through the backend for the amd64 in it.
- **`passes/`** — `array-address` and `lower-builtins` rewrite the symbolic calls the
  front end leaves standing; `restore-tail-position` puts back the tail position
  SimplifyCFG merged away; `eh-gather` and `finally-range` are `MachineFunctionPass`es
  that emit nothing and instead fill in the side channel the EH sections are written from.

No pass includes a mono header — not one of them, and that is the rule for new ones.
Where a pass needs something only the front end knew, the front end emits a call to a
declaration whose *name* says what the site means (`mono.array.address.*`,
`mono.builtin.*`) and whose attributes carry the numbers, and the pass rewrites it into
real IR before the optimizer runs. Encode the fact in the declaration rather than
reverse-engineering it from the emitted arithmetic.

**Exception handling does not ride `.eh_frame`.** The compiler writes three of its own
sections next to the code, all target-neutral and code-relative: `.mono_lsda` (the
clause table), `.mono_guards` (where each finally body landed and where its guard byte
sits, which is what the thread-abort delay needs) and `.mono_unwind` (the CFI program,
recorded at the MC layer while it is still semantics rather than DWARF bytes). A fourth,
`.mono_lines`, is written the same way and carries the IL offset in effect at each code
offset - what stack traces print and what sequence points are recovered from.
`sidetables.hpp` is the wire format the writer and `jinfo.cpp` agree on. The personality
routine a landing pad names is never actually called: mono's own unwinder re-enters
frames through the pads.

**There is one tier.** `run_tier0_pipeline ()` is the stock O1 *function* simplification
pipeline with this backend's own IR passes around it — `array-address` and
`lower-builtins` before, `restore-tail-position` and the arch's legacy-ABI lowering
after — and codegen then runs at `CodeGenOptLevel::None`, which selects FastISel. The
module and CGSCC layers are skipped deliberately: a module holds a single method and
every call leaves it by symbol, so there is no callee body to inline and nothing to
specialize, and running them anyway cost a large fraction of compile time. So the JIT
does not inline across methods, and beyond taking a site the IL already settled —
non-virtual, `final`, or resolved by `constrained.` — it does not devirtualize either.

What that costs is worth knowing before optimizing anything here: **87% of a compile is
LLVM and 5.5% is the CIL→IR front end**, and 70% of the total is a per-method floor that
does not vary with method size — a three-instruction property getter takes ~900 µs, of
which the translator is ~36. Making the translator faster is not where the time is;
`.claude/plans/compile-latency-b461931af78.md` has the split.

If devirtualization does arrive, it is exact-only: a rewrite must prove the receiver's
class exactly, so the failure mode is a site left alone rather than a site left wrong,
and no later assembly load can invalidate one. Guarded devirtualization, type profiling
and class-hierarchy analysis are out of scope — check with the user first.

Depatching notes (running off unmodified LLVM): the `nest` attribute replaces
`CallingConv::Mono`, since it pins an argument to `%r10` — exactly
`MONO_ARCH_IMT_REG` — which is how an IMT thunk or a generic-virtual trampoline gets
the key telling it which method was asked for. See the design docs.

Build/link specifics: LLVM is `-fno-rtti` — subclassing polymorphic LLVM classes
(memory managers, passes) with RTTI on is a silent ABI break, so backend TUs compile
`-std=c++17 -fno-rtti -funwind-tables`. Exceptions stay **on**: the ORC APIs report
failures through `llvm::Error` and the unwinder needs the tables.

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
