# Native unit tests.  Each one is a separate executable, which is not just
# tidiness: several of them start a runtime, and a runtime cannot be shut down
# and started again in one process, so a binary gets at most one of those.
#
# The bodies are gtest, and gtest_discover_tests turns each case into its own
# ctest test running in its own process -- so a crash still names one check.
# ENTRY is the older shape, a body plus main.c compiled with -DMAIN=<function>;
# tests that have no ENTRY are the gtest ones.


if(NOT MONO_ENABLE_SGEN)
  # The tests link the SGen runtime.
  return()
endif()

# test-mono-callspec.cpp calls mono_set_assemblies_path ("../../mcs/class/lib/
# net_4_x") and opens callspec.exe by bare name, both relative to the working
# directory, which is why these tests run from <build>/mono/unit-tests.

# RUNTIME means the test starts a runtime and therefore links the whole JIT;
# the others only need metadata and the collector.
function(mono_add_unit_test name)
  cmake_parse_arguments(ARG "RUNTIME" "ENTRY" "SOURCES" ${ARGN})

  set(_sources ${ARG_SOURCES})
  if(ARG_ENTRY)
    # Source properties are scoped to a directory, not to a target, so every
    # test needs its own copy of the shim to carry its own -DMAIN.
    set(_shim "${CMAKE_CURRENT_BINARY_DIR}/main-${name}.c")
    configure_file(main.c "${_shim}" COPYONLY)
    set_source_files_properties("${_shim}" PROPERTIES
      COMPILE_DEFINITIONS "MAIN=${ARG_ENTRY}")
    list(APPEND _sources "${_shim}")
  endif()

  add_executable(${name} ${_sources})

  target_include_directories(${name} PRIVATE
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/mono"
    "${CMAKE_SOURCE_DIR}/samples"
    "${CMAKE_SOURCE_DIR}/external/bdwgc/include"
    "${CMAKE_SOURCE_DIR}/external/bdwgc/libatomic_ops/src")
  target_compile_definitions(${name} PRIVATE
    MONO_BINDIR="${CMAKE_INSTALL_FULL_BINDIR}/"
    MONO_ASSEMBLIES="${MONO_ASSEMBLIES_DIR}"
    MONO_CFG_DIR="${MONO_CFG_DIR}")
  target_link_libraries(${name} PRIVATE mono::common mono::eglib_headers)

  # RUNTIME tests link the whole runtime archive; the rest only reach the
  # metadata and collector members, which archive semantics leave to the
  # linker to work out.
  if(ARG_RUNTIME)
    target_link_libraries(${name} PRIVATE monosgen-static)
  else()
    target_link_libraries(${name} PRIVATE
      monoruntime_sgen monoruntime_config monoruntime_support monoculture
      monosgen monoutils eglib)
  endif()

  # The LLVM libraries have to come after libmini, which references them.
  target_link_libraries(${name} PRIVATE mono::llvm m)
  if(ZLIB_FOUND)
    target_link_libraries(${name} PRIVATE ZLIB::ZLIB)
  endif()
  # libmini contains C++, so the test binaries need the C++ runtime.
  set_target_properties(${name} PROPERTIES LINKER_LANGUAGE CXX)

  if(ARG_ENTRY)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
      SKIP_RETURN_CODE 77
      ENVIRONMENT "MONO_PATH=${MONO_MCS_LIBDIR}/build")
  else()
    target_link_libraries(${name} PRIVATE GTest::gtest GTest::gtest_main)

    # PRE_TEST because discovery has to happen where the tests run: the working
    # directory is load-bearing (see above) and MONO_PATH has to be set.
    #
    # A GTEST_SKIP () has to reach ctest as a skip rather than a pass, and gtest
    # says so only in its output -- the process still exits 0.
    gtest_discover_tests(${name}
      TEST_PREFIX ${name}/
      DISCOVERY_MODE PRE_TEST
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
      PROPERTIES
        SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]"
        ENVIRONMENT "MONO_PATH=${MONO_MCS_LIBDIR}/build")
  endif()
endfunction()

mono_add_unit_test(test-sgen-qsort           SOURCES test-sgen-qsort.cpp)
mono_add_unit_test(test-memfuncs             SOURCES test-memfuncs.cpp)
mono_add_unit_test(test-mono-linked-list-set SOURCES test-mono-linked-list-set.cpp)
mono_add_unit_test(test-conc-hashtable       SOURCES test-conc-hashtable.cpp)
mono_add_unit_test(test-mono-handle          SOURCES test-mono-handle.cpp)
mono_add_unit_test(test-mono-callspec        SOURCES test-mono-callspec.cpp   RUNTIME)
mono_add_unit_test(test-mono-string          SOURCES test-mono-string.cpp     RUNTIME)
mono_add_unit_test(test-mono-embed           SOURCES test-mono-embed.cpp      RUNTIME)
mono_add_unit_test(test-embed-invoke         SOURCES test-embed-invoke.cpp    RUNTIME)
mono_add_unit_test(test-path                 SOURCES test-path.cpp            RUNTIME)

if(MONO_ENABLE_LLVM)
  # C++: it drives the .mono_lsda parser through mono/llvm/mono_lsda.hpp, a
  # C++-only interface, so it cannot live inside libmono's C surface.
  mono_add_unit_test(test-llvm-ehtable
    SOURCES test-llvm-ehtable.cpp    ENTRY test_llvm_ehtable_main     RUNTIME)
endif()

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
  _mono_test_assembly(test-embed.exe "${CMAKE_SOURCE_DIR}/samples/embed/test.cs")
  _mono_test_assembly(test-embed-invoke-cs.exe "${CMAKE_SOURCE_DIR}/samples/embed/invoke.cs")

  add_custom_target(unit-test-assemblies ALL DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/callspec.exe"
    "${CMAKE_CURRENT_BINARY_DIR}/test-embed.exe"
    "${CMAKE_CURRENT_BINARY_DIR}/test-embed-invoke-cs.exe")
  mono_tools_runtime_depends(unit-test-assemblies)
endif()

# Checks that run a managed corpus and assert on what the runtime recorded while
# compiling it, rather than on what the corpus computed.  They need the corpus
# plumbing, which mono/mini sets up.
if(MONO_CORPUS_ENABLED)
  add_subdirectory(il-offsets)
endif()
