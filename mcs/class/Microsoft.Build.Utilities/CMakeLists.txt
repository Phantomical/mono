# Converted from class/Microsoft.Build.Utilities/Makefile.

mono_declare_managed(
  NAME       Microsoft.Build.Utilities.dll
  PROFILES   net_4_x unityjit
  OUTPUT_NAME Microsoft.Build.Utilities.v4.0.dll
  KEYFILE    ../msfinal.pub
  REFS       System System.Core System.Xml Microsoft.Build.Framework
)

mono_declare_managed(
  NAME       Microsoft.Build.Utilities.dll
  PROFILES   xbuild_12
  OUTPUT_NAME Microsoft.Build.Utilities.v12.0.dll
  KEYFILE    ../msfinal.pub
  PACKAGE    xbuild/12.0/bin
  INSTALL_DIR lib/mono/xbuild/12.0/bin
  REFS       ../net_4_x/System ../net_4_x/System.Core ../net_4_x/System.Xml Microsoft.Build.Framework
)

mono_declare_managed(
  NAME       Microsoft.Build.Utilities.dll
  PROFILES   xbuild_14
  OUTPUT_NAME Microsoft.Build.Utilities.Core.dll
  KEYFILE    ../msfinal.pub
  PACKAGE    xbuild/14.0/bin
  INSTALL_DIR lib/mono/xbuild/14.0/bin
  REFS       ../net_4_x/System ../net_4_x/System.Core ../net_4_x/System.Xml Microsoft.Build.Framework
)
