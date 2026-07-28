# Converted from class/Microsoft.Build.Framework/Makefile.

mono_declare_managed(
  NAME       Microsoft.Build.Framework.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../msfinal.pub
  REFS       System
)

mono_declare_managed(
  NAME       Microsoft.Build.Framework.dll
  PROFILES   xbuild_12
  KEYFILE    ../msfinal.pub
  PACKAGE    xbuild/12.0/bin
  INSTALL_DIR lib/mono/xbuild/12.0/bin
  REFS       ../net_4_x/System
)

mono_declare_managed(
  NAME       Microsoft.Build.Framework.dll
  PROFILES   xbuild_14
  KEYFILE    ../msfinal.pub
  PACKAGE    xbuild/14.0/bin
  INSTALL_DIR lib/mono/xbuild/14.0/bin
  REFS       ../net_4_x/System
)
