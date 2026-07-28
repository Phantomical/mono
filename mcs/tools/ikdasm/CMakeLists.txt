# Converted from tools/ikdasm/Makefile.

mono_declare_managed(
  PROGRAM
  NAME       ikdasm.exe
  PROFILES   net_4_x
  REFS       System System.Core System.Security
  FLAGS      -d:NO_SYMBOL_WRITER
)
