# Converted from class/System.Net/Makefile.

mono_declare_managed(
  NAME       System.Net.dll
  PROFILES   net_4_x unityjit
  REFS       System System.Core System.Xml
  FLAGS      -unsafe -d:NET_3_5 -nowarn:1720
)
