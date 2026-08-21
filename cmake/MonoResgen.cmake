# Compiles one .resx into a .resources.  Run as `cmake -D ... -P`.
#
# A script rather than a plain COMMAND, because of the fallback that
# add_custom_command cannot express: resgen.exe is a managed tool, so it fails
# in the bootstrap profile, and in any tree where the runtime cannot yet run
# it. The checked-in .prebuilt beside the .resx is used instead.
#
# Inputs, all -D:
#   RESGEN      the argv, ;-separated
#   RESGEN_ENV  environment assignments for it, ;-separated (may be empty)
#   INPUT       the .resx
#   OUTPUT      the .resources to produce
#   PREBUILT    the checked-in fallback (may not exist)
#   WORKDIR     directory to run in -- -useSourcePath resolves against it

cmake_minimum_required(VERSION 3.28)

get_filename_component(_outdir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_outdir}")

set(_cmd ${RESGEN})
if(RESGEN_ENV)
  set(_cmd "${CMAKE_COMMAND}" -E env ${RESGEN_ENV} ${_cmd})
endif()

execute_process(
  COMMAND ${_cmd} ${RESGEN_FLAGS} "${INPUT}" "${OUTPUT}"
  WORKING_DIRECTORY "${WORKDIR}"
  RESULT_VARIABLE _rc)

if(NOT _rc EQUAL 0)
  if(EXISTS "${PREBUILT}")
    message(STATUS "resgen failed for ${INPUT}; using ${PREBUILT}")
    file(COPY_FILE "${PREBUILT}" "${OUTPUT}")
  else()
    message(FATAL_ERROR
      "resgen failed (${_rc}) for ${INPUT} and there is no ${PREBUILT}")
  endif()
endif()
