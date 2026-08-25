# What `--jitdump` tells perf about the code this runtime writes.
#
# A record's range is what perf resolves a sample against. Where two records
# claim one address, the name a sample gets falls out of the order the records
# were written, which nothing states. The ranges change no code the runtime
# executes, and a report built on a wrong one still names real methods, so the
# corpus stays green and the profile lies. check_jitdump.py reads the dump back.

if(NOT MONO_ENABLE_LLVM)
  return()
endif()

set(MONO_CORPUS_OUTPUTS "")

mono_corpus_cs("batched-methods.exe" SOURCES "batched-methods.cs")

mono_add_corpus_check("jitdump/batched-methods"
  SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/check_jitdump.py"
  CORPUS "${CMAKE_CURRENT_BINARY_DIR}/batched-methods.exe")

mono_corpus_target(jitdump-corpora)
