# Converted from class/Mono.CodeContracts/Makefile.

mono_declare_managed(
  NAME       Mono.CodeContracts.dll
  PROFILES   net_4_x
  KEYFILE    ../mono.pub
  REFS       System System.Core Mono.Cecil Mono.Cecil.Mdb
  FLAGS      -nowarn:618
)
