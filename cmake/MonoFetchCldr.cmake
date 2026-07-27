# Script mode: fetch and unpack a CLDR core.zip.  Run by
# tools/locale-builder, which needs the data at build time rather than at
# configure time -- nobody configuring the runtime should pay for a download
# they will not use.
#
# Expects URL, DEST and EXTRACT_TO on the command line.

if(EXISTS "${EXTRACT_TO}/common/supplemental/supplementalData.xml")
  return()
endif()

if(NOT EXISTS "${DEST}")
  file(DOWNLOAD "${URL}" "${DEST}"
       SHOW_PROGRESS
       STATUS _status)
  list(GET _status 0 _code)
  if(NOT _code EQUAL 0)
    list(GET _status 1 _msg)
    file(REMOVE "${DEST}")
    message(FATAL_ERROR "Downloading ${URL} failed: ${_msg}")
  endif()
endif()

file(MAKE_DIRECTORY "${EXTRACT_TO}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E tar xf "${DEST}"
  WORKING_DIRECTORY "${EXTRACT_TO}"
  RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "Unpacking ${DEST} failed")
endif()
