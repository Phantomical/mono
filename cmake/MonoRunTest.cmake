# Runs one program from the runtime corpus as a single CTest test.
#
#   cmake -DMONO_TEST_EXPECT=<code> [-DMONO_TEST_TIMEOUT=<seconds>]
#         [-DMONO_TIMEOUT_BINARY=<path to timeout(1)>] [-DMONO_TEST_TMPDIR=<dir>]
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
if(MONO_TEST_TIMEOUT AND MONO_TIMEOUT_BINARY)
  list(PREPEND _cmd "${MONO_TIMEOUT_BINARY}" -s QUIT --kill-after=10 "${MONO_TEST_TIMEOUT}")
endif()

# A temporary directory of the test's own, so that programs writing fixed names
# under Path.GetTempPath () do not collide. Several of them do -- bug-349190.2
# saves an assembly to res.exe and then loads it back -- and every such name is
# shared by the two collector arms, by any other worktree running the suite, and
# by whatever else on the machine writes to /tmp. What that costs is worse than
# a lost file: the loader maps an assembly, so a second process rewriting one
# out from under the mapping faults the reader with SIGBUS on a page past the
# new end of file, which is not an error any managed code can catch.
#
# Kept on failure, since a test that writes temporary files is a test whose
# temporary files are worth reading afterwards.
if(MONO_TEST_TMPDIR)
  file(REMOVE_RECURSE "${MONO_TEST_TMPDIR}")
  file(MAKE_DIRECTORY "${MONO_TEST_TMPDIR}")
  set(ENV{TMPDIR} "${MONO_TEST_TMPDIR}")
endif()

execute_process(COMMAND ${_cmd} RESULT_VARIABLE _rc)

if(MONO_TEST_TMPDIR AND _rc EQUAL MONO_TEST_EXPECT)
  file(REMOVE_RECURSE "${MONO_TEST_TMPDIR}")
endif()

if(NOT _rc EQUAL MONO_TEST_EXPECT)
  string(STRIP "${_pretty}" _pretty)
  if(_rc EQUAL 124 OR _rc EQUAL 137)
    message(FATAL_ERROR
      "timed out after ${MONO_TEST_TIMEOUT}s (thread dump above, if any):\n  ${_pretty}")
  endif()
  message(FATAL_ERROR
    "exit code ${_rc}, expected ${MONO_TEST_EXPECT}:\n  ${_pretty}")
endif()
