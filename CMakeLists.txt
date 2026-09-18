cmake_minimum_required(VERSION 3.28)

# The vcpkg toolchain reads this as project() loads it below, so it cannot move
# down with the rest of the configuration.
set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_LIST_DIR}/vcpkg-triplets")

# The ports have to use the CRT LLVM's own archives were built against.
# LLVMConfig.cmake states that by setting CMAKE_MSVC_RUNTIME_LIBRARY on whoever
# finds it, but find_package(LLVM) runs below, by which time the toolchain
# project() loads has already built every port.
set(MONO_MSVC_CRT dynamic)
if(CMAKE_HOST_WIN32 AND EXISTS "${MONO_LLVM_PREFIX}/lib/cmake/llvm/LLVMConfig.cmake")
  file(STRINGS "${MONO_LLVM_PREFIX}/lib/cmake/llvm/LLVMConfig.cmake" _llvm_crt
       REGEX "^set[(]CMAKE_MSVC_RUNTIME_LIBRARY +MultiThreaded")
  if(_llvm_crt AND NOT _llvm_crt MATCHES "DLL")
    set(MONO_MSVC_CRT static)
    set(VCPKG_TARGET_TRIPLET x64-windows-static-crt)
  endif()
endif()

project(mono
        VERSION 6.13.0
        DESCRIPTION "Mono runtime (Unity fork, LLVM 18 tiered JIT)"
        HOMEPAGE_URL "https://github.com/mono/mono"
        LANGUAGES C CXX)

# The hand-written assembly.  Windows spells it MASM, in .asm files ml64
# assembles; everything else spells it GAS, in .S files the C compiler driver
# hands to the assembler.  CMake calls the two different languages, so a
# directory picks its sources by host and this picks the assembler.
if(WIN32)
  enable_language(ASM_MASM)
else()
  enable_language(ASM)
endif()

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

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Keep the build tree from showing up in git, whatever it is called.
if(NOT CMAKE_BINARY_DIR STREQUAL CMAKE_SOURCE_DIR)
  file(WRITE "${CMAKE_BINARY_DIR}/.gitignore" "*\n")
endif()

include(GNUInstallDirs)

# Test timeouts
set(MONO_CTEST_TIMEOUT 300 CACHE STRING
    "Seconds a CTest test that names no budget of its own may run")
set(DART_TESTING_TIMEOUT "${MONO_CTEST_TIMEOUT}" CACHE STRING
    "Maximum time allowed before CTest will kill the test." FORCE)

include(CTest)

include(MonoOptions)
include(MonoCompilerFlags)

# Everything the build does not produce itself is searched for here, so a
# machine that is missing one of them hears about it at configure time.  What a
# directory does with a result stays with the directory: the error that stops a
# documentation build without perl belongs to docs/.
#
# mono/btls is the exception -- BoringSSL is a sub-build with a CMake project of
# its own.

# Build without googletest with -DBUILD_TESTING=OFF, which drops every test
# rather than an arbitrary subset.
if(BUILD_TESTING)
  find_package(GTest REQUIRED)
  include(GoogleTest)
endif()

find_package(Threads REQUIRED)
find_package(ZLIB)
find_package(Git QUIET)

if(MONO_ENABLE_MCS_BUILD AND MONO_ENABLE_EXECUTABLES)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()

if(MONO_ENABLE_LLVM)
  # LLVMSupport's link interface names zstd::libzstd_static, which
  # LLVMConfig.cmake imports through LLVM's own Findzstd.cmake -- and that
  # module takes a library as the static one only when the file is called
  # zstd_static.lib, a suffix vcpkg patches off.  So the module finds vcpkg's
  # zstd.lib, defines no target, and CMake reports the dangling one at generate
  # time against LLVMExports.cmake.  Config mode names the target rather than
  # the file, and the module's own NOT TARGET guards leave it standing.
  if(MSVC)
    find_package(zstd CONFIG REQUIRED)
  endif()

  # LLVMDebugInfoPDB's link interface names DIASDK::Diaguids, and the
  # FindDIASDK.cmake that defines it locates the SDK from VSINSTALLDIR, which
  # only a developer prompt sets.  From any other shell that target dangles the
  # same way.
  if(MSVC AND NOT DEFINED MSVC_DIA_SDK_DIR AND NOT DEFINED ENV{VSINSTALLDIR})
    find_program(MONO_VSWHERE vswhere
      PATHS "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer")
    if(MONO_VSWHERE)
      execute_process(
        COMMAND "${MONO_VSWHERE}" -latest -products * -property installationPath
        OUTPUT_VARIABLE _vs_install
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
      if(IS_DIRECTORY "${_vs_install}/DIA SDK")
        set(MSVC_DIA_SDK_DIR "${_vs_install}/DIA SDK")
      endif()
    endif()
  endif()

  # We check the actual version in MonoLLVM.cmake
  find_package(LLVM CONFIG QUIET PATHS "${MONO_LLVM_PREFIX}" NO_DEFAULT_PATH)
endif()

# A miss here gets as far as the link and fails on a port's dllimport CRT
# symbols, naming zlib rather than the CRT or LLVM.
if(MSVC)
  set(_crt dynamic)
  if(CMAKE_MSVC_RUNTIME_LIBRARY AND NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
    set(_crt static)
  endif()
  if(NOT _crt STREQUAL MONO_MSVC_CRT)
    message(FATAL_ERROR
      "LLVM at ${MONO_LLVM_PREFIX} wants the ${_crt} CRT and vcpkg built its "
      "ports against the ${MONO_MSVC_CRT} one -- the triplet is picked from "
      "CMAKE_MSVC_RUNTIME_LIBRARY in LLVMConfig.cmake, read before project()")
  endif()
endif()

find_program(MONO_BOOTSTRAP_RUNTIME NAMES mono mono-sgen)

# mono-sgen before mono: the latter is a launcher script on some distributions,
# and one more process per compile is not free when there are five thousand of
# them.
find_program(MONO_SYSTEM_RUNTIME_EXECUTABLE NAMES mono-sgen mono)

# The host's own strong-name tool.  A managed program the host runs itself has
# to carry a signature that host will accept, and the bootstrap profile's
# sn.exe is one of the programs that needs signing.
#
# By .exe on Windows rather than by the bare name.  Mono ships an sn of its own
# whose Windows spelling is a shell script, so a mono on PATH otherwise wins
# this search, and the signing it feeds is optional -- the bootstrap assemblies
# then stay delay-signed and the first ilasm run refuses to load one.
if(WIN32)
  find_program(MONO_HOST_SN NAMES sn.exe)
else()
  find_program(MONO_HOST_SN sn)
endif()

find_program(MONO_PERL perl)
find_program(MONO_DOXYGEN doxygen)

if(MONO_ENABLE_NLS)
  find_program(MONO_MSGFMT msgfmt)
  find_program(MONO_XGETTEXT xgettext)
endif()

find_program(MONO_NM nm)

# timeout(1), which the runtime corpus runs each program under so that a hang
# gets SIGQUIT and prints every thread's stack.  Windows has a TIMEOUT.EXE that
# waits for a keypress and shares nothing but the name, and no signal makes the
# runtime dump its threads there, so the search is skipped rather than allowed
# to find it.  MonoRunTest.cmake leaves the prefix off when this is empty, and
# CTest's own TIMEOUT still ends a hung test -- what is lost on Windows is the
# dump, not the limit.
if(NOT WIN32)
  find_program(MONO_TIMEOUT_BINARY timeout)
endif()
find_program(MONO_GNU_MAKE NAMES gmake make)
find_program(MONO_PERF_BINARY perf DOC "perf(1), for the profiled microbenchmarks")
find_library(MONO_XATTR_LIB attr)

include(MonoConfigureChecks)
include(MonoConfigureFixed)
include(MonoLLVM)
include(MonoHelpers)
include(MonoToolsRuntime)
include(MonoManaged)
include(MonoCorpus)

# Coverage instrumentation, applied after the configure checks so that none of
# them is compiled with it.
if(MONO_ENABLE_COVERAGE)
  # cl.exe ignores the flags below with a warning, so the build silently
  # reports no coverage.  clang-cl takes them.
  if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    message(FATAL_ERROR "MONO_ENABLE_COVERAGE has no cl.exe spelling")
  endif()

  add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
  add_link_options(-fprofile-instr-generate)
endif()

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

if(MONO_ENABLE_BOEHM)
  # Boehm doesn't have a cmake build so we define our own.  Before mono/, which
  # links the collector.
  add_subdirectory(cmake/bdwgc)
endif()
add_subdirectory(mono)
if(MONO_ENABLE_SUPPORT)
  add_subdirectory(support)
endif()
if(MONO_ENABLE_BTLS)
  include(MonoBtls)
endif()
if(MONO_ENABLE_IKVM_NATIVE)
  add_subdirectory(ikvm-native)
endif()
add_subdirectory(data)
add_subdirectory(scripts)
add_subdirectory(man)
add_subdirectory(po)
add_subdirectory(runtime)
if(MONO_ENABLE_MCS_BUILD)
  add_subdirectory(mcs)
endif()
# After mcs/: package-ksp packages what it built.
include(MonoKspPackage)
# After runtime/: these need the mono-wrapper and the class libraries it drives.
add_subdirectory(samples)
add_subdirectory(docs)
add_subdirectory(tools)
if(BUILD_TESTING)
  add_subdirectory(acceptance-tests)
endif()

# Test entry points
#
# `ctest` is serial unless told otherwise, and every suite here is either
# internally parallel or tiny, so a bare `ctest` is much slower than the work
# requires. These targets run it with the machine's core count.
#
#   check        the fast set -- unit tests, mini regression, the LLVM tier, and
#                the runtime one-offs. Seconds, not minutes.
#   check-all    everything except the stress suites, the minutes-long ones and
#                the acceptance suite.
#
# The heavy labels (runtime, gshared, sgen, slow, stress, acceptance) are
# opt-in:
#   ctest --test-dir build -L runtime -j <n>
#
# `acceptance` additionally needs -D MONO_ENABLE_ACCEPTANCE_TESTS=ON and the
# corpus submodules under acceptance-tests/external. Run the `print-versions`
# target to see which of those are checked out.
include(ProcessorCount)
ProcessorCount(MONO_NPROC)
if(MONO_NPROC EQUAL 0)
  set(MONO_NPROC 1)
endif()

if(BUILD_TESTING)
  # `tier0$` and not `tier0`: the corpus at the default tier is as large as
  # `runtime` and goes with it, while `tier0-unit` is seconds and stays in.
  add_custom_target(check
    COMMAND "${CMAKE_CTEST_COMMAND}" -j ${MONO_NPROC} --output-on-failure
            -LE "runtime|tier0$|gshared|sgen|bcl|bcl-xunit|compiler|tools|eglib|slow|stress|acceptance"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    USES_TERMINAL
    VERBATIM)

  add_custom_target(check-all
    COMMAND "${CMAKE_CTEST_COMMAND}" -j ${MONO_NPROC} --output-on-failure
            -LE "slow|stress|acceptance"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    USES_TERMINAL
    VERBATIM)

  # The test inputs.  Every one of these is an ALL target, so an ordinary build
  # already leaves them on disk. Naming them here is what makes `--target check`
  # work on its own.
  #
  # Each target exists only under the option that builds it, and
  # add_dependencies on a target that was never created is a generate-time
  # error. The conditions the defining directories return on are therefore
  # restated here. Probing with if(TARGET) stays just as quiet for a target that
  # has been renamed as for one that was legitimately not built.
  if(MONO_CORPUS_ENABLED)
    add_dependencies(check     mini-corpora)
    add_dependencies(check-all mini-corpora runtime-corpora)
    if(MONO_ENABLE_LLVM AND MONO_ENABLE_SGEN AND MONO_ENABLE_LIBRARIES)
      add_dependencies(check     test-llvm-data)
      add_dependencies(check-all test-llvm-data)
    endif()
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

# The microbenchmarks. They carry `slow`, so `check` and `check-all` skip
# them. This is the way to ask for them. Serial on purpose -- they are timing
# programs, and running sixteen at once makes the numbers meaningless.
if(MONO_ENABLE_BENCHMARKS AND BUILD_TESTING AND MONO_CORPUS_ENABLED)
  add_custom_target(check-benchmarks
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure -L benchmark
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    USES_TERMINAL
    VERBATIM)
  add_dependencies(check-benchmarks benchmarks)
endif()

# Convenience targets
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

message(STATUS "")
message(STATUS "  mono ${PROJECT_VERSION}")
message(STATUS "    host/target:   ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "    GC:            ${DEFAULT_GC_NAME}")
message(STATUS "    suspend:       ${MONO_THREAD_SUSPEND}")
message(STATUS "    TLS:           ${MONO_WITH_TLS}")
message(STATUS "    LLVM back end: ${MONO_ENABLE_LLVM} ${MONO_LLVM_PREFIX}")
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
