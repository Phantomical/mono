mono_declare_managed(
  PROGRAM
  NAME       xbuild.exe
  PROFILES   net_4_x
  REFS       Microsoft.Build.Framework Microsoft.Build.Utilities.v4.0 Microsoft.Build.Engine Microsoft.Build.Tasks.v4.0 System System.Core
)

mono_declare_managed(
  PROGRAM
  NAME       xbuild.exe
  PROFILES   xbuild_12
  PACKAGE    xbuild/12.0/bin
  INSTALL_DIR <libdir>/mono/xbuild/12.0/bin
  REFS       Microsoft.Build.Framework Microsoft.Build.Utilities.v12.0 Microsoft.Build.Engine Microsoft.Build.Tasks.v12.0 ../net_4_x/System ../net_4_x/System.Core
)

mono_declare_managed(
  PROGRAM
  NAME       xbuild.exe
  PROFILES   xbuild_14
  PACKAGE    xbuild/14.0/bin
  INSTALL_DIR <libdir>/mono/xbuild/14.0/bin
  REFS       Microsoft.Build.Framework Microsoft.Build.Utilities.Core Microsoft.Build.Engine Microsoft.Build.Tasks.Core ../net_4_x/System ../net_4_x/System.Core
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
