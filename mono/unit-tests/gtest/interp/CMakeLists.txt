# The interpreter's tests.  They reach the transform's internals rather than
# running managed code through it, which is why they link the runtime archive
# instead of going through the mono binary.

if(NOT MONO_ENABLE_INTERPRETER OR NOT MONO_ENABLE_SGEN OR NOT MONO_ENABLE_LIBRARIES)
  # No interpreter to test, or no monosgen-static to link it against.
  return()
endif()

# Each case transforms a method out of a `.il` file under il/ that ilasm
# assembles into an assembly the test loads.  The sources compile either way and
# the fixtures skip themselves when there is no corpus: a case that disappears
# from `ctest -N` reads exactly like a case that passed.
if(MONO_CORPUS_ENABLED)
  # RELATIVE, because mono_corpus_il resolves its source against the current
  # source directory itself.
  file(GLOB MONO_INTERP_TEST_IL CONFIGURE_DEPENDS
       RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" il/*.il)
  foreach(ilfile IN LISTS MONO_INTERP_TEST_IL)
    cmake_path(GET ilfile STEM testname)
    mono_corpus_il("${testname}.dll" "${ilfile}" LIBRARY)
  endforeach()

  add_custom_target(test-interp-data ALL DEPENDS ${MONO_CORPUS_OUTPUTS})
  mono_tools_runtime_depends(test-interp-data)
endif()

mono_gtest_binary(test-interp
  SOURCES harness.cpp transform-tests.cpp intrins-tests.cpp
  LIBRARIES monosgen-static
  INCLUDES
    "${CMAKE_SOURCE_DIR}/mono/interp"
    "${CMAKE_SOURCE_DIR}/mono/mini"
    "${CMAKE_BINARY_DIR}/mono/mini"
  DEFINITIONS
    MONO_INTERP_TESTS_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    MONO_INTERP_TESTS_ASSEMBLIES="${MONO_CORPUS_CLASS_DIR}")

gtest_discover_tests(test-interp
  TEST_PREFIX test-interp/
  DISCOVERY_MODE PRE_TEST
  PROPERTIES
    LABELS interp-unit
    SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]"
    ENVIRONMENT "MONO_CFG_DIR=${CMAKE_BINARY_DIR}/runtime/etc")
