# Tier-1 stack traces against the classic JIT's, method by method and offset by
# offset.
#
# Tier 1 reads its native_offset -> il_offset map back out of the emitted object
# rather than building it during codegen, so the map can be wrong -- or carry a
# body it inlined -- with the corpus still green, because a bad map only makes
# stack traces lie.  check_il_offsets.py diffs it against the classic JIT, which
# computes the same mapping the direct way.
#
# One .cs per scenario, each built into its own program.  Main calls Scenario (),
# which does the throwing and the catching and prints one line per managed frame,
# so Main stays out of the trace and the compared frames are exactly the ones the
# scenario is about.  Nothing asserts an offset by value, so what makes a scenario
# worth having is a frame whose offset could plausibly be attributed to the wrong
# method: a call site tier 1 may inline through, a throw the optimizer may move or
# merge, or a frame below one of those.
#
# A body meant to be inlined is padded well past the classic JIT's own front-end
# inliner (20 IL bytes), which would otherwise fold it and leave the oracle with
# no frame to report while tier 1, which does no front-end inlining, still reports
# one.  See check_il_offsets.py.

if(NOT MONO_ENABLE_LLVM)
  return()
endif()

file(GLOB IL_OFFSET_TESTS CONFIGURE_DEPENDS *.cs)

set(MONO_CORPUS_OUTPUTS "")

foreach(src ${IL_OFFSET_TESTS})
  cmake_path(GET src FILENAME file_name)
  cmake_path(GET file_name STEM test_name)

  mono_corpus_cs("${test_name}.exe" SOURCES "${file_name}")

  mono_add_corpus_check("${test_name}:il-offsets"
    SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/check_il_offsets.py"
    CORPUS "${CMAKE_CURRENT_BINARY_DIR}/${test_name}.exe")
endforeach()

mono_corpus_target(il-offsets-corpora)
