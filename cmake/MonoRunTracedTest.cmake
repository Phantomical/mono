# Runs a program with the JIT trace on and requires named methods to have been
# compiled during the run.
#
#   cmake -DMONO_TRACE_REQUIRE="Tests:foo;Tests:bar"
#         -P MonoRunTracedTest.cmake -- <runtime> <args...> <test.exe>
#
# This is for tests whose subject is a method being compiled while something
# else is running, where the compile is a background one and nothing in the
# program can wait for it. Such a test passes both when it exercised the
# crossing and when the compile never landed, and those two outcomes are worth
# telling apart -- so the run has to say which happened. MONO_LLVM_JIT_TRACE
# names every method the backend translates, which is that signal.
#
# Output is captured rather than streamed so it can be searched, and the tail is
# printed on failure. The tail is the useful end: a trace runs to thousands of
# lines, and an unhandled exception prints last.

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
  message(FATAL_ERROR "MonoRunTracedTest: no command given after --")
endif()

set(ENV{MONO_LLVM_JIT_TRACE} 1)

execute_process(COMMAND ${_cmd}
                RESULT_VARIABLE _rc
                OUTPUT_VARIABLE _out
                ERROR_VARIABLE _err)
set(_all "${_out}${_err}")

# Report a bad exit before a missing method: a run that died early is why the
# method would be missing, and naming the trace instead would misdirect.
function(_fail_with_tail _why)
  string(REPLACE "\n" ";" _lines "${_all}")
  list(LENGTH _lines _n)
  if(_n GREATER 80)
    math(EXPR _from "${_n} - 80")
    list(SUBLIST _lines ${_from} 80 _lines)
    set(_elided "  [... ${_from} earlier lines elided ...]\n")
  endif()
  string(REPLACE ";" "\n" _tail "${_lines}")
  message(FATAL_ERROR "${_why}\n${_elided}${_tail}")
endfunction()

if(NOT _rc EQUAL 0)
  _fail_with_tail("exit code ${_rc}, expected 0")
endif()

foreach(_want IN LISTS MONO_TRACE_REQUIRE)
  if(NOT _all MATCHES "translating ${_want}")
    _fail_with_tail(
      "${_want} was never compiled, so the run did not test what it is for. "
      "Its caller may have finished before the background compile landed.")
  endif()
endforeach()
