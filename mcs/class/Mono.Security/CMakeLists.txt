# Converted from class/Mono.Security/Makefile.

mono_declare_managed(
  NAME       Mono.Security.dll
  PROFILES   build net_4_x unityjit
  KEYFILE    ../mono.pub
  API_BIN_REFS System
  FLAGS      -unsafe -nowarn:1030,3009
)
