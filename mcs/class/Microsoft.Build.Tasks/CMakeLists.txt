# Converted from class/Microsoft.Build.Tasks/Makefile.

mono_declare_managed(
  NAME       Microsoft.Build.Tasks.dll
  PROFILES   net_4_x unityjit
  OUTPUT_NAME Microsoft.Build.Tasks.v4.0.dll
  KEYFILE    ../msfinal.pub
  REFS       System System.Core System.Xml System.Windows.Forms Microsoft.Build.Utilities.v4.0 Microsoft.Build.Framework Microsoft.Build.Engine Mono.XBuild.Tasks
)

mono_declare_managed(
  NAME       Microsoft.Build.Tasks.dll
  PROFILES   xbuild_12
  OUTPUT_NAME Microsoft.Build.Tasks.v12.0.dll
  KEYFILE    ../msfinal.pub
  PACKAGE    xbuild/12.0/bin
  INSTALL_DIR lib/mono/xbuild/12.0/bin
  REFS       ../net_4_x/System ../net_4_x/System.Core ../net_4_x/System.Xml ../net_4_x/System.Windows.Forms Microsoft.Build.Utilities.v12.0 Microsoft.Build.Framework Microsoft.Build.Engine Mono.XBuild.Tasks
)

mono_declare_managed(
  NAME       Microsoft.Build.Tasks.dll
  PROFILES   xbuild_14
  OUTPUT_NAME Microsoft.Build.Tasks.Core.dll
  KEYFILE    ../msfinal.pub
  PACKAGE    xbuild/14.0/bin
  INSTALL_DIR lib/mono/xbuild/14.0/bin
  REFS       ../net_4_x/System ../net_4_x/System.Core ../net_4_x/System.Xml ../net_4_x/System.Windows.Forms Microsoft.Build.Utilities.Core Microsoft.Build.Framework Microsoft.Build.Engine Mono.XBuild.Tasks
)
