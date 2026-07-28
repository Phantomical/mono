# Converted from tools/pdb2mdb/Makefile.

mono_declare_managed(
  PROGRAM
  NAME       pdb2mdb.exe
  PROFILES   net_4_x
  REFS       Mono.Cecil Mono.CompilerServices.SymbolWriter System.Core
)
