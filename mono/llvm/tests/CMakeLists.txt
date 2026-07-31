# Tests for the IL -> LLVM IR translator.
#
# These drive method_to_llvm () over real metadata, so each case is a method in a
# `.il` file under il/ that ilasm assembles into an assembly the test loads.  That
# is why they live here rather than in mono/unit-tests: the corpus is part of the
# test, and the whole thing is gtest rather than the hand-rolled reporting the
# tests over there share.

if(NOT MONO_ENABLE_SGEN OR NOT MONO_ENABLE_LIBRARIES)
  return()
endif()
if(NOT MONO_CORPUS_ENABLED)
  # No ilasm to build the corpus with.
  return()
endif()

find_package(GTest QUIET)
if(NOT GTest_FOUND)
  message(STATUS "gtest not found -- not building the mono/llvm translator tests")
  return()
endif()
include(GoogleTest)

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

add_executable(test-llvm harness.cpp translator-tests.cpp)

target_include_directories(test-llvm PRIVATE
  "${CMAKE_BINARY_DIR}"
  "${CMAKE_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/.."
  "${CMAKE_SOURCE_DIR}/mono/mini"
  "${CMAKE_BINARY_DIR}/mono/mini"
  "${CMAKE_SOURCE_DIR}/external/bdwgc/include"
  "${CMAKE_SOURCE_DIR}/external/bdwgc/libatomic_ops/src")

target_compile_definitions(test-llvm PRIVATE
  MONO_LLVM_TESTS_DIR="${CMAKE_CURRENT_BINARY_DIR}"
  MONO_LLVM_TESTS_ASSEMBLIES="${MONO_CORPUS_CLASS_DIR}")

# mono::llvm carries -fno-rtti, which is how LLVM 18 itself is built.  gtest
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

gtest_discover_tests(test-llvm
  TEST_PREFIX test-llvm/
  DISCOVERY_MODE PRE_TEST
  PROPERTIES
    LABELS llvm
    ENVIRONMENT "MONO_TIERED=0;MONO_CFG_DIR=${CMAKE_BINARY_DIR}/runtime/etc")
