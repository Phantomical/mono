# One generated test that walks every GC descriptor shape. The generator emits
# a few thousand field layouts, which is why it is generated rather than
# written out.

set(_gcd_bin "${CMAKE_CURRENT_BINARY_DIR}")
set(_gcd_src "${CMAKE_CURRENT_SOURCE_DIR}")

# The generator reads its two input fragments from the working directory.
foreach(_f descriptor-tests-driver.cs descriptor-tests-prefix.cs)
  configure_file("${_gcd_src}/${_f}" "${_gcd_bin}/${_f}" COPYONLY)
endforeach()

add_custom_command(
  OUTPUT "${_gcd_bin}/descriptor-tests.cs"
  COMMAND sh -c "'${_gcd_src}/gen-descriptor-tests.py' > '${_gcd_bin}/descriptor-tests.cs'"
  DEPENDS "${_gcd_src}/gen-descriptor-tests.py"
          "${_gcd_bin}/descriptor-tests-driver.cs"
          "${_gcd_bin}/descriptor-tests-prefix.cs"
  WORKING_DIRECTORY "${_gcd_bin}"
  COMMENT "GEN descriptor-tests.cs"
  VERBATIM)

add_custom_command(
  OUTPUT "${_gcd_bin}/descriptor-tests.exe"
  COMMAND ${_csc_unsafe} -Warn:0 "-lib:${_class}"
          "-out:${_gcd_bin}/descriptor-tests.exe" "${_gcd_bin}/descriptor-tests.cs"
  DEPENDS "${_gcd_bin}/descriptor-tests.cs" mono-test-toolchain
  COMMENT "CSC descriptor-tests.exe"
  VERBATIM)

add_custom_target(gc-descriptor-tests DEPENDS "${_gcd_bin}/descriptor-tests.exe")
add_dependencies(runtime-corpora gc-descriptor-tests)

# The run is dominated by compiling the generated code, not by executing it:
# one pass allocates every shape, which drags roughly twenty thousand tiny
# methods through the JIT, and everything after that is fast. That is what puts
# the test on the `slow` label. Pass a pass count as argv[1] for a longer soak.
add_test(NAME gc-descriptors
         COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class}"
                 "${_wrapper}" descriptor-tests.exe
         WORKING_DIRECTORY "${_gcd_bin}")
set_tests_properties(gc-descriptors PROPERTIES
  LABELS slow TIMEOUT 1800)
