# The Microsoft conformance suites, from ms-test-suite.mk.
#
# Three NUnit-Lite suites -- conformance, systemruntimebringup and
# System.Linq.Expressions -- each with its own Makefile inside the checkout
# that takes MCS and NUNIT-CONSOLE from the caller.  Those Makefiles are part
# of the external repository, so they are still driven with make; this only
# supplies the toolchain and the CTest wiring.
#
# The checkout lives at git@github.com:xamarin/ms-test-suite.git, which is
# Xamarin-internal and not readable from outside.  ms-test-suite.mk handled
# that by swallowing the clone failure and printing a notice; here the suites
# simply are not registered when the checkout is absent.

if(NOT MONO_SUBMODULE_MS_TEST_SUITE_PRESENT)
  return()
endif()

set(_ms_dir "${MONO_SUBMODULE_MS_TEST_SUITE_PATH}")

# nunitlite.dll comes from mcs/tools/nunit-lite, which ms-test-suite.mk built
# on demand with a recursive make.
add_custom_target(nunitlite
  COMMAND "${MONO_GNU_MAKE}" -C "${MONO_MCS_TOPDIR}/tools/nunit-lite"
  COMMENT "MAKE nunit-lite"
  USES_TERMINAL
  VERBATIM)
add_dependencies(nunitlite acceptance-toolchain)

string(REPLACE ";" " " _ms_mcs "${_mcs}")

foreach(_suite conformance systemruntimebringup System.Linq.Expressions)
  string(TOLOWER "${_suite}" _slug)
  string(REPLACE "." "" _slug "${_slug}")

  add_test(NAME ms-test-suite-${_slug}
           COMMAND "${CMAKE_COMMAND}"
                   -D "MS_DIR=${_ms_dir}/${_suite}"
                   -D "MAKE=${MONO_GNU_MAKE}"
                   -D "MCS=${_ms_mcs} -debug:portable -t:library -warn:1 -r:${_class}/nunitlite.dll"
                   -D "NUNIT_CONSOLE=${_wrapper} --debug ${_class}/nunit-lite-console.exe -exclude=MonoBug,BadTest -format:nunit2"
                   -D "RESULT=${_bin}/TestResult-ms-test-suite-${_slug}.xml"
                   -D "MONO_PATH=${_class}"
                   -P "${_src}/ms-test-suite-run.cmake"
           WORKING_DIRECTORY "${_bin}")
  set_tests_properties(ms-test-suite-${_slug} PROPERTIES
    LABELS acceptance TIMEOUT 3600)
endforeach()
