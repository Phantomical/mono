# Converted from class/Mono.C5/Makefile.

mono_declare_managed(
  NAME       Mono.C5.dll
  PROFILES   net_4_x unityjit
  KEYFILE    c5.pub
  REFS       System
  FLAGS      -nowarn:169,219,414,1030,3001,3005,3006
  NO_INSTALL
)
