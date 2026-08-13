# Native unit tests.  A binary holds as many of them as can share one process,
# which is what keeps the number of executables down; the split that is left is
# forced.  Starting a runtime is a one-way door -- it cannot be shut down and
# started again -- so a test that tears one down gets a binary to itself, and
# two tests that want the thread-info subsystem stood up differently cannot
# agree on one either.
#
# The bodies are gtest, and gtest_discover_tests turns each case into its own
# ctest test running in its own process -- so a crash still names one check.

# Ahead of the SGen gate below, because this one is about the shared library
# rather than about the runtime it starts, and it wants whichever collectors
# were built. It links nothing: the point is to load the runtime the way an
# embedder does.
add_executable(test-dlopen-runtime test-dlopen-runtime.c)
target_link_libraries(test-dlopen-runtime PRIVATE ${CMAKE_DL_LIBS})

foreach(_runtime monosgen-2.0 monoboehm-2.0)
  if(TARGET ${_runtime})
    add_test(NAME dlopen-${_runtime}
             COMMAND test-dlopen-runtime "$<TARGET_FILE:${_runtime}>")
  endif()
endforeach()

if(NOT MONO_ENABLE_SGEN)
  # The tests link the SGen runtime.
  return()
endif()

# harness.cpp calls mono_set_assemblies_path ("../../mcs/class/lib/net_4_x") and
# test-mono-callspec.cpp opens callspec.exe by bare name, both relative to the
# working directory, which is why these tests run from <build>/mono/unit-tests.

# RUNTIME links the whole JIT as an archive; the rest link only metadata and the
# collector, whose objects are force-linked.
function(mono_add_unit_test name)
  cmake_parse_arguments(ARG "RUNTIME" "" "SOURCES" ${ARGN})

  add_executable(${name} ${ARG_SOURCES})

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

  target_link_libraries(${name} PRIVATE GTest::gtest GTest::gtest_main)
endfunction()

# Register TARGET's cases matching FILTER as ctest tests named
# <prefix>/<suite>.<case>.
#
# A binary carrying several files' worth of cases gets one call per file, so the
# ctest names stay tied to the source the case lives in rather than to whichever
# binary it was linked into.  The filter only narrows discovery: each case still
# runs on its own with --gtest_filter=<suite>.<case>.
function(mono_unit_test_cases target prefix filter)
  # PRE_TEST because discovery has to happen where the tests run: the working
  # directory is load-bearing (see above) and MONO_PATH has to be set.
  #
  # A GTEST_SKIP () has to reach ctest as a skip rather than a pass, and gtest
  # says so only in its output -- the process still exits 0.
  gtest_discover_tests(${target}
    TEST_PREFIX ${prefix}/
    TEST_FILTER ${filter}
    DISCOVERY_MODE PRE_TEST
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    PROPERTIES
      SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]"
      ENVIRONMENT "MONO_PATH=${MONO_MCS_LIBDIR}/build")
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
  test-function-pointer.cpp)
mono_unit_test_cases(test-mono-runtime test-mono-callspec "Callspec.*")
mono_unit_test_cases(test-mono-runtime test-mono-string   "MonoStringTest.*")
mono_unit_test_cases(test-mono-runtime test-path          "MonoPath.*")
mono_unit_test_cases(test-mono-runtime test-function-pointer "FunctionPointer.*")

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
  _mono_test_assembly(test-embed.exe "${CMAKE_SOURCE_DIR}/samples/embed/test.cs")
  _mono_test_assembly(test-embed-invoke-cs.exe "${CMAKE_SOURCE_DIR}/samples/embed/invoke.cs")

  add_custom_target(unit-test-assemblies ALL DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/callspec.exe"
    "${CMAKE_CURRENT_BINARY_DIR}/test-embed.exe"
    "${CMAKE_CURRENT_BINARY_DIR}/test-embed-invoke-cs.exe")
  mono_tools_runtime_depends(unit-test-assemblies)
endif()

# The backend's own tests.  Their own directory because they come with a corpus
# of .il fixtures and link the backend rather than just the runtime.
add_subdirectory(llvm)

# Checks that run a managed corpus and assert on what the runtime recorded while
# compiling it, rather than on what the corpus computed.  They need the corpus
# plumbing, which mono/mini sets up.
if(MONO_CORPUS_ENABLED)
  add_subdirectory(il-offsets)
endif()
