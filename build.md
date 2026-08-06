# Building Unity Mono locally on Linux / WSL2

These are reproducible, from-scratch instructions for building this repository
(Unity's fork of Mono, branch `unity-main`) on a Linux or WSL2 machine using the
**system toolchain**.

The build is CMake. The autotools files it replaced (`configure.ac`, `autogen.sh`,
the `Makefile.am`s, `m4/`) have been removed from the tree. `cmake/README.md`
describes the layout, how it maps onto the old one, and what it does not cover.

It produces a fully working `mono` runtime (`mono-sgen` / `mono-boehm`) plus the
C# class-library profiles (`build`, `net_4_x`, `unityjit`, `xbuild_12`,
`xbuild_14` and `binary_reference_assemblies`).

> **Why not `external/buildscripts/build_runtime_linux.pl` (the official CI path)?**
> That script first runs `external/buildscripts/bee`, which downloads a pinned
> CentOS clang toolchain and glibc-2.17 sysroot from Unity's **internal** Stevedore
> mirror (`artifactory-slo.bf.unity3d.com`). That host is only reachable from inside
> Unity's network, so the official path can't complete off-network. The instructions
> below avoid it entirely and build with your distro's gcc/clang instead. See
> [§ Official Unity build path](#official-unity-build-path) for how to use it when
> you *are* on-network.

Verified on: **Ubuntu 24.04.4 LTS (WSL2), x86_64**, gcc 13.3, CMake 3.28, Ninja
1.11, LLVM 22.1.8. Full build time was roughly 10 minutes on 16 cores.

---

## 1. Install dependencies

The build needs a C/C++ toolchain, CMake 3.28 or newer, a generator (Ninja or
make), `python3`, googletest, and an LLVM 14+ development install for the tier-1
backend.

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake ninja-build \
    python3 \
    git \
    zlib1g-dev \
    libgtest-dev
```

Notes:
- CMake 3.28 is the floor; earlier versions are not supported.
- googletest is required, not optional: the native test suites are written
  against it, so configure fails when it is missing rather than silently
  building fewer tests. `-DBUILD_TESTING=OFF` is the way to build without it,
  and it drops every test rather than an arbitrary subset. Ubuntu 24.04's
  `libgtest-dev` (1.14) ships the CMake config package, so nothing else is
  needed; on distros that ship only sources, build and install googletest and
  point `GTest_DIR` at it.
- The LLVM back end builds against LLVM 22 built from source at
  `~/projects/llvm-project` (installed to `~/projects/llvm-project/install`,
  RelWithDebInfo with assertions on); no distro package ships 22 yet. Configure
  without `MONO_LLVM_PREFIX` to build with the classic JIT only.
- No system Mono is needed: the class libraries are compiled by the Roslyn
  binaries under `external/roslyn-binaries`, running on the runtime this build
  produces.

## 2. Get the source (with submodules)

If you have not already cloned recursively, initialize the submodules — the build
pulls in Roslyn binaries, BoringSSL, bdwgc (Boehm), cecil, corefx, etc.:

```bash
cd /home/swlynch/projects/mono          # this repo
git submodule update --init --recursive
```

## 3. Configure

```bash
cd /home/swlynch/projects/mono

cmake -S . -B build -G Ninja \
  -DMONO_LLVM_PREFIX="$HOME/projects/llvm-project/install" \
  -DCMAKE_INSTALL_PREFIX="$PWD/tmp"
```

The defaults already match Unity's desktop Linux configuration — embedded eglib,
both collectors, the Unity class-library profiles, `UNITY` defined. What you may
want to change:

- `-DMONO_LLVM_PREFIX=<prefix>` — build the tier-1 LLVM backend against that LLVM
  install. Empty (the default) means no LLVM tier. This is the CMake spelling of
  the old `--with-llvm=`.
- `-DCMAKE_INSTALL_PREFIX="$PWD/tmp"` — install into an **in-repo** `tmp/` dir,
  never system-wide.
- `-DMONO_ENABLE_MCS_BUILD=OFF` — native runtime only, no class libraries. To
  build one profile rather than all of them, name its target instead:
  `cmake --build build --target mcs-net_4_x`.
- `-DMONO_USE_SYSTEM_RUNTIME_FOR_TOOLS=ON` — compile C# on a mono already
  installed on the machine instead of the one being built. See
  [Compiling C# on the system mono](#compiling-c-on-the-system-mono).

`cmake -LH build` lists the rest; they all start with `MONO_`.

Configure ends with a summary reporting the collector, the suspend policy, the
LLVM prefix, and which class-library profiles will be built.

## 4. Build

```bash
cmake --build build -j"$(nproc)"
```

This builds, in order: eglib, the bundled Boehm GC, BoringSSL (`btls`), the native
runtime (`libmonoruntime` / `libmonoruntimesgen`), the `mono-sgen` and `mono-boehm`
executables and tools, and then the C# class libraries.

Artifacts land at:
- `build/mono/mini/mono-sgen`, `build/mono/mini/mono-boehm` — the runtime executables
- `build/mcs/class/lib/net_4_x-linux/` — the runnable class-library profile
  (mscorlib, System.*, …)
- `build/mcs/class/lib/{build,net_4_x,unityjit}-linux/`, plus
  `build/mcs/class/lib/{xbuild_12,xbuild_14}/`. The unsuffixed `net_4_x`,
  `build` and `unityjit` names beside them are symlinks to the `-linux` ones.

Nothing is written under `mcs/` in the source tree, so two build directories
over one checkout do not interfere.

## 5. Verify it works

Compile a small program with your bootstrap `mcs` and run it on the **freshly
built** runtime, pointing `MONO_PATH` at a built profile:

```bash
cat > /tmp/Hello.cs <<'EOF'
using System;
using System.Linq;
class Hello {
    static void Main() {
        var sum = Enumerable.Range(1, 5).Select(x => x * x).Sum();
        Console.WriteLine($"Hello from Unity Mono! mscorlib={typeof(int).Assembly.GetName().Version}, sum={sum}");
    }
}
EOF

mcs -out:/tmp/Hello.exe /tmp/Hello.cs        # bootstrap compiler

MONO_PATH="$PWD/build/mcs/class/lib/net_4_x-linux" \
    ./build/mono/mini/mono-sgen /tmp/Hello.exe
```

Expected output:

```
Hello from Unity Mono! mscorlib=4.0.0.0, sum=55
```

And the runtime identifies itself as the build from this tree:

```bash
./build/mono/mini/mono-sgen --version
# Mono JIT compiler version 6.13.0 (<branch>/<git-hash> ...)
```

That is your locally built runtime executing managed code against the class
libraries you just compiled — the build is working end to end.

---

## Optional: install into the in-repo prefix

`cmake --install` copies everything into `./tmp` (the `CMAKE_INSTALL_PREFIX` you
set — **not** a system location):

```bash
cmake --install build
ls tmp/bin        # mono, mcs, csc, xbuild, monodis, gacutil, ...
```

> **Known wrinkle:** `tmp/bin/mono <app>.exe` fails with *"mscorlib.dll ... could
> not be loaded"*, naming `tmp/lib/mono/4.5/mscorlib.dll`. The message is
> misleading: on this fork the runtime rewrites framework version `4.5` to the
> platform directory `net_4_x-linux` before probing
> (`mono/metadata/assembly.c`), and nothing installs a corlib there. The install
> rules reproduce the layout the old make build produced, which never matched
> that probe either. Use the verified approach from step 5 (the in-tree
> `mono-sgen` + `MONO_PATH` pointing at a real profile such as
> `build/mcs/class/lib/net_4_x-linux`), or put a corlib where the runtime looks:
> ```bash
> mkdir -p tmp/lib/mono/net_4_x-linux
> cp build/mcs/class/lib/net_4_x-linux/mscorlib.dll tmp/lib/mono/net_4_x-linux/
> ```
> Unity itself does not consume an install prefix — it packages the profile
> directories and `builds/` artifacts directly.

> **A second wart, preserved from the make build:** `unityjit` declares
> framework version 4.5, the same as `net_4_x`, and installs after it. So
> `lib/mono/4.5` and the GAC end up holding `unityjit`'s assemblies, not
> `net_4_x`'s. This is what the old `install-profiles` did; the conversion keeps
> it rather than silently changing what gets installed.

## Optional: build with the LLVM backend

Mono can use LLVM as its code-generation engine instead of the built-in JIT
backend. Enabling it adds one cache variable.

Mono no longer vendors an LLVM source tree — the `external/llvm-project`
submodule (a Mono-patched LLVM 6.0.1 fork) has been removed. The LLVM backend
now builds against an **externally supplied, unmodified upstream LLVM (14 or
newer)**, so you must pass `--with-llvm=<prefix>`; `--enable-llvm` on its own is
a configure error.

### Extra prerequisites

Build an upstream LLVM 22 from source (the backend needs 22's ORC
redirection stack, which no distro package ships yet):

```bash
cd ~/projects/llvm-project/build
cmake -B . -S ../llvm -DCMAKE_BUILD_TYPE=RelWithDebInfo -DLLVM_ENABLE_ASSERTIONS=TRUE \
  -DCMAKE_INSTALL_PREFIX="$HOME/projects/llvm-project/install" -DLLVM_BUILD_LLVM_DYLIB=TRUE
cmake --build . && cmake --install .
# provides ~/projects/llvm-project/install/bin/llvm-config (must say 22.x)
```

### Configure + build

```bash
cd /home/swlynch/projects/mono
cmake -S . -B build -DMONO_LLVM_PREFIX="$HOME/projects/llvm-project/install"
cmake --build build -j"$(nproc)"
```

The configure summary should now report:

```
LLVM back end: ON /home/swlynch/projects/llvm-project/install
```

CMake queries `<prefix>/bin/llvm-config` for the include path, the libraries and
the API version, and hands them to `mono/mini` through the `mono::llvm` target.
There is no LLVM compile step — LLVM itself is already built.

### Verify the backend is present and engages

```bash
# 1. The runtime advertises LLVM (the number is the LLVM API version):
./build/mono/mini/mono-sgen --version | grep LLVM
#   LLVM:          yes(1800)

# 2. And it actually generates code via LLVM. Compile a tiny program, then dump
#    the IR the backend emits for it:
cat > /tmp/Hv.cs <<'EOF'
using System; using System.Linq;
class Hello { static void Main(){ Console.WriteLine(Enumerable.Range(1,5).Select(x=>x*x).Sum()); } }
EOF
mcs -out:/tmp/Hv.exe /tmp/Hv.cs

export MONO_PATH="$PWD/build/mcs/class/lib/net_4_x-linux"
./build/mono/mini/mono-sgen /tmp/Hv.exe                  # runs -> 55
MONO_VERBOSE_METHOD=Main ./build/mono/mini/mono-sgen /tmp/Hv.exe 2>&1 | grep -i llvm
#   converting llvm method void Hello:Main ()
#   *** Unoptimized LLVM IR for Hello:Main () ***   ...
```

Seeing the emitted *"LLVM IR for Hello:Main"* confirms the LLVM backend compiled
the method. (`<prefix>/bin/llvm-config --version` prints the upstream LLVM
version that was linked in.)

## Testing

The suites are registered with CTest:

```bash
cmake --build build --target check       # the inner loop: unit tests, eglib,
                                         # mini regression, the LLVM tier (~20s)
cmake --build build --target check-all   # everything but the slow/stress sets
```

Both run CTest with `-j $(nproc)`. Prefer them to a bare `ctest`, which is
serial and so takes minutes to do seconds of work.

Individual groups, if you want one:

```bash
ctest --test-dir build -j16 -L runtime   # the mono/tests corpus, ~700 programs
ctest --test-dir build -j16 -L gshared   # generic sharing
ctest --test-dir build -j16 -L sgen      # the SGen collector matrix
ctest --test-dir build -j16 -L interp    # the corpus under the interpreter
ctest --test-dir build -j16 -L stress    # long-running
```

The managed test assemblies are built by the regular build (`cmake --build
build`), so run that first; `ctest` itself never builds anything.

### Per-test timeouts

A test that names no budget of its own gets `MONO_CTEST_TIMEOUT` seconds, 300 by
default, and is killed and failed if it runs past that:

```bash
cmake -S . -B build -D MONO_CTEST_TIMEOUT=600
```

It is a default, not a cap. The suites that set their own budget keep it: the
`mono/tests` corpus gives each program 300s, the stress and SGen suites 900s,
and a class-library suite 1800s. Raising `MONO_CTEST_TIMEOUT` does not lengthen
any of those, and lowering it does not shorten them.

A handful of individual tests are slow enough that the suite budget is too tight
for them, and those are named where the suite is declared rather than being left
to fail. The runtime corpus takes `LONG <program>.exe`
(`mono/tests/runtime-suites.cmake`) and the class-library side has
`MONO_BCL_TESTS_LONG` (`cmake/MonoManagedTests.cmake`); each entry carries a note
saying what makes that test slow. Add to those lists rather than raising the
budget for a whole suite -- a suite-wide raise buys the one slow test its time by
giving every other test in the suite permission to hang for as long.

### Class-library suites that depend on the machine

`ctest -L bcl` will not come out clean on a bare machine, and the reasons are all
outside this build:

| suite | needs |
| ----- | ----- |
| `bcl-corlib` | the ICU and tzdata versions the expectations were written against; ~11 culture, file-time-epoch and DST cases differ otherwise |
| `bcl-System` | a working internet connection; one case is `[Category("InetAccess")]` and asserts on a live response's headers |
| `bcl-System.Messaging` | a RabbitMQ broker on localhost, for 58 of its 87 cases |
| `bcl-System.Windows.Forms` | an X display |
| `bcl-System.Data.Linq` | a build directory whose path is short enough that the test assembly's own path fits a 128-character connection-string field |

To tell one of these from a real code regression, re-run the single suite against
the machine's installed mono (`MONO_EXECUTABLE`): the machine-dependent failures
reproduce there too, a codegen bug in this tree does not.

### Acceptance tests

`acceptance-tests/` is a separate, much heavier set: the CoreCLR corpus (~4,700
assemblies), the DebianShootoutMono microbenchmarks, and the profiler stress
test. It is off by default and its tests carry the `acceptance` label, so they
never join `check` or `check-all`.

The corpora are git submodules under `acceptance-tests/external`, and this build
never fetches them. All of them are marked `update = none` in `.gitmodules`, so
the `git submodule update --init --recursive` in step 2 skips them — together
they are well over a gigabyte. Ask for one by name:

```bash
cmake --build build --target print-versions   # which are checked out, and the
                                              # exact git command for each
git submodule update --init --checkout acceptance-tests/external/coreclr
```

`ms-test-suite` is skipped for a second reason: it is Xamarin-internal and not
readable from outside, so `update = none` is what keeps a bare `--init` from
failing on it rather than merely saving a download.

With the checkouts in place:

```bash
cmake -B build -D MONO_ENABLE_ACCEPTANCE_TESTS=ON
ctest --test-dir build -j16 -L acceptance
```

Compiling the CoreCLR corpus happens in the regular build once the option is on,
and takes on the order of half an hour the first time; `coreclr-gcstress`,
`profiler-stress` and the microbenchmarks are additionally labelled
`stress`/`slow` because they run for hours by design.

### Microbenchmarks

`mono/benchmark` builds with `all` and registers 38 CTest entries labelled
`benchmark;slow`, so `check` and `check-all` skip them. Run them with:

```bash
cmake --build build --target check-benchmarks
```

They are timing programs with no expected output, so a pass only means the
program finished — the value is in the shapes they stress (inlining, constant
folding, register allocation, cmov selection) when you are changing codegen.

## The peripheral directories

Five directories are not part of the runtime and have their own switches:

| directory              | option                       | default |
| ---------------------- | ---------------------------- | ------- |
| `samples`              | `MONO_ENABLE_SAMPLES`        | ON      |
| `mono/benchmark`       | `MONO_ENABLE_BENCHMARKS`     | ON      |
| `po`                   | `MONO_ENABLE_NLS`            | ON      |
| `tools/locale-builder` | `MONO_ENABLE_LOCALE_BUILDER` | OFF     |
| `docs`                 | `MONO_ENABLE_DOCS`           | OFF     |

The samples are `EXCLUDE_FROM_ALL`, so ask for them by name:

```bash
cmake --build build --target samples
```

They are the only in-tree users of the embedding API and the profiler module
ABI, which is why they are compiled at all — nothing runs or installs them.

`po` needs `msgfmt`; without it the catalogues are skipped with a message at
configure time. Note that nothing actually reads them (the compiler has no
gettext binding), so they are installed for completeness only.

`docs` needs perl plus `mdoc.exe` out of the class libraries, and `culture-table`
under `tools/locale-builder` downloads a CLDR release and rewrites a checked-in
header — hence both being off by default.

## Rebuilding after changes

- **Runtime (C/C++) change:** `cmake --build build -j"$(nproc)"` (incremental).
- **Class-library (C#) change:** `cmake --build build -j"$(nproc)"` rebuilds
  exactly the assemblies containing the file. `--target mcs` stops at the class
  libraries; `--target mcs-net_4_x` stops at one profile.
- **Clean rebuild:** `rm -rf build` and repeat from step 3. There is no stale
  `config.status` to worry about — changing a cache variable and re-running
  `cmake -S . -B build` is enough.

## Compiling C# on the system mono

Every managed thing this build produces — the test corpora, the CoreCLR suites,
the class libraries — is compiled by Roslyn, which is itself a managed program
and so needs a mono to run on. By default that is the mono this build produces.
It is also the slow choice: that runtime is unoptimised with the LLVM tier on,
and every C# compile ends up downstream of the native link, so a one-line change
under `mono/mini` relinks the runtime before any C# can be compiled at all.

```bash
cmake -S . -B build -DMONO_USE_SYSTEM_RUNTIME_FOR_TOOLS=ON
# or name one explicitly:
cmake -S . -B build -DMONO_USE_SYSTEM_RUNTIME_FOR_TOOLS=ON \
                    -DMONO_SYSTEM_RUNTIME=/opt/mono/bin/mono-sgen
```

Empty picks `mono-sgen`, then `mono`, off `PATH`. Configure prints which one it
settled on:

```
--     C# compiles on: /usr/bin/mono-sgen (version 6.8.0.105)
```

Measured over twelve CoreMangLib tests: **54.5s in-tree against 12.5s** on the
system mono 6.8. The bigger win is the dependency edge that disappears — the
corpora no longer wait on the native build, so they can be compiled in a tree
where the runtime has never been linked.

**Tests still run on the runtime being built.** That is the thing under test and
it never moves; only the compiler's host changes. Likewise the tools that
surround the compiler in the class-library build — `resgen`, `ilasm`,
`cil-stringreplacer`, `gensources` and `gacutil` — stay on the in-tree runtime. They load assemblies out of `build/mcs/class/lib/build`, whose
`mscorlib.dll` belongs to this tree, and a foreign runtime rejects that pairing
outright (`Corlib not in sync with this runtime`).

### When not to use it

The class libraries and the mini corpora compile `-nostdlib` against an explicit
`-r:.../mscorlib.dll`, so for those the host runtime cannot affect the output —
verified by disassembling both and diffing: identical IL, differing only in the
MVID that a non-deterministic `csc` stamps into every build.

`mono/tests` and `acceptance-tests` do **not** pass `-nostdlib`. Those compiles
pick up whichever `mscorlib` the hosting runtime hands Roslyn, so with this on
they see the system mono's BCL surface rather than this tree's. In practice both
are 4.x Mono BCLs and the resulting assemblies run unchanged, but a test that
depends on an API present in only one of them will compile differently.

Treat the option as an iteration aid: fine while you are working on test wiring
or the corpora, but validate on a default configuration before believing a
result.

## Troubleshooting

- **`The assembly mscorlib.dll was not found`** when running a program → you didn't
  set `MONO_PATH` to a real profile dir, or you're using the installed `tmp/bin/mono`
  (see the install wrinkle above). Point `MONO_PATH` at `build/mcs/class/lib/net_4_x-linux`.
- **`external/bdwgc is empty`** or **`external/corefx is empty`** → rerun
  `git submodule update --init --recursive`, or turn the corresponding option off
  (`-DMONO_ENABLE_BOEHM=OFF`, `-DMONO_ENABLE_MONO_NATIVE=OFF`).
- **A submodule is empty / a file under `external/` is missing** → rerun
  `git submodule update --init --recursive`.
- **`No usable version of libssl was found`** — this only affects the older
  `build_classlibs_wsl.pl` path (see `Unity-build.md`), not these instructions.
- **`No llvm-config under <prefix>/bin`** → there is no in-tree LLVM any more;
  build an upstream LLVM 22 and point `MONO_LLVM_PREFIX` at its install, e.g.
  `-DMONO_LLVM_PREFIX=$HOME/projects/llvm-project/install`. See
  [§ build with the LLVM backend](#optional-build-with-the-llvm-backend).
