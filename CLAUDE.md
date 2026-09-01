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

**Build with `-j"$(nproc)"`.** A smaller `-j` is not how you keep out of another
worktree's way: it makes your own build longer, and it leaves the machine idle
whenever the other work is not runnable. Set the priority instead — `nice -n 20 cmake
--build build -j"$(nproc)"` for a background build. The scheduler then gives the cores
to whatever else wants them and gives them back the moment nothing does. Benchmarks
run at `nice 5` and sweeps at `nice 10`, so a build at 20 yields to both.

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
cmake --build build --target check-all   # everything but slow/stress/acceptance; ask first

ctest --test-dir build -L regression -j"$(nproc)"   # the mini corpora, one test each
ctest --test-dir build -R test-llvm  -j"$(nproc)"   # the LLVM backend unit tests
ctest --test-dir build -N                           # list without running
```

Labels: `regression`, `llvm`, `runtime`, `gshared`, `sgen`, `interp`, `bcl`,
`bcl-xunit`, `compiler`, `tools`, `benchmark`, `slow`, `stress`, `acceptance`.
`ctest --print-labels` is authoritative for the configuration you built. `check` is a
few hundred tests and seconds. Corpora are built by the regular build, not by ctest, so
build before you run `ctest` directly.

**Never run `check-all`, or a whole-tree `ctest` that stands in for it, unless the user
asks for it in so many words.** It is thousands of tests over both collectors and it
takes the machine for as long as it runs. Other worktrees on this box run their own
suites, and two `-j18` runs at once put the load average past 60, which makes both sets
of results worthless and gives the class-library suites timeouts that read as
regressions. Reach for `check`, or for the one label that covers what you changed, and
say what you ran. When a change really does need the wide gate, ask first and let the
user pick the moment.

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
gone, and mono rejects them like any other unknown option. Two LLVM-facing flags are
left. `--llvm-opt=OPT` forwards `OPT` to LLVM's own command-line parser; repeat it to
pass more than one. The AOT compiler refuses immediately.

`--ffast-math` is the other, and it is the only way to leave IEC 60559 here. It gives
every float operation a method's own IL asked for the flags `relaxed_float_flags ()`
(`runtime/options.cpp`) names: `reassoc`, `nsz`, `arcp`, `contract` and `afn`. `nnan`
and `ninf` are left out, because ECMA-335 I.12.1.3 makes a NaN and the two infinities
the answer an ordinary operation gives. Off by default, and off is the conforming
setting — task #234 and `.claude/handoff/float-strictness/` hold why no flag can be a
default. Three things follow from asking for it, and each is visible in one run:
- The interpreter relaxes nothing, so a method's answer changes when it promotes out
  of tier 0.
- Tier 1 selects with FastISel, which does not fuse, so a multiply-add contracts at
  tier 2 and not before.
- The arithmetic this backend writes to lower an opcode keeps strict semantics,
  because only the operations the IL asked for carry the flags.

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
  points are `il`, `mint`, `unopt-ir`, `tier1-ir`, `tier2-inlined-ir`, `tier2-ir`,
  `tier1-asm` and `tier2-asm`.
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
- `tier2-inlined-ir` — the IR the tier-2 inliners leave: behind `AlwaysInlinerPass`, the
  cost model and the sweep that takes the copies neither folded in back off, and in
  front of the lowering and the O3 pipeline. A type test and a vtable read are still
  one call each here, which is the half `tier2-ir` no longer shows. The simplification
  the cost model reads has already run in front of it, so this is not the translator's
  own IR — `unopt-ir` is that.
- `tier1-asm` / `tier2-asm` — the code the tier emits, side-table sections included,
  which is the half no offline `llc` run reproduces. Intel syntax, which `jit.cpp` asks
  for as a default; `--llvm-opt=-x86-asm-syntax=att` gets AT&T back. It costs a second
  codegen over a clone, so the published code is untouched.

**An IR point prints a module, not a function.** Each dump holds the method's body, the
bodies an inliner folded into it, and the declarations, the globals and the metadata
those bodies name. So `opt` reads the file as it stands, and an offline run of a
pipeline stands in for the one inside the process. The other bodies in the module are
dropped, and each dump costs a copy of that module.

A tier-1 promotion compiles up to `--llvm-opt=-mono-batch` methods in one module, and the
IR and assembly points still print one method for each dump: the IR point keeps that
method's body and drops the others, and the assembly point drops the other bodies from
the clone it codegens. So a method that promoted in the middle of a batch has a dump of
its own, and naming it in the filter finds it. Reading a whole batch therefore costs one
codegen for each method in it, which is what bounds an unfiltered `tier1-asm` sweep.

LLVM's own print options work through `--llvm-opt`: `-print-after=<pass>`,
`-print-before=<pass>`, `-print-after-all`, `-print-changed` and the flags beside them.
Each tier registers a `StandardInstrumentations` on its own callbacks (`jit.cpp`), and
they print to stderr. `-debug-only=` is unavailable, because the installed LLVM defines
`NDEBUG`.

`-pass-remarks-missed=<regex>` and the two remarks beside it print as well, through the
context's diagnostic handler rather than through the instrumentations. A pass that
declines silently emits no remark, so an empty output there says nothing about the pass.

Two compiles printing at once interleave into text that names no method, so a command
line carrying one of those options compiles on **one worker** by default.
`ir_printing_enabled ()` is what reads the command line for them, and a name it does not
know still prints — it only misses the worker setting. One worker narrows the overlap
rather than removing it: a compile the runtime asks for by name runs on the thread that
asked, and prints over the worker.

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
and the note says which split. Each is an LLVM command-line option, reachable through
`--llvm-opt=-mono-x=value` or, repeated, `--llvm-opt=-mono-x=v1 --llvm-opt=-mono-y=v2`,
and each is registered `cl::Hidden`, so `--llvm-opt=-help-hidden` is what lists them from
a built binary. A test drives one through `MONO_ENV_OPTIONS`, which `mono-sgen` and
`mono-boehm` read before they parse their own argv — a bare gtest binary has no such
argv to read, so `mono/unit-tests/gtest/llvm/harness.cpp` forwards the same variable's
`--llvm-opt=` tokens into the same registration by hand:
- `--llvm-opt=-mono-tier0-filter=<substr|0>` (`runtime/options.cpp`) — narrow tier 0,
  which is otherwise every method the interpreter accepts. A false value compiles
  everything, which separates a tier-0 bug from a backend one. A substring gets a
  compiled caller and an interpreted callee into one process, which no threshold
  produces, because a callee is called at least as often as its caller.
- `--llvm-opt=-mono-tier1-threshold=<n>` (`runtime/options.cpp`) — calls at tier 0
  before a method is asked for as tier 1, default 10. Zero never promotes, which
  separates a tier-0 entry bug from a promotion bug. One promotes on the first call,
  which puts the switch inside a loop.
- `--llvm-opt=-mono-tier2-threshold=<n>` (`runtime/options.cpp`) — what a tier-1 body
  spends before it asks for tier 2, default 100000000. One unit is one instruction that
  emits code, and a call costs `-mono-tier2-entry-weight` on top, so one counter reaches
  a body that is hot and a body that is heavy. Every body takes a constant off at its
  entry: the blocks no loop holds, plus the entry weight. A body with a loop adds the
  turns up in a register on top, one add per loop header, and takes that total off at
  each exit. The exits are three: a ret, a throw of the body's own, and the pad the
  body's own fault clause names as its handler, which is what charges a callee's
  exception that unwinds through the frame. A body that takes none of the three — one
  entered once that runs forever — keeps the constant and no more: the rest needs OSR,
  which does not exist here. Zero leaves a body instrumented and counting while it never
  promotes on its own, which is what a test driving the tiers through
  `Mono.Tiering.MonoTier::PromoteNow` wants.
- `--llvm-opt=-mono-tier2-entry-weight=<n>` (`runtime/options.cpp`) — what one call adds
  to that count, default 5000. It is the exchange rate between how hot a method is and
  how much it does, in the same units: a body that does no work promotes after threshold
  over weight calls, which the defaults put at twenty thousand. The two halves find
  different methods, and neither one alone finds both — the calls find a
  three-instruction getter called millions of times, and the work finds a kernel called
  eleven times that never leaves its loop. Zero counts work alone, which separates a
  promotion the work asked for from one the calls asked for. `passes/tier-counter.hpp`
  has the emission and `runtime/options.cpp` the workloads the defaults are set against.
- `--llvm-opt=-mono-tier2=<0|false|empty>` (`jit.cpp`) — turn tier 2 off. It is on by
  default, which is what puts profiling instrumentation in every tier-1 body, so this
  switch separates a tier-2 bug from a tier-1 one. `runtime/options.hpp`'s own
  `tier2_enabled ()` answers the same question by calling into `jit.cpp` rather than
  registering a second option under a second name.
- `--llvm-opt=-mono-batch=<n>` (`runtime/options.cpp`) — promoted methods per compile,
  default 32. A batch shares one module, one IR pipeline, one codegen and one link, so
  LLVM's per-compile floor is paid once. The whole batch compiles before any of its
  methods is published, so a bigger batch makes each method wait for the slowest in it,
  and that is what bounds the setting rather than the amortising running out. Measure
  occupancy by methods rather than by batches: the distribution is bimodal, so the
  average batch and the batch the average method arrives in are different numbers, and
  only the second says what the amortising acts on. One compiles every method on its
  own, which separates a batching bug from a backend one. Only tier-1 promotions batch.
  A tier-2 promotion, a dynamic method and any compile the runtime asks for by name each
  go alone.
- `--llvm-opt=-mono-workers=<n>` (`runtime/options.cpp`) — the most threads the compile
  queue runs promotions on at once, default `mono_cpu_count () - 2` capped at eight, and
  one while LLVM prints. Zero, which is also unset, leaves that default in force. The
  queue starts a thread only when work outruns the ones it has. One puts every
  background compile back on a single thread, which separates a bug in a compile from
  a bug in two overlapping. Do not
  expect throughput to follow the setting. ORC's session lock is taken twice per
  compile, and that measured 2.86x out of 18 threads on a compile-bound workload. The
  cap does not come off that measurement: it describes throughput, and what a promoted
  method waits for is latency, which keeps falling after throughput stops scaling. The
  process pays for the shorter wait in compile CPU and wins anyway, because a method
  waiting for a body runs interpreted.
  `.claude/plans/tier1-promotion-latency.md` has the sweeps and what is still open.
- `--llvm-opt=-mono-worker-idle-ms=<n>` (`runtime/options.cpp`) — how long a worker
  waits for work before the queue retires it, default 1000. A retired thread detaches
  and exits, and the next enqueue that wants a thread starts a fresh one on the entry it
  gave back. So this decides how long a program past its warm-up keeps compile threads,
  where `-mono-workers` decides how many it can have. Zero keeps every thread that
  started, which separates the cost of retiring threads from the cost of holding them.
  Holding one is not free: the default suspend policy is preemptive, so the collector
  signals an attached thread and waits for it at every collection, wherever that thread
  parked. A restart costs around 0.7 ms, most of it rebuilding the pipelines and the
  TargetMachine, which are per-thread.
- `MONO_LLVM_JIT_RECOMPILE=<substr>` — translate matching methods afresh on every
  request instead of answering from the cache, so they end up with several live bodies.
  No other setting produces one, and the code that has to cope has no other exerciser.
  Left an environment variable rather than an `--llvm-opt`: a test names the method it
  wants recompiled, and the recompiled method's own name is what the substring matches,
  so there is nothing here a caller would reach for `--llvm-opt`'s registry to find.
- `--llvm-opt=-mono-fold-casts=<0|false|empty>` (`runtime/options.cpp`) — turn the
  type-test fold off, so every `isinst` and `castclass` is lowered to the probe and the
  icall whatever the IR says about the operand. On by default. The translator writes the
  same call either way, so the two arms differ in one pass, which is what separates a
  wrong answer from a wrong probe. It is also the negative control for what the fold is
  worth: on `linq-devirt`'s `LinqOne` at the wide inline gates, off gives 9 icalls in 932
  lines and on gives 3 in 483.
- `--llvm-opt=-mono-fold-delegates=<0|false|empty>` (`runtime/options.cpp`) — turn the
  delegate-Invoke fold off, so every Invoke reads its entry off the delegate whatever the
  IR says the delegate is. On by default, and tier 2 only. The translator writes the same
  site either way, which is what separates a wrong target from a wrong dispatch. Two
  producers name a target: a read of an initonly static names the object, so the call
  becomes a direct one; the cache a C# compiler writes for a lambda or a method group
  names a candidate, so the call becomes a compare against `MonoDelegate::method_ptr`
  with the direct call on the arm that matches and today's dispatch on the arm that does
  not. `method_ptr` rather than `method`, because an `ldvirtftn` delegate never writes
  back the override it resolves and a combined delegate leaves `method_ptr` null, so a
  match proves both. `mono/tests/delegate-fold.cs` gates it and carries the off arm,
  reading the `MONO_FOLD_DELEGATES` environment variable the suite sets alongside the
  flag to know which arm it is in.
- `--llvm-opt=-mono-guard-arrays=<0|false|empty>` (`runtime/options.cpp`) — turn the
  array dispatch guard off, so a dispatch on an array receiver reads its callee out of
  the receiver's vtable whatever the IR says the slot is declared with. On by default,
  and tier 2 only. The translator writes the same site either way, so the two arms
  differ in one pass, which separates a wrong target from a wrong compare.
  `mono/tests/array-devirt.cs` is the program that tells the arms apart, because the
  enumerator an array answers with names the element class the dispatch reached.
- `--llvm-opt=-mono-guard-classes=<0|false|empty>` (`runtime/options.cpp`) — turn the
  guessed-class dispatch guard off, so a dispatch this compile can only guess a class
  for reads its callee out of the receiver's vtable whatever the IR says the guess is.
  On by default, and tier 2 only. The translator writes the same site either way, so the
  two arms differ in one pass, which separates a wrong target from a wrong compare.
  `mono/tests/class-devirt.cs` is the program that tells the arms apart, because its
  negative control changes what the guessed field holds after the compile has already
  guessed it.
- `--llvm-opt=-mono-thread-static-fast-path=<0|false|empty>` (`runtime/options.cpp`) —
  turn the thread-static fast path off, so every thread static reads back through
  `mono_domain_get ()` and the `mono_class_static_field_address` icall. On by default.
  `mono/tests/thread-static-fast-path.cs` compares the two arms at tier 2.
- `--llvm-opt=-mono-dyn-calls=<0|false|empty>` (`runtime/options.cpp`) — turn off the
  interpreter's dyn-call plan, so every jit call back into compiled code goes through a
  `gsharedvt_out_sig` wrapper instead. On by default. `mono/tests/dyn-call.cs` gates both
  arms.

Inlining. `MONO_LLVM_JIT_TRACE=1` prints a line for each fold, which is the only place a
fold is visible from outside. Every knob below is an LLVM command-line option, reached
the same way as the tiering ones above:
- `--llvm-opt=-mono-inline-il-limit=<n>` (`runtime/options.cpp`) — largest callee in IL
  bytes the shape-test pre-pass folds in, default 32. Both compiled tiers run that
  pre-pass. Zero turns it off, which separates a bug in a folded body from one in the
  method that folded it. The limit is the policy: the shape test in front of it refuses
  control flow and the opcodes that describe a frame, and lets everything else through,
  so this is what decides how large a body folds. 32 is the knee rather than a round
  number — `GoParse` folds 851 distinct callees at 16, 878 at 32 and 887 at 128, so below
  it loses bodies and above it buys almost none. Raising `-mono-inline-budget` with it
  moves that 878 to 889, so neither knob is a large lever past the defaults.
- `--llvm-opt=-mono-inline-cost-il-limit=<n>` (`runtime/options.cpp`) — largest callee
  the tier-2 cost model translates so it can weigh it, default 128. It bounds translation
  rather than code size, because LLVM's own threshold decides what is worth folding.
  Zero leaves tier 2 with the pre-pass alone, which separates a cost-model defect from a
  pre-pass one.
- `--llvm-opt=-mono-inline-threshold=<n>` (`passes/inline-cost.cpp`) — the tier-2 cost
  model's base budget, default 225. `-mono-inlinedefault-threshold=<n>` sets the same
  field and is what wins when `-mono-inline-threshold` is not given explicitly.
  `updateThreshold ()` only ever narrows this per site, through `min ()` against
  `-mono-inline-cold-callsite-threshold` (default 45) for a cold one and `max ()`
  against `-mono-inlinehint-threshold` (default 325) for a hinted one, and it zeroes
  every bonus on the cold arm besides. Raising the cold threshold past the base buys
  nothing until the base is raised too — `#301` and `#321` both named a cold threshold
  of 400 with the base left at 225, and both got 225.
- `--llvm-opt=-mono-inline-depth=<n>` (`runtime/options.cpp`) — folds deep past a method
  the cost model may go, default 4. A call graph with a cycle never runs out of sites, so
  the loop needs this whatever the budget says.
- `--llvm-opt=-mono-inline-prepass-depth=<n>` (`runtime/options.cpp`) — the same reach
  for the pre-pass, default 8. It drains its worklist least deep first, so this decides
  what the leftover count goes on rather than what the first folds are. The count below
  is what bounds the translation, which is why the reach can be generous.
- `--llvm-opt=-mono-inline-budget=<n>` (`runtime/options.cpp`) — bodies the pre-pass may
  fold into one method, default 16. A chain of forwarders is what spends it. Each batch
  member gets its own count, so `-mono-batch` changes how many compiles run, not what
  any one of them folds in.
- `--llvm-opt=-mono-inline-cost-budget=<n>` (`runtime/options.cpp`) — bodies the tier-2
  cost model may fold into one method, default 16. A count of its own, so what one
  inliner takes in does not decide what the other is left to fold. Zero refuses every
  method this root has not folded already, which separates a cost-model fold from a
  pre-pass one.
- `--llvm-opt=-mono-inline-rounds=<n>` (`runtime/options.cpp`) — times the tier-2
  inliner takes up a method's sites again, default 4. One reads them once, which is what
  separates a fold a round exposed from one the method arrived with. A dispatch is not a
  site — its callee is a load — so a virtual or interface call becomes foldable only
  after `DevirtualizePass` answers it, and that needs the receiver's class, which a fold
  is often what settles. On `tier2-inline-policy.cs` one round folds 15 bodies and never
  reaches `Box:Area`; four fold 22 and reach it at all three of its sites. The budget
  above is what bounds the work, and this count is what stops a cycle.
- `--llvm-opt=-mono-fold-clauses=<0|false|empty>` (`runtime/options.cpp`) — turn off the
  tier-2 cost model's ability to translate a clause-bearing callee at all, so it is
  refused the way the shape-test pre-pass always refuses one. On by default.
  `clause_survives_fold ()` (`passes/top-down-inline.cpp`) is what keeps the fold safe
  when this is on: it clones the call site, folds the callee there and runs the same
  simplification the round applies for real, and the cost model folds the callee only
  when none of its own landing pads are left standing. `mono/tests/tier2-inline-clause.cs`
  gates both arms, reading the `MONO_FOLD_CLAUSES` environment variable the suite sets
  alongside the flag to know which arm it is in.

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
- **`passes/`** — `array-address`, `lower-builtins`, `cast-func` and `alloc-func` rewrite
  the symbolic calls the front end leaves standing. `restore-tail-position` puts back the tail position
  SimplifyCFG merged away. `devirtualize` and `fold-cast` answer a site whose operands
  the optimizer settled. `top-down-inline` is tier 2's cost model and `inline-copies`
  the sweep behind it. `eh-gather` and `finally-range` are `MachineFunctionPass`es that
  emit nothing and instead fill in the side channel the EH sections are written from.
- **`analysis/`** — what a pass asks about the IR, answering rather than rewriting.
  `constant-values` is `MonoConstantValues`, the function analysis every rule below
  reads a value through: it settles the whole function once, and answers the constant
  a value holds or the set of values it can be reached by. `operand-class` says what
  class a value holds, `escape` whether an allocation's pointer leaves the function,
  and `vtable-info` what a class's vtable symbol carries. `strip-casts` is the one
  traversal left, for a rule that compares two spellings of one address.

Where a pass needs something only the front end knew, the front end emits a call to a
declaration. That declaration's *name* says what the site means (`mono.array.address.*`,
`mono.builtin.*`, `mono.array.shape.*`) and its attributes carry what only the front end
can say. The pass rewrites it into real IR before the optimizer runs. Encode the fact in
the declaration rather than reverse-engineering it from the emitted arithmetic.

A pass may include a mono header for what a header already states: a field offset, the
width of a scalar typedef, a struct's layout. `passes/array-shape.cpp` reads MonoArray
that way. What travels on the declaration instead is the runtime state behind such a
fact — a class looked up by name, a metadata token, a parsed signature — and the symbol a
site falls back to. That split is also what lets a test drive a pass with no runtime under
it.

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
()`, which is `ByReference<T>`, whose IL only throws). A `MONO_WRAPPER_DYNAMIC_METHOD`
is the one wrapper it accepts, because it carries IL of its own from Reflection.Emit
and `create_delegate_method_ptr ()` otherwise compiles it on the thread that makes the
delegate over it. `--interpreter` is a different thing and still means the interpreter
as the whole engine, with no tier to leave for.

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
carries profiling instrumentation, and a body that has spent a hundred million units — one
for each instruction it runs and five thousand for each call — is compiled again against
the counts it gathered, at O3 with an optimizing selector. `TierCounterPass` puts that
counter in: every body takes a constant off at its entry, and a body with a loop adds the
turns up in a register on top and takes that total off at each exit. Such a body also gets
a fault clause over the whole of it, if it calls anything that can unwind. That clause's
pad charges the turns when a callee's exception unwinds through the frame. The calls that
can unwind become invokes on to the pad, and `mono_lsda.cpp` reads the set back as one
clause, so what the runtime holds does not grow with the calls.

**A type test is one call until late.** `emit_cast ()` writes `mono.cast.isinst` or
`mono.cast.castclass` carrying the class, the site's cache and the wrapper behind it, and
`LowerCastFuncPass` writes the probe and the icall back for every site nothing answered.
In between, `FoldCastPass` answers a site from what the IR says the operand is: an
allocation states its class, and a parameter states the class its slot is declared with.
A declared class is a bound, so an answer needs every class that slot admits to agree,
and `cast_answer ()` (`passes/fold-cast.cpp`) is that rule, with the argument for each
arm beside it. Two of them are worth knowing from outside, because both are places an
obvious rule is wrong: an interface target can be answered no only for an array operand,
since any subclass may implement an interface; and the single-inheritance
argument that two unrelated classes share no instance does not reach arrays, because
covariance puts `Derived[]` under both `Base[]` and `IMarker[]`. `mono/tests/cast-fold.cs`
gates both.

**An allocation is one call until late as well.** `emit_object_alloc ()` and
`emit_vector_alloc ()` write `mono.alloc.object` or `mono.alloc.vector` carrying the
vtable, the size or the element count, and the allocator behind it, and
`lower_allocations ()` (`passes/alloc-func.cpp`) writes that call back at
`LowerStage::post_inline`. The allocator is the collector's own managed allocator where
it has one, and the runtime's new-object or array-new icall where it does not, so the
collector reaches the IR as an operand rather than as the shape of the site.

Two attributes ride on the declaration, and both are what the one shape buys:

- `memory(argmem: read, inaccessiblemem: readwrite)`, so a store into a field reaches a
  load below an allocation. A call with no memory effects clobbers every location
  instead. The argument that the claim holds while SGen moves objects sits at the
  attribute, and it rests on `mini_gc_init ()` setting no `thread_mark_func`. A precise
  mark function, or a major collector that compacts, makes it wrong with no build error.
- `allockind("alloc")`, so LLVM erases an allocation nothing reads. A class the program
  can tell an erasure on — a finalizer, weak fields, or an allocation that can answer
  with a proxy — takes `mono.alloc.object.kept` instead, which carries no alloc kind, and
  so does every class while sequence points are on, because a debugger hands any object a
  frame holds to a method it is asked to call. `allocation_is_observable ()` is that rule.
  A collector acting on each allocation, as under `--gc-debug=collect-before-allocs`, is
  deliberately not part of it: a tool showing where the allocations are has to show the
  ones the optimizer took away. `mono/tests/alloc-elide.cs` gates both attributes, and its
  `runtime-alloc-kept` arm is what reaches the kept forms for a class nothing else marks.

Beyond taking a site the IL already settled — non-virtual, `final`, or resolved by
`constrained.` — the JIT devirtualizes where it can prove the receiver's class exactly,
so the failure mode is a site left alone rather than a site left wrong. No later
assembly load invalidates one.

**One site is taken on a class it cannot prove, and a compare is what makes it safe.**
`GuardDispatchPass` (`passes/devirtualize.cpp`) writes that compare, and it asks two
rules for the class to compare against. The array rule is asked first and answers from
the class the slot's declared type sets. The guess rule follows and answers from a class
an allocation or an initonly static read states outright, on a site the array rule
leaves alone. Neither class is one the compile can prove the receiver holds, so both
rules stand behind the same compare.

An array slot admits every array of its rank with the same cast class, so an `int[]`
parameter also holds a `uint[]` and an array of an enum over int, each with a vtable of
its own. The array rule sends such a dispatch through `receiver->vtable == <the slot's
class>`, calls that class's implementation directly on the arm that matches, and keeps
the dispatch on the arm that does not. `mono/tests/array-devirt.cs` gates both arms.

What it takes is the six reduced types ECMA-335 I.8.7 names, which is what I.8.7.1
compares to decide that two array types hold each other's values: a cast class of
`byte`, `int16`, `int32`, `int64`, `char` or `bool`. An enum needs no case of its own,
because II.14.3 admits only underlying types already in that set and an array of an enum
takes its underlying type's cast class. A reference element is left dispatching because
covariance puts a `Derived[]` in a `Base[]` slot whenever the program uses one and the
compare would miss. `float[]`, `double[]` and an array of an ordinary struct are left
dispatching because reaching one with another class needs an enum over a type II.14.3
does not admit. This loader takes such an enum without complaint, which is why
`exact_class ()` still refuses every array and no unguarded fold reaches those either.

CoreCLR draws the same line from the other side. It rejects the enum at load
(`MethodTableBuilder::SetupMethodTable2`) and still refuses to call any array with a
primitive element exact (`isExactTypeHelper`, `vm/jitinterface.cpp`), for the reason
above. RyuJIT's class GDV is the same instrument as this pass: a method-table compare
with the original dispatch on the arm that misses.

A second rule answers where the array rule does not. `guessed_class ()` reads a class
an allocation or an initonly static read states outright, reached through channels
that are not proofs: a field whose object escapes
still answers from the stores the walk can see, a merge answers where only some arms
name a class, and the zero a fresh allocation reads is skipped. A parameter's declared
class is refused, because a compare against a bound misses every subclass it admits.
`--llvm-opt=-mono-guard-classes` turns the guess off on its own, which leaves a wrong
target and a wrong compare one pass apart. `mono/tests/class-devirt.cs` gates it:
`runtime-class-guard` is the arm whose threshold reaches the guard, and
`runtime-class-guard-off` runs the same file with the guess off, which is the answer the
guess has to agree with.

Type profiling and class-hierarchy analysis stay out of scope beyond the two rules
above. Check with the user before speculating past them.

### Inlining

**Both compiled tiers inline, and only where the IL settles it.** `AlwaysInlinerPass`
sits in front of the simplification in each pipeline, behind the tier-1 instrumentation.
The counters and the CFG hash beside them describe the body with its calls still
standing, which is the shape tier 2 matches the profile against.

What the pass finds are bodies `materialize_trivial_callees ()`
(`runtime/trivial-inlines.cpp`) translated in beside the method, right after the method
itself and before naming and resolution, each marked always-inline and given local
linkage. A candidate is one straight line, then at most one call, then `ret` or `throw`:
a constant, a chain of field accesses, arithmetic on the arguments, a forward to one
other method, a throw, or an object made and returned. `declines_a_fold ()` names what
the line may not hold, and it is a denylist rather than an allowlist: control flow, and
the opcodes that describe the frame the body runs in. A body that reaches itself through
the forwarder chain is refused too, because no inliner takes that call away. The rest is
the size limit's question, and `--llvm-opt=-mono-inline-il-limit` bounds it. A candidate
must also pass gates that are about correctness rather than cost (`may_fold ()`,
`runtime/inline-scope.cpp`): no wrapper, no dynamic method, no clauses, no `NoInlining`
on the callee, no shared body, no call instrumentation, and nothing at all while
`gen-seq-points` is on. It also refuses a callee something else owns the entry of: a
detour or an override on the record, and an override the *table* holds
(`registered_override_for ()`), which is the one that catches a declared override before
anything has asked for the method's record and installed it.

**A folded body keeps a frame any walk that asks for it can see.** The compiler
writes `.mono_inlines` beside the code, `jinfo.cpp` turns it into the rows
`mono_jinfo_inline_frame ()` reads, and `mono_walk_stack_full ()` reports one
`FRAME_TYPE_INLINED` frame per folded body to a walk that asked for
`MONO_UNWIND_INLINED_FRAMES`. `mono_stack_walk ()` and `mono_stack_walk_no_il ()`
(`mono/metadata/loader.c`) both ask, which is what puts the folded frame in front of the
icalls that read their caller — `Assembly.GetCallingAssembly ()`,
`MethodBase.GetCurrentMethod ()`, the reflection stack marks and the core-clr security
checks all reach one of those two. So a fold does not change what managed code sees of
its own callers, and no gate refuses a callee for reaching such an icall.

Such a frame owns no code: it reports the native offset of the call site it was folded
at. `mono/tests/test-inline-call-stack.cs` is the gate, and it fails on
`GetCurrentMethod`, `GetExecutingAssembly` and `GetCallingAssembly` if either half of
this is taken out.

Two walks stay blind to a folded frame, and both are async-safe: the thread dump
(`mono/metadata/threads.c`) and `mono_stack_walk_async_safe ()`. Neither reads caller
identity, and a fold the inliners have always allowed is already invisible to them.

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
each of them, and each member gets its own `folded` and its own counts. The two inliners
keep a count each: what one of them takes in leaves the other's untouched, and a compile
translates at most the two added together.

**Behind it is a cost model, and it goes top-down.** `TopDownInlinerPass`
(`passes/top-down-inline.cpp`) runs after the first simplification and ranks the method's
call sites by the block counts the profile gave them, hottest first. Each candidate is
translated on demand. `ProfileInliner` (`runtime/profile-inlines.cpp`) is what the pass
asks, because the pass itself names no metadata, so a site the gates or `getInlineCost`
refuse costs nothing but the questions. A candidate arrives with its own trivial callees
already folded in. Everything past `may_fold ()` is a correctness gate of its own: no
clauses, and inside `--llvm-opt=-mono-inline-cost-il-limit` bytes of IL.
`NoInlining` on a call target is not one of the gates: the mark says
do not fold that target, which `may_fold ()` already enforces. A caller with no profile still
inlines, off the static frequencies BFI falls back to.

**The `getInlineCost ()` it calls is a copy of LLVM's**, `passes/inline-cost.cpp`, taken
from `llvm/lib/Analysis/InlineCost.cpp` at `llvmorg-22.1.8`. `CallAnalyzer` is in no
header, so a subclass outside LLVM cannot reach it. Two kinds of change touch the copy:
the mechanical ones that let it build outside LLVM, and the calls below into
`passes/inline-policy.cpp`. `passes/inline-cost.md` records both and how to read them
against a later release. Two of the mechanical ones matter from outside. Each command-line
option carries a `mono-` prefix, because LLVM's CommandLine calls `report_fatal_error ()`
on a name registered twice and the runtime links the dylib that registers all of them —
so `--llvm-opt=-mono-inline-threshold=N` tunes this copy and `-inline-threshold=N` tunes
the one the rest of the pipeline reads. And each of these entry points has to be called
qualified, because the arguments are llvm types and an unqualified call finds LLVM's
overload beside ours.

**What the copy asks that LLVM's cannot answer lives in `passes/inline-policy.cpp`**, so
the copy carries two calls and none of the policy. Each of the four answers has an option
of its own, which puts a run back on LLVM's own answers without a rebuild:
- `-mono-inline-implicit-null-free` — leave the raising arm of a `!make.implicit` branch
  out of a callee's cost, because ImplicitNullChecks folds the test into the dereference
  and mono raises from the faulting instruction rather than entering the handler. Every
  managed dereference carries such a check, so the arms are most of what a freshly
  translated body costs: a body with nine of them measured 30 against 0.
- `-mono-inline-devirt-return-bonus` — the callee answers with an object it allocated
  under a class it names, and the caller dispatches on that answer.
- `-mono-inline-devirt-arg-bonus` — the site passes an object of a named class into a
  parameter the callee dispatches on.
- `-mono-inline-scalarize-arg-bonus` — the site passes a fresh allocation into a
  parameter the callee does not capture, so the fold hands SROA the accesses a call was
  hiding. What lets LLVM erase the allocation behind the scalarized fields is the alloc
  kind on `mono.alloc.object`, which both collectors emit, so this answers the same
  under either.

The three bonuses are threshold bonuses rather than cost discounts, and each is priced
as a count of calls the fold takes away. They go in behind `SingleBBBonus` and
`VectorBonus`, which are shares of the threshold, and behind the cold-callsite clamp,
which is what lets one reach a cold site at all. A hot site is weighed against
`HotCallSiteThreshold`, which is large enough that none of them decides anything there.

What states a fresh object's class in the IR is the vtable store `emit_object_alloc ()`
writes, rather than anything the allocation itself carries. `passes/inline-policy.hpp` says what each one recognizes
and `mono/tests/tier2-inline-policy.cs` gates the two that a threshold can be calibrated
against.

**A copy is kept only where the runtime resolves what it names.** Each inliner resolves
a copy's own externals as it builds it, and drops the copy when one of them fails. A
class the copy names that will not load is a failure the program is owed at the call,
inside whatever try the call sits in. A compile that fails instead raises it at the
root's entry, with the root's clauses gone. Dropping the copy leaves the call on the
callee's thunk, where the callee's own compile raises it.

**A body neither inliner folded in is taken back off.** `StripInlineCopiesPass`
(`passes/inline-copies.cpp`) erases every copy still standing and puts the call back on
the callee's thunk. That is what lets a cost model translate, weigh and refuse without
owing a cleanup. Without it, such a body is entered by a direct call with no jit info of
its own, and a stack walk over its frame finds nothing.

**A detour or an override reaches a folded copy through the record.** A copy sits under
no thunk, so redirecting the method's entry misses it. Each method's record names the
methods that folded it in (`note_folded_into ()`), and `install_detour ()` takes each of
those entries back to the lazy resolver it started at, so the next call compiles the
method again and `may_fold ()` keeps the copy out. Both compiled tiers run the pre-pass,
so an earlier body is no safer than the newest one — that is why the entry goes back past
all of them rather than down one tier. `mono/tests/tier2-inline-override.cs` holds both
arms. A thread already inside such a body stays there, because no on-stack replacement
exists here.

A compile that spans the replacement is refused rather than published: the record counts
the replacements (`folds_epoch ()`) and a body stamped with an older count never takes the
entry. `entry_point ()` then compiles the method again.

`is_small_and_clause_free ()` still refuses a clause-bearing callee at the pre-pass. The
tier-2 cost model does not: it folds one once `clause_survives_fold ()`
(`passes/top-down-inline.cpp`) has shown, on a clone of the call site folded and simplified
the way the round does for real, that none of the callee's own landing pads are left
standing — eh-gather.cpp reads a folded body's clauses off the root's own `!mono.clauses`
alone, so nothing describes one that survives.

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

**Invoke the `comment-review` skill before you write, change or review a comment.** It
holds the house rules, the procedure that applies them, and the catalogue of what gets
written in their place. New guidance goes there. Nothing about comments is restated here,
because a second copy is what the rules themselves forbid.

Working from memory of the rules does not substitute. A comment has to be argued past the
governing test, and the shapes that fail it read as good reasons until they are held
against the catalogue.
