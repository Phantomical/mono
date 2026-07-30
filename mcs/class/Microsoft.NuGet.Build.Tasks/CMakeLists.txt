mono_declare_managed(
  NAME       Microsoft.NuGet.Build.Tasks.dll
  PROFILES   xbuild_14
  SNK        <topdir>/class/mono.snk
  PACKAGE    xbuild/14.0/bin
  INSTALL_DIR <libdir>/mono/xbuild/Microsoft/NuGet
  REFS       ../net_4_x/System ../net_4_x/System.Core ../net_4_x/System.Data ../net_4_x/System.Xml ../net_4_x/System.Xml.Linq ../net_4_x/System.Runtime.Serialization Microsoft.Build.Utilities.Core Microsoft.Build.Framework
  FLAGS      -nowarn:3021 /delaysign /keyfile:<topdir>/../external/nuget-buildtasks/build/PublicKey.snk
  RESOURCE_DEFS Microsoft.NuGet.Build.Tasks.Strings,../../../external/nuget-buildtasks/src/Microsoft.NuGet.Build.Tasks/Strings.resx
)

# Whatever else the Makefile did.  Hand-written, and optional.
include(${CMAKE_CURRENT_SOURCE_DIR}/extra.cmake OPTIONAL)
