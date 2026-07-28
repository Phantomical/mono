# Converted from class/Microsoft.Build/Makefile.

mono_declare_managed(
  NAME       Microsoft.Build.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../msfinal.pub
  REFS       System System.Core System.Xml Microsoft.Build.Engine Microsoft.Build.Framework
  FLAGS      /d:MICROSOFT_BUILD_DLL
)

mono_declare_managed(
  NAME       Microsoft.Build.dll
  PROFILES   xbuild_12
  KEYFILE    ../msfinal.pub
  PACKAGE    xbuild/12.0/bin
  INSTALL_DIR lib/mono/xbuild/12.0/bin
  REFS       ../net_4_x/System ../net_4_x/System.Core ../net_4_x/System.Xml Microsoft.Build.Engine Microsoft.Build.Framework
  FLAGS      /d:MICROSOFT_BUILD_DLL
)

mono_declare_managed(
  NAME       Microsoft.Build.dll
  PROFILES   xbuild_14
  KEYFILE    ../msfinal.pub
  PACKAGE    xbuild/14.0/bin
  INSTALL_DIR lib/mono/xbuild/14.0/bin
  REFS       ../net_4_x/System ../net_4_x/System.Core ../net_4_x/System.Xml Microsoft.Build.Engine Microsoft.Build.Framework
  FLAGS      /d:MICROSOFT_BUILD_DLL
)
