# One of the resources the suite embeds is an assembly rather than a checked-in
# file: the tests that read metadata out of a reference need something to read.
mono_test_fixture_assembly(
  PROFILE  net_4_x
  ASSEMBLY Microsoft.Build.Tasks.v4.0.dll
  NAME     test.dll
  SOURCES  Test/resources/test.cs)

# The suite drives a real xbuild, which needs its targets and framework lists
# staged where an uninstalled build can find them.
include(${MONO_MCS_TOPDIR}/tools/xbuild/test-data.cmake)
mono_test_environment(
  PROFILE     net_4_x
  ASSEMBLY    Microsoft.Build.Tasks.v4.0.dll
  ENVIRONMENT ${MONO_XBUILD_TEST_ENV}
  DEPENDS     mcs-xbuild-test-data)
