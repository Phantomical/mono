# Tests that validate that the devirt pass is actually devirtualizing calls.
#
# One .cs per fixture, each built into its own program.  The fixtures are
# differential by construction: a [MethodImpl(NoInlining)] "hot" wrapper looped
# past MONO_TIERED_CALL_THRESHOLD, so it promotes and becomes the LLVM module's
# root, computing its answer in a way that does not depend on which tier ran it.
# A wrong devirtualization then changes the answer on the tier-1 run only.
#
# See check_devirt_tags.py and check_inliner_tags.py for the syntax to use.

if(NOT MONO_ENABLE_LLVM)
  return()
endif()

file(GLOB DEVIRT_TESTS CONFIGURE_DEPENDS *.cs)

set(MONO_CORPUS_OUTPUTS "")

foreach(src ${DEVIRT_TESTS})
  cmake_path(GET src FILENAME file_name)
  cmake_path(GET file_name STEM test_name)

  mono_corpus_cs("${test_name}.exe"
    SOURCES "${file_name}"
    REFS "${MONO_CORPUS_MINI_DIR}/TestDriver.dll")

  mono_add_corpus_check("${test_name}:devirt"
    SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/check_devirt_tags.py"
    CORPUS "${CMAKE_CURRENT_BINARY_DIR}/${test_name}.exe"
    SOURCE "${src}")
  mono_add_corpus_check("${test_name}:inliner"
    SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../inliner/check_inliner_tags.py"
    CORPUS "${CMAKE_CURRENT_BINARY_DIR}/${test_name}.exe"
    SOURCE "${src}")

  foreach(n 0 1)
    mono_add_tiered_test("${test_name}" "${test_name}.exe" ${n})
  endforeach()
endforeach()

mono_corpus_target(devirt-corpora DEPENDS mini-test-driver)
