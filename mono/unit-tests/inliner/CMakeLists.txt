# Tests that validate that the tier-1 inliner is actually folding calls, and folding
# them correctly.
#
# One .cs per fixture, each built into its own program.  The fixtures are differential
# by construction: the candidate callee is reached from a small
# [MethodImpl(NoInlining)] "hot" wrapper looped past MONO_TIERED_CALL_THRESHOLD, so the
# wrapper promotes and becomes the LLVM module's root and its calls are what the pass
# looks at.  A wrong decision changes the answer on the tier-1 run only.
#
# Two conventions worth knowing before adding a fixture:
#
#  - A test that works out its expected value by recomputing the callee's logic inline
#    carries [MethodImpl(NoOptimization)], which keeps the oracle on the classic JIT.
#    Without it the test method promotes too and the reference arithmetic ends up
#    compiled by the very backend it is meant to be checking.  Comparing against a
#    literal needs no attribute.
#
#  - Leaf candidates look padded with unused arithmetic on purpose.  Classic mini has
#    its own front-end inliner that folds any callee under ~100 IL bytes before the LLVM
#    translator sees the call at all, so padding past that is what keeps the site
#    intact.  The padding avoids `/` and `%`, whose overflow-check call would make the
#    body read as non-leaf for a reason unrelated to the gate under test.  A callee with
#    clauses, or one that is NoInlining/Synchronized or already non-leaf, needs none.
#
# See check_inliner_tags.py for the syntax to use.

if(NOT MONO_ENABLE_LLVM)
  return()
endif()

file(GLOB INLINER_TESTS CONFIGURE_DEPENDS *.cs)

set(MONO_CORPUS_OUTPUTS "")

# C# has no syntax for a bare fault clause, so that one callee is hand-written IL.
# Every corpus gets the reference; only try-fault-leaf.cs uses it.
mono_corpus_il(inliner-fault.dll inliner-fault.il LIBRARY)

foreach(src ${INLINER_TESTS})
  cmake_path(GET src FILENAME file_name)
  cmake_path(GET file_name STEM test_name)

  mono_corpus_cs("${test_name}.exe"
    SOURCES "${file_name}"
    REFS "${MONO_CORPUS_MINI_DIR}/TestDriver.dll"
         "${CMAKE_CURRENT_BINARY_DIR}/inliner-fault.dll")

  mono_add_corpus_check("${test_name}:inliner"
    SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/check_inliner_tags.py"
    CORPUS "${CMAKE_CURRENT_BINARY_DIR}/${test_name}.exe"
    SOURCE "${src}")

  foreach(n 0 1)
    mono_add_tiered_test("${test_name}" "${test_name}.exe" ${n})
  endforeach()
endforeach()

mono_corpus_target(inliner-corpora DEPENDS mini-test-driver)
