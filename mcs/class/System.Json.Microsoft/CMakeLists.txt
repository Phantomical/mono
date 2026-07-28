# Converted from class/System.Json.Microsoft/Makefile.

mono_declare_managed(
  NAME       System.Json.Microsoft.dll
  PROFILES   net_4_x unityjit
  REFS       System System.Xml System.Core System.Runtime.Serialization
  FLAGS      /d:ASPNETMVC -keyfile:../winfx.pub -delaysign
)
