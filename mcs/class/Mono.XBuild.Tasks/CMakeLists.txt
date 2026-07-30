mono_declare_managed(
  NAME       Mono.XBuild.Tasks.dll
  PROFILES   net_4_x
  KEYFILE    ../mono.pub
  SNK        <topdir>/class/mono.snk
  TEST_CONFIG_RUNTIME Test/test-config-file-net_4_x
  REFS       System System.Xml
)

mono_declare_managed(
  NAME       Mono.XBuild.Tasks.dll
  PROFILES   unityjit
  KEYFILE    ../mono.pub
  SNK        <topdir>/class/mono.snk
  TEST_CONFIG_RUNTIME Test/test-config-file-unityjit
  REFS       System System.Xml
)

mono_declare_managed(
  NAME       Mono.XBuild.Tasks.dll
  PROFILES   xbuild_12
  KEYFILE    ../mono.pub
  SNK        <topdir>/class/mono.snk
  PACKAGE    xbuild/12.0/bin
  INSTALL_DIR <libdir>/mono/xbuild/12.0/bin
  TEST_CONFIG_RUNTIME Test/test-config-file-xbuild_12
  REFS       ../net_4_x/System ../net_4_x/System.Xml
)

mono_declare_managed(
  NAME       Mono.XBuild.Tasks.dll
  PROFILES   xbuild_14
  KEYFILE    ../mono.pub
  SNK        <topdir>/class/mono.snk
  PACKAGE    xbuild/14.0/bin
  INSTALL_DIR <libdir>/mono/xbuild/14.0/bin
  TEST_CONFIG_RUNTIME Test/test-config-file-xbuild_14
  REFS       ../net_4_x/System ../net_4_x/System.Xml
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
