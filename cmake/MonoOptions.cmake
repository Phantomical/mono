# Build options.  These are the CMake spelling of the ./configure switches the
# tree actually uses.  Everything the old configure.ac supported for platforms
# outside this port's scope (Darwin, mobile, wasm, cross builds, the netcore
# profile, AOT-only runtimes) is gone rather than carried along as dead
# conditionals.

include(CMakeDependentOption)

# --- the host ---------------------------------------------------------------
# Which host this is, answered once for the whole build.  Everything from the
# flag sets down to the per-directory source lists branches on these two rather
# than on CMAKE_SYSTEM_NAME, so adding a third host is adding a variable here.
#
# This is the build system's own answer.  What config.h says about the host is
# HOST_WIN32, which MonoConfigureFixed.cmake derives from these.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(MONO_HOST_LINUX ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(MONO_HOST_WINDOWS ON)
else()
  message(FATAL_ERROR
    "Unsupported host ${CMAKE_SYSTEM_NAME}; this build supports Linux and Windows")
endif()

# --- garbage collectors -----------------------------------------------------
option(MONO_ENABLE_SGEN   "Build the SGen collector and mono-sgen"   ON)
option(MONO_ENABLE_BOEHM  "Build the Boehm collector and mono-boehm" ON)

# --- the LLVM tier ----------------------------------------------------------
# The autotools spelling was --with-llvm=<prefix>; --enable-llvm on its own was
# an error.  Same rule here: point MONO_LLVM_PREFIX at an LLVM install (or leave
# it empty to build without the tier).
set(MONO_LLVM_PREFIX "" CACHE PATH
    "Prefix of the LLVM install to build the tier-1 backend against (e.g. /usr/lib/llvm-18). Empty disables LLVM.")

# How the runtime is linked when it ships inside a Unity player.  The player
# defines the process's global operator new, so an allocation that binds there
# reaches the engine's shared heap, and one lock covers it.  Compile threads
# spend 38-46% of their samples inside that heap.
#
# The option links LLVM as archives and compiles mono/mini/mono-shim.cpp, which
# defines the operator new family on g_malloc.  MonoCompilerFlags.cmake already
# passes -Bsymbolic for every non-MSVC link, which is what binds those
# definitions locally.
#
# Linux only.  A Unity player on Windows has the same problem and none of
# these flags has a link.exe spelling.
option(MONO_UNITY_BUILD "Link the runtime the way a Unity player ships it" OFF)
if(MONO_UNITY_BUILD AND NOT MONO_HOST_LINUX)
  message(FATAL_ERROR "MONO_UNITY_BUILD is Linux-only")
endif()

# --- runtime pieces ---------------------------------------------------------
option(MONO_ENABLE_INTERPRETER    "Build the IL interpreter"                ON)
option(MONO_ENABLE_DEBUGGER_AGENT "Build the soft debugger agent"           ON)
option(MONO_ENABLE_PROFILER       "Build the profiler modules"              ON)
option(MONO_ENABLE_ILGEN          "Build runtime IL generation into libmono" ON)
option(MONO_ENABLE_ICALL_TABLES   "Build the icall tables into libmono"     ON)
option(MONO_ENABLE_JIT            "Build the JIT (as opposed to interp-only)" ON)
option(MONO_ENABLE_EXECUTABLES    "Build the mono/monodis/pedump binaries"  ON)
option(MONO_ENABLE_LIBRARIES      "Build the shared runtime libraries"      ON)
# An ELF executable exports its own symbols, so a profiler module dlopen'ed
# into it resolves mono_* against the binary and one copy of the runtime is in
# the process either way.  A PE module has to name the image each import comes
# from, so the runtime has to be a DLL both the executable and the module link.
#
# A player build links LLVM into the runtime, so linking the runtime into each
# binary as well copies a gigabyte of archive into every one of them.  Off, the
# binaries link the runtime library and only that library carries LLVM.
if(MONO_HOST_WINDOWS OR MONO_UNITY_BUILD)
  set(_mono_static_mono_default OFF)
else()
  set(_mono_static_mono_default ON)
endif()
option(MONO_STATIC_MONO           "Link libmini into the mono binaries statically"
       ${_mono_static_mono_default})
# The structured crash reporter dumps a summary of every thread, and it gets
# there by signalling each one and waiting on a semaphore in the handler.  There
# is no Windows arm of that code.  winconfig.h -- what the msvc/ project files
# use for config.h -- defines DISABLE_CRASH_REPORTING for the same reason.
if(MONO_HOST_WINDOWS)
  set(_mono_crash_reporting_default OFF)
else()
  set(_mono_crash_reporting_default ON)
endif()
option(MONO_ENABLE_CRASH_REPORTING "Build the structured crash reporter"
       ${_mono_crash_reporting_default})
option(MONO_CRASH_PRIVACY         "Scrub private data from crash dumps"     ON)

# Which linker every link runs through, as -fuse-ld=<name>.  Empty leaves the
# toolchain default.  MonoCompilerFlags.cmake rejects a name the toolchain
# cannot link through, rather than leaving it to fail at the first link.
#
# lld is worth naming for a link that is large for any reason: a player build's
# is over a gigabyte of LLVM archive, which GNU ld reads into memory whole.
set(MONO_USE_LINKER "" CACHE STRING
    "Linker to link through (lld, mold, gold, bfd); empty leaves the toolchain default")
set_property(CACHE MONO_USE_LINKER PROPERTY STRINGS "" "lld" "mold" "gold" "bfd")

# Ninja runs a link per core, and a player build's links each want several GB
# because of the LLVM archive in them.  Lower this where they do not all fit.
include(ProcessorCount)
ProcessorCount(_mono_nproc)
if(_mono_nproc EQUAL 0)
  set(_mono_nproc 1)
endif()
set(MONO_MAX_CONCURRENT_LINKS "${_mono_nproc}" CACHE STRING
    "Most links to run at once")

if(CMAKE_GENERATOR MATCHES "Ninja")
  set_property(GLOBAL PROPERTY JOB_POOLS mono_link=${MONO_MAX_CONCURRENT_LINKS})
  set(CMAKE_JOB_POOL_LINK mono_link)
endif()

# --- instrumentation --------------------------------------------------------
option(MONO_ENABLE_COVERAGE "Instrument the whole native build for llvm-cov" OFF)

# --- test registration ------------------------------------------------------
# The gtest suites and the managed method suites hold many cases in one binary.
# Each lists its cases when ctest starts and registers one ctest test per case,
# so a case runs in a process of its own.  That is what makes a failure name
# the case, and it is what lets ctest run the cases in parallel.
#
# This drops the listing and registers one test per suite, which runs the whole
# suite in one process.  A failure then names the suite, and a case that takes
# the process down takes the suite's remaining cases with it.
option(MONO_MERGED_TESTS "Run each test suite as one ctest test rather than one per case" OFF)

# --- helper libraries -------------------------------------------------------
# libmono-native is corefx's System.Native, and every file it is built from
# lives under the submodule's Unix/ directory.  The class libraries P/Invoke it
# only from the sources their Unix profile compiles, so a Windows build wants
# neither half.
if(MONO_HOST_WINDOWS)
  set(_mono_native_default OFF)
else()
  set(_mono_native_default ON)
endif()
option(MONO_ENABLE_MONO_NATIVE  "Build libmono-native (System.Native)"
       ${_mono_native_default})
option(MONO_ENABLE_SUPPORT      "Build libMonoPosixHelper / libMonoSupportW" ON)
option(MONO_ENABLE_IKVM_NATIVE  "Build libikvm-native"                      ON)
option(MONO_ENABLE_BTLS         "Build the BoringSSL-based TLS provider"    ON)

# --- class libraries --------------------------------------------------------
# Building a subset is `--target mcs-<profile>`; the profiles are nodes in the
# same dependency graph as everything else, so there is nothing to configure.
option(MONO_ENABLE_MCS_BUILD "Build the mcs class libraries"            ON)
option(MONO_UNITY_DEFINE     "Define UNITY in config.h"                 ON)

# --- the peripheral directories ---------------------------------------------
# None of these are part of the runtime.  They are on by default when they cost
# almost nothing to build and catch something when they break, and off when
# they need a tool or a download that a plain checkout does not have.
option(MONO_ENABLE_SAMPLES    "Build the embedding and profiler samples"      ON)
option(MONO_ENABLE_BENCHMARKS "Build the mono/benchmark microbenchmarks"      ON)
option(MONO_ENABLE_NLS        "Compile and install the gettext catalogues"    ON)
# Regenerating the culture tables downloads a CLDR release and overwrites a
# checked-in generated header, so it is opt-in and not part of `all`.
option(MONO_ENABLE_LOCALE_BUILDER "Build tools/locale-builder"                OFF)
# The API documentation needs monodoc's mdoc/monodocer plus perl, none of which
# this build otherwise depends on.
option(MONO_ENABLE_DOCS       "Build the monodoc API documentation"           OFF)

# --- build-time managed tools -----------------------------------------------
# This option picks which mono hosts csc and ilasm while building.  See
# MonoToolsRuntime.cmake for what moves, what does not, and why this is off by
# default.
option(MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS
       "Compile C# on a mono already installed on this machine instead of the one being built" OFF)
set(MONO_SYSTEM_RUNTIME "" CACHE FILEPATH
    "Runtime to compile C# on when MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS is on; empty picks mono-sgen, then mono, off PATH")

# --- misc -------------------------------------------------------------------
option(MONO_ENABLE_COMPILE_WARNINGS "Use the maintainer warning set"    ON)
option(MONO_ENABLE_VISIBILITY_HIDDEN "Compile the runtime with -fvisibility=hidden" ON)
option(MONO_ENABLE_PARALLEL_MARK "Build Boehm with parallel marking"    ON)
option(MONO_ENABLE_DEV_RANDOM   "Use /dev/random for crypto seeding"    ON)
option(MONO_ENABLE_BIG_ARRAYS   "Allow arrays larger than Int32.MaxValue" OFF)
# perf(1) reads these, and mono/mini/mini-runtime.c writes them through
# sys/mman.h, elf.h and a clock_gettime syscall.  Windows has no perf and none
# of the three.
if(MONO_HOST_WINDOWS)
  set(_mono_jit_dump_default OFF)
else()
  set(_mono_jit_dump_default ON)
endif()
option(MONO_ENABLE_JIT_DUMP     "Emit perf jitdump files"  ${_mono_jit_dump_default})
option(MONO_ENABLE_INTERP_TRACE "Build the interpreter's execution tracer" OFF)
option(MONO_XEN_OPT             "Optimize for Xen guests"               ON)
set(MONO_WITH_TLS "__thread" CACHE STRING "Thread-local storage mechanism (__thread or pthread)")
set_property(CACHE MONO_WITH_TLS PROPERTY STRINGS "__thread" "pthread")
set(MONO_THREAD_SUSPEND "preemptive" CACHE STRING "Thread suspend policy")
set_property(CACHE MONO_THREAD_SUSPEND PROPERTY STRINGS "preemptive" "coop" "hybrid")

# --- derived ----------------------------------------------------------------
if(MONO_LLVM_PREFIX)
  set(MONO_ENABLE_LLVM ON)
else()
  set(MONO_ENABLE_LLVM OFF)
endif()

if(NOT MONO_ENABLE_SGEN AND NOT MONO_ENABLE_BOEHM)
  message(FATAL_ERROR "At least one of MONO_ENABLE_SGEN / MONO_ENABLE_BOEHM must be on")
endif()

# mono-sgen is the preferred binary when both collectors are built.  The
# `mono` symlink and the un-suffixed libmono*.so aliases point at whichever
# wins.
if(MONO_ENABLE_SGEN)
  set(MONO_DEFAULT_GC_SUFFIX "sgen")
else()
  set(MONO_DEFAULT_GC_SUFFIX "boehm")
endif()
