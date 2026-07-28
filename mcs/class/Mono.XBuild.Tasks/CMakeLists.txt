# Converted from class/Mono.XBuild.Tasks/Makefile.

mono_declare_managed(
  NAME       Mono.XBuild.Tasks.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../mono.pub
  REFS       System System.Xml
)

mono_declare_managed(
  NAME       Mono.XBuild.Tasks.dll
  PROFILES   xbuild_12
  KEYFILE    ../mono.pub
  PACKAGE    xbuild/12.0/bin
  INSTALL_DIR lib/mono/xbuild/12.0/bin
  REFS       ../net_4_x/System ../net_4_x/System.Xml
)

mono_declare_managed(
  NAME       Mono.XBuild.Tasks.dll
  PROFILES   xbuild_14
  KEYFILE    ../mono.pub
  PACKAGE    xbuild/14.0/bin
  INSTALL_DIR lib/mono/xbuild/14.0/bin
  REFS       ../net_4_x/System ../net_4_x/System.Xml
)
