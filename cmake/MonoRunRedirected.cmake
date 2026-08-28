# Runs a command with its standard input, its standard output, or both attached
# to a file.
#
# A custom command cannot redirect, and the shells the two hosts have spell a
# redirection differently, so the redirection is done here instead:
#
#   cmake -D "CMD=<program>;<arg>..." [-D "INPUT=<file>"] [-D "OUTPUT=<file>"]
#         -P MonoRunRedirected.cmake
#
# CMD is a CMake list, so a `;` in an argument would split it. Nothing that
# uses this passes one.
#
# A failing command takes its output file with it. Ninja records the output as
# built as soon as the rule exits, so a half-written file left behind is read as
# up to date by every later build.

if(NOT DEFINED CMD)
  message(FATAL_ERROR "MonoRunRedirected.cmake: CMD is required")
endif()

if(NOT DEFINED INPUT AND NOT DEFINED OUTPUT)
  message(FATAL_ERROR "MonoRunRedirected.cmake: INPUT or OUTPUT is required")
endif()

set(_redirect "")
if(DEFINED INPUT)
  list(APPEND _redirect INPUT_FILE "${INPUT}")
endif()
if(DEFINED OUTPUT)
  list(APPEND _redirect OUTPUT_FILE "${OUTPUT}")
endif()

execute_process(COMMAND ${CMD} ${_redirect} RESULT_VARIABLE _rc)

if(NOT _rc EQUAL 0)
  if(DEFINED OUTPUT)
    file(REMOVE "${OUTPUT}")
  endif()
  message(FATAL_ERROR "failed (${_rc}): ${CMD}")
endif()
