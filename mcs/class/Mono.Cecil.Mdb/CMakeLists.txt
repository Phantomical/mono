# Converted from class/Mono.Cecil.Mdb/Makefile.

mono_declare_managed(
  NAME       Mono.Cecil.Mdb.dll
  PROFILES   net_4_x
  KEYFILE    ../mono.snk
  PACKAGE    none
  REFS       System Mono.Cecil
  FLAGS      -d:CECIL
  NO_INSTALL
)
