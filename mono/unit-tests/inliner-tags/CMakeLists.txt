# What the tier-1 inliner did, asserted from the pass's own trace.
#
# The corpus only ever checks that tier-1 code computes the right answer, which
# it does whether or not anything was inlined -- so an inliner that quietly
# stands down is a green run, and that has happened before.  check_inliner_tags.py
# turns MONO_INLINER_TRACE into assertions against expectations written next to
# the fixtures.

if(NOT MONO_ENABLE_LLVM)
  return()
endif()

set(MONO_CORPUS_OUTPUTS "")
mono_corpus_il(inliner-fault.dll inliner-fault.il LIBRARY)
mono_corpus_cs(inliner-tests.exe SOURCES inliner-tests.cs
               REFS "${MONO_CORPUS_MINI_DIR}/TestDriver.dll"
                    "${CMAKE_CURRENT_BINARY_DIR}/inliner-fault.dll")
mono_corpus_target(inliner-tags-corpus DEPENDS mini-test-driver)

mono_add_corpus_check(tiered-inliner-tags
  SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/check_inliner_tags.py"
  CORPUS "${CMAKE_CURRENT_BINARY_DIR}/inliner-tests.exe"
  SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/inliner-tests.cs")

# The corpus also has to keep computing the right answers, at every threshold:
# the tag check runs eagerly at threshold 0, which is the one configuration
# where nothing is promoted on a background worker.
foreach(_n 1000 20 1)
  mono_add_tiered_test(inliner-regression inliner-tests.exe ${_n} ARGS --regression)
endforeach()
foreach(_n 1000 20 1 0)
  mono_add_tiered_test(inliner inliner-tests.exe ${_n})
endforeach()
