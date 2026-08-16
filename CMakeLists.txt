cmake_minimum_required(VERSION 3.28)

project(mono
        VERSION 6.13.0
        DESCRIPTION "Mono runtime (Unity fork, LLVM 18 tiered JIT)"
        HOMEPAGE_URL "https://github.com/mono/mono"
        LANGUAGES C CXX ASM)

#
# This is the version of the corlib-runtime interface.  Change it whenever a
# change on one side requires the other to change too: icall signatures or
# semantics, the layout of a class the runtime knows about, and so on.  There
# is no ordering -- corlib and runtime must match exactly.
#
# Generate a new one with uuidgen.
#
set(MONO_CORLIB_VERSION_GUID "1A5E0066-58DC-428A-B21C-0AD6CDAE2789")

set(MONO_API_VERSION "2.0")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# CMake writes compile_commands.json itself, which is why this tree no longer
# carries the bear/citnames wrappers the automake build needed for it.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)

# What a test gets when it names no budget of its own.  CTest's own default is
# 1500s, which is loose enough that a wedged test sits there for twenty-five
# minutes before anyone hears about it.  The tests this actually reaches -- the
# mini regression corpora, the unit tests, eglib, the gtest cases under
# mono/llvm -- run in under 20s each even with the machine busy, so 300s says
# "hung" long before it could say "slow".
#
# It is a default and not a ceiling: a suite that sets TIMEOUT keeps what it
# asked for.  Clamping the suites down to this instead would fail the handful
# whose work genuinely does not fit in five minutes, and a slow test reported
# as a timeout reads as a hang -- a false regression costs more than the late
# report it was meant to prevent.  DART_TESTING_TIMEOUT is how CTest spells the
# default, and it has to be set before include(CTest) to take.
set(MONO_CTEST_TIMEOUT 300 CACHE STRING
    "Seconds a CTest test that names no budget of its own may run")
set(DART_TESTING_TIMEOUT "${MONO_CTEST_TIMEOUT}" CACHE STRING
    "Maximum time allowed before CTest will kill the test." FORCE)

include(CTest)

# googletest is a build prerequisite of the test suites, not something they
# work around.  Found here rather than per-directory so that the answer is the
# same for every suite, and so that a machine without it is told once, at
# configure time, instead of quietly producing a smaller `ctest -N`.  The way
# to build without it is -DBUILD_TESTING=OFF, which drops every test rather
# than an arbitrary subset.
if(BUILD_TESTING)
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    message(FATAL_ERROR
      "googletest not found, and the native test suites are built on it.\n"
      "  Install it (Debian/Ubuntu: apt install libgtest-dev), or point\n"
      "  GTest_DIR at an install, or configure with -DBUILD_TESTING=OFF to\n"
      "  build no tests at all.")
  endif()
  include(GoogleTest)
endif()

include(MonoOptions)
include(MonoCompilerFlags)
include(MonoConfigureChecks)
include(MonoConfigureFixed)
include(MonoLLVM)
include(MonoHelpers)
include(MonoToolsRuntime)
include(MonoManaged)
include(MonoCorpus)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/config.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/config.h")

# A .pc file describing the runtime as it sits in the build tree, for anything
# that wants to link an uninstalled mono.
set(VERSION         "${PROJECT_VERSION}")
set(mono_build_root "${CMAKE_BINARY_DIR}")
set(abs_top_srcdir  "${CMAKE_SOURCE_DIR}")
set(export_ldflags  "-Wl,--export-dynamic")
set(libmono_ldflags "")
set(libmono_cflags  "")
configure_file(mono-uninstalled.pc.in
               "${CMAKE_CURRENT_BINARY_DIR}/mono-uninstalled.pc" @ONLY)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
# zlib first: it may define the ZLIB::ZLIB everything else links against.
add_subdirectory(mono/zlib)
add_subdirectory(mono/eglib)
if(BUILD_TESTING)
  add_subdirectory(mono/eglib/test)
endif()
add_subdirectory(mono/utils)
add_subdirectory(mono/culture)
add_subdirectory(mono/metadata)
if(MONO_ENABLE_SGEN)
  add_subdirectory(mono/sgen)
endif()
if(MONO_ENABLE_BOEHM)
  # external/bdwgc is a submodule; its build lives under cmake/ so the
  # submodule itself stays untouched.
  add_subdirectory(cmake/bdwgc)
endif()
add_subdirectory(mono/mini)
if(MONO_ENABLE_INTERPRETER)
  # After mono/mini: mono/mini owns the targets this hangs off.
  add_subdirectory(mono/interp)
endif()
add_subdirectory(mono/cil)
add_subdirectory(mono/dis)
if(MONO_ENABLE_PROFILER)
  add_subdirectory(mono/profiler)
endif()
if(MONO_ENABLE_MONO_NATIVE)
  add_subdirectory(mono/native)
endif()
if(MONO_ENABLE_LLVM)
  add_subdirectory(mono/llvm)
endif()
if(MONO_ENABLE_SUPPORT)
  add_subdirectory(support)
endif()
if(MONO_ENABLE_BTLS)
  include(MonoBtls)
endif()
if(MONO_ENABLE_IKVM_NATIVE)
  add_subdirectory(ikvm-native)
endif()
add_subdirectory(tools/pedump)
add_subdirectory(tools/sgen)
add_subdirectory(tools/mono-hang-watchdog)
add_subdirectory(data)
add_subdirectory(scripts)
add_subdirectory(man)
add_subdirectory(po)
add_subdirectory(runtime)
if(MONO_ENABLE_MCS_BUILD)
  add_subdirectory(mcs)
endif()
# After runtime/: these need the mono-wrapper and the class libraries it drives.
add_subdirectory(samples)
add_subdirectory(docs)
add_subdirectory(tools/locale-builder)
if(BUILD_TESTING)
  add_subdirectory(mono/unit-tests)
  add_subdirectory(mono/tests)
  add_subdirectory(mono/benchmark)
  add_subdirectory(acceptance-tests)
endif()

# ---------------------------------------------------------------------------
# Test entry points
# ---------------------------------------------------------------------------
# `ctest` is serial unless told otherwise, and every suite here is either
# internally parallel or tiny, so a bare `ctest` is much slower than the work
# requires. These two targets run it with the machine's core count.
#
#   check        the fast set -- unit tests, eglib, mini regression, the LLVM
#                tier, and the runtime one-offs. Seconds, not minutes.
#   check-all    everything except the stress suites and the minutes-long ones.
#   check-interp the interpreter's own tests under mono/interp/unit-tests.
#
# The heavy labels (runtime, gshared, sgen, interp, slow, stress, acceptance)
# are opt-in:
#   ctest --test-dir build -L runtime -j <n>
#
# `acceptance` additionally needs -D MONO_ENABLE_ACCEPTANCE_TESTS=ON and the
# corpus submodules under acceptance-tests/external; run the `print-versions`
# target to see which of those are checked out.
include(ProcessorCount)
ProcessorCount(MONO_NPROC)
if(MONO_NPROC EQUAL 0)
  set(MONO_NPROC 1)
endif()

if(BUILD_TESTING)
  add_custom_target(check
    COMMAND "${CMAKE_CTEST_COMMAND}" -j ${MONO_NPROC} --output-on-failure
            -LE "runtime|gshared|sgen|interp|bcl|bcl-xunit|compiler|tools|slow|stress|acceptance"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    USES_TERMINAL
    VERBATIM)

  add_custom_target(check-all
    COMMAND "${CMAKE_CTEST_COMMAND}" -j ${MONO_NPROC} --output-on-failure
            -LE "slow|stress|acceptance"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    USES_TERMINAL
    VERBATIM)

  # The interpreter's own tests, under mono/interp/unit-tests. Seconds. The rest
  # of the `interp` label is thousands of managed programs run interpreted, which
  # is a different question and `ctest -L interp` is how to ask it.
  add_custom_target(check-interp
    COMMAND "${CMAKE_CTEST_COMMAND}" -j ${MONO_NPROC} --output-on-failure
            -L interp-unit
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    USES_TERMINAL
    VERBATIM)

  # The test inputs.  Every one of these is an ALL target, so an ordinary build
  # already leaves them on disk; naming them here is what makes `--target check`
  # work on its own.
  #
  # Each exists only under the option that builds it, and add_dependencies on a
  # target that was never created is a generate-time error -- so the conditions
  # the defining directories return on are restated here rather than probed with
  # if(TARGET), which stays just as quiet for a target that has been renamed as
  # for one that was legitimately not built.
  if(MONO_CORPUS_ENABLED)
    add_dependencies(check     mini-corpora)
    add_dependencies(check-all mini-corpora runtime-corpora)
    if(MONO_ENABLE_LLVM AND MONO_ENABLE_SGEN AND MONO_ENABLE_LIBRARIES)
      add_dependencies(check     test-llvm-data)
      add_dependencies(check-all test-llvm-data)
    endif()
    if(MONO_ENABLE_INTERPRETER AND MONO_ENABLE_SGEN AND MONO_ENABLE_LIBRARIES)
      add_dependencies(check-interp test-interp-data)
    endif()
  endif()

  # check-interp names the binary as well as its inputs, which the two above
  # leave to ALL: it is the target to reach for after editing the interpreter, so
  # it has to rebuild what it is about to test.
  if(MONO_ENABLE_INTERPRETER AND MONO_ENABLE_SGEN AND MONO_ENABLE_LIBRARIES)
    add_dependencies(check-interp test-interp)
  endif()

  if(MONO_ENABLE_MCS_BUILD AND MONO_ENABLE_SGEN)
    add_dependencies(check     unit-test-assemblies)
    add_dependencies(check-all unit-test-assemblies)
  endif()

  # The CoreCLR suites and ms-test-suite, with the hours-long members
  # (coreclr-gcstress, profiler-stress, the microbenchmarks) left behind their
  # stress/slow labels.  The corpora are compiled by the regular build -- a long
  # first build once the option is on -- so this only runs the suites.
  if(MONO_ENABLE_ACCEPTANCE_TESTS)
    add_custom_target(check-acceptance
      COMMAND "${CMAKE_CTEST_COMMAND}" -j ${MONO_NPROC} --output-on-failure
              -L acceptance -LE "slow|stress"
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
      USES_TERMINAL
      VERBATIM)
    # if(TARGET) rather than a condition, because the one that matters --
    # whether the coreclr submodule is checked out -- is a variable local to
    # acceptance-tests/.
    if(TARGET coreclr-compile-tests)
      add_dependencies(check-acceptance coreclr-compile-tests)
    endif()
  endif()
endif()

# The microbenchmarks. They carry `slow`, so `check` and `check-all` skip them;
# this is the way to ask for them. Serial on purpose -- they are timing
# programs, and running sixteen at once makes the numbers meaningless.
if(MONO_ENABLE_BENCHMARKS AND BUILD_TESTING AND MONO_CORPUS_ENABLED)
  add_custom_target(check-benchmarks
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -L benchmark
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    USES_TERMINAL
    VERBATIM)
  add_dependencies(check-benchmarks benchmarks)
endif()

# ---------------------------------------------------------------------------
# Convenience targets
# ---------------------------------------------------------------------------
if(MONO_ENABLE_MCS_BUILD)
  # Refresh the bootstrap compiler used when no mono is available to build the
  # class libraries with.
  set(_monolite_name "monolite-linux-${MONO_CORLIB_VERSION_GUID}-latest")
  add_custom_target(get-monolite-latest
    COMMAND "${CMAKE_COMMAND}"
            -D "MONO_MONOLITE_URL=https://download.mono-project.com/monolite/${_monolite_name}.tar.gz"
            -D "MONO_MONOLITE_DIR=${CMAKE_BINARY_DIR}/mcs/class/lib"
            -D "MONO_MONOLITE_NAME=${_monolite_name}"
            -P "${CMAKE_SOURCE_DIR}/cmake/MonoGetMonolite.cmake"
    USES_TERMINAL
    VERBATIM)
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "  mono ${PROJECT_VERSION}")
message(STATUS "    host/target:   ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "    GC:            ${DEFAULT_GC_NAME}")
message(STATUS "    suspend:       ${MONO_THREAD_SUSPEND}")
message(STATUS "    TLS:           ${MONO_WITH_TLS}")
message(STATUS "    LLVM back end: ${MONO_ENABLE_LLVM} ${MONO_LLVM_PREFIX}")
message(STATUS "    interpreter:   ${MONO_ENABLE_INTERPRETER}")
message(STATUS "    Mono.Native:   ${MONO_ENABLE_MONO_NATIVE}")
message(STATUS "    BTLS:          ${MONO_ENABLE_BTLS}")
message(STATUS "    class libs:    ${MONO_ENABLE_MCS_BUILD}")
message(STATUS "    C# compiles on: ${MONO_TOOLS_RUNTIME} (${MONO_TOOLS_RUNTIME_VERSION})")
message(STATUS "    samples:       ${MONO_ENABLE_SAMPLES}")
message(STATUS "    benchmarks:    ${MONO_ENABLE_BENCHMARKS}")
message(STATUS "    catalogues:    ${MONO_ENABLE_NLS}")
message(STATUS "    locale-builder: ${MONO_ENABLE_LOCALE_BUILDER}")
message(STATUS "    docs:          ${MONO_ENABLE_DOCS}")
message(STATUS "    prefix:        ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
