# Run a command and succeed only when it exits with a specific status.
#
#   cmake -D EXPECT=100 -D "COMMAND=prog;arg1;arg2" -P expect-exit.cmake
#
# The CoreCLR harnesses report success as 100, which CTest reads as failure.
# This is the `if [ $? -ne 100 ]; then exit 1; fi` idiom coreclr.mk used.

if(NOT DEFINED EXPECT OR NOT DEFINED COMMAND)
  message(FATAL_ERROR "expect-exit.cmake needs -D EXPECT= and -D COMMAND=")
endif()

execute_process(COMMAND ${COMMAND} RESULT_VARIABLE _rc)

if(NOT _rc EQUAL EXPECT)
  message(FATAL_ERROR "exited with ${_rc}, expected ${EXPECT}")
endif()
