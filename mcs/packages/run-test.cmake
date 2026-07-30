# Runs the csi smoke script and checks it printed what it should have.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${MONO_PATH}"
          "${RUNTIME}" "${CSI}" "${SCRIPT}"
  OUTPUT_VARIABLE _out RESULT_VARIABLE _rc)
message(STATUS "${_out}")
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "csi exited ${_rc}")
endif()
if(NOT _out MATCHES "hello world")
  message(FATAL_ERROR "csi did not print `hello world`")
endif()
