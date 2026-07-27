# One generated test that walks every GC descriptor shape. The generator emits
# a few thousand field layouts, which is why it is generated rather than
# written out.

find_package(Python3 REQUIRED COMPONENTS Interpreter)

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

# Building this one is the coverage that matters, and it happens on every
# `cmake --build`. Running it is worth doing too, but the generator emits a few
# thousand field layouts and walking all of them takes well over fifteen
# minutes, so the test is labelled `slow` and stays out of the default run.
add_test(NAME gc-descriptors
         COMMAND "${CMAKE_COMMAND}" -E env "MONO_PATH=${_class}"
                 "${_wrapper}" descriptor-tests.exe
         WORKING_DIRECTORY "${_gcd_bin}")
set_tests_properties(gc-descriptors PROPERTIES
  LABELS slow FIXTURES_REQUIRED runtime_corpora TIMEOUT 3600)
