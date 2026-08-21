# Managed stack traces, frame by frame, against the source lines they name.
#
# The runtime reads a method's native_offset -> il_offset map back out of the
# emitted object rather than building it during codegen. The map can be wrong
# with the corpus still green -- a bad map only makes stack traces lie. Each
# fixture prints one frame per line and carries, as IL-FRAME markers, the
# trace it has to produce. check_il_offsets.py runs it and compares.
#
# One .cs per scenario, each built into its own program. Main calls
# Scenario (), which does the throwing and the catching. That keeps Main out
# of the trace, so the frames compared are exactly the ones the scenario is
# about. A scenario is worth having when the optimizer can mis-attribute a
# frame. That covers a throw sinking or cross-jumping with another, and a
# frame an unwind resumed.
#
# The corpora are built with a portable PDB, which is what lets the runtime turn
# an IL offset back into a line.  See check_il_offsets.py.

if(NOT MONO_ENABLE_LLVM)
  return()
endif()

file(GLOB IL_OFFSET_TESTS CONFIGURE_DEPENDS *.cs)

set(MONO_CORPUS_OUTPUTS "")

foreach(src ${IL_OFFSET_TESTS})
  cmake_path(GET src FILENAME file_name)
  cmake_path(GET file_name STEM test_name)

  mono_corpus_cs("${test_name}.exe" DEBUG SOURCES "${file_name}")

  mono_add_corpus_check("il-offsets/${test_name}"
    SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/check_il_offsets.py"
    CORPUS "${CMAKE_CURRENT_BINARY_DIR}/${test_name}.exe"
    SOURCE "${src}")
endforeach()

mono_corpus_target(il-offsets-corpora)
