# Converted from class/legacy/Mono.Cecil/Makefile.

mono_declare_managed(
  NAME       Mono.Cecil.dll
  PROFILES   net_4_x unityjit
  SUBDIR     legacy
  PACKAGE    none
  REFS       System.Core
  FLAGS      -keyfile:../../mono.snk -d:NET_3_5 /publicsign
)
