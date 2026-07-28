# Converted from class/System.Data.Services.Client/Makefile.

mono_declare_managed(
  NAME       System.Data.Services.Client.dll
  PROFILES   net_4_x unityjit
  KEYFILE    ../ecma.pub
  REFS       System System.Core System.Xml.Linq System.Data System.Xml WindowsBase
  FLAGS      -d:NET_3_5 -warn:2
)
