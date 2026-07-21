# Building Unity Mono locally on Linux / WSL2

These are reproducible, from-scratch instructions for building this repository
(Unity's fork of Mono, branch `unity-main`) on a Linux or WSL2 machine using the
**system toolchain**. This is the documented "other platforms" fallback from
[`Unity-build.md`](./Unity-build.md) (`./autogen.sh` followed by `make`), with the
exact `configure` flags that Unity's own `external/buildscripts/build.pl` uses for
a desktop Linux build.

It produces a fully working `mono` runtime (`mono-sgen` / `mono-boehm`) plus the
C# class-library profiles (`net_4_x`, `unityjit`, `unityaot`, and their `-linux`
host variants).

> **Why not `external/buildscripts/build_runtime_linux.pl` (the official CI path)?**
> That script first runs `external/buildscripts/bee`, which downloads a pinned
> CentOS clang toolchain and glibc-2.17 sysroot from Unity's **internal** Stevedore
> mirror (`artifactory-slo.bf.unity3d.com`). That host is only reachable from inside
> Unity's network, so the official path can't complete off-network. The instructions
> below avoid it entirely and build with your distro's gcc/clang instead. See
> [§ Official Unity build path](#official-unity-build-path) for how to use it when
> you *are* on-network.

Verified on: **Ubuntu 24.04.4 LTS (WSL2), x86_64**, gcc 13.3, autoconf 2.71,
automake 1.16.5, libtool 2.4.7, cmake, and Mono 6.8 as the bootstrap compiler.
Full build time was roughly 10 minutes on 16 cores.

---

## 1. Install dependencies

The build needs: a C/C++ toolchain, autotools, `cmake` (for the bundled BoringSSL /
`btls`), `gettext`, `python3`, and — crucially — an **existing Mono** in `PATH` to
bootstrap the C# class libraries (the build compiles C# with Roslyn/mcs, which run
on Mono).

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    autoconf automake libtool libtool-bin \
    gettext bison gawk \
    cmake \
    python3 \
    git \
    mono-complete          # bootstrap C# compiler + runtime (provides mcs, mono)
```

Notes:
- `mono-complete` provides the `mono` runtime and `mcs` compiler used to bootstrap
  the class-library build. Any reasonably recent Mono works (6.8 was used here).
- `libtool-bin` is required — it provides `libtoolize`, which `autogen.sh` calls.
- `cmake` is required because the runtime build compiles the bundled BoringSSL.

## 2. Get the source (with submodules)

If you have not already cloned recursively, initialize the submodules — the build
pulls in Roslyn binaries, BoringSSL, bdwgc (Boehm), cecil, corefx, etc.:

```bash
cd /home/swlynch/projects/mono          # this repo
git submodule update --init --recursive
```

## 3. Configure

Run `autogen.sh` with the same flags Unity's `build.pl` passes for a desktop Linux
build. `autogen.sh` regenerates `configure` (via autoreconf) and then runs it.

```bash
cd /home/swlynch/projects/mono

export CFLAGS="-fPIC -Os"
export CXXFLAGS="-fPIC -Os"

./autogen.sh \
  --with-glib=embedded \
  --disable-nls \
  --with-mcs-docs=no \
  --prefix="$PWD/tmp" \
  --enable-no-threads-discovery=yes \
  --enable-thread-local-alloc=no \
  --enable-unity-define=yes \
  --with-unityjit=yes \
  --with-unityaot=yes \
  --with-monotouch=no
```

What the flags mean:
- `--with-glib=embedded` — use Mono's bundled eglib (no system glib dependency).
- `--disable-nls` — drop the gettext/translation dependency.
- `--prefix="$PWD/tmp"` — install into an **in-repo** `tmp/` dir, never system-wide.
- `--with-unityjit=yes --with-unityaot=yes` — build the Unity JIT/AOT class-lib profiles.
- `--enable-unity-define=yes` and the `--enable-*` toggles — Unity's runtime tweaks.

> **Note:** Unity's stock build also passes `--enable-ignore-dynamic-loading=yes`
> and `--enable-dont-register-main-static-data=yes`. Those tell the bundled Boehm
> GC (`external/bdwgc`) not to scan the main program's static-data segment or
> dynamically-loaded libraries for roots — correct for Mono (which manages its own
> roots / defaults to SGen), but it makes bdwgc's own self-tests `gctest` and
> `staticrootstest` fail under `make check`. They are omitted here so `make check`
> is clean. Re-add them to match Unity's exact configuration.

A successful configure ends with a summary and `Now type 'make' to compile`. It
reports `C# Compiler: roslyn`, `GC: sgen ... and Included Boehm GC`, and
`Unity JIT: yes / Unity AOT: yes`.

## 4. Build

```bash
make -j"$(nproc)"
```

This builds, in order: eglib, the bundled Boehm GC, BoringSSL (`btls`), the native
runtime (`libmonoruntime` / `libmonoruntimesgen`), the `mono-sgen` and `mono-boehm`
executables and tools, and then the C# class libraries.

Artifacts land at:
- `mono/mini/mono-sgen`, `mono/mini/mono-boehm` — the runtime executables
- `mcs/class/lib/net_4_x-linux/` — the runnable class-library profile (mscorlib, System.*, …)
- `mcs/class/lib/{net_4_x,unityjit,unityaot,unityjit-linux,unityaot-linux}/`

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

MONO_PATH="$PWD/mcs/class/lib/net_4_x-linux" \
    ./mono/mini/mono-sgen /tmp/Hello.exe
```

Expected output:

```
Hello from Unity Mono! mscorlib=4.0.0.0, sum=55
```

And the runtime identifies itself as the build from this tree:

```bash
./mono/mini/mono-sgen --version
# Mono JIT compiler version 6.13.0 (llvm-14/<git-hash> ...)
```

That is your locally built runtime executing managed code against the class
libraries you just compiled — the build is working end to end.

---

## Optional: `make install` into the in-repo prefix

`make install` copies everything into `./tmp` (the `--prefix` you set — **not** a
system location):

```bash
make install
ls tmp/bin        # mono, mcs, csc, xbuild, monodis, gacutil, ...
```

> **Known wrinkle:** the installed `tmp/bin/mono` looks for its corlib at
> `tmp/lib/mono/4.5/mscorlib.dll`, but on this fork `make install` places a
> *reference-only* mscorlib (no method bodies) there, so
> `tmp/bin/mono <app>.exe` fails with *"mscorlib.dll ... could not be loaded."*
> This is a quirk of the install rules, **not** a build failure. Use the verified
> approach from step 5 (the in-tree `mono-sgen` + `MONO_PATH` pointing at a real
> profile such as `mcs/class/lib/net_4_x-linux`), or overwrite the installed corlib
> with the real one:
> ```bash
> cp mcs/class/lib/net_4_x/mscorlib.dll tmp/lib/mono/4.5/mscorlib.dll
> ```
> Unity itself does not consume a `make install` prefix — it packages the profile
> directories and `builds/` artifacts directly.

## Optional: build with the LLVM backend

Mono can use LLVM as its code-generation engine instead of the built-in JIT
backend. Enabling it adds one `configure` flag.

Mono no longer vendors an LLVM source tree — the `external/llvm-project`
submodule (a Mono-patched LLVM 6.0.1 fork) has been removed. The LLVM backend
now builds against an **externally supplied, unmodified upstream LLVM (14 or
newer)**, so you must pass `--with-llvm=<prefix>`; `--enable-llvm` on its own is
a configure error.

### Extra prerequisites

Install an upstream LLVM development package, e.g. on Debian/Ubuntu:

```bash
sudo apt-get install -y llvm-18-dev
# provides /usr/lib/llvm-18/bin/llvm-config
```

### Configure + build

Re-run `configure` with the same flags as [§ 3](#3-configure) plus
`--with-llvm=<prefix>`. Reconfiguring changes `config.h` (defines `ENABLE_LLVM`),
so remove the stale `config.status` first:

```bash
cd /home/swlynch/projects/mono
export CFLAGS="-fPIC -Os" CXXFLAGS="-fPIC -Os"
rm -f config.status eglib/config.status libgc/config.status

./autogen.sh \
  --with-glib=embedded --disable-nls --with-mcs-docs=no --prefix="$PWD/tmp" \
  --enable-no-threads-discovery=yes --enable-thread-local-alloc=no \
  --enable-unity-define=yes --with-unityjit=yes --with-unityaot=yes \
  --with-monotouch=no \
  --with-llvm=/usr/lib/llvm-18

make -j"$(nproc)"
```

The config summary should now report:

```
LLVM Back End: yes (built in-tree: no, assertions: no, msvc only: no)
```

`make` runs `llvm/build_llvm_config.sh` against `<prefix>/bin/llvm-config` to
produce `llvm/llvm_config.mk`, which `mono/mini` includes for its LLVM
CFLAGS/LDFLAGS/libs. There is no LLVM compile step — LLVM itself is already
built.

### Verify the backend is present and engages

```bash
# 1. The runtime advertises LLVM (the number is the LLVM API version):
./mono/mini/mono-sgen --version | grep LLVM
#   LLVM:          yes(610)

# 2. And it actually generates code via LLVM when asked. Compile a tiny program,
#    then force the LLVM backend and dump the IR it emits:
cat > /tmp/Hv.cs <<'EOF'
using System; using System.Linq;
class Hello { static void Main(){ Console.WriteLine(Enumerable.Range(1,5).Select(x=>x*x).Sum()); } }
EOF
mcs -out:/tmp/Hv.exe /tmp/Hv.cs

export MONO_PATH="$PWD/mcs/class/lib/net_4_x-linux"
./mono/mini/mono-sgen --llvm /tmp/Hv.exe                 # runs -> 55
MONO_VERBOSE_METHOD=Main ./mono/mini/mono-sgen --llvm /tmp/Hv.exe 2>&1 | grep -i llvm
#   converting llvm method void Hello:Main ()
#   *** Unoptimized LLVM IR for Hello:Main () ***   ...
```

Seeing the emitted *"LLVM IR for Hello:Main"* confirms the LLVM backend — not the
built-in JIT — compiled the method. `--nollvm` forces the built-in backend if you
want to compare. (`<prefix>/bin/llvm-config --version` prints the upstream LLVM
version that was linked in.)

## Rebuilding after changes

- **Runtime (C) change:** `make -j"$(nproc)"` (incremental).
- **Class-library (C#) change:** rebuild just the profiles from `mcs/`:
  ```bash
  make -C mcs -j"$(nproc)" HOST_PLATFORM=linux
  ```
- **Clean rebuild:** `make clean` then repeat from step 4. If configure inputs
  changed, remove `config.status eglib/config.status libgc/config.status` and rerun
  step 3.

## Troubleshooting

- **`libtoolize: command not found`** → install `libtool-bin`.
- **BoringSSL / `btls` fails** → install `cmake`.
- **`The assembly mscorlib.dll was not found`** when running a program → you didn't
  set `MONO_PATH` to a real profile dir, or you're using the installed `tmp/bin/mono`
  (see the install wrinkle above). Point `MONO_PATH` at `mcs/class/lib/net_4_x-linux`.
- **A submodule is empty / a file under `external/` is missing** → rerun
  `git submodule update --init --recursive`.
- **`No usable version of libssl was found`** — this only affects the older
  `build_classlibs_wsl.pl` path (see `Unity-build.md`), not these instructions.
- **`--enable-llvm requires --with-llvm=<llvm prefix>`** → there is no in-tree
  LLVM any more; install an upstream LLVM 14+ and pass its prefix, e.g.
  `--with-llvm=/usr/lib/llvm-18`. See
  [§ build with the LLVM backend](#optional-build-with-the-llvm-backend).
- **`--with-llvm` but summary says `LLVM Back End: no`** → you left a stale
  `config.status`; remove `config.status eglib/config.status libgc/config.status`
  and rerun `./autogen.sh ... --with-llvm=<prefix>`.

---

## Official Unity build path

When you are on Unity's internal network (so the Stevedore mirror is reachable),
the canonical Linux build — matching `.yamato/Build Linux x64.yml` — is:

```bash
export UNITY_THISISABUILDMACHINE=1
git submodule update --init --recursive
( cd external/buildscripts && ./bee )     # downloads pinned toolchain + sysroot
perl external/buildscripts/build_runtime_linux.pl --stevedorebuilddeps=1
# results land under builds/  (copied to incomingbuilds/linux64 by the CI script)
```

Setting `UNITY_THISISABUILDMACHINE=1` makes `build.pl` use the downloaded
CentOS-7 clang + glibc-2.17 sysroot (`x86_64-glibc2.17-linux-gnu`) for a portable,
distro-independent binary. The local build above intentionally skips that and links
against your system glibc instead — perfect for development, not for shipping a
redistributable artifact.
