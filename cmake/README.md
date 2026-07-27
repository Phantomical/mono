# The CMake build

This directory holds the shared pieces of the CMake build that replaced the
autotools one: the `configure.ac` checks, the flag sets, and the templates for
the files `config.status` used to generate.

```
cmake/
  MonoOptions.cmake            the --with/--enable switches
  MonoCompilerFlags.cmake      CPPFLAGS/CFLAGS/CXXFLAGS/LDFLAGS, as INTERFACE targets
  MonoConfigureChecks.cmake    the AC_CHECK_* probes
  MonoConfigureFixed.cmake     the config.h entries that are options, not probes
  MonoLLVM.cmake               llvm-config -> the mono::llvm interface target
  MonoBtls.cmake               the BoringSSL sub-build
  MonoHelpers.cmake            small shared helpers and install paths
  MonoMiniTests.cmake          the mini regression / tiered suites
  MonoGenVersion.cmake         build-time version.h
  MonoGenBuildVer.cmake        build-time buildver-<gc>.h
  MonoBuildTreeConfig.cmake    build-time etc/mono/config
  MonoCheckEglibRemap.cmake    the "no bare g_* exports" check
  config.h.in                  cmakedefine form of the autoheader template
  bdwgc/                       the Boehm collector's build and its config.h
  mono-wrapper.in              the uninstalled-runtime wrapper
  mcs-config.make.in           what mcs/build/config.make gets told about us
```

## Configuring

```bash
cmake -S . -B build -G Ninja \
      -DMONO_LLVM_PREFIX=/usr/lib/llvm-18 \
      -DCMAKE_INSTALL_PREFIX="$PWD/tmp"
cmake --build build
```

`MONO_LLVM_PREFIX` is the CMake spelling of `--with-llvm=`; leaving it empty
builds without the tier-1 backend, the same way omitting `--with-llvm` did.  The
rest of the switches are in `MonoOptions.cmake` and follow the `MONO_ENABLE_*` /
`MONO_WITH_*` naming.

## How the runtime is assembled

automake's `noinst_LTLIBRARIES` were *convenience* libraries: every object in
them landed in the final shared library whether or not anything referenced it.
libmono needs that -- most of its exported surface is reached only through
icalls and P/Invoke, so a static archive would drop it at link time.  CMake
OBJECT libraries give the same thing without `--whole-archive`, so each piece of
the runtime (`eglib`, `monoutils`, `monoruntime_sgen`, `mini`, ...) is an OBJECT
library and `libmonosgen-2.0`, `mono-sgen` and friends list the full set.

The one exception is `monosgen-static`, an archive form of the same objects used
by the unit tests, which need archive semantics: `test-path.c` includes
`mono/utils/mono-path.c` to reach its statics, so force-linking every object
would define those symbols twice.

`mono/metadata` is compiled twice, once per collector, because a handful of its
translation units inline the collector's allocation and write-barrier paths.

## Verifying against the autotools build

`config.h` was the risky part of the port, so it was checked rather than
asserted: the generated one was diffed against a `config.h` produced by
`./autogen.sh && ./configure` with the equivalent flags, and all 568 macros
matched.  The autotools files were deleted once that held; to repeat the
comparison, check them out from before their removal.

Two probes are deliberately faithful to a bug rather than to intent, because the
runtime has only ever been built and tested with the buggy answer:

* `HOST_LINUX` is never defined.  `configure.ac` guarded it with
  `if test echo x$target_os | grep -q linux`, which is `test` with three
  operands rather than a pipeline, so it was always false.  The automake
  conditional of the same name *was* true; `MONO_HOST_LINUX` carries that.
* `HAVE_RT_MSGHDR` only asks whether `<net/route.h>` exists -- the original
  probe body was a forward declaration, which always compiles.

Two rewrites in the build-tree `etc/mono/config` go the other way and were made
to fire: the automake rules for `MonoPosixHelper` and `libmono-btls-shared`
matched on a `$mono_libdir/` prefix that `data/config.in` no longer carries for
those two entries, so they silently did nothing and both libraries resolved
through `ld.so` -- which finds a system-wide mono's copies in preference to the
ones just built.

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
| `slow` | minutes-long single tests | no |
| `stress` | long-running stress tests | no |
| `fixture` | builds the managed inputs | pulled in on demand |

The suites that drive `test-runner.exe` already use every core, so they carry a
`PROCESSORS` property and CTest runs them one at a time while packing the
one-off tests around them.

The managed corpora are built through CTest fixtures rather than as part of
`all`, so a plain `cmake --build` does not wait on ~900 csc invocations and a
bare `ctest` still does the right thing. Those fixtures deliberately do **not**
depend on the `mcs` target: it wraps a foreign make system and is therefore
always out of date, so depending on it made every `ctest` invocation re-scan the
whole class library. The class libraries are in `all`, and the `check` targets
carry the dependency for the from-scratch case.

`mono/tests` keeps its corpus in `tests.cmake` (the lists), `special-tests.cmake`
(the ~60 tests automake gave a recipe of their own) and `runtime-suites.cmake`
(the CTest wiring). The lists are the amd64/Linux/JIT set; tests that only
applied to another architecture or to an AOT profile are absent, as everywhere
else in this port.

Disabled tests are not built, which is what automake did too -- it filtered them
out of the lists before those lists became build targets, and a few of them no
longer compile.

## What is not here

* The AOT modes. `mono/tests/fullaot-mixed` and `llvmonly-mixed`, the
  `TESTSAOT_*` lists and the `%.exe.so` rules that shadowed every test are out
  of this port's JIT-only scope.
* `msvc/`, which drives a Visual Studio build of the runtime.  Out of scope for
  a Linux/amd64 build; the `.vcxproj` files are untouched and still work under
  MSVC, they simply have no CMake entry point.
* `sdks/`, which carries its own make-based build for the mobile and wasm
  cross-targets.  Those are out of this port's scope and their makefiles still
  expect the deleted `configure`, so they no longer work.
* `scripts/ci/`, Mono's Jenkins entry points.  They shell out to `./autogen.sh`
  and read the version out of `configure.ac`, so they are dead; Unity's CI goes
  through `external/buildscripts` instead.

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

Two of them differ from what automake did, on purpose.  `samples` was never
compiled at all -- the old `Makefile.am` only listed the files for `make dist`
-- and it is built here because these are the only in-tree users of the
embedding API and the profiler module ABI.  `mono/benchmark` gains CTest
entries; the perl `test-driver` it used to run under is not ported, because
none of the benchmarks has an expected-output file, so a run only ever checked
the exit status, which CTest does natively.

The gettext catalogues under `po/` are compiled and installed as before, but be
aware that nothing reads them: the compiler has no gettext binding, so the four
translations have no effect at runtime.

Everything else that the autotools build produced for this configuration --
the runtime, both collectors, the LLVM tier, the interpreter, the debugger
agent, the profiler modules, `libmono-native`, `libMonoPosixHelper`,
`libMonoSupportW`, `libikvm-native`, BTLS, `monodis`, `pedump`,
`sgen-grep-binprot`, `mono-hang-watchdog`, the `.pc` files, the dllmap
configuration, the wrapper scripts, the man pages and the class libraries -- is
built and installed by CMake.
