# The suite drives a real xbuild, which needs its targets and framework lists
# staged where an uninstalled build can find them.
include(${MONO_MCS_TOPDIR}/tools/xbuild/test-data.cmake)
mono_test_environment(
  PROFILE     net_4_x
  ASSEMBLY    Microsoft.Build.dll
  ENVIRONMENT ${MONO_XBUILD_TEST_ENV}
  DEPENDS     mcs-xbuild-test-data)
