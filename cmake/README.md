# The CMake build

This directory holds the shared pieces of the build: the configure-time probes,
the flag sets, the class-library machinery and the generated-file templates.

```
cmake/
  MonoOptions.cmake            the build's switches
  MonoCompilerFlags.cmake      CPPFLAGS/CFLAGS/CXXFLAGS/LDFLAGS, as INTERFACE targets
  MonoConfigureChecks.cmake    the header/function/type probes behind config.h
  MonoConfigureFixed.cmake     the config.h entries that are options, not probes
  MonoLLVM.cmake               llvm-config -> the mono::llvm interface target
  MonoBtls.cmake               the BoringSSL sub-build
  MonoHelpers.cmake            small shared helpers and install paths
  MonoToolsRuntime.cmake       which mono the managed tools run on
  MonoMiniTests.cmake          the mini regression / tiered suites
  MonoManaged.cmake            the class-library build: profiles and declarations
  MonoCompileAssembly.cmake    the per-assembly compile driver, run as cmake -P
  MonoManagedTests.cmake       the per-library NUnit and xunit suites
  MonoResgen.cmake             .resx -> .resources
  MonoGacInstall.cmake         the install-time gacutil driver
  MonoGetMonolite.cmake        fetches the prebuilt bootstrap compiler
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
      -DMONO_LLVM_PREFIX=/usr/lib/llvm-18 \
      -DCMAKE_INSTALL_PREFIX="$PWD/tmp"
cmake --build build
```

`MONO_LLVM_PREFIX` points at an LLVM install; leaving it empty builds without
the tier-1 backend. The rest of the switches are in `MonoOptions.cmake` and
follow the `MONO_ENABLE_*` / `MONO_WITH_*` naming.

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
| (none) | `mono/unit-tests`, eglib's own suite | yes |
| `regression` | the `mono/mini` corpus, classic JIT | yes |
| `tiered` | the LLVM tier at each promotion threshold | yes |
| `runtime` | the `mono/tests` corpus (~700 programs) and its one-off suites | `check-all` |
| `gshared` | generic sharing, over four optimization sets | `check-all` |
| `sgen` | the SGen collector matrix, 42 configurations | `check-all` |
| `interp` | the whole corpus again under the interpreter | `check-all` |
| `bcl` | the class libraries' own NUnit suites, one per assembly | `check-all` |
| `bcl-xunit` | the same for the corefx-derived xunit suites | `check-all` |
| `compiler` | `mcs/tests` and `mcs/errors`, ~6000 programs each | `check-all` |
| `tools` | mdoc, the linker, mono-symbolicate, xbuild, monop, csi | `check-all` |
| `slow` | minutes-long single tests | no |
| `stress` | long-running stress tests | no |

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

## The peripheral directories

These are built, but none of them is part of the runtime and each has its own
switch (see `MonoOptions.cmake`):

| directory             | option                       | default | notes |
| --------------------- | ---------------------------- | ------- | ----- |
| `samples`             | `MONO_ENABLE_SAMPLES`        | ON      | built, not run and not installed; `EXCLUDE_FROM_ALL`, so `--target samples` |
| `mono/benchmark`      | `MONO_ENABLE_BENCHMARKS`     | ON      | assemblies build with `all`; the CTest entries carry `benchmark;slow` |
| `po`                  | `MONO_ENABLE_NLS`            | ON      | skipped with a message when `msgfmt` is absent |
| `tools/locale-builder`| `MONO_ENABLE_LOCALE_BUILDER` | OFF     | `culture-table` downloads CLDR and rewrites a checked-in header |
| `docs`                | `MONO_ENABLE_DOCS`           | OFF     | needs `mdoc.exe` from the class libraries and perl |

`samples` is compiled but never run: these are the only in-tree users of the
embedding API and the profiler module ABI, so building them is the only thing
that keeps those headers honest.

The gettext catalogues under `po/` are compiled and installed, but nothing reads
them: the compiler has no gettext binding, so the four translations have no
effect at runtime.
