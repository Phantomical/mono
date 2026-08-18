# The acceptance tests: large external corpora that live outside this
# repository.  Ported from Makefile.am and the five .mk files it included
# (versions.mk, coreclr.mk, microbench.mk, profiler-stress.mk, roslyn.mk,
# ms-test-suite.mk).
#
# These are nothing like `check`.  The CoreCLR corpus alone is ~4700 assemblies
# to compile and two suites to run; the microbenchmarks are BenchmarkDotNet
# harnesses that take minutes each.  So the whole directory is behind
# MONO_ENABLE_ACCEPTANCE_TESTS, off by default, and every test it registers
# carries the `acceptance` label so it stays out of `check` and `check-all`:
#
#   cmake -B build -D MONO_ENABLE_ACCEPTANCE_TESTS=ON
#   ctest --test-dir build -L acceptance
#
# The corpora are git submodules, and this build never fetches them.
# `print-versions` reports which are in place and prints the `git submodule
# update --init <path>` for the ones that are not.  Fetch them by name: coreclr
# alone is ~650M and most builds want none of them.
#
# There is no target for re-pinning a corpus, because git is the interface:
# check the submodule out at the revision you want and commit the gitlink.

include(MonoSubmodules)

# Fixed, unlike the old ACCEPTANCE_TESTS_PATH make variable: a submodule lives
# where .gitmodules says it lives.
set(_ext "${CMAKE_CURRENT_SOURCE_DIR}/external")

# slug|submodule path, in .gitmodules order.
set(_acceptance_submodules
  "CORECLR|acceptance-tests/external/coreclr"
  "BENCHMARKER|acceptance-tests/external/benchmarker"
  "DEBIANSHOOTOUTMONO|acceptance-tests/external/DebianShootoutMono"
  "ROSLYN|acceptance-tests/external/roslyn"
  "MS_TEST_SUITE|acceptance-tests/external/ms-test-suite")

option(MONO_ENABLE_ACCEPTANCE_TESTS
       "Build and register the acceptance-test suites (needs the submodule checkouts)" OFF)

# `make print-versions` from versions.mk, minus the network.  Always available,
# because it is how you find out what the suites below are waiting for.
set(_report "${CMAKE_CURRENT_BINARY_DIR}/print-versions.cmake")
set(_lines "")
foreach(_entry IN LISTS _acceptance_submodules)
  string(REGEX REPLACE "\\|.*$" "" _slug "${_entry}")
  string(REGEX REPLACE "^[^|]*\\|" "" _path "${_entry}")
  mono_submodule_status("${_path}" ${_slug})

  if(MONO_SUBMODULE_${_slug}_AT_REV)
    set(_state "ok")
  elseif(MONO_SUBMODULE_${_slug}_PRESENT)
    set(_state "WRONG REV (at ${MONO_SUBMODULE_${_slug}_HEAD})")
  else()
    set(_state "not checked out")
  endif()
  string(APPEND _lines
    "message(\"  ${_path}\")\n"
    "message(\"      state  ${_state}\")\n"
    "message(\"      rev    ${MONO_SUBMODULE_${_slug}_REV}\")\n")
  if(NOT MONO_SUBMODULE_${_slug}_AT_REV)
    mono_submodule_init_hint(${_slug} _hint)
    string(APPEND _lines "message(\"      get it  ${_hint}\")\n")
  endif()
endforeach()
file(WRITE "${_report}" "${_lines}")
add_custom_target(print-versions
  COMMAND "${CMAKE_COMMAND}" -P "${_report}"
  COMMENT "acceptance-tests: submodule checkouts"
  VERBATIM)

if(NOT MONO_ENABLE_ACCEPTANCE_TESTS)
  return()
endif()

if(NOT MONO_ENABLE_MCS_BUILD OR NOT MONO_ENABLE_EXECUTABLES)
  message(WARNING
    "MONO_ENABLE_ACCEPTANCE_TESTS needs the class libraries and the mono "
    "binaries; skipping the acceptance tests.")
  return()
endif()

# ---------------------------------------------------------------------------
# The toolchain, spelled as in Makefile.am
# ---------------------------------------------------------------------------
set(_src      "${CMAKE_CURRENT_SOURCE_DIR}")
set(_bin      "${CMAKE_CURRENT_BINARY_DIR}")
set(_class    "${MONO_MCS_LIBDIR}/${MONO_DEFAULT_PROFILE}")
set(_buildcls "${MONO_MCS_LIBDIR}/build")
set(_wrapper  "${CMAKE_BINARY_DIR}/runtime/mono-wrapper")

# RUNTIME / TOOLS_RUNTIME.  RUNTIME is what the suites are *testing*, so it is
# always the one this build produced.  TOOLS_RUNTIME only hosts the compiler and
# follows MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS -- see MonoToolsRuntime.cmake, and
# note that these compiles do not pass -nostdlib.
set(_runtime "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class}" "${_wrapper}" --debug)
mono_tools_runtime(_tools_runtime MONO_PATH "${_buildcls}" ARGS --debug)

# MCS and ILASM.  Note the -lib:/bare -r: spelling: acceptance-tests resolves
# its references off -lib, where mono/tests passes absolute paths.
set(_mcs ${_tools_runtime} "${MONO_CSC}" -nologo -noconfig "-lib:${_class}"
         -r:System.dll -r:System.Core.dll -r:System.Xml.dll -r:Microsoft.CSharp.dll)
# -quiet drops the per-file banner and the success line; warnings and errors are
# printed either way.
set(_ilasm ${_tools_runtime} "${_buildcls}/ilasm.exe" -quiet)

if(NOT MONO_GNU_MAKE)
  message(FATAL_ERROR
    "The acceptance tests need GNU make: ms-test-suite drives the corpora's "
    "own Makefiles")
endif()

# What every compile below has to wait for: the class libraries the -lib:/-r:
# references resolve against, and (without MONO_USE_SYSTEM_RUNTIME_FOR_TOOLS)
# the runtime hosting the compiler.
add_custom_target(acceptance-toolchain)
add_dependencies(acceptance-toolchain mcs)
mono_tools_runtime_depends(acceptance-toolchain)

# test-runner.exe, shared by the CoreCLR suites.  mono/tests builds its own
# copy in its own binary dir; this is the acceptance-tests one, exactly as
# coreclr.mk built it here.
add_custom_command(
  OUTPUT "${_bin}/test-runner.exe"
  COMMAND ${_mcs} -debug:portable -r:Mono.Posix.dll
          "-out:${_bin}/test-runner.exe"
          "${CMAKE_SOURCE_DIR}/mono/tests/test-runner.cs"
  DEPENDS "${CMAKE_SOURCE_DIR}/mono/tests/test-runner.cs" acceptance-toolchain
  COMMENT "CSC test-runner.exe (acceptance)"
  VERBATIM)
add_custom_target(acceptance-test-runner DEPENDS "${_bin}/test-runner.exe")

include(coreclr.cmake)
include(microbench.cmake)
include(profiler-stress.cmake)
include(ms-test-suite.cmake)
include(roslyn.cmake)
