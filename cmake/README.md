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

`config.h` was the risky part of the port, so it is checked rather than
asserted: generate it and diff against a `config.h` produced by `./autogen.sh &&
./configure` with the equivalent flags.  All 568 macros match.

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

## What is not here

These directories still have only their `Makefile.am` and are not built by
CMake:

* `mono/tests` -- the large managed regression corpus (3.3k lines of
  `Makefile.am`).  The suites this port actually runs, `mono/mini`'s regression
  and tiered checks and `mono/unit-tests`, are wired into CTest.
* `mono/benchmark`, `acceptance-tests`, `samples`, `docs`, `po`, `msvc`,
  `tools/locale-builder`, `sdks` -- out of scope for a Linux/amd64 JIT build, or
  (in the case of `po` and `docs`) disabled by the configuration this tree uses
  anyway.

Everything else that the autotools build produced for this configuration --
the runtime, both collectors, the LLVM tier, the interpreter, the debugger
agent, the profiler modules, `libmono-native`, `libMonoPosixHelper`,
`libMonoSupportW`, `libikvm-native`, BTLS, `monodis`, `pedump`,
`sgen-grep-binprot`, `mono-hang-watchdog`, the `.pc` files, the dllmap
configuration, the wrapper scripts, the man pages and the class libraries -- is
built and installed by CMake.
