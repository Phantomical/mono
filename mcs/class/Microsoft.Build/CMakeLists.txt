# Converted from class/Microsoft.Build/Makefile.

mono_declare_managed(
  NAME       Microsoft.Build.dll
  PROFILES   net_4_x unityjit xbuild_12 xbuild_14
  KEYFILE    ../msfinal.pub
  REFS       System System.Core System.Xml Microsoft.Build.Engine Microsoft.Build.Framework
  FLAGS      /d:MICROSOFT_BUILD_DLL
)
