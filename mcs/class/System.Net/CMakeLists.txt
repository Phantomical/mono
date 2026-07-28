# Converted from class/System.Net/Makefile.

mono_declare_managed(
  NAME       System.Net.dll
  PROFILES   net_4_x unityjit
  OUTPUT_NAME dummy-System.Net.dll
  REFS       System System.Core System.Xml
  FLAGS      -unsafe -d:NET_3_5 -nowarn:1720
  NO_INSTALL
  NO_SIGN
)
