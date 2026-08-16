# Build options.  These are the CMake spelling of the ./configure switches the
# tree actually uses; everything the old configure.ac supported for platforms
# outside this port's scope (Windows, Darwin, mobile, wasm, cross builds, the
# netcore profile, AOT-only runtimes) is gone rather than carried along as dead
# conditionals.

include(CMakeDependentOption)

# --- garbage collectors -----------------------------------------------------
option(MONO_ENABLE_SGEN   "Build the SGen collector and mono-sgen"   ON)
option(MONO_ENABLE_BOEHM  "Build the Boehm collector and mono-boehm" ON)

# --- the LLVM tier ----------------------------------------------------------
# The autotools spelling was --with-llvm=<prefix>; --enable-llvm on its own was
# an error.  Same rule here: point MONO_LLVM_PREFIX at an LLVM install (or leave
# it empty to build without the tier).
set(MONO_LLVM_PREFIX "" CACHE PATH
    "Prefix of the LLVM install to build the tier-1 backend against (e.g. /usr/lib/llvm-18). Empty disables LLVM.")

# --- runtime pieces ---------------------------------------------------------
option(MONO_ENABLE_INTERPRETER    "Build the IL interpreter"                ON)
option(MONO_ENABLE_DEBUGGER_AGENT "Build the soft debugger agent"           ON)
option(MONO_ENABLE_PROFILER       "Build the profiler modules"              ON)
option(MONO_ENABLE_ILGEN          "Build runtime IL generation into libmono" ON)
option(MONO_ENABLE_ICALL_TABLES   "Build the icall tables into libmono"     ON)
option(MONO_ENABLE_JIT            "Build the JIT (as opposed to interp-only)" ON)
option(MONO_ENABLE_EXECUTABLES    "Build the mono/monodis/pedump binaries"  ON)
option(MONO_ENABLE_LIBRARIES      "Build the shared runtime libraries"      ON)
option(MONO_STATIC_MONO           "Link libmini into the mono binaries statically" ON)
option(MONO_ENABLE_CRASH_REPORTING "Build the structured crash reporter"    ON)
option(MONO_CRASH_PRIVACY         "Scrub private data from crash dumps"     ON)

# --- instrumentation --------------------------------------------------------
option(MONO_ENABLE_COVERAGE "Instrument the whole native build for llvm-cov" OFF)

# --- helper libraries -------------------------------------------------------
option(MONO_ENABLE_MONO_NATIVE  "Build libmono-native (System.Native)"      ON)
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
# almost nothing to build and catch something when they break; off when they
# need a tool or a download that a plain checkout does not have.
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
# Which mono hosts csc and ilasm while building; see MonoToolsRuntime.cmake for
# what moves, what does not, and why this is off by default.
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
option(MONO_ENABLE_JIT_DUMP     "Emit perf jitdump files"               ON)
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

# mono-sgen is the preferred binary when both collectors are built; the `mono`
# symlink and the un-suffixed libmono*.so aliases point at whichever wins.
if(MONO_ENABLE_SGEN)
  set(MONO_DEFAULT_GC_SUFFIX "sgen")
else()
  set(MONO_DEFAULT_GC_SUFFIX "boehm")
endif()
