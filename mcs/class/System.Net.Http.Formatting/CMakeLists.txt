# Converted from class/System.Net.Http.Formatting/Makefile.

mono_declare_managed(
  NAME       System.Net.Http.Formatting.dll
  PROFILES   net_4_x unityjit
  REFS       System.Core System System.Net.Http System.Xml System.Runtime.Serialization System.Xml.Linq System.Data System.Configuration
  FLAGS      -d:ASPNETMVC -keyfile:../winfx.pub -delaysign
)
