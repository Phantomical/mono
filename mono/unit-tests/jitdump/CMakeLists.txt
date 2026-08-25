# What `--jitdump` tells perf about the code this runtime writes.
#
# A record's range is what perf resolves a sample against, so two records that
# claim the same address make a profile blame the wrong method. Nothing about
# executing the code depends on the ranges, and the report that comes out of a
# wrong one still names real methods -- so the corpus stays green and the
# profile lies. check_jitdump.py runs the fixture and reads the dump back.

if(NOT MONO_ENABLE_LLVM)
  return()
endif()

set(MONO_CORPUS_OUTPUTS "")

mono_corpus_cs("batched-methods.exe" SOURCES "batched-methods.cs")

mono_add_corpus_check("jitdump/batched-methods"
  SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/check_jitdump.py"
  CORPUS "${CMAKE_CURRENT_BINARY_DIR}/batched-methods.exe")

mono_corpus_target(jitdump-corpora)
