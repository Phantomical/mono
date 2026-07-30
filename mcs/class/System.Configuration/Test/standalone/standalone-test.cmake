# Runs one standalone configuration test and diffs its output.
#
# Inputs, all -D:
#   RUNTIME     the mono wrapper
#   PROFILE_DIR the assemblies to run against
#   RUN_DIR     the staged directory holding the exe and its config files
#   EXE         the program to run
#   EXPECTED    the output it must produce

cmake_minimum_required(VERSION 3.28)

execute_process(
  COMMAND "${RUNTIME}" "${EXE}"
  WORKING_DIRECTORY "${RUN_DIR}"
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err
  RESULT_VARIABLE _rc)

if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "${EXE} exited ${_rc}\n${_out}${_err}")
endif()

file(READ "${EXPECTED}" _want)

# The expectations are checked in with CRLF in places, and the Makefile diffed
# with --strip-trailing-cr.
string(REPLACE "\r\n" "\n" _want "${_want}")
string(REPLACE "\r\n" "\n" _out "${_out}")
string(STRIP "${_want}" _want)
string(STRIP "${_out}" _out)

if(NOT _out STREQUAL _want)
  message(FATAL_ERROR "output differs\n--- expected ---\n${_want}\n--- got ---\n${_out}")
endif()
