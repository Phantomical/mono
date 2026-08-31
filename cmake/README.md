# The CMake build

This directory holds the shared pieces of the build: the configure-time probes,
the flag sets, the class-library machinery and the generated-file templates.

```
cmake/
  MonoOptions.cmake            the build's switches
  MonoCompilerFlags.cmake      CPPFLAGS/CFLAGS/CXXFLAGS/LDFLAGS, as INTERFACE targets
  MonoConfigureChecks.cmake    the header/function/type probes behind config.h
  MonoConfigureFixed.cmake     the config.h entries that are options, not probes
  MonoLLVM.cmake               the LLVM package -> the mono::llvm interface target
  MonoBtls.cmake               the BoringSSL sub-build
  MonoHelpers.cmake            small shared helpers and install paths
  MonoSubmodules.cmake         a read-only view of the git submodules
  MonoToolsRuntime.cmake       which mono the managed tools run on
  MonoCorpus.cmake             building and running a managed test corpus
  MonoMiniTests.cmake          the mini regression and tier-seam suites
  MonoRunTest.cmake            one corpus program as one CTest test, run as cmake -P
  MonoRunTracedTest.cmake      the same, requiring named methods to have compiled
  shard-corpus.py              deals the compiler corpus into parallel staging dirs
  MonoManaged.cmake            the class-library build: profiles and declarations
  MonoCompileAssembly.cmake    the per-assembly compile driver, run as cmake -P
  MonoManagedTests.cmake       the per-library NUnit and xunit suites
  MonoBclDiscover.cmake        splits one of those assemblies into groups
  MonoBclTests.cmake.in        what the split writes, read by CTest each run
  MonoResgen.cmake             .resx -> .resources
  MonoGacInstall.cmake         the install-time gacutil driver
  MonoGetMonolite.cmake        fetches the prebuilt bootstrap compiler
  MonoFetchCldr.cmake          fetches a CLDR release for tools/locale-builder
  MonoGenVersion.cmake         build-time version.h
  MonoGenBuildVer.cmake        build-time buildver-<gc>.h
  MonoBuildTreeConfig.cmake    build-time etc/mono/config
  MonoCheckEglibRemap.cmake    the "no bare g_* exports" check
  config.h.in                  the config.h template
  bdwgc/                       the Boehm collector's build and its config.h
  mono-wrapper.in              the uninstalled-runtime wrapper
```

## Configuring

```bash
cmake -S . -B build -G Ninja \
      -DMONO_LLVM_PREFIX="$HOME/llvm/install" \
      -DCMAKE_INSTALL_PREFIX="$PWD/tmp" \
      -DMONO_USE_SYSTEM_RUNTIME_FOR_TOOLS=ON \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache \
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build -j"$(nproc)"
```

Everything in the tables below is a cache variable: pass it as `-D<name>=<value>`
to any `cmake -S . -B build`, and re-run that same command after changing one.
`cmake -LAH build` prints the whole set with its help text and `ccmake build`
edits it interactively.

No option below is chosen for you. Only the programs in the last table are
searched for. The defaults are Unity's desktop Linux configuration, with two
exceptions worth knowing before a first build: `MONO_LLVM_PREFIX` is empty,
which builds no JIT at all, and `MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS` is off,
which is the slow way to compile C#.

## The LLVM tier

| variable | default | what it does |
| --- | --- | --- |
| `MONO_LLVM_PREFIX` | empty | Prefix of the LLVM install the backend builds against. |

Setting this is what defines `ENABLE_LLVM`, and the backend under `mono/llvm` is
the only JIT this runtime has. Leaving it empty builds the runtime with no JIT.

The prefix must ship `lib/cmake/llvm`. A prefix without one is a configure error
naming the directory it looked in. `MonoLLVM.cmake` then refuses anything below
LLVM 14, but that floor is not the supported set: the backend is built against
22.x and no older major is exercised.

Three properties come off the install rather than from an option here: the
version, whether assertions are on, and whether RTTI is on. Pointing the prefix
at an LLVM that disagrees with the one a tree was last built against needs a
rebuild, not a reconfigure.

## Garbage collectors

| variable | default | what it does |
| --- | --- | --- |
| `MONO_ENABLE_SGEN` | ON | Build the SGen collector and `mono-sgen`. |
| `MONO_ENABLE_BOEHM` | ON | Build the Boehm collector and `mono-boehm`. |
| `MONO_ENABLE_PARALLEL_MARK` | ON | Build Boehm with parallel marking. |

At least one collector must be on, and turning both off is a configure error.
When both are on, `mono-sgen` is what the `mono` symlink and the unsuffixed
`libmono*.so` aliases point at, and the `mono/tests` corpus runs under each,
which is what the `@sgen` and `@boehm` suffixes on those test names mean.

## Runtime pieces

| variable | default | what it does |
| --- | --- | --- |
| `MONO_ENABLE_INTERPRETER` | ON | Build the IL interpreter. |
| `MONO_ENABLE_JIT` | ON | Build the JIT. Off compiles the runtime with `DISABLE_JIT`. |
| `MONO_ENABLE_DEBUGGER_AGENT` | ON | Build the soft debugger agent. |
| `MONO_ENABLE_PROFILER` | ON | Build the profiler modules. |
| `MONO_ENABLE_ILGEN` | ON | Build runtime IL generation into libmono. |
| `MONO_ENABLE_ICALL_TABLES` | ON | Build the icall tables into libmono. |
| `MONO_ENABLE_EXECUTABLES` | ON | Build the `mono`, `monodis` and `pedump` binaries. |
| `MONO_ENABLE_LIBRARIES` | ON | Build the shared runtime libraries. |
| `MONO_STATIC_MONO` | ON | Link libmini into the `mono` binaries statically. |
| `MONO_ENABLE_CRASH_REPORTING` | ON | Build the structured crash reporter. |
| `MONO_CRASH_PRIVACY` | ON | Scrub private data from crash dumps. |

The interpreter is tier 0, so turning it off changes how the runtime executes
rather than what it ships: every method then compiles on its first call, and
tier 1 becomes the entry tier. `mini_init ()` wires the two engines together.

Turning off `MONO_ENABLE_EXECUTABLES` or `MONO_ENABLE_MCS_BUILD` takes the test
corpora with it. `MonoCorpus.cmake` needs both.

## Helper libraries

| variable | default | what it does |
| --- | --- | --- |
| `MONO_ENABLE_MONO_NATIVE` | ON | Build `libmono-native` (System.Native). |
| `MONO_ENABLE_SUPPORT` | ON | Build `libMonoPosixHelper` / `libMonoSupportW`. |
| `MONO_ENABLE_IKVM_NATIVE` | ON | Build `libikvm-native`. |
| `MONO_ENABLE_BTLS` | ON | Build the BoringSSL-based TLS provider. |

`MONO_ENABLE_BTLS` needs `external/boringssl` checked out. An empty submodule is
a configure error that names both fixes: fetch it, or turn the option off.

## The class libraries and the build's own managed tools

| variable | default | what it does |
| --- | --- | --- |
| `MONO_ENABLE_MCS_BUILD` | ON | Build the `mcs` class libraries. |
| `MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS` | OFF | Compile C# on a mono already installed on the machine instead of the one being built. |
| `MONO_SYSTEM_RUNTIME` | empty | Which runtime that is. Empty picks `mono-sgen`, then `mono`, off `PATH`. |
| `MONO_BOOTSTRAP_RUNTIME` | found on `PATH` | The mono that bootstraps the `build` profile. Pinned to a system mono whatever the option above says, because it is producing the first mscorlib. |
| `MONO_MCS_COMPILER_SERVER` | ON | Use the Roslyn compiler server for class-library compiles. |
| `MONO_MCS_PIPENAME` | derived from the build directory | The server's pipe name. Two build trees get different ones so they cannot share a server. |

Turning on `MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS` is the largest single cut to build
time available here. Read `MonoToolsRuntime.cmake` before you do: it has the
measurements, and the one way the option changes what comes out of the build
rather than how long it takes.

## Tests

| variable | default | what it does |
| --- | --- | --- |
| `BUILD_TESTING` | ON | CTest's own switch. Off registers no tests and drops the googletest dependency with them. |
| `MONO_CTEST_TIMEOUT` | 300 | Seconds a test that names no budget of its own may run. |
| `MONO_MERGED_TESTS` | OFF | Register one ctest test per suite rather than one per case. |
| `MONO_ENABLE_NETWORK_TESTS` | OFF | Run the tests that reach a host off this machine, or a message broker. |
| `MONO_ENABLE_GUI_TESTS` | OFF | Run the tests that need an X display. |
| `MONO_ENABLE_ACCEPTANCE_TESTS` | OFF | Build and register the acceptance suites. |

`MONO_MERGED_TESTS` is what to reach for when the per-case listing costs more
than it buys. Off, the gtest and managed-method suites give each case a process,
so a failure names the case and CTest spreads the cases over the cores. On, a
failure names the suite, and a case that takes the process down takes the rest
of the suite with it.

The two suite options exclude rather than delete. On a box without the broker or
the display, those tests report the missing service and not a runtime defect.

`MONO_ENABLE_ACCEPTANCE_TESTS` additionally needs the corpus submodules under
`acceptance-tests/external`, which this build never fetches — the `print-versions`
target reports which are in place and prints the command for the ones that are
not. The CoreCLR corpus alone is ~4700 assemblies to compile, so the first build
after turning this on is a long one.

## The peripheral directories

None of these is part of the runtime. They are on when they cost almost nothing
and catch something when they break, and off when they need a tool or a download
that a plain checkout does not have.

| variable | default | what it does |
| --- | --- | --- |
| `MONO_ENABLE_SAMPLES` | ON | Build the embedding and profiler samples. `EXCLUDE_FROM_ALL`, so `--target samples`. |
| `MONO_ENABLE_BENCHMARKS` | ON | Build the `mono/benchmark` microbenchmarks. The assemblies build with `all`; the CTest entries carry `benchmark;slow`. |
| `MONO_ENABLE_NLS` | ON | Compile and install the gettext catalogues. Skipped with a message when `msgfmt` is absent. |
| `MONO_ENABLE_LOCALE_BUILDER` | OFF | Build `tools/locale-builder`. |
| `MONO_CLDR_VERSION` | 30.0.2 | The CLDR release `culture-table` generates from. |
| `MONO_MINIMAL_LOCALES` | `en` | Regex over `locales/` selecting what `culture-table-minimal` keeps. |
| `MONO_ENABLE_DOCS` | OFF | Build the monodoc API documentation. Needs `mdoc.exe` and perl. |

`samples` is compiled and never run: these are the only in-tree users of the
embedding API and the profiler module ABI, so building them is the only thing
that keeps those headers honest.

The catalogues under `po/` are compiled and installed, but nothing reads them:
the compiler has no gettext binding, so the four translations have no effect at
runtime.

`culture-table` downloads a CLDR release and rewrites a checked-in generated
header, which is why `MONO_ENABLE_LOCALE_BUILDER` is off and the target is not
part of `all`.

## Compilation, instrumentation and config.h

| variable | default | what it does |
| --- | --- | --- |
| `MONO_ENABLE_COMPILE_WARNINGS` | ON | Use the maintainer warning set. |
| `MONO_ENABLE_VISIBILITY_HIDDEN` | ON | Compile the runtime with `-fvisibility=hidden`. eglib is built without it either way. |
| `MONO_ENABLE_COVERAGE` | OFF | Instrument the whole native build for `llvm-cov`. Clang only — the flags are `-fprofile-instr-generate -fcoverage-mapping`. |
| `MONO_THREAD_SUSPEND` | `preemptive` | Thread suspend policy: `preemptive`, `coop` or `hybrid`. |
| `MONO_WITH_TLS` | `__thread` | Thread-local storage mechanism: `__thread` or `pthread`. |
| `MONO_XEN_OPT` | ON | Optimize for Xen guests. |
| `MONO_ENABLE_DEV_RANDOM` | ON | Seed crypto from `/dev/random`. |
| `MONO_ENABLE_BIG_ARRAYS` | OFF | Allow arrays larger than `Int32.MaxValue`. |
| `MONO_ENABLE_JIT_DUMP` | ON | Emit perf jitdump files. |
| `MONO_ENABLE_INTERP_TRACE` | OFF | Build the interpreter's execution tracer. |
| `MONO_UNITY_DEFINE` | ON | Define `UNITY` in config.h. |

Two standard variables do not behave as they do elsewhere.
`CMAKE_EXPORT_COMPILE_COMMANDS` is set in the top-level `CMakeLists.txt`, which
shadows the cache, so `-D` cannot turn it off. `CMAKE_BUILD_TYPE` defaults to
`MinSizeRel` with `-Os -g`, this tree's usual `CFLAGS`, and passing a build type
takes that back.

`CMAKE_C_COMPILER_LAUNCHER` and `CMAKE_CXX_COMPILER_LAUNCHER` are the way to put
ccache in front of the compiler. `CC="ccache cc"` is not, because CMake's own
compiler checks trip over it. Naming ccache there also makes the cache shareable
across worktrees. `MonoCompilerFlags.cmake` has how, and what it costs in debug
info.

## Programs and libraries the configure step looks for

Each of these is a cache entry, so a machine where the search finds the wrong one
is fixed with a `-D`, not with `PATH`.

| variable | what it is |
| --- | --- |
| `MONO_BOOTSTRAP_RUNTIME` | `mono` or `mono-sgen`, for bootstrapping the class libraries |
| `MONO_SYSTEM_RUNTIME_EXECUTABLE` | `mono-sgen` or `mono`, the default host under `MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS` |
| `MONO_GNU_MAKE` | `gmake` or `make`, which drives the ms-test-suite corpus |
| `MONO_PERL` | perl, for the opcode table and the documentation build |
| `MONO_DOXYGEN` | doxygen |
| `MONO_MSGFMT` / `MONO_XGETTEXT` | the gettext tools, under `MONO_ENABLE_NLS` |
| `MONO_NM` | `nm`, for the "no bare `g_*` exports" check |
| `MONO_TIMEOUT_BINARY` | `timeout`, for the test harnesses |
| `MONO_PERF_BINARY` | `perf(1)`, for the profiled microbenchmarks |
| `MONO_XATTR_LIB` | `libattr` |

## How the runtime is assembled

Every piece of the runtime (`eglib`, `monoutils`, `monoruntime_sgen`, `mini`,
...) is an OBJECT library, and `libmonosgen-2.0`, `mono-sgen` and friends list
the full set.  libmono needs that: most of its exported surface is reached only
through icalls and P/Invoke, so a static archive would drop it at link time.

The one exception is `monosgen-static`, an archive form of the same objects used
by the unit tests, which need archive semantics: `test-path.c` includes
`mono/utils/mono-path.c` to reach its statics, so force-linking every object
would define those symbols twice.

`mono/metadata` is compiled twice, once per collector, because a handful of its
translation units inline the collector's allocation and write-barrier paths.

Two `config.h` entries look wrong and are deliberate -- the runtime has only
ever been built and tested with these answers, so changing them is a behaviour
change, not a fix:

* `HOST_LINUX` is never defined; `MONO_HOST_LINUX` is the one that is true.
* `HAVE_RT_MSGHDR` only asks whether `<net/route.h>` exists.

## The class libraries

Output goes to `build/mcs/class/lib/<profile>/`; nothing is written under
`mcs/`. The profiles are `build` (the bootstrap compiler and its tools),
`net_4_x`, `unityjit`, `xbuild_12`, `xbuild_14` and the install-only
`binary_reference_assemblies`.

A per-directory `CMakeLists.txt` only *declares*, with
`mono_declare_managed()`. The same source directory builds in several profiles
and references are bare assembly names, so nothing can be resolved until every
directory has been read; `mcs/CMakeLists.txt` materializes the whole set in an
epilogue, resolving each reference to the target that produces it. Add an
assembly by declaring it -- do not write `add_custom_command` yourself.

Each assembly is one command running `MonoCompileAssembly.cmake`, which expands
the directory's `.sources` through `gensources.exe`, writes a depfile naming
every `.cs` it selected, and runs `csc`. Ninja reads the depfile afterwards, so
editing a source rebuilds exactly the assemblies containing it.

Anything a directory needs beyond a compile -- a generated source, an IL module,
an extra install rule, a test fixture -- goes in a hand-written `extra.cmake` or
`generated-sources.cmake` beside the declaration, included by its
`CMakeLists.txt`. A file that several directories include must set the variables
its includers read *before* any `return()` guard, or only the first includer
gets them.

Two ways to embed a resource, and they are not interchangeable. `RESX` names
`.resources` files compiled from a `.resx` beside them, embedded under the file's
own name. `RESOURCE_DEFS` takes `<id>,<file>` pairs and embeds under `<id>`, so
the input can be a `.txt`, or a `.resx` in `external/`, and the assembly still
finds it by the name it asks for.

A `.sources` file name may be prefixed with a host platform, a profile, or
both. `gensources` takes the two valid name sets on its command line, from
`MONO_MANAGED_PLATFORM_NAMES` and `MONO_MANAGED_PROFILES`; it uses them to
enumerate a directory's whole set of targets, which is a mode nothing in this
build asks for -- the per-assembly compile names its platform and profile
outright.

## Tests

```bash
cmake --build build --target check       # the inner loop, ~20s
cmake --build build --target check-all   # everything but the slow/stress sets
```

Use those rather than a bare `ctest`: CTest is serial unless told otherwise,
and almost everything here is either internally parallel or tiny, so a bare
`ctest` takes minutes to do seconds of work. The targets pass `-j` with the
machine's core count.

| label | what | in `check`? |
|---|---|---|
| (none) | `mono/unit-tests` | yes |
| `regression` | the `mono/mini` corpus | yes |
| `llvm` | the LLVM backend's own gtest suites | yes |
| `eglib` | eglib's own suite | `check-all` |
| `runtime` | the `mono/tests` corpus (~700 programs) and its one-off suites | `check-all` |
| `gshared` | generic sharing, over four optimization sets | `check-all` |
| `sgen` | the SGen collector matrix, 42 configurations | `check-all` |
| `interp` | the whole corpus again under the interpreter | `check-all` |
| `interp-unit` | the interpreter's own tests, under `mono/unit-tests` | `check-all` |
| `interp-jit` / `interp-tier2` | the same programs with the interpreter off, and again at tier 2 | `check-all` |
| `interp-mixed` | the same programs promoting mid-run | no |
| `bcl` | the class libraries' own NUnit suites, one per assembly | `check-all` |
| `bcl-xunit` | the same for the corefx-derived xunit suites | `check-all` |
| `compiler` | `mcs/tests` and `mcs/errors`, ~6000 programs each | `check-all` |
| `tools` | mdoc, the linker, mono-symbolicate, xbuild, monop, csi | `check-all` |
| `slow` | minutes-long single tests | no |
| `stress` | long-running stress tests | no |
| `acceptance` | the external corpora, under `MONO_ENABLE_ACCEPTANCE_TESTS` | no |

Both targets select by regex over the labels, so `interp` in `check`'s exclusion
list takes the four `interp-*` sets with it. The four have targets of their own:
`check-interp`, `check-interp-jit`, `check-interp-tier2` and
`check-interp-mixed`. A program that answers differently under two of them is
the two engines, or the two compiled tiers, disagreeing about the same IL.

The suites that drive `test-runner.exe` already use every core, so they carry a
`PROCESSORS` property and CTest runs them one at a time while packing the
one-off tests around them.

Everything a test needs on disk -- the managed corpora and the class libraries'
test assemblies included -- is built by the regular build: the aggregates are
`ALL` targets ordered after the class libraries, so a finished `cmake --build`
has every test input in place and running ctest never builds anything.

A handful of `bcl` suites cannot pass here whatever the code does, and it is
worth knowing which before chasing one: corlib has ~11 failures that follow the
machine's ICU and tzdata, `System` has one live HTTP test, `System.Messaging`
wants a RabbitMQ broker, and `System.Windows.Forms` wants an X display. Re-run
the suite against the machine's installed mono (`MONO_EXECUTABLE`) to tell one
of those from a codegen bug: the environmental failures reproduce there too.

`mono/tests` keeps its corpus in `tests.cmake` (the lists),
`special-tests.cmake` (the tests with a recipe of their own) and
`runtime-suites.cmake` (the CTest wiring). The lists are the amd64/Linux/JIT
set; disabled tests are not built, and a few of them no longer compile.

## What is not here

* The AOT modes: this build is JIT-only.
* `msvc/`, the Visual Studio build. The `.vcxproj` files still work under MSVC;
  they simply have no CMake entry point.
* `sdks/`, the mobile and wasm cross-targets, which carry their own build.
