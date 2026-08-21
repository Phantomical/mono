# The Microsoft conformance suites, from ms-test-suite.mk.
#
# Three NUnit-Lite suites -- conformance, systemruntimebringup and
# System.Linq.Expressions -- each with its own Makefile inside the checkout
# that takes MCS and NUNIT-CONSOLE from the caller.  Those Makefiles are part
# of the external repository, so they are still driven with make. This only
# supplies the toolchain and the CTest wiring.
#
# The checkout lives at git@github.com:xamarin/ms-test-suite.git, which is
# Xamarin-internal and not readable from outside.  ms-test-suite.mk printed a
# notice and carried on when the checkout was missing. Here the suites are
# not registered instead.

if(NOT MONO_SUBMODULE_MS_TEST_SUITE_PRESENT)
  return()
endif()

set(_ms_dir "${MONO_SUBMODULE_MS_TEST_SUITE_PATH}")

# The suites are compiled against nunitlite and run by its console.
add_custom_target(nunitlite)
add_dependencies(nunitlite acceptance-toolchain
                 mcs-net_4_x-nunitlite mcs-net_4_x-nunit-lite-console)

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
