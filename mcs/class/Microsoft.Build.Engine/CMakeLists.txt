# Converted from class/Microsoft.Build.Engine/Makefile.

mono_declare_managed(
  NAME       Microsoft.Build.Engine.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../msfinal.pub
  REFS       System System.Core System.Xml Microsoft.Build.Framework Microsoft.Build.Utilities.v4.0
)

mono_declare_managed(
  NAME       Microsoft.Build.Engine.dll
  PROFILES   xbuild_12
  KEYFILE    ../msfinal.pub
  PACKAGE    xbuild/12.0/bin
  INSTALL_DIR lib/mono/xbuild/12.0/bin
  REFS       ../net_4_x/System ../net_4_x/System.Core ../net_4_x/System.Xml Microsoft.Build.Framework Microsoft.Build.Utilities.v12.0
)

mono_declare_managed(
  NAME       Microsoft.Build.Engine.dll
  PROFILES   xbuild_14
  KEYFILE    ../msfinal.pub
  PACKAGE    xbuild/14.0/bin
  INSTALL_DIR lib/mono/xbuild/14.0/bin
  REFS       ../net_4_x/System ../net_4_x/System.Core ../net_4_x/System.Xml Microsoft.Build.Framework Microsoft.Build.Utilities.Core
)
