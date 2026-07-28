# Converted from class/Microsoft.NuGet.Build.Tasks/Makefile.

mono_declare_managed(
  NAME       Microsoft.NuGet.Build.Tasks.dll
  PROFILES   xbuild_14
  INSTALL_DIR lib/mono/xbuild/Microsoft/NuGet
  REFS       System System.Core System.Data System.Xml System.Xml.Linq System.Runtime.Serialization
  FLAGS      -nowarn:3021 /delaysign /keyfile:/../external/nuget-buildtasks/build/PublicKey.snk
)
