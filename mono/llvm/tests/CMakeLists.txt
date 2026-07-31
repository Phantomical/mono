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

set(MONO_LLVM_TEST_IL arith arrays boxing calls casts eh fields flow locals refused stack)
foreach(_name IN LISTS MONO_LLVM_TEST_IL)
  mono_corpus_il(${_name}.dll il/${_name}.il LIBRARY)
endforeach()

# CTest cannot build, so assembling the corpus is itself a test the rest depend on.
add_custom_target(mono-llvm-test-corpora DEPENDS ${MONO_CORPUS_OUTPUTS})
mono_tools_runtime_depends(mono-llvm-test-corpora)
add_test(NAME mono-llvm-test-corpora
         COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}"
                 --target mono-llvm-test-corpora)
set_tests_properties(mono-llvm-test-corpora PROPERTIES
  FIXTURES_SETUP mono_llvm_test_corpora LABELS fixture)

add_executable(test-llvm-translator harness.cpp translator-tests.cpp)

target_include_directories(test-llvm-translator PRIVATE
  "${CMAKE_BINARY_DIR}"
  "${CMAKE_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/.."
  "${CMAKE_SOURCE_DIR}/mono/mini"
  "${CMAKE_BINARY_DIR}/mono/mini"
  "${CMAKE_SOURCE_DIR}/external/bdwgc/include"
  "${CMAKE_SOURCE_DIR}/external/bdwgc/libatomic_ops/src")

target_compile_definitions(test-llvm-translator PRIVATE
  MONO_LLVM_TESTS_DIR="${CMAKE_CURRENT_BINARY_DIR}"
  MONO_LLVM_TESTS_ASSEMBLIES="${MONO_CORPUS_CLASS_DIR}")

# mono::llvm carries -fno-rtti, which is how LLVM 18 itself is built.  gtest
# notices and turns GTEST_HAS_RTTI off, which costs only the type names it prints
# in a few failure messages -- and matching LLVM matters more, since the
# alternative is a TU that sees LLVM's polymorphic classes with a different ABI
# than the backend it is testing.
target_link_libraries(test-llvm-translator PRIVATE
  monosgen-static mono::common mono::eglib_headers mono::llvm
  GTest::gtest GTest::gtest_main m)
if(ZLIB_FOUND)
  target_link_libraries(test-llvm-translator PRIVATE ZLIB::ZLIB)
endif()

gtest_discover_tests(test-llvm-translator
  TEST_PREFIX test-llvm-translator/
  DISCOVERY_MODE PRE_TEST
  PROPERTIES
    FIXTURES_REQUIRED mono_llvm_test_corpora
    LABELS llvm
    ENVIRONMENT "MONO_TIERED=0;MONO_CFG_DIR=${CMAKE_BINARY_DIR}/runtime/etc")
