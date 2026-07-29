# What the tier-1 devirtualizer did, asserted from the pass's own trace.
#
# devirt-tests.exe computes the same answers whether or not a single call site
# was resolved, so the decisions themselves have to be asserted.  The negative
# expectations matter most -- a walk that answered with a common base instead of
# an exact class would still be green on every arithmetic check in the corpus.

if(NOT MONO_ENABLE_LLVM)
  return()
endif()

set(MONO_CORPUS_OUTPUTS "")
mono_corpus_cs(devirt-tests.exe SOURCES devirt-tests.cs
               REFS "${MONO_CORPUS_MINI_DIR}/TestDriver.dll")
mono_corpus_target(devirt-tags-corpus DEPENDS mini-test-driver)

mono_add_corpus_check(tiered-devirt-tags
  SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/check_devirt_tags.py"
  CORPUS "${CMAKE_CURRENT_BINARY_DIR}/devirt-tests.exe"
  SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/devirt-tests.cs")

# The direct call is not the point -- what the inliner can then do with it is.
# Resolving a site to something the inliner does not recognize as a managed
# callee would leave the pass green but pointless, so run the inliner's own
# assertions over this corpus as well.
mono_add_corpus_check(tiered-devirt-inlines
  SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../inliner-tags/check_inliner_tags.py"
  CORPUS "${CMAKE_CURRENT_BINARY_DIR}/devirt-tests.exe"
  SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/devirt-tests.cs")

foreach(_n 1000 20 1 0)
  mono_add_tiered_test(devirt devirt-tests.exe ${_n})
endforeach()
