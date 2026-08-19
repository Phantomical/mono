# The runtime and the utility libraries under it.

if(NOT MONO_ENABLE_SGEN)
  # The tests link the SGen runtime.
  return()
endif()

# Builds one of this suite's binaries out of SOURCES.  RUNTIME links the whole
# runtime archive, which is what a test that starts a runtime or reaches a
# private symbol needs.
function(mono_add_unit_test name)
  cmake_parse_arguments(ARG "RUNTIME" "" "SOURCES" ${ARGN})

  # Naming the members rather than the archive leaves the rest of the runtime
  # out, which archive semantics let the linker work out on its own.
  if(ARG_RUNTIME)
    set(_libs monosgen-static)
  else()
    set(_libs monoruntime_sgen monoruntime_config monoruntime_support
              monoculture monosgen monoutils eglib)
  endif()

  mono_gtest_binary(${name}
    SOURCES ${ARG_SOURCES}
    LIBRARIES ${_libs}
    INCLUDES
      "${CMAKE_SOURCE_DIR}/mono"
      "${CMAKE_SOURCE_DIR}/samples"
    DEFINITIONS
      MONO_BINDIR="${CMAKE_INSTALL_FULL_BINDIR}/"
      MONO_ASSEMBLIES="${MONO_ASSEMBLIES_DIR}"
      MONO_CFG_DIR="${MONO_CFG_DIR}"
      MONO_UNIT_TESTS_ASSEMBLIES="${MONO_MCS_LIBDIR}/${MONO_DEFAULT_PROFILE}")
endfunction()

# Registers the cases of target that match filter as ctest tests under prefix.
#
# A binary carrying several files' worth of cases gets one call per file, so the
# ctest names stay tied to the source the case lives in rather than to whichever
# binary it was linked into.
function(mono_unit_test_cases target prefix filter)
  mono_gtest_tests(${target}
    PREFIX ${prefix}
    FILTER ${filter}
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    SKIP_REGEX "\\[  SKIPPED \\]"
    PROPERTIES ENVIRONMENT "MONO_PATH=${MONO_MCS_LIBDIR}/build")
endfunction()

# The tests that need neither a runtime nor the thread-info subsystem, plus the
# one -- ConcHashTable -- that stands it up first.
mono_add_unit_test(test-mono-utils SOURCES
  test-sgen-qsort.cpp test-memfuncs.cpp test-mono-handle.cpp
  test-conc-hashtable.cpp)
mono_unit_test_cases(test-mono-utils test-sgen-qsort     "SgenQsort.*")
mono_unit_test_cases(test-mono-utils test-memfuncs       "MemFuncs.*")
mono_unit_test_cases(test-mono-utils test-mono-handle    "MonoHandle.*")
mono_unit_test_cases(test-mono-utils test-conc-hashtable "ConcHashTable.*")

# Alone because it wants mono_thread_info_init (0) where ConcHashTable wants
# sizeof (MonoThreadInfo), and whichever runs first decides for the process.
mono_add_unit_test(test-mono-linked-list-set SOURCES test-mono-linked-list-set.cpp)
mono_unit_test_cases(test-mono-linked-list-set test-mono-linked-list-set "*")

# The tests that run against a live runtime, which harness.cpp brings up once for
# the binary.  test-path is here without needing one: it reaches mono-path.c's
# statics by including the file, and only the archive semantics of the RUNTIME
# link let the linker leave the library's own copy of those symbols out.
mono_add_unit_test(test-mono-runtime RUNTIME SOURCES
  harness.cpp test-mono-callspec.cpp test-mono-string.cpp test-path.cpp
  test-function-pointer.cpp test-detour.cpp)
mono_unit_test_cases(test-mono-runtime test-mono-callspec "Callspec.*")
mono_unit_test_cases(test-mono-runtime test-mono-string   "MonoStringTest.*")
mono_unit_test_cases(test-mono-runtime test-path          "MonoPath.*")
mono_unit_test_cases(test-mono-runtime test-function-pointer "FunctionPointer.*")
mono_unit_test_cases(test-mono-runtime test-detour        "MethodDetour.*")

# One case each, and there could not be a second: the samples they are built out
# of run a domain to completion and call mono_jit_cleanup, and nothing in the
# process gets a runtime after that.  They take harness.cpp for its class-library
# check alone -- the runtime they use is the one the sample starts itself.
mono_add_unit_test(test-mono-embed   SOURCES harness.cpp test-mono-embed.cpp   RUNTIME)
mono_unit_test_cases(test-mono-embed test-mono-embed "*")
mono_add_unit_test(test-embed-invoke SOURCES harness.cpp test-embed-invoke.cpp RUNTIME)
mono_unit_test_cases(test-embed-invoke test-embed-invoke "*")

# The managed side of the tests that need one.
if(MONO_ENABLE_MCS_BUILD)
  set(_class_dir "${MONO_MCS_LIBDIR}/${MONO_DEFAULT_PROFILE}")
  set(_build_dir "${MONO_MCS_LIBDIR}/build")
  mono_tools_runtime(_host MONO_PATH "${_build_dir}")
  set(_mcs ${_host}
           "${MONO_CSC}" -debug:portable -noconfig -nologo -unsafe
           -nowarn:0162 -nowarn:0168 -nowarn:0219 -nowarn:0414 -nowarn:0618
           -nowarn:0169 -nowarn:1690 -nowarn:0649 -nowarn:0612 -nowarn:3021
           -nowarn:0197
           "-r:${_class_dir}/System.dll"
           "-r:${_class_dir}/System.Xml.dll"
           "-r:${_class_dir}/System.Core.dll")

  function(_mono_test_assembly out source)
    add_custom_command(
      OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${out}"
      COMMAND ${_mcs} "-out:${CMAKE_CURRENT_BINARY_DIR}/${out}" "${source}"
      DEPENDS "${source}" mcs
      COMMENT "CSC ${out}"
      VERBATIM)
  endfunction()

  _mono_test_assembly(callspec.exe "${CMAKE_CURRENT_SOURCE_DIR}/callspec.cs")
  _mono_test_assembly(detour.exe "${CMAKE_CURRENT_SOURCE_DIR}/detour.cs")
  _mono_test_assembly(test-embed.exe "${CMAKE_SOURCE_DIR}/samples/embed/test.cs")
  _mono_test_assembly(test-embed-invoke-cs.exe "${CMAKE_SOURCE_DIR}/samples/embed/invoke.cs")

  add_custom_target(unit-test-assemblies ALL DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/callspec.exe"
    "${CMAKE_CURRENT_BINARY_DIR}/detour.exe"
    "${CMAKE_CURRENT_BINARY_DIR}/test-embed.exe"
    "${CMAKE_CURRENT_BINARY_DIR}/test-embed-invoke-cs.exe")
  mono_tools_runtime_depends(unit-test-assemblies)
endif()
