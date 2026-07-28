# Converted from tools/mdb2ppdb/Makefile.

mono_declare_managed(
  PROGRAM
  NAME       mdb2ppdb.exe
  PROFILES   net_4_x
  REFS       Mono.Cecil Mono.Cecil.Mdb
  NO_INSTALL
)
