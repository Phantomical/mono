# How tier-1 encodes a class's static fields, asserted from the const trace.
#
# tiered-statics.exe reads and writes the right values whether each field
# carries its own symbol or they all share their class's block, so the encoding
# the corpus exists to cover has to be asserted from the trace rather than from
# the answers.

if(NOT MONO_ENABLE_LLVM)
  return()
endif()

set(MONO_CORPUS_OUTPUTS "")
mono_corpus_cs(tiered-statics.exe SOURCES tiered-statics.cs
               REFS "${MONO_CORPUS_MINI_DIR}/TestDriver.dll")
mono_corpus_target(statics-symbols-corpus DEPENDS mini-test-driver)

mono_add_corpus_check(tiered-statics-symbols
  SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/check_statics_symbols.py"
  CORPUS "${CMAKE_CURRENT_BINARY_DIR}/tiered-statics.exe")

foreach(_n 1000 20 1 0)
  mono_add_tiered_test(statics tiered-statics.exe ${_n})
endforeach()
