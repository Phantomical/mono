## What this repo is

Unity Technologies' fork of Mono (branch `unity-main`). The **active work** here is a
new **LLVM-only JIT** under `mono/llvm/`: greenfield, ORCv2, a direct CIL→LLVM-IR
translator in `mono/llvm/method-to-llvm/`, developed on branch `llvm18-tiered-jit`.
It is the runtime's only JIT. Every compile routes through it, the classic mini back
end never engages, and no command-line switch selects a backend.

It builds against **unmodified upstream LLVM 22** — a local RelWithDebInfo build of
`llvmorg-22.1.8`, installed at `~/projects/llvm-project/install`. That prefix is the
production configuration and it has **assertions off**. If the backend fails to build
against an LLVM that looks correct, make sure that `llvm-config --version` in that
prefix says 22.x.

`llvm-config --assertion-mode` is worth reading before you measure anything, because
the two builds are not interchangeable. A runtime built against an assertions LLVM and
then run against this one dies at startup with `undefined symbol:
_ZN4llvm23EnableABIBreakingChecksE`, which names the library rather than the change
that caused it. `MONO_LLVM_JIT_VERIFY` also keys its default off assertions, so the
same command costs 11-15% more compile CPU under an assertions build. Numbers from
either side of a switch are not comparable, and a worktree needs a rebuild after one.

An assertions build stays useful for finding a fault, because it catches the API
misuse the distro LLVM tolerated in silence. Reach for it to explain a crash, not to
measure.

Scope: amd64 and Linux first, JIT only, unmodified LLVM. AOT and llvmonly are out of
scope and deleted. The design doc is `.claude/plans/orc-direct-multitier.md`.

## Build

CMake and Ninja. autotools is gone, so no `autogen.sh`, `configure` or `Makefile.am`
exists any more. `mcs/` keeps its own hand-written make system, which CMake invokes.
`build.md` has the from-scratch instructions.

```bash
cmake -S . -B build -G Ninja \
  -DMONO_LLVM_PREFIX="$HOME/projects/llvm-project/install" \
  -DCMAKE_INSTALL_PREFIX="$PWD/tmp" \
  -DMONO_USE_SYSTEM_RUNTIME_FOR_TOOLS=ON \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

cmake --build build -j"$(nproc)"
```

**Always pass all of these.** The build turns none of them on by itself.

`MONO_LLVM_PREFIX` is what defines `ENABLE_LLVM`. Empty, which is the default, means no
LLVM tier at all. It replaces the old `--with-llvm=`, and the remaining defaults already
match Unity's desktop Linux configuration.

`MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS=ON` compiles all C# on the mono installed on the
machine rather than the one being built. Roslyn on the in-tree runtime costs roughly 4x
on ordinary sources and is pathological on generated ones. One CoreCLR CSE fixture took
eight hours and then died, against 88 seconds on the system mono. The option also keeps
C# compiles off the native link, so a one-line change under `mono/mini` no longer
relinks the runtime before any C# can compile. Leave it on unless you are deliberately
investigating the in-tree runtime as a compiler host.

The option moves only the *compiler's* host. Tests still run on the runtime being
built, as do resgen, ilasm, cil-stringreplacer and gensources. One caveat: `mono/tests`
and `acceptance-tests` compile without `-nostdlib`, so they see the system mono's BCL
rather than this tree's. Runtime behavior does not change. If a test fails in a way that
looks like a missing or differently-shaped API rather than a codegen bug, reconfigure
with the option off before you chase it.

`ccache` covers the native half, where a rebuild after a branch switch re-runs the
compiler over ~1500 mostly unchanged sources. `CMAKE_*_COMPILER_LAUNCHER` is the right
knob rather than `CC="ccache cc"`, which CMake's own compiler checks trip over.
Configure makes the cache shareable across worktrees on its own, so `ccache.conf` needs
nothing. The cost is that debug info names `/mono` rather than the tree it was built in.
gdb started from the build directory still finds the sources. Started anywhere else it
wants `set substitute-path /mono <worktree>`.

Other things worth knowing:
- Changing a cache variable means re-running `cmake -S . -B build`. No `config.status`
  needs deleting and no stale-config trap exists.
- The build needs an **existing `mono`/`mcs` in `PATH`** to bootstrap the class
  libraries. `$(EXTERNAL_RUNTIME)` is literally `mono`, resolved through `PATH`, so make
  sure that a worktree has not put something else there.
- If the bootstrap fails, remove `mcs/build/deps/use-monolite` before you retry. The
  failed check writes that flag, and it latches, redirecting later runs onto the in-tree
  runtime in silence.
- Ctrl-C does not stop the class libraries. `gmake` under `mcs/` is spawned through a
  `cmake -E env` wrapper and outlives ninja. If the next build misbehaves, look for
  stray `gmake` and `VBCSCompiler` processes.
- Artifacts land in the build tree: `build/mono/mini/mono-sgen`,
  `build/mono/mini/mono-boehm`, and the class libs under `build/mcs/class/lib/*`.

## Worktrees

Submodules are **not** shared with the main worktree, so a fresh worktree needs its own
checkout. A bare `git submodule update --init --recursive` re-clones every one of them
over the network, which is ~790M and several minutes. Run this from inside the new
worktree instead:

```bash
/home/swlynch/projects/mono/.claude/scripts/init-submodules.sh
```

It borrows the main tree's object stores and lands at ~9M in seconds. The script finds
the main tree from the current directory, so it takes no argument. `.claude/scripts/`
is untracked, which is why the command above names the main tree's copy — a fresh
worktree has no copy of its own.

Do not "simplify" it to one `git submodule update --reference <main worktree>`. That
form silently re-downloads everything and leaves a dead alternates entry behind. The
script's header documents why, and documents `--reference-tree` and `--dissociate`.

Two submodules fail in ways that name something else entirely:
- Without `external/bdwgc`'s own `libatomic_ops`, the native build stops at
  `atomic_ops.h: No such file or directory`.
- Without `external/api-doc-tools` and its nested `Lucene.Net.Light`, the class-library
  build stops at `No rule to make target '.../linux_net_4_x__monodoc.dll.sources'`,
  which reads like a broken makefile. `gensources` runs on the **in-tree** runtime, so
  this one is easy to mistake for a codegen bug in whatever you are working on.

An interrupted submodule update does not heal on a re-run. Git clones with
`--no-checkout`, so `HEAD` already sits at the remote's default branch. For any
submodule whose recorded SHA *is* that tip, a later update sees `HEAD == gitlink` and
skips the checkout for good. The symptom is an empty directory that `git submodule
status` calls clean and that `git submodule update --init <path>` exits 0 on without
doing anything. `git -C <path> reset --hard HEAD` is the fix.

A worktree's build tree is its own. Configure it with the **same flags as the main
build**, `MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS=ON` included. Leaving that option off is
worse in a worktree than in the main tree, because the C# compile has no warm class libs
to fall back on. The whole bootstrap then runs on the runtime you are actively changing.

A worktree also needs `MONO_CFG_DIR=$PWD/build/runtime/etc` when running `mono-sgen`
directly. Without it, every corpus that touches the filesystem dies with
`DllNotFoundException: System.Native`.

## Test / verify

Everything runs through **CTest**, selected by label rather than by directory. A bare
`ctest` is serial and much slower than the work needs. The two targets below pass `-j`
for you.

```bash
cmake --build build --target check       # fast: unit tests, mini regression, one-offs
cmake --build build --target check-all   # everything except slow/stress/acceptance

ctest --test-dir build -L regression -j"$(nproc)"   # the mini corpora, one test each
ctest --test-dir build -R test-llvm  -j"$(nproc)"   # the LLVM backend unit tests
ctest --test-dir build -N                           # list without running
```

Labels: `regression`, `llvm`, `runtime`, `gshared`, `sgen`, `interp`, `bcl`,
`bcl-xunit`, `compiler`, `tools`, `benchmark`, `slow`, `stress`, `acceptance`.
`ctest --print-labels` is authoritative for the configuration you built. `check` is a
few hundred tests and seconds. `check-all` is a few thousand and minutes, so it is not
the target to reach for while iterating. Corpora are built by the regular build, not by
ctest, so build before you run `ctest` directly.

The mini regression harness (`--regression`) runs `.exe` corpora built from the in-tree
`*.cs`, one pass per corpus, with every test body compiled through the LLVM backend like
any other method. To run one corpus directly:

```bash
MONO_PATH=build/mcs/class/lib/net_4_x \
  build/mono/mini/mono-sgen --regression build/mono/mini/basic.exe
```

**A run whose output you intend to read must capture that output itself:**

```bash
ctest --test-dir build -j"$(nproc)" --output-on-failure > my-run.log 2>&1
```

`Testing/Temporary/LastTest.log` will not do. CTest keeps one per build directory and
rewrites it per invocation, so a second `ctest` in the same tree overwrites it *while
the first still runs*. That includes `ctest -N`, which runs nothing at all. What is left
looks like an intact log and is a fragment of the other run. A whole-tree sweep takes
hours, so give one its own worktree where no other run can reach its build directory.

Class-library suites are sensitive to what else the machine is doing, and the System.Web
ones especially. A page test forks a C# compiler that then runs on the runtime being
built, all inside ASP.NET's 110s `executionTimeout`. A busy box therefore turns into
`HttpException: The request timed out` on a case that passes on a quiet one. Read the
load average before you believe a BCL failure, and re-run it quiet before you call it a
regression.

Every program in `mono/tests` is its own CTest test, named `<suite>/<program>@<gc>`, so
a failure names the program and the collector that broke. That is what makes `ctest -R
runtime/bug-18026` and `--rerun-failed` useful. Each such test gets a `TMPDIR` of its
own, so the two collector arms and two worktrees running at once do not share temporary
files. Running a corpus program by hand gets no such isolation, and the class-library
suites do not have it yet.

Everything runs on **both collectors**, `@sgen` and `@boehm`, selected through
`MONO_EXECUTABLE`. Pick one with `ctest -R '@boehm$'`. It is a name suffix rather than a
label, because `ctest -L` is a regex and `-L sgen` already means the SGen collector
matrix. That matrix is the one suite pinned to SGen, because `mono-boehm` accepts
`--gc=sgen`, ignores it, and exits 0.

The class-library suites are named per assembly (`bcl-corlib`, `bcl-xunit-<assembly>`),
and the largest are cut into one test per namespace (`bcl-corlib/MonoTests.System.IO`).
The groups keep the assembly's name, so `ctest -R bcl-corlib` still selects the lot.
`cmake/MonoManagedTests.cmake` and `cmake/MonoBclDiscover.cmake` hold the split, the
timeouts, the slow list and the per-test core counts, each with a comment on why. Read
them there rather than guessing. Two rules do not live in either file:
- Splitting a suite must never leave a test less time than it had whole. A namespace can
  carry most of an assembly, so a smaller group budget is a bet on how the cases spread.
- The slow suites are not disabled, because most of what they spend is this runtime's
  own compile latency. Their wall time is a rough alarm on the JIT.

`acceptance` additionally needs `-DMONO_ENABLE_ACCEPTANCE_TESTS=ON` and the corpus
submodules. The `print-versions` target reports which are checked out.

## Backend debugging

**The `mono/llvm/` backend is the only JIT.** Every method the runtime compiles goes
through `mono_llvm_jit_compile_method ()`. A method it cannot translate raises an
ExecutionEngineException rather than falling back anywhere. `--llvm` and `--nollvm` are
gone, and mono rejects them like any other unknown option. `--llvm-opt=OPT` is the one
LLVM-facing flag left, and it forwards `OPT` to LLVM's own command-line parser. Repeat
it to pass more than one. The AOT compiler refuses immediately.

Each variable below is declared in `runtime/options.cpp` unless named otherwise, and
each is documented at its declaration.

Tracing:
- `MONO_LLVM_JIT_TRACE=1` — print every method the backend translates, every worker
  thread it starts and every body an inliner folds in. A method reached as a callee is
  compiled without the runtime ever being asked for it, so no other output says it
  happened.
- `MONO_LLVM_JIT_GDB=1` (`gdb-jit.cpp`) — hand every compiled object to gdb through its
  JIT interface, so `info functions` names JIT'd methods and a `bt` from runtime C code
  unwinds managed frames with names instead of `??`. The module carries no `.debug_*`
  sections, so there is no source-level stepping. Off by default, because it keeps a
  copy of every object alive for as long as the method.

Dumping. Both engines print through `mono/mini/jit-dump.hpp`, so one variable selects
the stages and one filter selects the methods:
- `MONO_JIT_DUMP=<points>` — the stages to print, separated by `;` or `,`. `all` names
  every one. A name nothing matches is reported on stderr with the list of names. The
  points are `il`, `mint`, `unopt-ir`, `tier1-ir`, `tier2-ir`, `tier1-asm` and
  `tier2-asm`.
- `MONO_JIT_DUMP_FILTER=<substr>` — dump only the methods whose name contains this. Every
  point matches it against the same string, `Class:Method (argtypes)@0xADDR`, so a filter
  that selects a method at one point selects it at all of them. Unset takes every method.
- `MONO_JIT_DUMP_DIR=<dir>` — write each dump to `<dir>/<point>/<method>.<ext>` instead
  of to stdout, which is what to reach for under a player whose stdout is a shared log.
  A name already taken gets a counted suffix, so a method compiled more than once keeps
  each dump.

What each point prints:
- `il` — the method's CIL, inside the class and signature it is declared with. It prints
  once for each method, from whichever engine reached it first.
- `mint` — the bytecode the interpreter runs, after the transform has compacted it.
  `MONO_VERBOSE_METHOD` still prints the same dump and the transform's tracing with it.
- `unopt-ir` — the IR the translator wrote, before any pipeline. A body the pre-pass
  folded in is still a function of its own here, so it prints after the caller.
- `tier1-ir` / `tier2-ir` — that IR after its tier's pipeline.
- `tier1-asm` / `tier2-asm` — the code the tier emits, side-table sections included,
  which is the half no offline `llc` run reproduces. Intel syntax, which `jit.cpp` asks
  for as a default; `--llvm-opt=-x86-asm-syntax=att` gets AT&T back. It costs a second
  codegen over a clone, so the published code is untouched.

A tier-1 promotion compiles up to `MONO_LLVM_JIT_BATCH` methods in one module, and the
IR and assembly points still print one method for each dump: the IR point prints that
method's function, and the assembly point drops the other bodies from the clone it
codegens. So a method that promoted in the middle of a batch has a dump of its own, and
naming it in the filter finds it. Reading a whole batch therefore costs one codegen for
each method in it, which is what bounds an unfiltered `tier1-asm` sweep.

`--llvm-opt=-print-after=<pass>` and `-print-after-all` are **inert**, and print nothing
rather than failing. They need `StandardInstrumentations::registerCallbacks ()`, and
`ThreadPipelines` (`jit.cpp`) builds the `PassInstrumentationCallbacks` both tiers share
without it. `-debug-only=` is unavailable as well, because the installed LLVM defines
`NDEBUG`.

Measurement:
- `MONO_LLVM_JIT_VERIFY=<0|off|each|all|other>` (`jit.cpp`) — how much IR the verifier
  sees. It follows LLVM's assertions when unset, so it is **off** in the production
  configuration above and has to be asked for. `0` and `off` turn it off. `each` and
  `all` are the same setting, the widest: every stock pass as well. **Any other value**,
  `1` among them, gets the middle level, which checks the translator's output, the
  module after each pass written here, and the module codegen is handed. That middle
  level is what an assertions build turns on by itself, and it costs **11-15%** of
  compile CPU. A failure names the method and the pass, prints the module, then aborts.
- `MONO_LLVM_JIT_TIMING=<words>` (`timing.cpp`) — at exit, print how long each phase of
  a compile took, summed over every method. Phases nest and the self column is a share
  of the whole, so it sums to 100. Reach for this before theorising about compile
  latency: it says which phase, and `perf record -g -e cpu-clock` says which function
  inside it. `cpu` charges thread CPU time instead of wall clock, which makes a run on a
  loaded box comparable to a quiet one. `fine` splits the four expensive phases into the
  pieces a per-compile floor is made of. `--llvm-opt=-time-passes` stays unusable,
  because it aborts under concurrent compiles.

Tiering and compilation policy. Every one of these exists to split one suspect in two,
and the note says which split:
- `MONO_LLVM_JIT_TIER0=<substr|0>` — narrow tier 0, which is otherwise every method the
  interpreter accepts. A false value compiles everything, which separates a tier-0 bug
  from a backend one. A substring gets a compiled caller and an interpreted callee into
  one process, which no threshold produces, because a callee is called at least as often
  as its caller.
- `MONO_LLVM_JIT_TIER1_THRESHOLD=<n>` — calls at tier 0 before a method is asked for as
  tier 1, default 10. Zero never promotes, which separates a tier-0 entry bug from a
  promotion bug. One promotes on the first call, which puts the switch inside a loop.
- `MONO_LLVM_JIT_TIER2_THRESHOLD=<n>` — entries of a tier-1 body before it asks for tier
  2, default 20000. The counter counts entries, so a method that spends its time inside
  one call never reaches it at any setting. Zero leaves a body instrumented and counting
  while it never promotes on its own, which is what a test driving the tiers through
  `Mono.Tiering.MonoTier::PromoteNow` wants.
- `MONO_LLVM_JIT_TIER2=<0|false|empty>` (and its own copy in `jit.cpp`) — turn tier 2
  off. It is on by default, which is what puts profiling instrumentation in every tier-1
  body, so this switch separates a tier-2 bug from a tier-1 one.
- `MONO_LLVM_JIT_BATCH=<n>` — promoted methods per compile, default 32. A batch shares
  one module, one IR pipeline, one codegen and one link, so LLVM's per-compile floor is
  paid once. The whole batch compiles before any of its methods is published, so a
  bigger batch makes each method wait for the slowest in it, and that is what bounds the
  setting rather than the amortising running out. Measure occupancy by methods rather
  than by batches: the distribution is bimodal, so the average batch and the batch the
  average method arrives in are different numbers, and only the second says what the
  amortising acts on. One compiles every method on its own, which separates a batching
  bug from a backend one. Only tier-1 promotions batch. A tier-2 promotion, a dynamic
  method and any compile the runtime asks for by name each go alone.
- `MONO_LLVM_JIT_WORKERS=<n>` — the most threads the compile queue runs promotions on at
  once, default `mono_cpu_count () - 2` capped at eight. The queue starts a thread only
  when work outruns the ones it has. One puts every background compile back on a single
  thread, which separates a bug in a compile from a bug in two overlapping. Do not
  expect throughput to follow the setting. ORC's session lock is taken twice per
  compile, and that measured 2.86x out of 18 threads on a compile-bound workload. The
  cap does not come off that measurement: it describes throughput, and what a promoted
  method waits for is latency, which keeps falling after throughput stops scaling. The
  process pays for the shorter wait in compile CPU and wins anyway, because a method
  waiting for a body runs interpreted.
  `.claude/plans/tier1-promotion-latency.md` has the sweeps and what is still open.
- `MONO_LLVM_JIT_RECOMPILE=<substr>` — translate matching methods afresh on every
  request instead of answering from the cache, so they end up with several live bodies.
  No other setting produces one, and the code that has to cope has no other exerciser.

Inlining. `MONO_LLVM_JIT_TRACE=1` prints a line for each fold, which is the only place a
fold is visible from outside:
- `MONO_LLVM_JIT_INLINE_IL_LIMIT=<n>` — largest callee in IL bytes the shape-test
  pre-pass folds in, default 32. Both compiled tiers run that pre-pass. Zero turns it
  off, which separates a bug in a folded body from one in the method that folded it. The
  limit is a backstop rather than a policy, because the shape test in front of it
  already refuses everything but a straight line ending in one call.
- `MONO_LLVM_JIT_INLINE_COST_IL_LIMIT=<n>` — largest callee the tier-2 cost model
  translates so it can weigh it, default 128. It bounds translation rather than code
  size, because LLVM's own threshold decides what is worth folding. Zero leaves tier 2
  with the pre-pass alone, which separates a cost-model defect from a pre-pass one.
- `MONO_LLVM_JIT_INLINE_DEPTH=<n>` — folds deep past a method the cost model may go,
  default 4. A call graph with a cycle never runs out of sites, so the loop needs this
  whatever the budget says.
- `MONO_LLVM_JIT_INLINE_BUDGET=<n>` — bodies one method's compile may fold in, default
  16. Both inliners spend the one count, and a chain of forwarders is what spends it.
  Each batch member gets its own count, so `MONO_LLVM_JIT_BATCH` changes how many
  compiles run, not what any one of them folds in.

## Architecture of the backend (`mono/llvm/`)

Everything here is **C++**, and a header only C++ includes is a `.hpp`. A `.h` is
reachable from something that is not C++, so it holds only what that other language
reads. `runtime.h` is the interface the C runtime compiles methods through: its
declarations sit inside `MONO_BEGIN_DECLS` and name only types C sees. Keep that
surface small. `arch/amd64/interp-entry-offsets.h` is read by `interp-entry-thunk.S`
as well as by `amd64.hpp`, so it holds nothing but `#define`s.
`debugging/perf/perf.h` declares what the C runtime owes the perf jit dump.

- **`runtime.h` + `runtime/`** — the engine. `runtime/entrypoints.cpp` is the boundary.
  `runtime/backend.cpp` holds the state, one `MethodState` per method with its thunk,
  trampoline and jit infos together. The rest of the directory is what a compile is made
  of: `naming`, `translate`, `externals`, `thrower`, `dispatcher`, `interp`, `options`.
  `runtime/builtins.cpp` registers the runtime helpers and libcalls generated code names.
- **`method-to-llvm.cpp` + `method-to-llvm/`** — the CIL→IR front end. One class,
  `MethodLLVMEmitter`, split by opcode family: `call.cpp`, `casts.cpp`, `exceptions.cpp`,
  `fields.cpp`, `newobj.cpp`, `signature.cpp` and more.
- **`jit.cpp` / `jit.hpp`** — `MonoJit`, the ORCv2 stack: the JITLink object layer, the
  pipeline, symbol resolution, a JITDylib per compile. It knows nothing about mono
  deliberately, so the unit tests drive it directly.
- **`compiler.cpp`** — `TargetMachine::addPassesToEmitMC` restated, so the EH passes get
  a seat between the machine passes and the AsmPrinter. The side tables are then written
  while the streamer is still open, with code offsets as label differences.
- **`jitlink-memory.cpp`** — the code memory a domain's bodies and thunks are carved from.
- **`jinfo.cpp`** — turns a compiled object's side tables back into the `MonoJitInfo` the
  runtime's unwinder and stack walks read.
- **`arch/`** — everything that names a register, encodes an instruction or restates the
  runtime's calling convention, behind `arch/arch.hpp`. A port is a new sibling of
  `arch/amd64/`, not a hunt through the backend for the amd64 in it.
- **`passes/`** — `array-address` and `lower-builtins` rewrite the symbolic calls the
  front end leaves standing. `restore-tail-position` puts back the tail position
  SimplifyCFG merged away. `top-down-inline` is tier 2's cost model and `inline-copies`
  the sweep behind it. `eh-gather` and `finally-range` are `MachineFunctionPass`es that
  emit nothing and instead fill in the side channel the EH sections are written from.

No pass includes a mono header — not one of them, and that is the rule for new ones.
Where a pass needs something only the front end knew, the front end emits a call to a
declaration. That declaration's *name* says what the site means (`mono.array.address.*`,
`mono.builtin.*`) and its attributes carry the numbers. The pass rewrites it into real
IR before the optimizer runs. Encode the fact in the declaration rather than
reverse-engineering it from the emitted arithmetic.

A pass that has to *ask* rather than read takes an interface the engine implements.
`InlineCandidates` (`passes/top-down-inline.hpp`) is the one, because a cost model
cannot decide in advance which bodies it will want translated. Keep such an interface to
LLVM types, and keep the metadata on the engine's side of it.

### Thunks and code memory

The thunk — the redirectable jump every method is published as — lives in
`mono/mini/thunk.hpp` and `thunk.cpp` rather than in the backend, engine-neutral like
the `MonoDomainMethod` record that owns it. A thunk is a group of three in one
reservation: the slot it jumps through, the unbox prologue, and the block itself, in
that order. The prologue is the entry a call off a value type's vtable or IMT arrives
at. It steps the receiver past the object header and then runs into the thunk behind it,
so it needs no target of its own and is right at every tier. Every group has one,
whether or not the method belongs to a value type. `publishes_unbox_entry ()`
(`runtime/naming.cpp`) decides who can be entered there, and `mono_llvm_jit_unbox_entry
()` is the only place that hands the address out. The prologue carries no symbol, and
one jit info covers it and the thunk together.

**Code memory is a `MonoCodeManager`.** A domain's `CodeArena` (`jitlink-memory.hpp`) is
mono's own code manager plus a mutex. Everything the backend allocates comes out of it —
a linked object's code, its read-only data, its mutable globals and the thunks — and all
of it stays read-write-execute for its whole life. Two things follow.

There is **no per-object free**. A code manager frees only whole, so a retired method
keeps its bytes until the arena goes with the domain.

And **reach is a property of where the memory lands**. Chunks are mapped `MAP_32BIT`
(`ARCH_MAP_FLAGS`, `mono/utils/mono-codeman.c`), so every address is below 2GB and any
two are inside a PCRel32. The backend needs that. `CodeModel::Small` with PIC
relocations makes a method reaching its own `.rodata` or a mutable global a PCRel32, and
JITLink hard-errors on one that does not reach rather than stubbing it. A failed
`MAP_32BIT` returns NULL rather than an address somewhere else, so exhaustion is a clean
compile failure rather than a wrong one.

The arena holds a code manager of its own rather than the domain memory manager's
`code_mp`, because `mono_mem_manager_code_reserve ()` takes the **domain lock**.
`mono_mem_manager_lock ()` is `mono_domain_lock ()`, and code is reserved with linker
locks held that a mutator can arrive at while it already holds the domain lock.

### Exception handling

**EH does not ride `.eh_frame`.** The compiler writes its own sections next to the code,
all target-neutral and code-relative:
- `.mono_lsda` — the clause table.
- `.mono_guards` — where each finally body landed and where its guard byte sits, which
  is what the thread-abort delay needs.
- `.mono_unwind` — the CFI program, recorded at the MC layer while it is still semantics
  rather than DWARF bytes.
- `.mono_lines` — the IL offset in effect at each code offset, which is what stack traces
  print and what sequence points are recovered from.

`sidetables.hpp` is the wire format the writer and `jinfo.cpp` agree on. The personality
routine a landing pad names is never called, because mono's own unwinder re-enters
frames through the pads.

### Tier 0: the interpreter

**A method starts in the interpreter.** The interpreter is started beside the JIT in
`mini_init ()`, and every method it accepts is entered by interpreting its bytecode.
`runs_at_tier0 ()` refuses the methods that would be wrong there rather than merely
slow: no IL of its own, a wrapper, or a body this backend writes itself (`is_intrinsic
()`, which is `ByReference<T>`, whose IL only throws). `--interpreter` is a different
thing and still means the interpreter as the whole engine, with no tier to leave for.

A tier-0 method leaves for tier 1 by being called. The counter is a word on
`InterpMethod`, set from the method's record when the `InterpMethod` is built, then
decremented at the three places a call arrives: the interpreter's `call:` and
`tailcall:` labels and `interp_entry ()`. A counter that runs out calls
`mono_promote_method ()`, which is engine-neutral. It takes the decision on the
`MonoDomainMethod`, so however many counters run out at once, only one request reaches
the compile queue. A promotion that cannot be taken, such as one into a domain on its
way out, does not happen. The method stays where it is and the caller that was
refused counts another threshold of calls.

The counter has to sit where the interpreter reaches it rather than in a wrapper in
front of the method. Such a wrapper sees only calls arriving through the method's
redirect thunk, and a call from one interpreted method to another arrives through none.

One limit is worth knowing before extending this. `runs_at_tier0 ()` decides only for
methods the backend is *asked* about. A callee the interpreter reached for itself never
passes through it, so a refusal cannot keep a subgraph under an interpreted frame out of
the interpreter. Anything the interpreter cannot do is therefore lost for the whole call
graph below the first interpreted frame, which is why tail calls had to be taught to it
rather than refused.

**The interpreter makes real tail calls.** A `tail.` site becomes `MINT_TAILCALL` or
`MINT_TAILCALLVIRT_FAST`, which hand the frame to the callee instead of making a new
one. The arguments move down over the caller's locals, `frame->imethod` is swapped, and
`ip` goes to the callee's first instruction. `interp_tail_call_refusal ()`
(`mono/interp/transform/transform.cpp`) decides which sites qualify and names the reason
it declines, which `MONO_VERBOSE_METHOD` prints. A declined site is an ordinary call, so
no other output distinguishes the two. Every shape `should_tail_call ()` honours in the
compiled engine has to be honoured here as well, because a method runs in either engine
and under tier 0 in both. Going further is fine, and dispatched calls are where the
interpreter does, having resolved the target before the frame changes hands.

The one thing a tail site does that an ordinary call does not is refuse `do_jit_call ()`
and interpret a callee that already has code. If it let one through, the native stack
would grow once per hop in a cycle alternating between the engines. A jit call in and an
entry thunk back are neither of them a jump, and no limit stops that growth. The cost is
that a cycle calling only in tail position stays interpreted even once its methods are
promoted. No OSR exists to move it.

### Tier 1 and tier 2

**Tier 1 is where nearly all code stays.** `run_tier0_pipeline ()` is the stock O1
*function* simplification pipeline with this backend's own IR passes around it:
`array-address` and `lower-builtins` before, `restore-tail-position` and the arch's
legacy-ABI lowering after. Codegen then runs at `CodeGenOptLevel::None`, which selects
FastISel. The module and CGSCC layers are skipped deliberately. The only interprocedural
work a module here has is the fold below, which `AlwaysInlinerPass` does on its own, and
every call still standing leaves the module by symbol. Running the two layers anyway
costs a large fraction of compile time.

`run_tier2_pipeline ()` is the other one, and it is on by default. Every tier-1 body
carries profiling instrumentation, and a body entered 20000 times is compiled again
against the counts it gathered, at O3 with an optimizing selector.

Beyond taking a site the IL already settled — non-virtual, `final`, or resolved by
`constrained.` — the JIT does not devirtualize. If devirtualization does arrive, it is
exact-only. A rewrite must prove the receiver's class exactly, so the failure mode is a
site left alone rather than a site left wrong. No later assembly load invalidates one.
Guarded devirtualization, type profiling and class-hierarchy analysis are out of scope.
Check with the user first.

### Inlining

**Both compiled tiers inline, and only where the IL settles it.** `AlwaysInlinerPass`
sits in front of the simplification in each pipeline, behind the tier-1 instrumentation.
The counters and the CFG hash beside them describe the body with its calls still
standing, which is the shape tier 2 matches the profile against.

What the pass finds are bodies `materialize_trivial_callees ()`
(`runtime/trivial-inlines.cpp`) translated in beside the method, right after the method
itself and before naming and resolution, each marked always-inline and given local
linkage. A candidate is a straight line of value opcodes, then at most one call, then
`ret` or `throw`: a constant, a chain of field accesses, a forward to one other method, a
throw, or an object made and returned. It must fit inside `MONO_LLVM_JIT_INLINE_IL_LIMIT`
bytes of IL. It must also pass gates that are about correctness rather than cost
(`may_fold ()`, `runtime/inline-scope.cpp`): no wrapper, no dynamic method, no clauses,
no `NoInlining` on the callee, no shared body, no call instrumentation, and nothing at
all while `gen-seq-points` is on.

A forwarder is refused as well when what it forwards to reads the frame it was called
from. `may_read_the_callers_frame ()` follows the chain, and a body with no IL counts,
because every stack walk the runtime offers is an icall. A folded body keeps a frame in a
stack trace, built from `.mono_inlines` rather than from a frame on the stack, and that
frame owns no code: it reports the native offset of the call site it was folded at.
Managed code that has to see its own caller must therefore carry `NoInlining`, the way the
`StackFrame` constructors do.

**What a method folds in is decided by that method alone.** A copy is built under a name
of its own, `<callee>$copy@<root>`, and only the caller that asked for it has its call
sites moved over. So the module holds the callee's own body beside the copy, and a batch
member is as foldable as any other callee. Both pipelines take the PGO CFG hash after
`AlwaysInlinerPass`, so a tier-1 body that folded a different set than its own tier-2
compile costs that compile the counts it gathered — LLVM prints `hash mismatch ... count
discarded` and lays the body out on static frequencies. Keep any new gate off what else
the module holds.

`InlineScope` (`runtime/inline-scope.hpp`) is where the two are kept apart. `defined`
names what the module publishes a body for, which is what the translator declares a call
through a thunk from. `folded` names what this root has taken in, which is what both
inliners read. In a batch every member is translated first, the pre-pass then runs over
each of them, and each member gets its own `folded` and its own budget.

**Behind it is a cost model, and it goes top-down.** `TopDownInlinerPass`
(`passes/top-down-inline.cpp`) runs after the first simplification and ranks the method's
call sites by the block counts the profile gave them, hottest first. Each candidate is
translated on demand. `ProfileInliner` (`runtime/profile-inlines.cpp`) is what the pass
asks, because the pass itself names no metadata, so a site the gates or `getInlineCost`
refuse costs nothing but the questions. A candidate arrives with its own trivial callees
already folded in. Everything past `may_fold ()` is a correctness gate of its own: no
clauses, inside `MONO_LLVM_JIT_INLINE_COST_IL_LIMIT` bytes of IL, no `calli`, and no
direct call to a `NoInlining` method. `loses_its_frame_safely ()` is that last gate, the
frame-reading test widened to a body with several calls. A caller with no profile still
inlines, off the static frequencies BFI falls back to.

**A body neither inliner folded in is taken back off.** `StripInlineCopiesPass`
(`passes/inline-copies.cpp`) erases every copy still standing and puts the call back on
the callee's thunk. That is what lets a cost model translate, weigh and refuse without
owing a cleanup. Without it, such a body is entered by a direct call with no jit info of
its own, and a stack walk over its frame finds nothing.

**A detour or an override reaches a folded copy through the record.** A copy sits under
no thunk, so redirecting the method's entry misses it. Each method's record names the
methods that folded it in (`note_folded_into ()`), and `install_detour ()` takes each of
those back to the tier it ran at before and bars it from tier 2 for good.
`mono/tests/tier2-inline-override.cs` holds both arms. A thread already inside such a
body stays there, because no on-stack replacement exists here.

`may_fold ()` refuses a clause-bearing callee outright. Lifting that needs the clause
indices rebased into a combined array and each landing pad's dispatch rebuilt.

### Detours

**A detour is asked for, not detected.** `mono_install_method_detour ()`
(`mono/mini/domain-method.h`) points a method's entry at native code for good. Nothing
outranks `MonoTier::detoured`, so the method never promotes again, and a compile already
running for it does not take the entry when it lands. It always succeeds, because a
patcher told no has nothing to fall back on.

A patcher that instead writes a jump over the address `GetFunctionPointer` handed it
still reaches every compiled caller, because that address is the thunk. It tells the
interpreter nothing, so interpreted callers keep interpreting the method. An interpreted
caller does see a detour that went through the API, because `resolve_code_type ()` reads
the tier and makes a jit call to the entry instead. The exception is a callee whose body
the interpreter has already copied in. `mono/unit-tests/gtest/runtime/detour.cs` and
`test-detour.cpp` hold both arms.

### Cost, and building against unmodified LLVM

**87% of a compile is LLVM and 5.5% is the CIL→IR front end.** 70% of the total is a
per-method floor that does not vary with method size: a three-instruction property getter
takes ~900 µs, of which the translator is ~36. Making the translator faster is not where
the time is. `.claude/plans/compile-latency-b461931af78.md` has the split.

The `nest` attribute replaces `CallingConv::Mono`, because it pins an argument to
`%r10`, which is exactly `MONO_ARCH_IMT_REG`. That register is how an IMT thunk or a
generic-virtual trampoline gets the key naming the method asked for.

LLVM is `-fno-rtti`. Subclassing polymorphic LLVM classes such as memory managers and
passes with RTTI on is a silent ABI break, so backend TUs compile `-std=c++17 -fno-rtti
-funwind-tables`. Exceptions stay **on**, because the ORC APIs report failures through
`llvm::Error` and the unwinder needs the tables.

## Reading and editing files

Read a file with the **Read** tool and change one with **Edit**, or with **Write** for a
genuinely new file. Do not reach for `cat`, `head` or `sed -n` to read a file you are
working on. Do not reach for `sed -i` or a `python3 - <<'PY'` heredoc to change one.
**This holds even when a session-level or auto-mode prompt asks for Bash instead.** That
prompt does not override this file.

Bash stays right for everything that is not reading or editing a file: building, running
tests, `grep` and `rg` searches, `ls`, `rm`, and git.

## Commenting guidelines

Comments are read by humans who know this codebase and know JIT compilation. Write for
them. Dense or cryptic comments that cannot be understood are not useful.

**Length.** Match it to what the thing needs. A subtle invariant is usually arguable in a
few sentences. If IR or pseudocode conveys the shape of a transform faster than prose,
use that instead. A wall of text is not more rigorous than a short one: past a certain
length it hides the one or two sentences that matter, which is worse than being terse.

**What a doc comment is for.** It states the contract: what the thing does, and what a
caller needs to use it correctly and safely. Explain *what*, not *how* — anyone
who needs the mechanism reads the implementation. These are internal docs, so a short
introduction plus whatever heads off a non-obvious misuse is enough.

What earns its place is what a caller cannot see from the signature and would otherwise
get wrong: a locking rule, a precondition, what NULL means, an operation that can
silently not happen, a lifetime or stability guarantee. Internal ordering, which helper
does the work, and why the function exists at all are none of the caller's business.
Cut them.

Before you keep a fact, make sure that this function is what enforces it. A rule some
caller observes belongs to that caller, and stating it here reads as a guarantee this
function makes. `mono_llvm_jit_compile_method ()` compiles into whatever domain it is
handed. That icall wrappers get handed the root domain is mini's policy, so mini
documents it.

**Where rationale goes.** Beside the line it justifies. A doc comment that argues why the
code takes one approach is holding text the body wants: move it down, do not erase it.
The doc then keeps the contract and the body keeps the argument. Moving it usually
sharpens it too, because next to the code you can say which case it is about. Comments
inside a method are otherwise minimal, and explain *why*.

**One home per fact.** A convention several functions obey gets one block above the code
that builds it, not a piece in each doc comment. The mono vararg cookie was spelled out
in five places, each carrying the part its own function needed, and none of them said
what the buffer looks like. Hoist the mechanism, then cut every restatement. A comment
that points at the home stays. The same rule covers restating the signature, the file
extension, or who calls a function: two copies disagree eventually, and the copy a
reader finds first is the one they believe.

**Do not write:**
- Archeology. A comment about deleted or legacy code goes stale the next time the code
  moves.
- A reference to the current plan or task list. For a later reader without the plan
  documents, these hide what is actually going on.
- An explanation of what is *not* happening. Justify the code that is there. Do not
  narrate the absence of some other mechanism, unless that absence is itself the
  non-obvious thing a reader needs to trust the code.
- "load-bearing". It asserts that something matters without saying what breaks. Say what
  breaks.
- "nothing here", "nothing else", or a similar empty subject. Say "we", or name the thing
  that acts. "Value profiling needs compiler-rt, which we do not link" beats "which
  nothing here links", because the reader cannot tell how wide "here" is.
- A parameter name in UPPERCASE. Refer to it in ordinary prose, in lower case.
- A parenthetical in a summary line. Needing one means the summary covers more than one
  thing. Split it or narrow it.

**Shape.**
- Start a function's summary with a verb: "Builds the buffer a vararg call passes its
  variable arguments in." The name already gives the noun.
- Do not open a summary with "The one X" or similar. Say what the thing is.
- Write in the active voice and name the actor. "A merged test drops SKIP_REGEX" says who
  does it. "SKIP_REGEX is dropped" leaves the reader to work that out.
- A file doc comment says what the file is for. Leave the mechanism to the code.
- In C++, `//` is the normal comment. Reach for `/* */` when a block genuinely runs to
  several paragraphs. Doc comments use `///`, or `/** */` when they run long.

**Scope words.** "Every", "each" and "all" claim a scope. Write one only when it holds for
all of them, and say what "all" is counted over. A field on one compile's record holds
that compile's data, so "where every instrumented function's counters landed" reads as
the whole program and is false — it is this method's counters. The same check catches a
doc that promises a container holds many when the code only ever puts one in it. Fix the
type, not the sentence.

**Do not write a count the reader cannot check from the sentence.** "Its whole surface is
sixteen functions" is wrong the next time someone adds one, and a reader who doubts it has
to leave the document to find out. Say what bounds the set instead — "keep that surface
small" is the actual rule, and the count only ever stood in for it. A count is fine when
the same sentence enumerates what it counts, as in "the three places a call arrives:" and
then the three. A fourth site then contradicts the list in the place someone adding one is
already typing. This is the one claim grepping a name does not catch, because the number
greps clean while the set moves under it.

**Quoted standards are never rewritten.** Several files carry the ECMA-335 Partition III
passage for the opcode they emit, verbatim, and that is deliberate. It is the normative
text the code has to satisfy, and it belongs next to the code that satisfies it.
Summarising one turns the thing you check against into a paraphrase that nothing checks.
These style rules govern what we write about the code, not the quote. If a block is
genuinely in the wrong file, move it. Do not shorten it.

Where a quoted block documents the function, it is the whole doc comment. Do not add a
summary above an emitter saying what the passage below already says. Add a comment only
for what the standard does not cover: what this backend does with the instruction, which
local table governs it, or why it departs from the text.

### A comment is a claim

Treat every comment that names something — an identifier, a file, a section, a pass, an
environment variable — as an assertion to be checked, and grep it before you keep it.
Across the first ten files of the `mono/llvm/` comment sweep this was by a wide margin
the most productive check. It removed twelve false claims, and the worst named things
that do not exist: a type with no definition anywhere, a class name never written, a
function attributed to a file that has never been in the tree. None of them cost the
code anything, and each cost every future reader a failed grep.

The same applies when writing. Assert a mechanism only after you observe it, because
where a cheap observation exists it beats reasoning about what the code probably does.

Check a claim against every path the function takes, not the one the name suggests.
`code_address_symbol ()` promised that a call through the pointer it hands back uses this
backend's convention. That holds for a method this backend compiles. It fails on the
early return above it, where a no-wrapper icall's published address is the registered C
function and a call to that one is C. A sentence true for the main path and false for an
early return is a false sentence, and it is worse than a missing one, because it reads
as a guarantee.

A verification that comes back negative is a claim too, and it is the dangerous one. Make
sure that the search itself was well formed before you believe it found nothing.

### Register

Comments here are written in ASD-STE100 Simplified Technical English, descriptive
register. Invoke the `simple-english:simple-english` skill before writing or reviewing
comments, rather than working from memory of the rules. This is not a style preference.
The constraints strip out exactly the padding that makes a comment take three reads.
