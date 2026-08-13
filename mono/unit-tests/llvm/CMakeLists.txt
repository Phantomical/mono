# The backend's native tests, all in one binary.
#
# Unlike the tests one directory up, these link the backend itself and take its
# build settings with it, which is why they have their own CMakeLists rather than
# going through mono_add_unit_test.

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
    stub-tests.cpp codemem-tests.cpp mono-lsda-tests.cpp gdb-jit-tests.cpp
    harness.cpp translator-tests.cpp jit-tests.cpp compiler-tests.cpp
    compile-queue-tests.cpp interp-entry-tests.cpp perf-eh-frame-tests.cpp)

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

add_executable(test-llvm ${MONO_LLVM_TEST_SOURCES})

target_include_directories(test-llvm PRIVATE
  "${CMAKE_BINARY_DIR}"
  "${CMAKE_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/mono/llvm"
  "${CMAKE_SOURCE_DIR}/mono/mini"
  "${CMAKE_BINARY_DIR}/mono/mini"
  "${CMAKE_SOURCE_DIR}/external/bdwgc/include"
  "${CMAKE_SOURCE_DIR}/external/bdwgc/libatomic_ops/src")

target_compile_definitions(test-llvm PRIVATE
  MONO_LLVM_TESTS_DIR="${CMAKE_CURRENT_BINARY_DIR}"
  MONO_LLVM_TESTS_ASSEMBLIES="${MONO_CORPUS_CLASS_DIR}")

# mono::llvm carries -fno-rtti, which is how LLVM itself is built.  gtest
# notices and turns GTEST_HAS_RTTI off, which costs only the type names it prints
# in a few failure messages -- and matching LLVM matters more, since the
# alternative is a TU that sees LLVM's polymorphic classes with a different ABI
# than the backend it is testing.
target_link_libraries(test-llvm PRIVATE
  monosgen-static mono::common mono::eglib_headers mono::llvm
  GTest::gtest GTest::gtest_main m)
if(ZLIB_FOUND)
  target_link_libraries(test-llvm PRIVATE ZLIB::ZLIB)
endif()

# A GTEST_SKIP () has to reach ctest as a skip rather than a pass, and gtest says
# so only in its output -- the process still exits 0.
gtest_discover_tests(test-llvm
  TEST_PREFIX test-llvm/
  DISCOVERY_MODE PRE_TEST
  PROPERTIES
    LABELS llvm
    SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]"
    ENVIRONMENT "MONO_CFG_DIR=${CMAKE_BINARY_DIR}/runtime/etc")
