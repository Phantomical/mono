# Runs one program from the runtime corpus as a single CTest test.
#
#   cmake -DMONO_TEST_EXPECT=<code> [-DMONO_TEST_TIMEOUT=<seconds>]
#         -P MonoRunTest.cmake -- <runtime> <args...> <test.exe>
#
# CTest can already run a command and check that it exited 0, so this exists
# for the two things it cannot do: expect a specific non-zero exit code (the
# unhandled-exception suites assert that the runtime exits 1 or 255), and get
# a thread dump out of a test that hangs.
#
# The dump is the reason for the `timeout` prefix rather than CTest's own
# TIMEOUT property: SIGQUIT makes the runtime print every thread's stack, which
# is the only useful artifact a hang produces. CTest's timeout is still set, a
# little higher, so a test that survives even SIGKILL is not left running.

set(_cmd "")
set(_pretty "")
set(_seen_sep FALSE)
foreach(_i RANGE ${CMAKE_ARGC})
  if(NOT DEFINED CMAKE_ARGV${_i})
    continue()
  endif()
  if(_seen_sep)
    # Escape first: appending a value that contains a semicolon would otherwise
    # split it into several arguments.
    string(REPLACE ";" "\\;" _arg "${CMAKE_ARGV${_i}}")
    list(APPEND _cmd "${_arg}")
    string(APPEND _pretty " ${CMAKE_ARGV${_i}}")
  elseif(CMAKE_ARGV${_i} STREQUAL "--")
    set(_seen_sep TRUE)
  endif()
endforeach()

if(NOT _cmd)
  message(FATAL_ERROR "MonoRunTest: no command given after --")
endif()

if(NOT DEFINED MONO_TEST_EXPECT)
  set(MONO_TEST_EXPECT 0)
endif()

# --kill-after gives the runtime a window to finish writing the dump before
# SIGKILL lands.
if(MONO_TEST_TIMEOUT)
  find_program(_timeout timeout)
  if(_timeout)
    list(PREPEND _cmd "${_timeout}" -s QUIT --kill-after=10 "${MONO_TEST_TIMEOUT}")
  endif()
endif()

execute_process(COMMAND ${_cmd} RESULT_VARIABLE _rc)

if(NOT _rc EQUAL MONO_TEST_EXPECT)
  string(STRIP "${_pretty}" _pretty)
  if(_rc EQUAL 124 OR _rc EQUAL 137)
    message(FATAL_ERROR
      "timed out after ${MONO_TEST_TIMEOUT}s (thread dump above, if any):\n  ${_pretty}")
  endif()
  message(FATAL_ERROR
    "exit code ${_rc}, expected ${MONO_TEST_EXPECT}:\n  ${_pretty}")
endif()
