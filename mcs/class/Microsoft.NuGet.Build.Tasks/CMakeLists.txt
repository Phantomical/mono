# Converted from class/Microsoft.NuGet.Build.Tasks/Makefile.

mono_declare_managed(
  NAME       Microsoft.NuGet.Build.Tasks.dll
  PROFILES   xbuild_14
  PACKAGE    xbuild/14.0/bin
  INSTALL_DIR lib/mono/xbuild/Microsoft/NuGet
  REFS       ../net_4_x/System ../net_4_x/System.Core ../net_4_x/System.Data ../net_4_x/System.Xml ../net_4_x/System.Xml.Linq ../net_4_x/System.Runtime.Serialization Microsoft.Build.Utilities.Core Microsoft.Build.Framework
  FLAGS      -nowarn:3021 /delaysign /keyfile:/../external/nuget-buildtasks/build/PublicKey.snk
)
