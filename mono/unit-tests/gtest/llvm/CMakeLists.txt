# The backend's tests.  They link the backend and drive it directly rather than
# running managed code through the mono binary.

if(NOT MONO_ENABLE_LLVM OR NOT MONO_ENABLE_SGEN OR NOT MONO_ENABLE_LIBRARIES)
  # No backend to test, or no monosgen-static to link it against.
  return()
endif()

# Most of the cases drive method_to_llvm () over real metadata, so each one is a
# method in a `.il` file under il/ that ilasm assembles into an assembly the test
# loads.  Their sources are compiled either way and the fixtures skip themselves
# when there is no corpus to load: a case that disappears from `ctest -N` reads
# exactly like a case that passed.
set(MONO_LLVM_TEST_SOURCES
    thunk-tests.cpp jitlink-memory-tests.cpp mono-lsda-tests.cpp gdb-jit-tests.cpp
    harness.cpp translator-tests.cpp jit-tests.cpp compiler-tests.cpp
    compile-queue-tests.cpp interp-entry-tests.cpp perf-eh-frame-tests.cpp
    unwind-records-tests.cpp)

if(MONO_CORPUS_ENABLED)
  # RELATIVE, because mono_corpus_il resolves its source against the current
  # source directory itself.
  file(GLOB MONO_LLVM_TEST_IL CONFIGURE_DEPENDS
       RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" il/*.il)
  foreach(ilfile IN LISTS MONO_LLVM_TEST_IL)
    cmake_path(GET ilfile STEM testname)
    mono_corpus_il("${testname}.dll" "${ilfile}" LIBRARY)
  endforeach()

  add_custom_target(test-llvm-data ALL DEPENDS ${MONO_CORPUS_OUTPUTS})
  mono_tools_runtime_depends(test-llvm-data)
endif()

mono_gtest_binary(test-llvm
  SOURCES ${MONO_LLVM_TEST_SOURCES}
  LIBRARIES monosgen-static
  INCLUDES
    "${CMAKE_SOURCE_DIR}/mono/llvm"
    "${CMAKE_SOURCE_DIR}/mono/mini"
    "${CMAKE_BINARY_DIR}/mono/mini"
  DEFINITIONS
    MONO_LLVM_TESTS_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    MONO_LLVM_TESTS_ASSEMBLIES="${MONO_CORPUS_CLASS_DIR}")

mono_gtest_tests(test-llvm
  PREFIX test-llvm
  SKIP_REGEX "\\[  SKIPPED \\]"
  PROPERTIES
    LABELS llvm
    ENVIRONMENT "MONO_CFG_DIR=${CMAKE_BINARY_DIR}/runtime/etc")
