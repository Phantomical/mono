# Whether enabling sequence points changes the code mini emits.
#
# A sequence point is meant to be a marker the JIT records and otherwise steps
# over, so an optimization that does not know about OP_IL_SEQ_POINT tends to
# stop firing across one and the method silently grows.  check_seq_points.py
# compiles a corpus twice and compares the per-method code sizes.
#
# This has no corpus of its own -- it runs over the mini regression set, which
# is where the interesting variety of shapes already is.
#
# Disabled, not deleted, which is how automake had it too ("This test is
# fragile so disabled by default", check-seq-points-force, never part of
# `make check`): single-imm-size support is incomplete, so the sizes move for
# reasons that have nothing to do with sequence points.  Ten of the eighteen
# corpora currently report differences, from one method to forty-four.  Clear
# DISABLED per corpus as each is brought back to parity.

if(NOT MONO_MINI_REGTESTS)
  return()
endif()

foreach(_corpus IN LISTS MONO_MINI_REGTESTS)
  string(REGEX REPLACE "\\.exe$" "" _stem "${_corpus}")
  mono_add_corpus_check("seq-points/${_stem}"
    SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/check_seq_points.py"
    CORPUS "${MONO_CORPUS_MINI_DIR}/${_corpus}"
    LABELS manual)
  set_tests_properties("seq-points/${_stem}" PROPERTIES DISABLED TRUE)
endforeach()
