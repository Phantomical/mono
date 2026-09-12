# Runs test-remset-missing and judges it the way MONO_SGEN_STRICT_REMSET_CHECK
# predicts.
#
#   cmake -DMONO_SGEN_STRICT_REMSET_CHECK=<bool>
#         -P check-remset-missing.cmake -- <binary>
#
# ctest's PASS_REGULAR_EXPRESSION counts a signal death as a failure before it
# reads the regex at all (verified against ctest 3.28). Under the option a
# confirmed miss aborts, which is exactly such a death, so that property cannot
# judge this test.
#
# A CMake script rather than a shell one, because ctest resolves a test command
# through PATH at run time. On Windows `sh` is on Git Bash's PATH alone, so a
# ctest started from any other shell would report this test Not Run.

set(_cmd "")
set(_seen_sep FALSE)
foreach(_i RANGE ${CMAKE_ARGC})
  if(NOT DEFINED CMAKE_ARGV${_i})
    continue()
  endif()
  if(_seen_sep)
    string(REPLACE ";" "\\;" _arg "${CMAKE_ARGV${_i}}")
    list(APPEND _cmd "${_arg}")
  elseif(CMAKE_ARGV${_i} STREQUAL "--")
    set(_seen_sep TRUE)
  endif()
endforeach()

if(NOT _cmd)
  message(FATAL_ERROR "check-remset-missing: no command given after --")
endif()

execute_process(COMMAND ${_cmd}
                RESULT_VARIABLE _rc
                OUTPUT_VARIABLE _out
                ERROR_VARIABLE _err)
set(_all "${_out}${_err}")

# Printed whatever the verdict is, because ctest reads SKIP_REGULAR_EXPRESSION
# off what the test wrote. A run that skipped for want of a class library
# otherwise counts as a failure.
message(NOTICE "${_all}")

# Names RemsetHolder, so an unrelated missing barrier somewhere else in the run
# cannot stand in for the one this test builds.
string(FIND "${_all}" "Missing write barrier: .RemsetHolder field 'Slot'" _confirmed)

string(FIND "${_all}" "not found in remsets, but object is pinned" _excused)

if(MONO_SGEN_STRICT_REMSET_CHECK)
  # A confirmed miss aborts the process by design.
  if(_rc STREQUAL "0")
    message(FATAL_ERROR "exit code 0, expected the abort a confirmed miss takes")
  endif()
  if(_confirmed EQUAL -1)
    message(FATAL_ERROR "died with ${_rc} without confirming the miss on RemsetHolder")
  endif()
else()
  if(NOT _rc STREQUAL "0")
    message(FATAL_ERROR "exit code ${_rc}, expected 0")
  endif()
  # The excuse line has to be there. Without it the run proves nothing: a test
  # that built no pinned miss at all also exits 0 and logs nothing.
  if(_excused EQUAL -1)
    message(FATAL_ERROR "the checker never excused a pinned miss, so the run tested nothing")
  endif()
  if(NOT _confirmed EQUAL -1)
    message(FATAL_ERROR "the checker confirmed the miss with the option off")
  endif()
endif()
