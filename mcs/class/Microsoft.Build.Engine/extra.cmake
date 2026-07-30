# The suite embeds a tiny task assembly as a resource and then loads it back
# out at run time, to check that xbuild can bind a task from a file.
mono_test_fixture_assembly(
  PROFILE  net_4_x
  ASSEMBLY Microsoft.Build.Engine.dll
  NAME     TestTasks-net_4_x.dll
  SOURCES  Test/resources/TestTasks.cs
  REFS     Microsoft.Build.Framework Microsoft.Build.Utilities.v4.0)

# The suite drives a real xbuild, which needs its targets and framework lists
# staged where an uninstalled build can find them.
include(${MONO_MCS_TOPDIR}/tools/xbuild/test-data.cmake)
mono_test_environment(
  PROFILE     net_4_x
  ASSEMBLY    Microsoft.Build.Engine.dll
  ENVIRONMENT ${MONO_XBUILD_TEST_ENV}
  DEPENDS     mcs-xbuild-test-data)
