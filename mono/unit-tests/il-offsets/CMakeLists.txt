# Tier-1 stack traces against the classic JIT's, method by method and offset by
# offset.
#
# Tier 1 reads its native_offset -> il_offset map back out of the emitted object
# rather than building it during codegen, so the map can be wrong -- or carry a
# body it inlined -- with the corpus still green, because a bad map only makes
# stack traces lie.  check_il_offsets.py diffs it against the classic JIT, which
# computes the same mapping the direct way.

if(NOT MONO_ENABLE_LLVM)
  return()
endif()

set(MONO_CORPUS_OUTPUTS "")
mono_corpus_cs(il-offset-tests.exe SOURCES il-offset-tests.cs
               REFS "${MONO_CORPUS_MINI_DIR}/TestDriver.dll")
mono_corpus_target(il-offsets-corpus DEPENDS mini-test-driver)

mono_add_corpus_check(tiered-il-offsets
  SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/check_il_offsets.py"
  CORPUS "${CMAKE_CURRENT_BINARY_DIR}/il-offset-tests.exe")
