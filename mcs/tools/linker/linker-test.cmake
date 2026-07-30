# Links one test program against the class libraries and runs the result.
#
# The point of each case is that the program still behaves correctly after the
# linker has removed everything it believes unreachable, so the run is the
# assertion: the programs throw on failure.
#
#   RUNTIME     mono-wrapper
#   TOOLS_PATH  the bootstrap profile, which is where monolinker runs
#   LINKER      monolinker.exe
#   PROFILE_DIR the class libraries to link against
#   TEST_EXE    the program to link
#   OUT_DIR     where the linked output goes
#   ROOTS       what to keep beyond what the program reaches (`none`, `mideast`)

file(REMOVE_RECURSE "${OUT_DIR}")
file(MAKE_DIRECTORY "${OUT_DIR}")

get_filename_component(_name "${TEST_EXE}" NAME)

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${TOOLS_PATH}"
          "${RUNTIME}" "${LINKER}" -c link -o "${OUT_DIR}" -b true
          -d "${PROFILE_DIR}" -l "${ROOTS}" -a "${TEST_EXE}"
  RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "monolinker failed (${_rc}) on ${_name}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${OUT_DIR}"
          "${RUNTIME}" --debug -O=-aot "${OUT_DIR}/${_name}"
  RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "${_name} failed (${_rc}) after linking; "
                      "the linked output is left in ${OUT_DIR}")
endif()

file(REMOVE_RECURSE "${OUT_DIR}")
