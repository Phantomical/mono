# Runs one mono-symbolicate round trip.
#
# StackTraceDumper prints a set of stack traces with no line information. The
# symbolicator is then asked to put the line numbers back from the .msym store,
# and the result is diffed against Test/symbolicate.expected.
#
#   RUNTIME    mono-wrapper
#   LIB_PATH   the profile directory, which is also MONO_PATH
#   PROGRAM    mono-symbolicate.exe
#   TEST_EXE   the built StackTraceDumper.exe (its .pdb sits beside it)
#   OUT_DIR    scratch directory for this variant
#   EXPECTED   Test/symbolicate.expected
#   AOT        empty, `plain`, or `msym`

set(_mono "${RUNTIME}" -O=-inline)
set(_env "MONO_PATH=${LIB_PATH}")
set(_msym "${OUT_DIR}/msymdir")

function(_run)
  cmake_parse_arguments(R "" "OUTPUT_FILE;OUTPUT_VARIABLE" "COMMAND" ${ARGN})
  set(_extra "")
  if(R_OUTPUT_FILE)
    list(APPEND _extra OUTPUT_FILE "${R_OUTPUT_FILE}")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${_env} ${R_COMMAND}
                  ${_extra} OUTPUT_VARIABLE _out RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "failed (${_rc}): ${R_COMMAND}\n${_out}")
  endif()
  if(R_OUTPUT_VARIABLE)
    set(${R_OUTPUT_VARIABLE} "${_out}" PARENT_SCOPE)
  endif()
endfunction()

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${_msym}")

get_filename_component(_exedir "${TEST_EXE}" DIRECTORY)
get_filename_component(_exe "${TEST_EXE}" NAME)
string(REGEX REPLACE "\\.exe$" ".pdb" _pdb "${_exe}")
file(COPY "${TEST_EXE}" "${_exedir}/${_pdb}" DESTINATION "${OUT_DIR}")

# The store has to cover both the test program and the class libraries: several
# of the expected frames come from inside them, not the test program.
_run(COMMAND ${_mono} "${PROGRAM}" store-symbols "${_msym}" "${OUT_DIR}")
_run(COMMAND ${_mono} "${PROGRAM}" store-symbols "${_msym}" "${LIB_PATH}")

if(AOT STREQUAL "plain")
  _run(COMMAND ${_mono} --aot "${OUT_DIR}/${_exe}")
elseif(AOT STREQUAL "msym")
  _run(COMMAND ${_mono} "--aot=msym-dir=${_msym}" "${OUT_DIR}/${_exe}")
endif()

_run(COMMAND ${_mono} "${TEST_EXE}" OUTPUT_FILE "${OUT_DIR}/stacktrace.out")
_run(COMMAND ${_mono} "${PROGRAM}" "${_msym}" "${OUT_DIR}/stacktrace.out"
     OUTPUT_VARIABLE _raw)

# Normalize away everything that legitimately varies: the separator on Windows
# paths in the pdb, the absolute prefix in front of every source file, and the
# ids, which change with every build.
string(REPLACE "\\" "/" _result "${_raw}")
string(REGEX REPLACE "[^\n]*\\[MVID\\][^\n]*\n" "" _result "${_result}")
string(REGEX REPLACE "[^\n]*\\[AOTID\\][^\n]*\n" "" _result "${_result}")
string(REGEX REPLACE "\\) [^\n]* in [^\n]*/mcs/" ") in mcs/" _result "${_result}")
string(REGEX REPLACE "\\) [^\n]* in [^\n]*/external/" ") in external/"
       _result "${_result}")

set(_resultfile "${OUT_DIR}/symbolicate.result")
file(WRITE "${_resultfile}" "${_result}")

execute_process(COMMAND diff -up "${EXPECTED}" "${_resultfile}"
                OUTPUT_VARIABLE _diff RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR
          "Symbolicate tests failed.\n"
          "If ${_resultfile} is correct copy it to ${EXPECTED}.\n"
          "Otherwise runtime sequence points need to be fixed.\n${_diff}")
endif()
