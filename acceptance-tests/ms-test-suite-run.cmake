# Build and run one ms-test-suite suite.
#
# Each suite ships a Makefile with `build` and `run` targets that take MCS and
# NUNIT-CONSOLE from the environment, so this is the two sub-makes
# ms-test-suite.mk ran, in order, with the failure of either reported.

foreach(_v MS_DIR MAKE MCS NUNIT_CONSOLE RESULT)
  if(NOT DEFINED ${_v})
    message(FATAL_ERROR "ms-test-suite-run.cmake needs -D ${_v}=")
  endif()
endforeach()

execute_process(
  COMMAND "${MAKE}" -C "${MS_DIR}" build "MCS=${MCS}"
  RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "build failed (${_rc})")
endif()

execute_process(
  COMMAND "${MAKE}" -C "${MS_DIR}" run
          "NUNIT-CONSOLE=${NUNIT_CONSOLE}"
          "NUNIT_XML_RESULT=-result:${RESULT}"
  RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "run failed (${_rc})")
endif()
